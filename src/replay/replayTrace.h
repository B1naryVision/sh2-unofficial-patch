#pragma once

#include <cstddef>
#include <cstdint>

// Phase-7 diagnostic trace. This is deliberately a small, in-memory witness,
// not the replay container: it identifies the authoritative lockstep tick and
// the common Chore admission/execution boundaries before the recorder format is
// designed around them.

#pragma pack(push, 1)
struct ReplayTraceRecord {
    uint32_t serial;
    uint32_t simulationTick; // engine uint16 tick lifted across 0xffff -> 0
    uint16_t engineTick;     // raw lockstep protocol tick
    uint16_t order;       // phase-local order within tick
    uint8_t phase;        // 1 = World admission, 2 = Chore execution
    // admission: World mode, or 4 for the multiplayer receive path;
    // execution: 0 queue, 1 direct
    uint8_t route;
    uint16_t reserved;
    uint32_t vtableRva;
    uint32_t methodRva;   // vtable slot 2 (the execute method)
};

struct ReplayTraceSnapshotHeader {
    uint32_t magic;       // "TRC1" as little-endian 0x31435254
    uint16_t version;
    uint16_t currentEngineTick;
    uint32_t totalRecords;
    uint16_t count;
    uint16_t capacity;
    // bit 0 admission, bit 1 queued exec, bit 2 direct exec, bit 3 net enqueue
    uint32_t installStatus;
    uint32_t currentSimulationTick;
};

// Admission census record. This is diagnostic input to the eventual replay
// recorder, not the replay container itself. `raw` is intentionally retained
// only for live schema recovery; it may contain vtable/pointer words and must
// never be treated as portable replay payload. `payloadHash` is nonzero only
// when a pointer-free schema is known for the concrete Chore.
struct ReplayCommandRecord {
    uint32_t serial;
    uint32_t simulationTick;
    uint16_t engineTick;
    uint16_t order;
    uint32_t vtableRva;
    uint32_t methodRva;
    uint32_t callerRva;       // immediate return site at World admission
    uint32_t parentCallerRva; // one frame up, when the frame chain is readable
    uint32_t typeNameHash;    // FNV-1a of the mangled RTTI name
    uint32_t playerSlot;      // 0xffffffff when not recovered for this schema
    uint32_t payloadHash;     // canonical, pointer-free fields; 0 = unknown
    uint32_t rawHash;         // ephemeral diagnostic bytes, never replay input
    uint16_t payloadSize;     // known object size captured into raw, else zero
    // 0 unknown, 1 local UI-stack, 2 AI RTTI, 3 system RTTI,
    // 4 remote peer (arrived through NetGameMgr::onPacket)
    uint8_t originHint;
    uint8_t schema;           // versioned canonical schema id; 0 = unknown
    // The game's own multiplayer encoding of this exact command, produced by
    // the stock Serializable::serial. This is the portable payload; `raw` above
    // stays a diagnostic view of the live object and is never replay input.
    uint16_t stockClassIndex; // NetGameMgr wire class index, 0xffff if absent
    uint16_t stockLength;     // stock payload byte count, 0 when unavailable
    uint32_t stockHash;       // FNV-1a of the stock payload
    uint8_t raw[24];          // first bytes for known small Chores only
};

struct ReplayCommandSnapshotHeader {
    uint32_t magic;       // "CMD1" as little-endian 0x31444d43
    uint16_t version;
    uint16_t currentEngineTick;
    uint32_t totalRecords;
    uint16_t count;
    uint16_t capacity;
    uint32_t installStatus; // bit 0 admission hook
    uint32_t currentSimulationTick;
};

// Multiplayer receive-path witness.
//
// NetGameMgr owns exactly one lockstep job queue and everything that ever
// enters it passes through NetGameMgr::enqueue (RVA 0x366c00). There are two
// routes in:
//
//   local   World::admit (0x37efb0) -> NetGameMgr::post (0x367050) -> enqueue
//   remote  NetGameMgr::onPacket (0x367250) -> createSharedInstance
//                                           -> serial(in) -> enqueue
//
// The remote route does NOT pass the World admission site, so every recorder
// build captured only the local peer's commands and a
// multiplayer container silently described half a match. These counters exist
// so that is measured rather than assumed, and so a live session can be asked
// which mode the session manager thinks it is in.
struct ReplayNetStateRecord {
    uint32_t magic;            // "NET1" as little-endian 0x3154454e
    uint16_t version;
    uint16_t engineTick;       // NetGameMgr+0x06, the raw protocol tick
    uint32_t simulationTick;   // wrap-lifted
    uint32_t installStatus;    // same bits as the trace snapshot, bit 3 = enqueue
    uint32_t netModeGlobal;    // exe global 0x26cd7f0; 2 == networked session
    uint32_t sessionMode;      // NetGameMgr+0x48; measured 2 in single player
    uint32_t syncTick;         // NetGameMgr+0x44, the join-handshake tick
    uint32_t liveJobCount;     // NetGameMgr+0x20
    uint32_t classCount;       // (mgr+0x60 - mgr+0x5c) / 4, the 88-class table
    uint32_t peerCount;        // (mgr+0x70 - mgr+0x6c) / 4
    uint32_t localEnqueues;    // enqueues whose caller was not onPacket
    uint32_t remoteCommands;   // remote commands captured at origin 4
    uint32_t remoteEndJobs;    // remote EndJobChore arrivals, deliberately unrecorded
    uint32_t remoteRejected;   // remote enqueues whose object was unreadable
};
#pragma pack(pop)

static_assert(sizeof(ReplayTraceRecord) == 24, "replay trace wire record changed");
static_assert(sizeof(ReplayTraceSnapshotHeader) == 24, "replay trace wire header changed");
static_assert(sizeof(ReplayCommandRecord) == 80, "replay command wire record changed");
static_assert(sizeof(ReplayCommandSnapshotHeader) == 24,
              "replay command wire header changed");
static_assert(sizeof(ReplayNetStateRecord) == 56,
              "replay net state wire record changed");

void installReplayTrace();

// Diagnostic, for tooling; call on the sim thread. Writes a chronological snapshot
// no larger than cap and returns the byte count, or zero when cap is too small.
size_t replayTraceSnapshot(void *out, size_t cap);

// Returns the latest pointer-free admission census plus bounded raw diagnostic
// bytes. Call it on the simulation thread, like replayTraceSnapshot.
size_t replayCommandSnapshot(void *out, size_t cap);
// Same diagnostic snapshot, restricted to one concrete Chore vtable RVA. The
// filter searches the retained in-process ring, so a slow human response does
// not roll a rare Lord command out of the fixed wire window.
size_t replayCommandSnapshotForVtable(void *out, size_t cap,
                                      uint32_t vtableRva);

// Serves the multiplayer receive-path witness above. Read-only; safe in any
// session, and meaningful in single player too (it should report
// remoteCommands 0 and a nonzero localEnqueues, which is what proves the hook
// is live rather than merely installed).
size_t replayTraceNetState(void *out, size_t cap);

// True when NetGameMgr::post would serialise a command onto the wire, i.e. the
// session is a live networked match. The auto-arm checkpoint asks this because
// a stock SaveGame ends a networked session and has to be taken with the
// mpSaveRecovery state suppression instead.
bool replayTraceSessionIsNetworked();

// Append-only admission/execution witness. Pointer tokens correlate objects
// within ONE process only; the witness itself is never replay input, but the
// recording process pairs admissions with executions from it to retime the
// command log (replaySession.cpp).
//
// One admission and one execution entry per recorded command. 65536 entries
// (the earlier capacity) is 32768 commands, which a 5-player,
// 49-minute match passed. 2^19 covers an 8-player, two-hour match with margin:
// that match's busiest player posted 2.5 commands/s, x 8 players x 7200 s =
// 144k commands. 16 MB of .bss, only touched as it fills.
static const uint32_t REPLAY_BOUNDARY_CAPACITY = 524288;
#pragma pack(push, 1)
struct ReplayBoundaryEntry {
    ReplayTraceRecord trace;
    uint32_t eventToken;  // process-local identity, NOT portable payload
    uint16_t scheduledJob;
    uint8_t netCounter;
    uint8_t jobValid;
};
#pragma pack(pop)
static_assert(sizeof(ReplayBoundaryEntry) == 32, "boundary entry layout");
void replayTraceRecordingReset();
uint32_t replayTraceRecordingSave(const char *path);
// The recorded witness so far (game thread). `hooks` is the install bitmask;
// retiming needs 0xf and dropped == 0.
void replayTraceBoundaryView(const ReplayBoundaryEntry **entries, uint32_t *count,
                             uint32_t *dropped, uint32_t *hooks);

// Shared with the Phase-7 fingerprint so both diagnostics report exactly the
// same wrap-lifted clock rather than maintaining competing epochs.
uint16_t replayTraceCurrentEngineTick();
uint32_t replayTraceCurrentSimulationTick();

// Origin classification by posting routine, exposed so playback can re-apply an
// improved classifier to a container recorded by an older build. Returns the
// same origin hint vocabulary as ReplayCommandRecord::originHint, and 0 when
// neither call site is a known routine. Class-name evidence is not available
// here: the caller already has the recorded hint and only asks about routes.
uint8_t replayTraceClassifyCaller(uint32_t callerRva, uint32_t parentCallerRva);
