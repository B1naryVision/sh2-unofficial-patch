#include "replayRetime.h"

#include "replayRecorder.h"

#include <cstdlib>
#include <cstring>
#include <windows.h>

static const uint32_t MAX_EVENTS = REPLAY_MAX_EVENTS;
static const uint32_t MAX_BOUNDARIES = REPLAY_BOUNDARY_CAPACITY;
static const uint32_t MAX_PLAN_WORDS = 262144;
static const uint32_t MAX_PLAN_ITEMS = 256;
static const uint32_t NONE = 0xffffffffu;
// Admission-to-execution delay bound for an injected command without a
// recorded execution: admitted this close to the witness end it may still have
// been in flight; earlier, the live game dropped it. Live MP executed injected
// commands at most 4 jobs after admission (34,690 commands of an
// 8-player match); 64 leaves a wide margin. The pairing uses the same bound to
// expire admissions that never executed.
static const uint32_t IN_FLIGHT_JOBS = 64;
// Admissions awaiting their execution at any one moment, and the hash that
// finds them. Expiry keeps this to what is admitted within IN_FLIGHT_JOBS.
static const uint32_t MAX_PENDING = 16384;
static const uint32_t PENDING_SLOTS = 2 * MAX_PENDING; // power of two, half full at most
// s_pair: the partner boundary's index (admission <-> execution), PAIR_NONE if
// unpaired; an admission's PAIR_INJECTED bit marks a retimed injected command.
static const uint32_t PAIR_INDEX = 0x7fffffffu;
static const uint32_t PAIR_NONE = PAIR_INDEX;
static const uint32_t PAIR_INJECTED = 0x80000000u;
// s_kind, per log index.
static const uint8_t KIND_INJECTED = 1, KIND_RETIMED = 2, KIND_UNRETIMED = 4;

// Working storage, allocated by replayRetimeAcquire for a recording.
static ReplayRetimeRef *s_refs = nullptr;
static uint8_t *s_kind = nullptr;
static uint32_t *s_pair = nullptr;
static uint32_t *s_planWords = nullptr;
static uint32_t *s_slots = nullptr; // PENDING_SLOTS boundary indices or NONE
// Pending admissions in admission order, for expiry: a ring of MAX_PENDING.
static uint32_t *s_fifo = nullptr;

static void *allocate(size_t bytes) {
    return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void release(void *p) {
    if (p) {
        VirtualFree(p, 0, MEM_RELEASE);
    }
}

bool replayRetimeAcquire() {
    if (!s_refs) {
        s_refs = (ReplayRetimeRef *)allocate(MAX_EVENTS * sizeof(ReplayRetimeRef));
        s_kind = (uint8_t *)allocate(MAX_EVENTS);
        s_pair = (uint32_t *)allocate(MAX_BOUNDARIES * sizeof(uint32_t));
        s_planWords = (uint32_t *)allocate(MAX_PLAN_WORDS * sizeof(uint32_t));
        s_slots = (uint32_t *)allocate(PENDING_SLOTS * sizeof(uint32_t));
        s_fifo = (uint32_t *)allocate(MAX_PENDING * sizeof(uint32_t));
    }

    if (!s_refs || !s_kind || !s_pair || !s_planWords || !s_slots || !s_fifo) {
        replayRetimeRelease();
        return false;
    }

    return true;
}

void replayRetimeRelease() {
    release(s_refs);
    release(s_kind);
    release(s_pair);
    release(s_planWords);
    release(s_slots);
    release(s_fifo);
    s_refs = nullptr;
    s_kind = nullptr;
    s_pair = nullptr;
    s_planWords = nullptr;
    s_slots = nullptr;
    s_fifo = nullptr;
}

static int compareRefs(const void *a, const void *b) {
    const ReplayRetimeRef *x = (const ReplayRetimeRef *)a, *y = (const ReplayRetimeRef *)b;

    if (x->newTick != y->newTick) {
        return x->newTick < y->newTick ? -1 : 1;
    }

    if (x->execOrder != y->execOrder) {
        return x->execOrder < y->execOrder ? -1 : 1;
    }

    return x->index < y->index ? -1 : x->index > y->index ? 1 : 0;
}

static uint32_t minTick(uint32_t a, uint32_t b) { return a < b ? a : b; }

static uint32_t slotOf(const ReplayBoundaryEntry &e) {
    uint32_t h = e.eventToken * 2654435761u ^ e.trace.vtableRva * 40503u;
    return (h ^ (h >> 15)) & (PENDING_SLOTS - 1);
}

static bool samePending(const ReplayBoundaryEntry &a, const ReplayBoundaryEntry &b) {
    return a.eventToken == b.eventToken && a.trace.vtableRva == b.trace.vtableRva;
}

// Linear probing with backward-shift deletion, so no tombstones accumulate.
static uint32_t findPending(const ReplayBoundaryEntry *b, const ReplayBoundaryEntry &e) {
    for (uint32_t s = slotOf(e);; s = (s + 1) & (PENDING_SLOTS - 1)) {
        if (s_slots[s] == NONE || samePending(b[s_slots[s]], e)) {
            return s;
        }
    }
}

static void erasePending(const ReplayBoundaryEntry *b, uint32_t hole) {
    s_slots[hole] = NONE;

    for (uint32_t s = (hole + 1) & (PENDING_SLOTS - 1); s_slots[s] != NONE;
         s = (s + 1) & (PENDING_SLOTS - 1)) {
        const uint32_t home = slotOf(b[s_slots[s]]);
        // Move the entry back into the hole unless its home lies cyclically
        // in (hole, s], where it is already reachable.
        const bool reachable = hole <= s ? (home > hole && home <= s) : (home > hole || home <= s);

        if (!reachable) {
            s_slots[hole] = s_slots[s];
            s_slots[s] = NONE;
            hole = s;
        }
    }
}

// Pairs admissions with executions by (token, class) in one pass. An admission
// still unpaired IN_FLIGHT_JOBS after it was made never executed (the live game
// dropped it on every peer) and is expired, which also frees its token for
// reuse by a later command at the same address. Returns 0 on success.
static uint32_t pairBoundaries(const ReplayBoundaryEntry *b, uint32_t count) {
    for (uint32_t i = 0; i < PENDING_SLOTS; ++i) {
        s_slots[i] = NONE;
    }

    uint32_t head = 0, tail = 0, live = 0;

    for (uint32_t i = 0; i < count; ++i) {
        s_pair[i] = PAIR_NONE;
        const ReplayBoundaryEntry &e = b[i];

        // Expire from the oldest admission. Entries already paired are skipped.
        while (head != tail) {
            const uint32_t oldest = s_fifo[head % MAX_PENDING];

            if (s_pair[oldest] != PAIR_NONE) {
                ++head;
                continue;
            }

            if (b[oldest].trace.simulationTick + IN_FLIGHT_JOBS >= e.trace.simulationTick) {
                break;
            }

            erasePending(b, findPending(b, b[oldest]));
            --live;
            ++head;
        }

        if (e.trace.phase == 1) {
            const uint32_t s = findPending(b, e);

            if (s_slots[s] != NONE) {
                return 3; // token reused before execution: ambiguous
            }

            if (live == MAX_PENDING || tail - head == MAX_PENDING) {
                return 4;
            }

            s_slots[s] = i;
            s_fifo[tail++ % MAX_PENDING] = i;
            ++live;
        } else if (e.trace.phase == 2) {
            const uint32_t s = findPending(b, e);

            // An unpaired execution was admitted before the recording started.
            if (s_slots[s] != NONE) {
                const uint32_t adm = s_slots[s];
                s_pair[adm] = i;
                s_pair[i] = adm;
                erasePending(b, s);
                --live;
            }
        }
    }

    return 0;
}

void replayRetime(
    const uint8_t *log, uint32_t used, uint32_t epochTick, const ReplayBoundaryEntry *b,
    uint32_t count, uint32_t dropped, uint32_t hooks, uint32_t limitTick, ReplayRetimeResult *out
) {
    memset(out, 0, sizeof(*out));

    if (!s_refs) {
        out->failReason = 6;
        return;
    }

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

        const bool injected = replayEventRequiresInjection(e);
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

const ReplayRetimeRef *replayRetimeRefs() { return s_refs; }

const uint32_t *replayRetimePlanWords() { return s_planWords; }
