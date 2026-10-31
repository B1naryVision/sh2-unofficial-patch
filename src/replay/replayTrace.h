#pragma once

#include <cstddef>
#include <cstdint>

// Command admission and execution hooks: the authoritative lockstep clock, the
// command log feed (replayRecorderNote), the admission/execution witness the
// retimer pairs, and the spectator guard's admission backstop.
//
// Four sites, all or none: World admission (RVA 0x37efb0), the two execution
// paths (0x367c61 queued, 0x37f018 direct) and NetGameMgr::enqueue (0x366c00)
// for commands that arrive from peers. In a match that is neither recorded nor
// replayed, each hook keeps the clock and the within-tick order and returns.

#pragma pack(push, 1)
struct ReplayTraceRecord {
    uint32_t serial;
    uint32_t simulationTick; // engine uint16 tick lifted across 0xffff -> 0
    uint16_t engineTick; // raw lockstep protocol tick
    uint16_t order; // phase-local order within tick
    uint8_t phase; // 1 = World admission, 2 = Chore execution
    // admission: World mode, or 4 for the multiplayer receive path;
    // execution: 0 queue, 1 direct
    uint8_t route;
    uint16_t reserved;
    uint32_t vtableRva;
    uint32_t methodRva; // vtable slot 2 (the execute method)
};
#pragma pack(pop)

static_assert(sizeof(ReplayTraceRecord) == 24, "replay trace wire record changed");

void installReplayTrace();
bool replayTraceInstalled();

// True when NetGameMgr::post would serialise a command onto the wire, i.e. the
// session is a live networked match. The checkpoint asks this because a stock
// SaveGame ends a networked session and has to be taken with the state
// announcement suppressed instead.
bool replayTraceSessionIsNetworked();

// Append-only admission/execution witness, kept while recording. Pointer tokens
// correlate objects within ONE process only; the witness itself is never
// replay input, but the recording process pairs admissions with executions from
// it to retime the command log (replayRetime.h).
//
// One admission and one execution entry per recorded command. 2^19 covers an
// 8-player, two-hour match with margin: that match's busiest player posted 2.5
// commands/s, x 8 players x 7200 s = 144k commands. 16 MB, allocated by the
// replay session for a recording (replayTraceAcquire) and freed after it.
static const uint32_t REPLAY_BOUNDARY_CAPACITY = 524288;
#pragma pack(push, 1)
struct ReplayBoundaryEntry {
    ReplayTraceRecord trace;
    uint32_t eventToken; // process-local identity, NOT portable payload
    uint16_t scheduledJob;
    uint8_t netCounter;
    uint8_t jobValid;
};
#pragma pack(pop)
static_assert(sizeof(ReplayBoundaryEntry) == 32, "boundary entry layout");

bool replayTraceAcquire();
void replayTraceRelease();
void replayTraceRecordingReset();
// The recorded witness so far. `hooks` is the install bitmask; retiming needs
// 0xf and dropped == 0. The count is published after the entries it covers.
void replayTraceBoundaryView(
    const ReplayBoundaryEntry **entries, uint32_t *count, uint32_t *dropped, uint32_t *hooks
);

uint32_t replayTraceCurrentSimulationTick();

// Origin classification by posting routine, exposed so playback can re-apply
// the classifier to an event the recorder left at origin 0. Returns the origin
// hint vocabulary of ReplayEventRecord, and 0 when neither call site is a known
// routine.
uint8_t replayTraceClassifyCaller(uint32_t callerRva, uint32_t parentCallerRva);
