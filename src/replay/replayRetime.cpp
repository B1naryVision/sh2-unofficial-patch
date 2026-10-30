#include "replayRetime.h"

#include "replayRecorder.h"

#include <cstdlib>
#include <cstring>

static const uint32_t MAX_EVENTS = REPLAY_MAX_EVENTS;
static const uint32_t MAX_BOUNDARIES = REPLAY_BOUNDARY_CAPACITY;
static const uint32_t MAX_PENDING = 4096;
static const uint32_t MAX_PLAN_WORDS = 262144;
static const uint32_t MAX_PLAN_ITEMS = 256;
static const uint32_t NONE = 0xffffffffu;
// Admission-to-execution delay bound for an injected command without a
// recorded execution: admitted this close to the witness end it may still have
// been in flight; earlier, the live game dropped it. Live MP executed injected
// commands at most 4 jobs after admission (34,690 commands of an
// 8-player match); 64 leaves a wide margin.
static const uint32_t IN_FLIGHT_JOBS = 64;
// s_pair: the partner boundary's index (admission <-> execution), PAIR_NONE if
// unpaired; an admission's PAIR_INJECTED bit marks a retimed injected command.
static const uint32_t PAIR_INDEX = 0x7fffffffu;
static const uint32_t PAIR_NONE = PAIR_INDEX;
static const uint32_t PAIR_INJECTED = 0x80000000u;
// s_kind, per log index.
static const uint8_t KIND_INJECTED = 1, KIND_RETIMED = 2, KIND_UNRETIMED = 4;

static ReplayRetimeRef s_refs[MAX_EVENTS];
static uint8_t s_kind[MAX_EVENTS];
static uint32_t s_pair[MAX_BOUNDARIES];
static uint32_t s_pending[MAX_PENDING];
static uint32_t s_planWords[MAX_PLAN_WORDS];

static bool isInjected(const ReplayEventRecord &e) {
    return (e.flags & REPLAY_FLAG_FOREIGN_CALLER) || e.originHint == 1 || e.originHint == 4;
}

static int compareRefs(const void *a, const void *b) {
    const ReplayRetimeRef *x = (const ReplayRetimeRef *)a, *y = (const ReplayRetimeRef *)b;
    if (x->newTick != y->newTick) return x->newTick < y->newTick ? -1 : 1;
    if (x->execOrder != y->execOrder) return x->execOrder < y->execOrder ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index ? 1 : 0;
}

static uint32_t minTick(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

// Pairs admissions with executions by (token, class). Returns 0 on success.
static uint32_t pairBoundaries(const ReplayBoundaryEntry *b, uint32_t count) {
    uint32_t pending = 0;
    for (uint32_t i = 0; i < count; ++i) {
        s_pair[i] = PAIR_NONE;
        const ReplayBoundaryEntry &e = b[i];
        if (e.trace.phase == 1) {
            for (uint32_t p = 0; p < pending; ++p) {
                const ReplayBoundaryEntry &q = b[s_pending[p]];
                if (q.eventToken == e.eventToken && q.trace.vtableRva == e.trace.vtableRva) {
                    return 3; // token reused before execution: ambiguous
                }
            }
            if (pending == MAX_PENDING) {
                return 4;
            }
            s_pending[pending++] = i;
        } else if (e.trace.phase == 2) {
            for (uint32_t p = 0; p < pending; ++p) {
                const ReplayBoundaryEntry &q = b[s_pending[p]];
                if (q.eventToken == e.eventToken && q.trace.vtableRva == e.trace.vtableRva) {
                    s_pair[s_pending[p]] = i;
                    s_pair[i] = s_pending[p];
                    s_pending[p] = s_pending[--pending];
                    break;
                }
            }
            // An unpaired execution was admitted before the recording started.
        }
    }
    return 0;
}

void replayRetime(const uint8_t *log, uint32_t used, uint32_t epochTick,
                  const ReplayBoundaryEntry *b, uint32_t count, uint32_t dropped,
                  uint32_t hooks, uint32_t limitTick, ReplayRetimeResult *out) {
    memset(out, 0, sizeof(*out));
    bool retime = true;
    // `end`: the last job the output must be exact through; NONE = the whole log.
    uint32_t end = limitTick ? limitTick : NONE;
    if (count > MAX_BOUNDARIES) {
        count = MAX_BOUNDARIES;
        dropped = dropped ? dropped : 1;
    }
    if (hooks != 0xf) {
        out->failReason = 1;
        retime = false;
    } else if (uint32_t why = pairBoundaries(b, count)) {
        out->failReason = why;
        retime = false;
    } else if (dropped) {
        // The witness filled up. Entries are chronological and stop at the
        // overflow, so every job before its last entry's job is complete.
        out->failReason = 2;
        const uint32_t last = count ? b[count - 1].trace.simulationTick : epochTick;
        end = minTick(end, last ? last - 1 : 0);
    }

    // Walk the log and the admission witness together: both are chronological
    // in (tick, order), so a merge finds each event's admission record.
    uint32_t cursor = 0;
    uint32_t offset = 0;
    uint32_t n = 0;
    uint32_t firstInFlight = NONE; // earliest admission of an injected event possibly in flight
    const uint32_t witnessEnd = count ? b[count - 1].trace.simulationTick : epochTick;
    for (uint32_t index = 0; offset + sizeof(ReplayEventRecord) <= used; ++index) {
        ReplayEventRecord e;
        memcpy(&e, log + offset, sizeof(e));
        if (n == MAX_EVENTS) {
            // This event and the rest do not fit. It may execute in its own
            // admission job (remote delay 0), so only earlier jobs are known.
            out->failReason = 5;
            end = minTick(end, e.simulationTick ? e.simulationTick - 1 : 0);
            break;
        }
        ReplayRetimeRef &ref = s_refs[n++];
        ref.newTick = e.simulationTick;
        ref.execOrder = e.order;
        ref.index = index;
        ref.offset = offset;
        offset += sizeof(e) + e.payloadLength;

        const bool injected = isInjected(e);
        s_kind[index] = injected ? KIND_INJECTED : 0;
        if (!retime) {
            continue;
        }
        uint32_t adm = NONE;
        while (cursor < count) {
            const ReplayBoundaryEntry &a = b[cursor];
            if (a.trace.phase != 1) {
                ++cursor;
                continue;
            }
            if (a.trace.simulationTick < e.simulationTick ||
                (a.trace.simulationTick == e.simulationTick && a.trace.order < e.order)) {
                ++cursor;
                continue;
            }
            if (a.trace.simulationTick == e.simulationTick && a.trace.order == e.order &&
                a.trace.vtableRva == e.vtableRva) {
                adm = cursor++;
            }
            break;
        }
        if (!injected) {
            continue;
        }
        const uint32_t exec = adm != NONE ? (s_pair[adm] & PAIR_INDEX) : PAIR_NONE;
        if (exec == PAIR_NONE || b[exec].trace.simulationTick == 0 ||
            b[exec].trace.simulationTick - 1 < epochTick) {
            // No recorded execution: left out below. Only one admitted near
            // the end of the witness can have executed after it.
            s_kind[index] |= KIND_UNRETIMED;
            if (e.simulationTick + IN_FLIGHT_JOBS > witnessEnd) {
                firstInFlight = minTick(firstInFlight, e.simulationTick);
            }
            continue;
        }
        s_pair[adm] |= PAIR_INJECTED;
        const uint32_t tick = b[exec].trace.simulationTick - 1;
        if (tick != ref.newTick) {
            s_kind[index] |= KIND_RETIMED;
        }
        ref.newTick = tick;
        ref.execOrder = b[exec].trace.order;
    }
    // A truncated output keeps only what is exact: an in-flight command's
    // execution job is unknown, so the cut also comes before its admission.
    if (end != NONE && retime) {
        end = minTick(end, firstInFlight);
    }
    qsort(s_refs, n, sizeof(ReplayRetimeRef), compareRefs);

    // Keep the events that execute no later than `end` (playback executes an
    // event one job after its newTick) and count what is kept.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const ReplayRetimeRef &ref = s_refs[i];
        if (end != NONE && ref.newTick >= end) {
            ++out->cutEvents;
            continue;
        }
        const uint8_t kind = s_kind[ref.index];
        if (kind & KIND_UNRETIMED) {
            // Admitted but never executed while the witness ran: every peer
            // dropped it (lockstep), so playback must not execute it either.
            ++out->unretimed;
            continue;
        }
        out->injected += (kind & KIND_INJECTED) ? 1 : 0;
        out->retimed += (kind & KIND_RETIMED) ? 1 : 0;
        ReplayEventRecord e;
        memcpy(&e, log + ref.offset, sizeof(e));
        out->bytes += sizeof(e) + e.payloadLength;
        s_refs[kept++] = ref;
    }
    n = kept;
    out->events = n;
    out->endTick = end != NONE ? end : 0;
    out->firstTick = n ? s_refs[0].newTick : 0;
    out->lastTick = n ? s_refs[n - 1].newTick : 0;
    if (!retime) {
        return;
    }

    // Intra-drain plans. Executions of one job are contiguous in the witness
    // (admissions made by an executing command may interleave; skip those).
    uint32_t words = 3;
    uint32_t jobs = 0;
    s_planWords[0] = 0x524F5845u; // "EXOR"
    s_planWords[1] = 1;
    uint32_t i = 0;
    while (i < count) {
        if (b[i].trace.phase != 2) {
            ++i;
            continue;
        }
        const uint32_t tick = b[i].trace.simulationTick;
        uint32_t items[MAX_PLAN_ITEMS];
        uint16_t orders[MAX_PLAN_ITEMS];
        uint32_t k = 0;
        bool overflow = false;
        for (; i < count && !(b[i].trace.phase == 2 && b[i].trace.simulationTick != tick); ++i) {
            if (b[i].trace.phase != 2) {
                continue;
            }
            if (k == MAX_PLAN_ITEMS) {
                overflow = true;
                continue;
            }
            const uint32_t adm = s_pair[i] & PAIR_INDEX;
            const bool inj = adm != PAIR_NONE && (s_pair[adm] & PAIR_INJECTED);
            // Insertion by execution order (normally already ascending).
            uint32_t at = k++;
            while (at > 0 && orders[at - 1] > b[i].trace.order) {
                orders[at] = orders[at - 1];
                items[at] = items[at - 1];
                --at;
            }
            orders[at] = b[i].trace.order;
            items[at] = (inj ? 0x80000000u : 0) | (b[i].trace.vtableRva & 0x7fffffffu);
        }
        bool injectedBeforeOther = false, seenInjected = false;
        for (uint32_t j = 0; j < k; ++j) {
            if (items[j] & 0x80000000u) {
                seenInjected = true;
            } else if (seenInjected) {
                injectedBeforeOther = true;
            }
        }
        if (!injectedBeforeOther || overflow || tick < epochTick || (end != NONE && tick > end) ||
            words + 2 + k > MAX_PLAN_WORDS) {
            continue;
        }
        s_planWords[words++] = tick - epochTick;
        s_planWords[words++] = k;
        memcpy(s_planWords + words, items, k * 4);
        words += k;
        ++jobs;
    }
    s_planWords[2] = jobs;
    out->planJobs = jobs;
    out->planWords = words;
}

const ReplayRetimeRef *replayRetimeRefs() {
    return s_refs;
}

const uint32_t *replayRetimePlanWords() {
    return s_planWords;
}
