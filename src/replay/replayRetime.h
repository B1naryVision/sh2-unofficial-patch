#pragma once

#include <cstdint>

#include "replayTrace.h"

// Command-log retiming.
//
// Offline playback admits an injected command at its scheduled job and the
// stock drain executes it exactly one job later. Live MP does not have a
// constant delay: a remote packet arriving mid-job executes in its arrival job
// (delay 0) and late host bursts two jobs later. The recorder's boundary
// witness pairs every admission with its execution (same object token and
// class), so each injected command moves to its execution job - 1. Drains in
// which the live game executed an injected command BEFORE an autonomous one get
// an intra-drain order plan (execorder.bin format). Hooks missing or an
// inconsistent pairing leave the log on arrival scheduling rather than guessing.
//
// Truncation. A witness that filled up (dropped entries) or a log longer than
// REPLAY_MAX_EVENTS is complete only up to some job. Rather than giving up on
// the whole recording (a 5-player, 49-minute match overflowed the
// witness in its last 130 jobs and lost the timing of all 34702 commands), the
// output is cut at the last job whose executions are all known: events that
// would execute after it are left out and `endTick` says where the replay now
// ends. `limitTick` (0 = none) imposes the same cut from outside, for a
// recorder arena or iterator-1 record that filled first, or a recording ended
// at the capacity limit. Injected commands still in flight at the cut are left
// out too, so everything kept is exact.
//
// Never executed. An injected command admitted but never executed while the
// witness ran is left out (`unretimed`): the live game dropped it on every
// peer. In one recorded match a
// peer's 73 wall/building deletes arrived in a network catch-up, never ran
// (the player deleted the same walls again 1,300 jobs later), and playback
// that ran them at arrival diverged 20 jobs on. Only one admitted within
// IN_FLIGHT_JOBS of a truncated witness's end may have executed unseen; the
// truncation cut comes before it.
//
// Pure computation over caller-supplied buffers (static working storage, one
// caller at a time), so it can be exercised offline against recorded
// sidecars.

// Log events the retimer holds. Two boundary entries per event, so it matches
// REPLAY_BOUNDARY_CAPACITY / 2 (replayTrace.h).
static const uint32_t REPLAY_MAX_EVENTS = 262144;

struct ReplayRetimeRef {
    uint32_t newTick;   // playback schedule tick for this event
    uint32_t execOrder; // sort key within the tick
    uint32_t index;     // position in the recorded log
    uint32_t offset;    // byte offset of the record in the log
};

struct ReplayRetimeResult {
    uint32_t events;     // events kept (all but the never-executed, unless truncated)
    uint32_t injected;   // injected and retimed are over the kept events
    uint32_t retimed;    // injected events moved to their execution job - 1
    uint32_t unretimed;  // injected events never executed, left out (before any cut)
    uint32_t planJobs;
    uint32_t planWords;  // 0 when retiming did not run (no EXOR section)
    uint32_t firstTick;
    uint32_t lastTick;
    // 0 ok. Retiming skipped: 1 boundary hooks not all installed; 3 token
    // reused before execution; 4 too many in flight. Output truncated at
    // endTick: 2 witness dropped entries or too large; 5 too many events.
    uint32_t failReason;
    uint32_t bytes;      // record + payload bytes of the kept events
    // Truncated output: the last job (absolute) the kept events are exact
    // through, else 0.
    uint32_t endTick;
    uint32_t cutEvents;  // events left out by the truncation
};

// `log` is the recorder arena (records + payloads, no file header).
void replayRetime(const uint8_t *log, uint32_t used, uint32_t epochTick,
                  const ReplayBoundaryEntry *boundaries, uint32_t count,
                  uint32_t dropped, uint32_t hooks, uint32_t limitTick,
                  ReplayRetimeResult *out);

// Valid until the next replayRetime call.
const ReplayRetimeRef *replayRetimeRefs();
const uint32_t *replayRetimePlanWords(); // "EXOR", 1, jobs, then per job
