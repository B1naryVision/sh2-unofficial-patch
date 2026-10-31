#pragma once

#include <cstddef>
#include <cstdint>

// The replay command log.
//
// This is not the census. The census is a bounded diagnostic ring that answers
// "what has the game admitted lately"; this is an append-only log of every
// admission for a whole match, carrying the stock codec payload so playback can
// rebuild each command exactly.
//
// Deliberately records EVERY admission, autonomous ones included, rather than
// filtering to human input at record time. Playback has to reproduce autonomous
// outcomes as well as human ones, and a recorded autonomous command is the
// witness that proves the replayed simulation re-derived it. Filtering is a
// playback decision, made against a log that kept everything.
//
// The arena lives in our .bss and is never handed to the game.

// Command log arena, recorder and player alike (the player refuses a larger
// log). A 5-player test match averaged 55 bytes per command; 16 MB is
// ~290k commands, above the retimer's REPLAY_MAX_EVENTS.
static const uint32_t REPLAY_LOG_CAPACITY = 16u * 1024u * 1024u;

#pragma pack(push, 1)

struct ReplayEventRecord {
    uint32_t serial;
    uint32_t simulationTick;  // wrap-lifted authoritative clock
    uint16_t engineTick;      // raw lockstep protocol tick
    uint16_t order;           // admission order within the tick
    uint16_t classIndex;      // stock wire class index, 0xffff if unregistered
    uint16_t payloadLength;   // stock payload bytes following this record
    uint32_t vtableRva;
    uint32_t callerRva;       // 0xffffffff when the caller is outside the exe
    uint32_t parentCallerRva;
    uint32_t playerSlot;      // 0xffffffff when the schema does not carry one
    // 0 unknown, 1 local UI-stack, 2 AI RTTI, 3 system RTTI,
    // 4 remote peer (arrived through NetGameMgr::onPacket)
    uint8_t originHint;
    uint8_t schema;           // hand-recovered schema id, 0 = none
    uint16_t flags;           // see REPLAY_FLAG_* below
};

// bit 0: a stock codec payload follows this record.
static const uint16_t REPLAY_FLAG_PAYLOAD = 1 << 0;
// bit 1: the admitting caller is outside the executable, i.e. this command was
// injected by our own DLL rather than originated by the game. Playback uses
// this to tell a replayed command from a re-derived one.
static const uint16_t REPLAY_FLAG_FOREIGN_CALLER = 1 << 1;
// bit 2: the log was full when this admission arrived, so its payload was
// dropped. A log with any of these is not acceptable replay input.
static const uint16_t REPLAY_FLAG_TRUNCATED = 1 << 2;

struct ReplayLogFileHeader {
    char magic[8];        // "SH2RCMD1"
    uint32_t version;
    uint32_t recordSize;  // sizeof(ReplayEventRecord)
    uint32_t eventCount;
    uint32_t byteCount;   // arena bytes following this header
    uint32_t firstTick;
    uint32_t lastTick;
    uint32_t droppedEvents;
    uint32_t classCount;  // registered command classes at save time
    // The simulation tick of the first in-match frame this process observed.
    // A fresh process does not resume the recorded absolute clock,
    // so every recorded tick is only meaningful relative to this epoch.
    uint32_t epochTick;
};

// Periodic canonical state witnesses, stored beside the command stream.
// Sampling is driven off the simulation clock relative to the in-match epoch,
// never off wall-clock time: both the recording and the replaying process must
// sample at exactly the same relative ticks or the comparison is meaningless.
struct ReplayFingerprintFileHeader {
    char magic[8];        // "SH2RFP1"
    uint32_t version;
    uint32_t sampleSize;  // sizeof(SimulationFingerprint)
    uint32_t sampleCount;
    uint32_t interval;    // ticks between samples
    uint32_t epochTick;
    uint32_t droppedSamples;
};

struct ReplayRecorderStatus {
    uint32_t magic;       // "RRS1"
    uint16_t version;
    uint16_t recording;
    uint32_t eventCount;
    uint32_t byteCount;
    uint32_t capacity;
    uint32_t firstTick;
    uint32_t lastTick;
    uint32_t droppedEvents;
    uint32_t truncatedEvents;
    uint32_t unregisteredEvents; // admissions with no stock class index
    uint32_t currentTick;
    uint32_t epochTick;          // first in-match tick, 0 until observed
    uint32_t fingerprintInterval; // 0 = fingerprint sampling off
    uint32_t fingerprintSamples;
    uint32_t fingerprintDropped;
    // Commands the game admitted after the in-match epoch but before the
    // recorder was armed. Any nonzero value means the container does not
    // describe the whole match and playback will start from a world the
    // pinned checkpoint cannot reproduce. Arm the recorder BEFORE loading.
    uint32_t preArmAdmissions;
    // Interval boundaries that passed without a sample being taken. Before
    // The sampler once ran on the frame callback and tested
    // `(tick - epoch) % interval == 0`, so any boundary the simulation stepped
    // over between frames was never sampled and never counted: three playbacks
    // of one container over the same span took 88, 75 and 59 samples and all
    // three reported `dropped=0`. Sampling now runs on the simulation tick
    // body, which cannot skip a tick, and this counts any boundary missed
    // anyway so a holed witness can be refused rather than believed.
    uint32_t fingerprintMissed;
    // Auto-arm at match start. 1 while waiting for the
    // match, 1 once the checkpoint has been written and the recorder armed,
    // and 1 if the game's own SaveGame reported success. A cold Kingmaker
    // start has no checkpoint the player can pin by hand, so the recorder
    // makes one at t0 and arms itself against it in the same frame.
    uint32_t autoArmPending;
    uint32_t autoArmDone;
    uint32_t autoArmSaved;
    // Which mpSaveRecovery state suppression produced the checkpoint: 0 is the
    // stock SaveGame path every single-player recording uses, 2 is the block
    // removal a live networked session needs. A stock
    // save taken during a multiplayer match ENDS the session, so a value of 0
    // on a two-peer container means the checkpoint killed the match.
    uint32_t autoArmSuppress;
    uint32_t epochNetCounter; // NetGameMgr+4 at checkpoint/epoch, uint8 phase
    uint32_t epochNetMode;    // network mode at checkpoint, not collection time
};

#pragma pack(pop)

static_assert(sizeof(ReplayEventRecord) == 36, "replay event wire record changed");
static_assert(sizeof(ReplayLogFileHeader) == 44, "replay log file header changed");
static_assert(sizeof(ReplayFingerprintFileHeader) == 32,
              "replay fingerprint file header changed");
static_assert(sizeof(ReplayRecorderStatus) == 92, "replay recorder status changed");

// Called from the World admission hook on the simulation thread. Does nothing
// unless recording is armed.
void replayRecorderNote(const ReplayEventRecord &record, const void *payload);

// Registers the per-frame callback that observes the in-match load boundary.
// The epoch has to be sampled by the simulation thread itself: a boundary observed
// from another thread is however many frames late that thread happened to be.
void installReplayRecorder();

// Samples the fingerprint if this tick is an interval boundary. Called from the
// simulation tick body's SIM_DONE hook -- after the six subsystem updates, the
// same point the canonical iterator-1 witness is taken -- so both the recording
// and the replaying process sample the identical simulation moment and no
// boundary can be stepped over by a fast frame.
void replayRecorderSimulationTick();

// Arm the recorder automatically when the next match starts, freezing that
// moment into `savePath` first so the container has a checkpoint to replay
// from. `delayFrames` lets the world settle after the in-match epoch before
// the save is taken; the tick the save lands on becomes the container's epoch,
// because the save IS that tick's world.
//
// This is what makes a lobby-started match replayable: the recording produces
// its own checkpoint, and playback loads it through the ordinary Load menu.
// `suppressForce` is 0xffffffff to choose the save path by session -- stock
// SaveGame offline, mpSaveRecovery suppress 2 in a networked match -- or an
// explicit mode to pin it. Pinning exists so a single-player container can be
// taken through suppress 2 deliberately, which is the control the multiplayer
// divergence needs.
uint32_t replayRecorderAutoArm(const char *savePath, uint32_t fingerprintInterval,
                               uint32_t delayFrames, uint32_t suppressForce);

// The first in-match tick this process observed, or 0 if it has not happened.
uint32_t replayRecorderEpochTick();

// True while the log is armed. The PRNG guard gates its per-tick capture on
// this so a passive process records nothing.
bool replayRecorderIsRecording();

// 0 = stop, 1 = start, 2 = reset and stop. Returns the resulting state.
uint32_t replayRecorderControl(uint32_t mode);

size_t replayRecorderStatus(void *out, size_t cap);

// Writes the whole log to `path` as a ReplayLogFileHeader followed by the raw
// arena. Recording must be stopped first: a moving arena would be torn.
// Returns the byte count written, or 0.
uint32_t replayRecorderSave(const char *path);

// Sets the fingerprint sampling interval in simulation ticks; 0 disables it.
// Returns the interval actually in force.
uint32_t replayRecorderFingerprintInterval(uint32_t ticks);

// Writes the collected fingerprint samples to `path`. Returns bytes written.
uint32_t replayRecorderSaveFingerprints(const char *path);

// Read-only view of the live command log for the replay session's sidecar
// writer (game thread only; the arena is only appended to on that thread).
struct ReplayRecorderLogView {
    const uint8_t *bytes;
    uint32_t used;
    uint32_t events;
    uint32_t dropped;
    uint32_t firstTick;
    uint32_t lastTick;
    uint32_t epochTick;
    uint32_t classCount;
    // Admission tick of the first command the full arena dropped (0 = none):
    // the log is complete only for earlier jobs.
    uint32_t firstDroppedTick;
};
void replayRecorderLogView(ReplayRecorderLogView *out);

// Forget the in-match epoch so the next in-match frame samples a new one.
// Called when a match ends or a save is loaded: a process that plays several
// matches must not measure the second against the first one's epoch.
void replayRecorderResetEpoch();
// The epoch the recorded data belongs to: the live epoch, or the one in force
// before the last reset. Save headers use this.
uint32_t replayRecorderDataEpochTick();

// The checkpoint path of a pending auto-arm, or nullptr when none is pending.
const char *replayRecorderAutoArmPath();
// Frames the last auto-arm checkpoint waited for an empty lockstep queue, and
// the commands still queued when it was taken (0xffffffff = queue unreadable).
void replayRecorderArmGate(uint32_t *waitedFrames, uint32_t *queuedAtArm);
// Frames of that wait during which a building placement was still deferred.
uint32_t replayRecorderArmPlacementWait();
// RandomIter #0's cursor just before / after the auto-arm checkpoint save
// (0xffffffff = not taken). Written to the sidecar header (BUNDLE_ITER0_AT_SAVE).
void replayRecorderIter0AtSave(uint32_t *before, uint32_t *after);
