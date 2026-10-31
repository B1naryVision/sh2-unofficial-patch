#include "replayRecorder.h"

#include "../core/exeBase.h"
#include "../core/observerContext.h"
#include "choreCodec.h"
#include "prngGuard.h"
#include "replayCommon.h"
#include "replayTrace.h"

#include <cstring>
#include <cwchar>
#include <windows.h>

static const uint32_t ARENA_CAPACITY = REPLAY_LOG_CAPACITY;

static uint8_t *s_arena = nullptr;
static uint32_t s_used = 0;
static uint32_t s_events = 0;
static uint32_t s_dropped = 0;
static uint32_t s_firstDroppedTick = 0;
static bool s_recording = false;
static uint32_t s_epochTick = 0;
static uint32_t s_epochNetCounter = 0;
static uint32_t s_epochNetMode = 0;

// A command already in the lockstep queue when the checkpoint is taken runs
// after it, yet its admission precedes the log's restart: it is in neither the
// save nor the log. The same holds for a placement deferred in the
// BuildingPlacer retry queue: at match start the estates' stockpiles wait there
// about two updates, and a save taken inside that window loses one stockpile
// per estate. So the save waits for a frame with neither pending, at most
// ARM_QUEUE_WAIT_MAX frames, and a queue that never drains abandons the
// recording rather than take a save that would lose commands.
static const uint32_t ARM_QUEUE_WAIT_MAX = 900;

static ReplayArmState s_armState = REPLAY_ARM_NONE;
static uint32_t s_armDelayFrames = 0;
static uint32_t s_armFrames = 0;
static uint32_t s_armQueueWait = 0;
static wchar_t s_armPath[MAX_PATH] = L"";
// RandomIter #0's cursor (Game at RVA 0x6e8bcc, +0x40078) either side of the
// checkpoint save, written to the sidecar. A save that draws from it would
// desync a networked match; these two tell (docs, Known Issues).
static uint32_t s_iter0BeforeSave = 0xffffffffu;
static uint32_t s_iter0AfterSave = 0xffffffffu;

static void captureEpochTiming() {
    const uintptr_t base = exeBase();
    s_epochNetCounter = *(const uint8_t *)(base + 0x17e6a34);
    s_epochNetMode = *(const uint32_t *)(base + 0x26cd7f0);
}

static uint32_t readIter0Cursor() {
    const uintptr_t game = *(const volatile uintptr_t *)(exeBase() + 0x6e8bcc);
    return game ? *(const volatile uint16_t *)(game + 0x40078) : 0xffffffffu;
}

static bool busy(uint32_t count) { return count != 0 && count != 0xffffffffu; }

// Empties the log and every per-recording witness that is keyed to its epoch,
// so a process that records several matches never mixes two.
static void resetLog() {
    s_recording = false;
    __atomic_store_n(&s_used, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_events, 0u, __ATOMIC_RELEASE);
    s_dropped = 0;
    s_firstDroppedTick = 0;
    replayTraceRecordingReset();
    prngGuardResetRecord();
}

bool replayRecorderAcquire() {
    if (!s_arena) {
        s_arena =
            (uint8_t *)VirtualAlloc(NULL, ARENA_CAPACITY, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    return s_arena != nullptr;
}

void replayRecorderRelease() {
    s_armState = REPLAY_ARM_NONE;
    resetLog();

    if (s_arena) {
        VirtualFree(s_arena, 0, MEM_RELEASE);
        s_arena = nullptr;
    }
}

void replayRecorderFrame() {
    if (s_armState != REPLAY_ARM_PENDING) {
        return;
    }

    // Let the world settle before freezing it. Whatever the match does in
    // these frames is captured by the save, because the save and the arming
    // are the same moment.
    if (s_armFrames < s_armDelayFrames) {
        ++s_armFrames;
        return;
    }

    if (busy(replayNetQueuedCommands()) || busy(replayPendingPlacements())) {
        if (s_armQueueWait >= ARM_QUEUE_WAIT_MAX) {
            s_armState = REPLAY_ARM_FAILED;
            return;
        }

        ++s_armQueueWait;
        return;
    }

    // A stock SaveGame ends a live networked session: 158 bytes in, it
    // announces state 6 to the session state machine and never restores it.
    // Suppress mode 2 removes that announcement for the duration of the call.
    // Single player keeps the stock path.
    const int suppress = replayTraceSessionIsNetworked() ? 2 : 0;
    bool saved = false;

    {
        // SaveGame computes floats, and the frame tick is a mid-function site.
        ObserverFpScope fp;
        s_iter0BeforeSave = readIter0Cursor();
        saved = replayCheckpointSave(s_armPath, suppress);
        s_iter0AfterSave = readIter0Cursor();
    }

    if (!saved) {
        s_armState = REPLAY_ARM_FAILED;
        return;
    }

    // Arm against the tick the checkpoint was taken on: on playback the loaded
    // save comes up here, so this is what relative scheduling is measured from.
    resetLog();
    s_epochTick = replayTraceCurrentSimulationTick();
    captureEpochTiming();
    s_recording = true;
    s_armState = REPLAY_ARM_ARMED;
    // Same frame as the save: the live occupancy-list order and the damage of
    // every missile in flight, which the checkpoint cannot carry.
    replayCaptureTileOrder();
    replayCaptureFlyState();
}

bool replayRecorderAutoArm(const wchar_t *savePath, uint32_t delayFrames) {
    if (!s_arena || !savePath || !*savePath || wcslen(savePath) >= MAX_PATH) {
        return false;
    }

    wcscpy(s_armPath, savePath);
    s_armDelayFrames = delayFrames;
    s_armFrames = 0;
    s_armQueueWait = 0;
    s_iter0BeforeSave = s_iter0AfterSave = 0xffffffffu;
    s_armState = REPLAY_ARM_PENDING;
    return true;
}

void replayRecorderCancelAutoArm() {
    if (s_armState == REPLAY_ARM_PENDING) {
        s_armState = REPLAY_ARM_NONE;
    }
}

ReplayArmState replayRecorderArmState() { return s_armState; }

bool replayRecorderIsRecording() { return s_recording; }

void replayRecorderStop() { s_recording = false; }

uint32_t replayRecorderEpochTick() { return s_epochTick; }

void replayRecorderEpochTiming(uint32_t *netCounter, uint32_t *netMode) {
    *netCounter = s_epochNetCounter;
    *netMode = s_epochNetMode;
}

void replayRecorderIter0AtSave(uint32_t *before, uint32_t *after) {
    *before = s_iter0BeforeSave;
    *after = s_iter0AfterSave;
}

void replayRecorderLogView(ReplayRecorderLogView *out) {
    out->bytes = s_arena;
    out->used = __atomic_load_n(&s_used, __ATOMIC_ACQUIRE);
    out->events = __atomic_load_n(&s_events, __ATOMIC_ACQUIRE);
    out->dropped = s_dropped;
    out->epochTick = s_epochTick;
    out->classCount = choreCodecClassCount();
    out->firstDroppedTick = s_dropped ? s_firstDroppedTick : 0;
}

void replayRecorderNote(const ReplayEventRecord &record, const void *payload) {
    if (!s_recording) {
        return;
    }

    ReplayEventRecord stored = record;
    const uint32_t payloadLength = (payload && record.payloadLength) ? record.payloadLength : 0;
    const uint32_t need = sizeof(ReplayEventRecord) + payloadLength;

    if (s_used + need > ARENA_CAPACITY) {
        // Never overwrite: a log that silently lost its oldest commands would
        // look complete and replay wrongly. The session cuts its sidecar before
        // the first loss instead.
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
    }

    stored.serial = s_events + 1;
    uint32_t used = s_used;
    memcpy(s_arena + used, &stored, sizeof(stored));
    used += sizeof(stored);

    if (payloadLength) {
        memcpy(s_arena + used, payload, payloadLength);
        used += payloadLength;
    }

    // Published after the bytes, for the crash filter's reader.
    __atomic_store_n(&s_used, used, __ATOMIC_RELEASE);
    __atomic_store_n(&s_events, s_events + 1, __ATOMIC_RELEASE);
}
