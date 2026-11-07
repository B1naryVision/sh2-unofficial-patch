#include "replaySeeker.h"

#include "../core/exeBase.h"
#include "../core/frameTick.h"
#include "../core/observerContext.h"
#include "replayCommon.h"
#include "replayPlayer.h"
#include "replaySession.h"
#include "replayViewer.h"
#include "spectatorGuard.h"

#include <windows.h>

// World (RVA 0xd781d0) fields read by the main-loop body FUN_00421e00:
// +0xce30c is this frame's update count, re-read by the update loop on every
// iteration (0x421ec8), so writing 0 from inside an update ends the frame's
// loop after that update. +0xce34c is the pacer's time debt, which a seek has
// run far ahead of: zeroed at the end so the next frames do not burst.
static const uintptr_t UPDATE_COUNT_RVA = 0xe464dc;
static const uintptr_t PACER_DEBT_RVA = 0xe4651c;

// SoundUtil::setMuteSound(bool), cdecl, through the exe's import (IAT VA
// 0x900618). It mutes stream types 1 and 2 (dragonfly 0x1001ace0), which the
// stock options toggle uses for sound effects and speech; music, type 0, plays
// on. The options object (VA 0x1177d28, getter 0x415460) keeps the player's own
// setting at +0x48 ("sound on"). Restoring from that byte rather than from a
// remembered state puts back whatever the player has chosen, even if they
// changed it in the options during the seek.
static const uintptr_t IAT_SET_MUTE_SOUND = 0x500618;
static const uintptr_t SOUND_ON_RVA = 0xd77d70;

// The particle system's gate (dword VA 0x10800b0), read in three places only:
// the emitter manager's per-frame pass (0x75d36f, the sole caller of the spawn
// 0x7bc330, which also tests it at 0x7bc340) and the particle update in the
// simulation tick after SIM_END (0x76acff). Emitters are simulation objects
// created whatever the gate says; the gate only stops them emitting, so they
// emit again when it is restored and nothing stays missing. A late-game skip
// spent up to 30 ms a frame drawing particles: with the gate off the same 50
// minutes skipped in 236 s instead of 265 s, and stayed exact against the
// recording (docs/features/replay-seeking.md).
static const uintptr_t PARTICLE_GATE_RVA = 0xc800b0;

// Frames are aimed at this length, so the window keeps pumping messages, the
// camera and the panel stay usable, and the progress shows.
// Frame length. Every frame draws the world once, and what that costs depends
// on the view: in a late-game skip of a busy town it took 25-55 ms of each
// 100 ms frame, while the simulation per job was the same as with an empty view.
// So the frame is lengthened until drawing is about an eighth of it, between
// 100 ms (the view stays smooth when drawing is cheap) and 400 ms (it still
// refreshes a few times a second, and P and the panel still answer).
static const int64_t FRAME_MIN_US = 100000;
static const int64_t FRAME_MAX_US = 400000;
static const int64_t OVERHEAD_SHARE = 8;
static const uint32_t UPDATES_START = 20;
static const uint32_t UPDATES_MAX = 5000;

typedef bool(__cdecl *SetMuteSoundFn)(bool muted);

static bool s_installed = false;
static int64_t s_qpcFreq = 0;
static SetMuteSoundFn s_setMuteSound = nullptr;
static volatile uint32_t *s_updateCount = nullptr; // World+0xce30c, for the pump

static volatile LONG s_request = -1; // target job, or -1
static volatile LONG s_cancel = 0;
static bool s_active = false;
static uint32_t s_target = 0;
static volatile uint32_t s_reached = 0; // set by the pump at the target
static uint32_t s_updates = 0;
static int64_t s_lastQpc = 0;
static int64_t s_frameTargetUs = FRAME_MIN_US;
static int64_t s_overheadUs = 0; // smoothed time per frame outside the simulation
// Jobs the pump saw this frame, with the clock at the first and the last.
static uint32_t s_frameJobs = 0;
static int64_t s_firstJobQpc = 0, s_lastJobQpc = 0;
static bool s_muted = false;
static bool s_particlesOff = false;
static uint32_t s_particleGate = 0;

static int64_t qpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static bool soundOn() { return *(const volatile uint8_t *)(exeBase() + SOUND_ON_RVA) != 0; }

// Dragonfly code may use the FPU, and the frame tick is a mid-function site.
static void setSoundMuted(bool muted) {
    if (!s_setMuteSound) {
        return;
    }

    ObserverFpScope fp;
    s_setMuteSound(muted);
}

static void finish() {
    const uintptr_t base = exeBase();
    replayViewerSetUpdateOverride(0);
    *(volatile int32_t *)(base + PACER_DEBT_RVA) = 0;

    if (s_muted) {
        setSoundMuted(!soundOn());
        s_muted = false;
    }

    volatile uint32_t *gate = (volatile uint32_t *)(base + PARTICLE_GATE_RVA);

    // Put back only what the skip took away.
    if (s_particlesOff && *gate == 0) {
        *gate = s_particleGate;
    }

    s_particlesOff = false;
    s_active = false;
    s_reached = 0;
}

static void start(uint32_t target) {
    uint32_t now = 0, end = 0, msPerJob = 0;

    if (!replayPlayerPlaybackJob(&now) || !replaySessionTimeline(&end, &msPerJob)) {
        return;
    }

    if (target > end) {
        target = end;
    }

    if (target <= now) {
        return;
    }

    s_target = target;

    if (s_active) {
        return; // a new target for the running seek
    }

    s_reached = 0;
    s_updates = UPDATES_START;
    s_lastQpc = qpcNow();
    s_frameTargetUs = FRAME_MIN_US;
    s_overheadUs = 0;
    s_frameJobs = 0;

    if (soundOn()) {
        setSoundMuted(true);
        s_muted = true;
    }

    volatile uint32_t *gate = (volatile uint32_t *)(exeBase() + PARTICLE_GATE_RVA);
    s_particleGate = *gate;
    s_particlesOff = s_particleGate != 0;
    *gate = 0;
    s_active = true;
    replayViewerSetUpdateOverride(s_updates);
}

// The frame's time outside the simulation: the frame minus the span from its
// first job to its last, scaled to all of its jobs. Smoothed, and the frame
// target follows it.
static void measureOverhead(int64_t frameUs) {
    if (s_frameJobs < 3) {
        return; // too few jobs to tell the simulation's share
    }

    const int64_t spanUs = (s_lastJobQpc - s_firstJobQpc) * 1000000 / s_qpcFreq;
    const int64_t simUs = spanUs * s_frameJobs / (s_frameJobs - 1);
    int64_t overheadUs = frameUs - simUs;

    if (overheadUs < 0) {
        overheadUs = 0;
    }

    s_overheadUs = (3 * s_overheadUs + overheadUs) / 4;
    int64_t target = s_overheadUs * OVERHEAD_SHARE;

    if (target < FRAME_MIN_US) {
        target = FRAME_MIN_US;
    }

    if (target > FRAME_MAX_US) {
        target = FRAME_MAX_US;
    }

    s_frameTargetUs = target;
}

// Next frame's update count: scaled so the frame lasts about the target, at
// most doubling or halving per frame.
static void adapt(int64_t frameUs) {
    if (frameUs <= 0) {
        return;
    }

    uint64_t next = (uint64_t)s_updates * (uint64_t)s_frameTargetUs / (uint64_t)frameUs;

    if (next > (uint64_t)s_updates * 2) {
        next = (uint64_t)s_updates * 2;
    }

    if (next < s_updates / 2) {
        next = s_updates / 2;
    }

    if (next < 1) {
        next = 1;
    }

    if (next > UPDATES_MAX) {
        next = UPDATES_MAX;
    }

    s_updates = (uint32_t)next;
}

static void seekerTick() {
    if (InterlockedExchange(&s_cancel, 0) && s_active) {
        finish();
    }

    const LONG request = InterlockedExchange(&s_request, -1);

    if (request >= 0 && spectatorGuardActive() && replayInGame()) {
        start((uint32_t)request);
    }

    if (!s_active) {
        return;
    }

    if (s_reached || !spectatorGuardActive() || !replayInGame() || replayViewerEnded()) {
        finish();
        return;
    }

    const int64_t now = qpcNow();
    const int64_t frameUs = (now - s_lastQpc) * 1000000 / s_qpcFreq;
    s_lastQpc = now;

    // The in-game menu holds the seek (the viewer's pause wins over the
    // override); a frame spent there says nothing about the simulation's speed.
    if (!replayViewerMenuOpen()) {
        measureOverhead(frameUs);
        adapt(frameUs);
    }

    s_frameJobs = 0;
    replayViewerSetUpdateOverride(s_updates);
}

void replaySeekerOnJob(uint32_t relativeJob) {
    if (!s_active || s_reached) {
        return;
    }

    // QueryPerformanceCounter reads the clock without touching the FPU.
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);

    if (!s_frameJobs) {
        s_firstJobQpc = t.QuadPart;
    }

    s_lastJobQpc = t.QuadPart;
    ++s_frameJobs;

    if (relativeJob < s_target) {
        return;
    }

    s_reached = 1;
    *s_updateCount = 0;
}

void replaySeekerRequest(uint32_t targetJob) {
    if (targetJob > 0x7fffffffu) {
        targetJob = 0x7fffffffu;
    }

    InterlockedExchange(&s_request, (LONG)targetJob);
}

void replaySeekerCancel() { InterlockedExchange(&s_cancel, 1); }

bool replaySeekerActive(uint32_t *targetJob) {
    if (!s_active) {
        return false;
    }

    if (targetJob) {
        *targetJob = s_target;
    }

    return true;
}

bool replaySeekerInstalled() { return s_installed; }

void installReplaySeeker() {
    LARGE_INTEGER f;

    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0 || !replayPlayerInstalled()) {
        return;
    }

    s_qpcFreq = f.QuadPart;
    s_updateCount = (volatile uint32_t *)(exeBase() + UPDATE_COUNT_RVA);
    const uintptr_t fn = *(const uintptr_t *)(exeBase() + IAT_SET_MUTE_SOUND);
    MEMORY_BASIC_INFORMATION mbi;

    // Without the import a seek still runs, only with its sound.
    if (fn && VirtualQuery((const void *)fn, &mbi, sizeof(mbi)) == sizeof(mbi) &&
        mbi.State == MEM_COMMIT &&
        (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
        s_setMuteSound = (SetMuteSoundFn)fn;
    }

    s_installed = registerFrameTick(seekerTick);
}
