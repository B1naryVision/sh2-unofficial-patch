#include "replaySeeker.h"

#include "../core/exeBase.h"
#include "../core/frameTick.h"
#include "../core/observerContext.h"
#include "prngGuard.h"
#include "replayCommon.h"
#include "replayKeyframes.h"
#include "replayPlayer.h"
#include "replaySession.h"
#include "replayViewer.h"
#include "spectatorGuard.h"

#include <cstring>
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

// Loading. A rewind, and a forward seek with a keyframe well ahead, reload the
// world: from the newest keyframe at or before the target (replayKeyframes.h),
// otherwise from the replay's own save, then skip forward to the target. Two
// rules from the phase 0 restarts (docs/features/replay-seeking.md), both of
// which a restart broke when ignored:
//   - Load only at a job with no command queued and no placement deferred:
//     LoadGame keeps the lockstep queue, and the new world would execute the
//     old one's commands. Until such a job comes the simulation runs on, a few
//     updates a frame, so the wait is a frame or two even with the replay
//     paused (logged waits: 0-8 frames).
//   - Sample the playback epoch straight after the load. The load runs here, in
//     the frame tick after the player's own; left to the next frame, this
//     frame's update loop would run the new world ahead of its schedule.
static const uint32_t LOAD_WAIT_UPDATES = 10;
static const uint32_t LOAD_WAIT_FRAMES_MAX = 600;
// A forward seek loads a keyframe only when it saves at least this much
// skipping: below it the load (about 400 ms with its wait) gains nothing.
static const uint32_t KEYFRAME_JUMP_MIN_JOBS = 600;

enum SeekPhase : uint32_t {
    SEEK_IDLE = 0,
    SEEK_LOAD_WAIT = 1, // running to a quiet job, then loading
    SEEK_FORWARD = 2, // running fast to the target
};

typedef bool(__cdecl *SetMuteSoundFn)(bool muted);

static bool s_installed = false;
static int64_t s_qpcFreq = 0;
static SetMuteSoundFn s_setMuteSound = nullptr;
static volatile uint32_t *s_updateCount = nullptr; // World+0xce30c, for the pump

static volatile LONG s_request = -1; // target job, or -1
static volatile LONG s_cancel = 0;
static bool s_active = false;
static uint32_t s_phase = SEEK_IDLE;
static uint32_t s_waitFrames = 0;
static uint32_t s_target = 0;
static volatile uint32_t s_reached = 0; // set by the pump at the target
static uint32_t s_updates = 0;
static int64_t s_lastQpc = 0;
static int64_t s_frameTargetUs = FRAME_MIN_US;
static int64_t s_overheadUs = 0; // smoothed time per frame outside the simulation
// Jobs the pump saw this frame, with the clock at the first and the last.
static uint32_t s_frameJobs = 0;
static int64_t s_firstJobQpc = 0, s_lastJobQpc = 0;
// The run a keyframe started, until it is shown to have left the recording:
// prngGuard checks every job against it, and on the first job that differs the
// keyframe is dropped and the seek is redone from an older one or the replay's
// own save, which is always exact. A rewind that lands on a keyframe the load
// cannot reproduce therefore costs time, never correctness.
static bool s_fromKeyframe = false;
static uint32_t s_sourceKeyframe = 0;
static bool s_forceReload = false;
// Every divergence seen so far showed within 40 jobs of the load. A keyframe
// at least this far before the target is checked during the skip, before the
// viewer sees anything; one that has played this far in step is trusted after.
static const uint32_t VERIFY_JOBS = 60;
static uint32_t s_repairs = 0;
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
    s_phase = SEEK_IDLE;
    s_reached = 0;
}

// What every seek, either way, turns off while it runs.
static void enter() {
    if (soundOn()) {
        setSoundMuted(true);
        s_muted = true;
    }

    volatile uint32_t *gate = (volatile uint32_t *)(exeBase() + PARTICLE_GATE_RVA);
    s_particleGate = *gate;
    s_particlesOff = s_particleGate != 0;
    *gate = 0;
    s_active = true;
}

static void beginForward() {
    s_reached = 0;
    s_updates = UPDATES_START;
    s_lastQpc = qpcNow();
    s_frameTargetUs = FRAME_MIN_US;
    s_overheadUs = 0;
    s_frameJobs = 0;
    s_phase = SEEK_FORWARD;
    replayViewerSetUpdateOverride(s_updates);
}

static void beginLoadWait() {
    s_waitFrames = 0;
    s_phase = SEEK_LOAD_WAIT;
    replayViewerSetUpdateOverride(LOAD_WAIT_UPDATES);
}

// A forward target with a keyframe far enough past the current job to be worth
// loading.
static bool keyframeJumpPays(uint32_t now, uint32_t target) {
    uint32_t keyframe = 0;
    return replayKeyframeAtOrBefore(target, VERIFY_JOBS, &keyframe) &&
           keyframe > now + KEYFRAME_JUMP_MIN_JOBS;
}

// A target ahead of the current job runs forward (through a keyframe when one
// is well ahead); one behind it loads; one on it ends a seek. A new request
// replaces the running seek's target, so further presses keep adding to it,
// either way.
static void start(uint32_t target) {
    uint32_t now = 0, end = 0, msPerJob = 0;

    if (!replayPlayerPlaybackJob(&now) || !replaySessionTimeline(&end, &msPerJob)) {
        return;
    }

    if (target > end) {
        target = end;
    }

    if (target == now) {
        if (s_active) {
            finish();
        }

        return;
    }

    s_target = target;

    if (!s_active) {
        enter();
    }

    if (target > now && !keyframeJumpPays(now, target)) {
        if (s_phase != SEEK_FORWARD) {
            beginForward();
        }

        return;
    }

    if (s_phase != SEEK_LOAD_WAIT) {
        beginLoadWait();
    }
}

// After a load: on the target, or a forward seek from where the load left off.
static void afterLoad() {
    uint32_t now = 0;

    if (!replayPlayerPlaybackJob(&now) || s_target <= now) {
        finish();
        return;
    }

    beginForward();
}

// The replay from its own save: always possible, and the fallback when a
// keyframe fails.
static void restart() {
    const wchar_t *playing = replaySessionPlaybackPath();

    if (!playing) {
        finish();
        return;
    }

    // The session rewrites its own copy inside the load.
    wchar_t path[MAX_PATH];
    memcpy(path, playing, sizeof(path));
    bool loaded = false;

    {
        // LoadGame computes floats, and the frame tick is a mid-function site.
        ObserverFpScope fp;
        loaded = replayLoadSave(path);
    }

    if (!loaded || !replayPlayerStartNow()) {
        finish();
        return;
    }

    replayKeyframesBeginRun();
    prngGuardVerifyReset();
    s_fromKeyframe = false;
    afterLoad();
}

// The load, once the job is quiet. The target may have moved during the wait,
// so the choice is made here: the newest keyframe at or before it, when that is
// behind the current job (a rewind) or well ahead of it (a forward jump);
// otherwise a restart for a rewind and a plain forward seek for the rest.
static void load() {
    uint32_t now = 0, keyframe = 0;
    replayPlayerPlaybackJob(&now);
    // A repair reloads even for a target on the current job.
    const bool back = s_target < now || s_forceReload;
    s_forceReload = false;
    const bool haveKeyframe = replayKeyframeAtOrBefore(s_target, VERIFY_JOBS, &keyframe);

    if (!back && !(haveKeyframe && keyframe > now)) {
        beginForward();
        return;
    }

    if (haveKeyframe) {
        bool loaded = false;

        if (replayKeyframeRestore(keyframe, &loaded)) {
            replayKeyframesBeginRun();
            s_fromKeyframe = true;
            s_sourceKeyframe = keyframe;
            afterLoad();
            return;
        }

        // Nothing replaced: a forward seek just carries on from here.
        if (!loaded && !back) {
            beginForward();
            return;
        }
    }

    restart();
}

static void loadWaitFrame() {
    // The in-game menu holds a load as it holds any seek.
    if (replayScreenPopupOn()) {
        return;
    }

    if (replayNetQueuedCommands() == 0 && replayPendingPlacements() == 0) {
        load();
        return;
    }

    if (++s_waitFrames >= LOAD_WAIT_FRAMES_MAX) {
        finish(); // never quiet: give up rather than load into a broken run
        return;
    }

    replayViewerSetUpdateOverride(LOAD_WAIT_UPDATES);
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

// A run from a keyframe that has left the recording: drop that keyframe and
// what this run took since, then seek again to where the viewer was going.
static void verifyFrame() {
    uint32_t job = 0, checked = 0, now = 0;

    if (!s_fromKeyframe || s_phase == SEEK_LOAD_WAIT) {
        return;
    }

    if (!prngGuardVerifyMismatch(&job, &checked)) {
        // Still checked after this, in case a later job leaves the recording.
        if (checked >= VERIFY_JOBS) {
            replayKeyframesMarkVerified(s_sourceKeyframe);
        }

        return;
    }

    if (!replayPlayerPlaybackJob(&now)) {
        return;
    }

    // The world was still on the recording at the start of `job`.
    s_fromKeyframe = false;
    replayKeyframesDiscard(job, true);
    replayKeyframesDiscard(s_sourceKeyframe, false);

    const uint32_t target = s_active ? s_target : now;
    ++s_repairs;
    s_target = target;
    s_forceReload = true;

    if (!s_active) {
        enter();
    }

    beginLoadWait();
}

static void seekerTick() {
    if (InterlockedExchange(&s_cancel, 0) && s_active) {
        finish();
    }

    const LONG request = InterlockedExchange(&s_request, -1);

    if (!replayInGame()) {
        s_fromKeyframe = false;
    }

    if (request >= 0 && spectatorGuardActive() && replayInGame()) {
        start((uint32_t)request);
    }

    if (spectatorGuardActive() && replayInGame()) {
        verifyFrame();
    }

    // Keyframes are taken while watching and while skipping, never while a
    // load is pending: that world is about to be replaced.
    if (s_phase != SEEK_LOAD_WAIT && spectatorGuardActive() && replayInGame() &&
        replayKeyframesFrame()) {
        s_lastQpc = qpcNow(); // a frame that held a save says nothing about the simulation
    }

    if (!s_active) {
        return;
    }

    if (!spectatorGuardActive() || !replayInGame()) {
        finish();
        return;
    }

    // A rewind may start from the end of the recording, which is what it is
    // for; a forward seek stops there.
    if (s_phase == SEEK_LOAD_WAIT) {
        loadWaitFrame();
        return;
    }

    if (s_reached || replayViewerEnded()) {
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
    if (s_phase != SEEK_FORWARD || s_reached) {
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

uint32_t replaySeekerRepairs() { return s_repairs; }

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
