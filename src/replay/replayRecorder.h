#pragma once

#include <cstddef>
#include <cstdint>

// The replay command log: an append-only record of every admission of a
// recorded match, carrying the stock codec payload so playback can rebuild each
// command exactly.
//
// Every admission is recorded, autonomous ones included. Playback injects only
// human and remote commands, but which those are is a playback decision, made
// against a log that kept everything.
//
// The recorder does nothing on its own. The replay session acquires its arena,
// arms it at match start, drives the arm from its frame tick while it is
// recording, and releases it when the recording ends.

// Command log arena, recorder and player alike (the player refuses a larger
// log). A 5-player test match averaged 55 bytes per command; 16 MB is about
// 290k commands, above the retimer's REPLAY_MAX_EVENTS.
static const uint32_t REPLAY_LOG_CAPACITY = 16u * 1024u * 1024u;

#pragma pack(push, 1)

struct ReplayEventRecord {
    uint32_t serial;
    uint32_t simulationTick; // wrap-lifted authoritative clock
    uint16_t engineTick; // raw lockstep protocol tick
    uint16_t order; // admission order within the tick
    uint16_t classIndex; // stock wire class index, 0xffff if unregistered
    uint16_t payloadLength; // stock payload bytes following this record
    uint32_t vtableRva;
    uint32_t callerRva; // 0xffffffff when the caller is outside the exe
    uint32_t parentCallerRva;
    uint32_t playerSlot; // 0xffffffff when the schema does not carry one
    // 0 unknown, 1 local UI-stack, 2 AI RTTI, 3 system RTTI,
    // 4 remote peer (arrived through NetGameMgr::onPacket)
    uint8_t originHint;
    uint8_t schema; // hand-recovered schema id, 0 = none
    uint16_t flags; // see REPLAY_FLAG_* below
};

// bit 0: a stock codec payload follows this record.
static const uint16_t REPLAY_FLAG_PAYLOAD = 1 << 0;
// bit 1: the admitting caller is outside the executable, i.e. this command was
// injected by our own DLL rather than originated by the game. Playback uses
// this to tell a replayed command from a re-derived one.
static const uint16_t REPLAY_FLAG_FOREIGN_CALLER = 1 << 1;
// bit 2: the codec produced no payload for this admission. Playback cannot
// rebuild it.
static const uint16_t REPLAY_FLAG_TRUNCATED = 1 << 2;

// Whether playback injects a recorded command rather than letting the
// simulation re-derive it. Local UI (1) and network (4) provenance, plus
// anything our own DLL admitted. Shared by the retimer and the player, which
// must agree on it exactly.
inline bool replayEventRequiresInjection(const ReplayEventRecord &e) {
    if (e.flags & REPLAY_FLAG_FOREIGN_CALLER) {
        return true;
    }

    return e.originHint == 1 || e.originHint == 4;
}

// The CMDS section header (replayFormat.h).
struct ReplayLogFileHeader {
    char magic[8]; // "SH2RCMD1"
    uint32_t version;
    uint32_t recordSize; // sizeof(ReplayEventRecord)
    uint32_t eventCount;
    uint32_t byteCount; // arena bytes following this header
    uint32_t firstTick;
    uint32_t lastTick;
    uint32_t droppedEvents;
    uint32_t classCount; // registered command classes at save time
    // The checkpoint's simulation tick. A fresh process does not resume the
    // recorded absolute clock, so every recorded tick is only meaningful
    // relative to this epoch.
    uint32_t epochTick;
};

#pragma pack(pop)

static_assert(sizeof(ReplayEventRecord) == 36, "replay event wire record changed");
static_assert(sizeof(ReplayLogFileHeader) == 44, "replay log file header changed");

// Allocates the arena (false when the address space has no 16 MB left).
// Release stops recording, cancels a pending arm and frees it.
bool replayRecorderAcquire();
void replayRecorderRelease();

// Called from the World admission hook on the simulation thread. Does nothing
// unless recording is armed.
void replayRecorderNote(const ReplayEventRecord &record, const void *payload);

enum ReplayArmState : uint32_t {
    REPLAY_ARM_NONE = 0,
    REPLAY_ARM_PENDING = 1, // waiting for the world to settle and the queues to drain
    REPLAY_ARM_ARMED = 2, // the checkpoint is written and the log is recording
    REPLAY_ARM_FAILED = 3, // no checkpoint: the save failed or the queue never drained
};

// Arms the recorder for the current match: after `delayFrames` frames, and on
// the first frame with no command in the lockstep queue and no deferred
// building placement, it calls the game's own SaveGame on `savePath` and starts
// the log in the same frame, so no admission can fall between the checkpoint
// and the log. The checkpoint's tick becomes the recording's epoch.
bool replayRecorderAutoArm(const wchar_t *savePath, uint32_t delayFrames);
void replayRecorderCancelAutoArm();

// Advances a pending arm. The replay session calls it from its frame tick while
// it is recording, and only then, so a stale arm can never fire in a later
// match or during a playback.
void replayRecorderFrame();

ReplayArmState replayRecorderArmState();
bool replayRecorderIsRecording();
// Stops appending. The log stays readable until replayRecorderRelease.
void replayRecorderStop();

// The checkpoint tick of the current (or last) recording, 0 before the first.
uint32_t replayRecorderEpochTick();
// NetGameMgr+4 phase byte and the network mode at the checkpoint.
void replayRecorderEpochTiming(uint32_t *netCounter, uint32_t *netMode);
// RandomIter #0's cursor just before / after the checkpoint save
// (0xffffffff = not taken).
void replayRecorderIter0AtSave(uint32_t *before, uint32_t *after);

// Read-only view of the log for the sidecar writer. The arena is append-only
// and the counts are published after the bytes they cover, so a reader on
// another thread that uses only `used` bytes never sees a torn record.
struct ReplayRecorderLogView {
    const uint8_t *bytes;
    uint32_t used;
    uint32_t events;
    uint32_t dropped;
    uint32_t epochTick;
    uint32_t classCount;
    // Admission tick of the first command the full arena dropped (0 = none):
    // the log is complete only for earlier jobs.
    uint32_t firstDroppedTick;
};
void replayRecorderLogView(ReplayRecorderLogView *out);
