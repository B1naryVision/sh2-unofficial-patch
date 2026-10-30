#include "replayRecorder.h"
#include "choreCodec.h"
#include "prngGuard.h"
#include "replayCommon.h"
#include "replayTrace.h"
#include "../core/frameTick.h"

#include <cstring>
#include <windows.h>

// Sized for a full match rather than a demonstration. A ten-minute stretch of
// active play observed roughly ten admissions per second, and the average
// record plus payload is well under fifty bytes, so four megabytes covers hours
// with margin. It lives in our .bss, so it costs nothing on disk and is
// committed for the life of the process.
// Raised 4 -> 8 MB for full human matches: 4 MB covers
// roughly two hours of dense play and an unrepeatable human session should not
// be sitting near that edge. Raised 8 -> 16 MB for 8-player,
// two-hour multiplayer matches (REPLAY_LOG_CAPACITY, shared with the player,
// which refuses a log larger than its own arena).
static const uint32_t ARENA_CAPACITY = REPLAY_LOG_CAPACITY;

static uint8_t s_arena[ARENA_CAPACITY];
static uint32_t s_used = 0;
static uint32_t s_events = 0;
static uint32_t s_dropped = 0;
static uint32_t s_firstDroppedTick = 0;
static uint32_t s_truncated = 0;
static uint32_t s_unregistered = 0;
static uint32_t s_firstTick = 0;
static uint32_t s_lastTick = 0;
static bool s_recording = false;
static uint32_t s_epochTick = 0;
static uint32_t s_preArmAdmissions = 0;
static bool s_epochSeen = false;
static uint32_t s_epochNetCounter = 0;
static uint32_t s_epochNetMode = 0;

static void captureEpochTiming() {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    s_epochNetCounter = *(const uint8_t *)(base + 0x17e6a34);
    s_epochNetMode = *(const uint32_t *)(base + 0x26cd7f0);
}

// Roughly nine seconds apart at the observed tick rate, which is frequent
// enough to localise a divergence to a small window and cheap enough that the
// actor/building/terrain walk does not disturb the frame it runs on.
// 2048 samples at the /64 interval is 131k ticks, only ~62 minutes at the
// stock 35 ticks/s -- shorter than a real match, and the samples that would
// have been dropped are the ones nearest the Win/Lose transition that
// acceptance clause 5 needs. 8192 covers ~4 hours at /64. 352 B each (v14).
static const uint32_t MAX_FINGERPRINT_SAMPLES = 16384; // 8192 = ~15 min of MP jobs at /1; doubled for longer matches
static uint32_t s_fingerprintCount = 0;
static uint32_t s_fingerprintDropped = 0;
static uint32_t s_fingerprintMissed = 0;
static uint32_t s_fingerprintInterval = 0;

// --- auto-arm at match start ---------------------------------------------
//
// The player's workflow is: start a Kingmaker match from the lobby, play,
// and replay it later through the Load menu. A lobby start has no checkpoint,
// so the recorder makes one: at match start it calls the game's own SaveGame,
// then arms itself against that exact tick. Nothing is typed, and the two
// halves happen in the SAME frame callback, so no admission can fall between
// the checkpoint and the arming -- which is the preArm hole that a
// wall-clock-ordered manual protocol keeps re-opening.
static bool s_autoArmPending = false;
static uint32_t s_autoArmDelayFrames = 0;
static uint32_t s_autoArmFrames = 0;
static uint32_t s_autoArmInterval = 64;
static uint32_t s_autoArmDone = 0;
static uint32_t s_autoArmSaved = 0;
// A command already in the lockstep queue when the checkpoint is taken runs
// after it, yet its admission precedes the log's restart: it is in neither the
// save nor the log. The same holds for a placement deferred in the
// BuildingPlacer retry queue (replayPendingPlacements): at match start the
// estates' stockpiles wait there about two updates, and recordings saved
// inside that window lost one stockpile per estate (iterator 0 behind by the estate count from job 1).
// The save waits for a frame with neither pending, at most ARM_QUEUE_WAIT_MAX
// frames, then is taken anyway.
static const uint32_t ARM_QUEUE_WAIT_MAX = 900;
static uint32_t s_autoArmQueueWait = 0;   // frames the save waited on the queue
static uint32_t s_autoArmPlacementWait = 0; // of those, frames with a placement pending
static uint32_t s_autoArmQueuedAtArm = 0; // commands queued when it was taken
// RandomIter #0's cursor (Game at RVA 0x6e8bcc, +0x40078) either side of the
// checkpoint save. One recording played back 5 draws behind the recording
// from job 1, with no command in the file to explain it; these two witnesses
// tell whether the gap opens inside the save or after it. 0xffffffff = unread.
static uint32_t s_iter0BeforeSave = 0xffffffffu;
static uint32_t s_iter0AfterSave = 0xffffffffu;

static uint32_t readIter0Cursor() {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const uintptr_t game = *(const volatile uintptr_t *)(base + 0x6e8bcc);
    return game ? *(const volatile uint16_t *)(game + 0x40078) : 0xffffffffu;
}
// Which mpSaveRecovery state suppression the checkpoint was taken with:
// 0 stock (single player), 2 the multiplayer-safe block. Reported in the
// status so a container says how its checkpoint was produced.
static uint32_t s_autoArmSuppress = 0;
// 0xffffffff = choose by session; otherwise the mode to force.
static uint32_t s_autoArmSuppressForce = 0xffffffffu;
static char s_autoArmPath[512] = {};
static bool s_haveFingerprintTick = false;
static uint32_t s_lastObservationTick = 0;
static bool s_haveObservationTick = false;

// Sampled by the simulation thread on the first in-match frame. The recorded
// clock is absolute and a fresh process does not resume it, so this epoch is
// what makes a recorded tick comparable across processes at all.
static void replayRecorderFrameTick() {
    if (!s_epochSeen) {
        if (!replayInGame()) {
            return;
        }
        s_epochSeen = true;
        s_epochTick = replayTraceCurrentSimulationTick();
        captureEpochTiming();
    }

    if (!s_autoArmPending) {
        return;
    }
    // Let the world settle after the in-match epoch before freezing it. The
    // delay costs nothing: whatever the match does in those frames is captured
    // by the save, because the save and the arming are the same moment.
    if (s_autoArmFrames < s_autoArmDelayFrames) {
        ++s_autoArmFrames;
        return;
    }
    const uint32_t queued = replayNetQueuedCommands();
    const uint32_t placements = replayPendingPlacements();
    if (((queued != 0 && queued != 0xffffffffu) || (placements != 0 && placements != 0xffffffffu)) &&
        s_autoArmQueueWait < ARM_QUEUE_WAIT_MAX) {
        if (placements != 0 && placements != 0xffffffffu) {
            ++s_autoArmPlacementWait;
        }
        ++s_autoArmQueueWait;
        return;
    }
    s_autoArmQueuedAtArm = queued;
    s_autoArmPending = false;

    // A stock SaveGame ENDS a live networked session: 158 bytes
    // in it announces state 6 to the session state machine and never restores
    // it. Suppress mode 2 -- the dispatch AND both
    // field writes -- is the only mode a two-player match survives, and it was
    // verified in a live match with play continuing across the save.
    //
    // Chosen by session rather than always-on so single-player auto-arm keeps
    // the exact stock path every single-player test recording used. Every
    // zero-divergence recording took its checkpoint through suppress 0,
    // and there is no reason to move them onto an untested one.
    // `s_autoArmSuppressForce` exists so the save path can be pinned
    // deliberately rather than only inferred from the session: a test needs a
    // SINGLE-PLAYER recording taken through suppress 2, because that
    // is the one variable separating the two diverging multiplayer containers
    // from every clean one -- and inferring the mode would make
    // that experiment impossible to run.
    const int suppress = (s_autoArmSuppressForce != 0xffffffffu)
                             ? (int)s_autoArmSuppressForce
                             : (replayTraceSessionIsNetworked() ? 2 : 0);
    s_autoArmSuppress = (uint32_t)suppress;

    wchar_t wide[512];
    if (MultiByteToWideChar(CP_UTF8, 0, s_autoArmPath, -1, wide,
                            sizeof(wide) / sizeof(wide[0])) > 0) {
        s_iter0BeforeSave = readIter0Cursor();
        s_autoArmSaved = replayCheckpointSave(wide, suppress) ? 1u : 0u;
        s_iter0AfterSave = readIter0Cursor();
    }

    // Arm against the tick the checkpoint was taken on. The epoch is re-sampled
    // rather than left at the match's first in-match tick because the save IS
    // this tick's world: on playback the loaded save comes up here, so this is
    // what relative scheduling must be measured from.
    replayRecorderControl(2);
    s_epochTick = replayTraceCurrentSimulationTick();
    captureEpochTiming();
    s_epochSeen = true;
    s_fingerprintInterval = s_autoArmInterval;
    s_recording = true;
    s_autoArmDone = 1;
    // Same frame as the save, after the re-arm reset: the live occupancy-list
    // order the checkpoint cannot carry (KB 8.1i resolution).
    replayCaptureTileOrder();
    // And the damage of every missile in flight, which it cannot carry either.
    replayCaptureFlyState();
}

void replayRecorderIter0AtSave(uint32_t *before, uint32_t *after) {
    *before = s_iter0BeforeSave;
    *after = s_iter0AfterSave;
}

uint32_t replayRecorderArmPlacementWait() { return s_autoArmPlacementWait; }

void replayRecorderArmGate(uint32_t *waitedFrames, uint32_t *queuedAtArm) {
    *waitedFrames = s_autoArmQueueWait;
    *queuedAtArm = s_autoArmQueuedAtArm;
}

uint32_t replayRecorderAutoArm(const char *savePath, uint32_t fingerprintInterval,
                               uint32_t delayFrames, uint32_t suppressForce) {
    if (!savePath || !*savePath) {
        return 0;
    }
    size_t i = 0;
    for (; savePath[i] && i + 1 < sizeof(s_autoArmPath); ++i) {
        s_autoArmPath[i] = savePath[i];
    }
    s_autoArmPath[i] = 0;
    s_autoArmInterval = fingerprintInterval ? fingerprintInterval : 64;
    s_autoArmDelayFrames = delayFrames;
    s_autoArmFrames = 0;
    s_autoArmQueueWait = 0;
    s_autoArmPlacementWait = 0;
    s_autoArmQueuedAtArm = 0;
    s_iter0BeforeSave = s_iter0AfterSave = 0xffffffffu;
    s_autoArmDone = 0;
    s_autoArmSaved = 0;
    s_autoArmSuppress = 0;
    s_autoArmSuppressForce = suppressForce;
    // Re-arm for the NEXT match: a session that already saw one match must not
    // fire the moment this is enabled.
    s_epochSeen = false;
    s_epochTick = 0;
    s_autoArmPending = true;
    return 1;
}

// Sampling is keyed to (tick - epoch) so a recording process and a replaying
// process, whose absolute clocks differ, still sample the same simulation
// moments. Sampling only while recording keeps a passive process free of the
// walk entirely.
//
// This is driven by the SIMULATION TICK BODY, not the frame. On the
// frame callback the `% interval == 0` test silently missed every boundary the
// simulation stepped over between frames, which is exactly what happens under
// load, after a window drag, or at a raised tick rate -- when the witness
// matters most. Three playbacks of one container over the same relative span
// took 88, 75 and 59 samples and all reported dropped=0. The tick body runs
// five times per job tick in MP (one in SP). Keep the first update per job,
// including the raw snapshot windows; `s_fingerprintMissed`
// counts any that are anyway, rather than leaving a holed witness to look
// complete.
void replayRecorderSimulationTick() {
    if (!s_recording || !s_fingerprintInterval || !s_epochSeen) {
        return;
    }
    const uint32_t now = replayTraceCurrentSimulationTick();
    if (now < s_epochTick) {
        return;
    }
    if (s_haveObservationTick && now == s_lastObservationTick) {
        return;
    }
    s_lastObservationTick = now;
    s_haveObservationTick = true;
}

uint32_t replayRecorderFingerprintInterval(uint32_t ticks) {
    s_fingerprintInterval = ticks;
    return s_fingerprintInterval;
}

void installReplayRecorder() {
    registerFrameTick(replayRecorderFrameTick);
    // The fingerprint sampler is NOT registered here; it runs on the simulation
    // tick body via prngGuardSimDone. See replayRecorderSimulationTick.
}

uint32_t replayRecorderEpochTick() {
    return s_epochTick;
}

const char *replayRecorderAutoArmPath() {
    return s_autoArmPending ? s_autoArmPath : nullptr;
}

// The epoch the logged data is relative to, kept across a reset so tooling
// that saves after the match ended still writes the recording's own epoch.
static uint32_t s_dataEpoch = 0;

void replayRecorderResetEpoch() {
    if (s_epochTick) {
        s_dataEpoch = s_epochTick;
    }
    s_epochSeen = false;
    s_epochTick = 0;
}

uint32_t replayRecorderDataEpochTick() {
    return s_epochTick ? s_epochTick : s_dataEpoch;
}

void replayRecorderLogView(ReplayRecorderLogView *out) {
    out->bytes = s_arena;
    out->used = s_used;
    out->events = s_events;
    out->dropped = s_dropped;
    out->firstTick = s_firstTick;
    out->lastTick = s_lastTick;
    out->epochTick = replayRecorderDataEpochTick();
    out->classCount = choreCodecClassCount();
    out->firstDroppedTick = s_dropped ? s_firstDroppedTick : 0;
}

bool replayRecorderIsRecording() {
    return s_recording;
}

void replayRecorderNote(const ReplayEventRecord &record, const void *payload) {
    if (!s_recording) {
        // A command admitted after the match started but before the recorder
        // was armed is invisible to the container, and playback then starts
        // from a world the checkpoint alone does not reproduce. This once
        // silently cost a whole human match -- the player placed the
        // forced Kingmaker stockpile and granary in the seconds between the
        // save loading and the recorder being armed, so every replayed
        // placement was refused for want of a stockpile and only the free
        // market appeared. Count it so the container can refuse itself.
        if (s_epochSeen) {
            ++s_preArmAdmissions;
        }
        return;
    }

    ReplayEventRecord stored = record;
    const uint32_t payloadLength =
        (payload && record.payloadLength) ? record.payloadLength : 0;
    const uint32_t need = sizeof(ReplayEventRecord) + payloadLength;

    if (s_used + need > ARENA_CAPACITY) {
        // Never overwrite: a replay log that silently lost its oldest commands
        // would look complete and replay wrongly. Count the loss instead and
        // let the container refuse to be accepted. The replay session cuts its
        // sidecar before the first loss instead.
        if (!s_dropped) {
            s_firstDroppedTick = record.simulationTick;
        }
        ++s_dropped;
        return;
    }

    if (payloadLength) {
        stored.flags |= REPLAY_FLAG_PAYLOAD;
    } else {
        stored.payloadLength = 0;
        stored.flags |= REPLAY_FLAG_TRUNCATED;
        ++s_truncated;
    }
    if (stored.classIndex == 0xffffu) {
        ++s_unregistered;
    }

    stored.serial = s_events + 1;
    memcpy(s_arena + s_used, &stored, sizeof(stored));
    s_used += sizeof(stored);
    if (payloadLength) {
        memcpy(s_arena + s_used, payload, payloadLength);
        s_used += payloadLength;
    }

    if (s_events == 0) {
        s_firstTick = record.simulationTick;
    }
    s_lastTick = record.simulationTick;
    ++s_events;
}

uint32_t replayRecorderControl(uint32_t mode) {
    if (mode == 2) {
        s_recording = false;
        s_used = 0;
        s_events = 0;
        s_dropped = 0;
        s_firstDroppedTick = 0;
        s_truncated = 0;
        s_unregistered = 0;
        s_firstTick = 0;
        s_lastTick = 0;
        s_fingerprintCount = 0;
        s_fingerprintDropped = 0;
        s_fingerprintMissed = 0;
        s_haveFingerprintTick = false;
        s_haveObservationTick = false;
        s_preArmAdmissions = 0;
        // The raw actor window is keyed to the epoch, and a re-arm moves the
        // epoch. Anything captured against the old one would sit in the same
        // file on a different relative clock.
        replayTraceRecordingReset();
        // Also the iterator-1 record (prngGuardControl(0) clears only the
        // record, never a loaded stream). A process that records several
        // matches must not carry one match's cursor stream into the next.
        prngGuardControl(0);
        return 0;
    }
    s_recording = mode != 0;
    return s_recording ? 1u : 0u;
}

size_t replayRecorderStatus(void *out, size_t cap) {
    if (!out || cap < sizeof(ReplayRecorderStatus)) {
        return 0;
    }
    ReplayRecorderStatus status = {};
    status.magic = 0x31535252u; // "RRS1"
    status.version = 4; // +epochNetCounter/epochNetMode
    status.recording = s_recording ? 1 : 0;
    status.eventCount = s_events;
    status.byteCount = s_used;
    status.capacity = ARENA_CAPACITY;
    status.firstTick = s_firstTick;
    status.lastTick = s_lastTick;
    status.droppedEvents = s_dropped;
    status.truncatedEvents = s_truncated;
    status.unregisteredEvents = s_unregistered;
    status.currentTick = replayTraceCurrentSimulationTick();
    status.epochTick = s_epochTick;
    status.fingerprintInterval = s_fingerprintInterval;
    status.fingerprintSamples = s_fingerprintCount;
    status.fingerprintDropped = s_fingerprintDropped;
    status.preArmAdmissions = s_preArmAdmissions;
    status.fingerprintMissed = s_fingerprintMissed;
    status.autoArmPending = s_autoArmPending ? 1u : 0u;
    status.autoArmDone = s_autoArmDone;
    status.autoArmSaved = s_autoArmSaved;
    status.autoArmSuppress = s_autoArmSuppress;
    status.epochNetCounter = s_epochNetCounter;
    status.epochNetMode = s_epochNetMode;
    memcpy(out, &status, sizeof(status));
    return sizeof(status);
}

uint32_t replayRecorderSaveFingerprints(const char *path) {
    if (!path || !*path || s_recording) {
        return 0;
    }
    return 0; // the witnesses are diagnostics-only
}

uint32_t replayRecorderSave(const char *path) {
    if (!path || !*path || s_recording) {
        return 0;
    }

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }

    ReplayLogFileHeader header = {};
    memcpy(header.magic, "SH2RCMD1", 8);
    header.version = 2;
    header.recordSize = sizeof(ReplayEventRecord);
    header.eventCount = s_events;
    header.byteCount = s_used;
    header.firstTick = s_firstTick;
    header.lastTick = s_lastTick;
    header.droppedEvents = s_dropped;
    header.classCount = choreCodecClassCount();
    header.epochTick = replayRecorderDataEpochTick();

    DWORD written = 0;
    uint32_t total = 0;
    if (WriteFile(file, &header, sizeof(header), &written, NULL) &&
        written == sizeof(header)) {
        total += written;
        if (s_used == 0 ||
            (WriteFile(file, s_arena, s_used, &written, NULL) && written == s_used)) {
            total += written;
        } else {
            total = 0;
        }
    }
    CloseHandle(file);
    return total;
}
