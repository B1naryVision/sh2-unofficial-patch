#include "prngGuard.h"


#include "replayRecorder.h"
#include "replayTrace.h"
#include "../core/hook.h"
#include "../core/readableMemory.h"

#include <cstring>
#include <cstdio>
#include <windows.h>

// Ghidra VAs use ImageBase 0x400000; runtime addresses are module base + RVA.
static const uintptr_t SIM_TICK_ENTRY_RVA = 0x36aca0;
static const uintptr_t SIM_TICK_DONE_RVA = 0x36acff;
static const uintptr_t FX_FLAG_RVA = 0xc800b0;
static const uintptr_t GAME_PTR_RVA = 0x6e8bcc;
// Monastery's prayer-stage clock is neither constructed nor serialized by the
// stock game (ctor 0x4739b0, serializer 0x4740a0). Heap leftovers can therefore
// trigger a simulation RNG draw at a different update after the same save load.
static const uintptr_t MONASTERY_CLOCK_RVA = 0x739ba;
static const uint8_t MONASTERY_CLOCK_STOCK[] = {0x89, 0x86, 0x60, 0x01, 0x00, 0x00};

static const uintptr_t GAME_RANDOM_OFF = 0x70;
static const uintptr_t ITER1_TABLE_OFF = 0x4007c;
static const uintptr_t ITER1_CURSOR_OFF = 0x40080;
static const uintptr_t ITER0_TABLE_OFF = 0x40074;
static const uintptr_t ITER0_CURSOR_OFF = 0x40078;

// Phase probe. Six `MOV ECX,imm32` boundaries inside the tick
// body, one before each simulation subsystem call. The immediates are absolute
// VAs in a Ghidra ImageBase-0x400000 world, so the expected live bytes are
// rebuilt from this module's actual base at install time.
static const size_t PHASE_SITES = 6;
static const uintptr_t PHASE_SITE_RVA[PHASE_SITES] = {
    0x36acc3, 0x36accd, 0x36acd7, 0x36ace1, 0x36aceb, 0x36acf5,
};
static const uintptr_t PHASE_SITE_IMM_RVA[PHASE_SITES] = {
    0x6e5f40,   // FUN_00768780
    0x6e5e74,   // FUN_00767f40
    0x6e8728,   // FUN_00769be0 -- the manager singleton ReturnLordChore uses
    0x6e8800,   // FUN_0076a140
    0xc7937c,   // FUN_00775090 -- ActorMgr
    0x6e88c8,   // FUN_0076a610
};
static const size_t PHASE_STOCK_LEN = 5;

// Phase attribution, second pass: the two carrier pumps are object-pool loops,
// so the question "which call site draws" becomes "which pooled object's
// virtual call draws". Four more read-only sites, all position-independent:
//
//   0x369c47  MOV EAX,[ESI] ; MOV EDX,[EAX+0x20]   element head, D loop 1
//   0x369cb8  MOV ESI,[EDI+8] ; ADD ESI,4          D loop 1 -> loop 2 (vslot 0x24)
//   0x369cd8  MOV ESI,[EDI+8] ; ADD ESI,4          D loop 2 -> loop 3 (vslots 0x44/0x48)
//   0x36a198  MOV EAX,[ESI] ; MOV EDX,[EAX+0x20]   element head, E
//
// Nothing branches into any of the overwritten ranges: 0x369cb8 and 0x369cd8
// are themselves branch targets (from 0x369c3f and 0x369cc1), which lands on
// the detour rather than inside it, and the two element heads have no
// predecessors other than fallthrough.
static const uintptr_t ELEM_D_RVA = 0x369c47;
static const uintptr_t LOOP_D2_RVA = 0x369cb8;
static const uintptr_t LOOP_D3_RVA = 0x369cd8;
static const uintptr_t ELEM_E_RVA = 0x36a198;
static const uint8_t ELEM_STOCK[] = {0x8b, 0x06, 0x8b, 0x50, 0x20};
// Third pass: phase G, FUN_0076a610, walks 30 object lists of the
// manager at RVA 0x6e88c8 and calls each object's vt+0x18. Its draws were
// invisible to the census (charged to nobody, or to the next tick). Element
// head at 0x36a656: `mov edx,[ecx] ; mov eax,[edx+0x18]` (ECX = the object),
// position-independent; 0x36a656 is a branch target (from 0x36a632), which
// lands on the detour. Charged with region RGN_OUTSIDE.
static const uintptr_t ELEM_G_RVA = 0x36a656;
static const uint8_t ELEM_G_STOCK[] = {0x8b, 0x11, 0x8b, 0x42, 0x18};
static const uint8_t LOOP_STOCK[] = {0x8b, 0x77, 0x08, 0x83, 0xc6, 0x04};

// Regions the unattributed draws are charged to when no element is pending.
enum { RGN_D1 = 0, RGN_D2, RGN_D3, RGN_E, RGN_OUTSIDE, RGN_COUNT };

struct PrngSiteBucket {
    uintptr_t vtable;
    uint32_t region;
    uint32_t draws;
    uint32_t hits;   // elements of this class whose call drew at least once
    uint32_t visits; // elements of this class walked
};
static const size_t MAX_SITE_BUCKETS = 512;


// fld dword ptr [ecx+8] ; inc dword ptr [ecx+0xc]
static const uint8_t ENTRY_STOCK[] = {0xd9, 0x41, 0x08, 0xff, 0x41, 0x0c};
// cmp dword ptr [<fx flag>], 0 -- the operand is an absolute address, so the
// expected bytes are built at install time from this module's actual base.
static const size_t DONE_STOCK_LEN = 7;

// 524,288 ticks is a little over four hours at the stock 35 ticks/s, 6 MB of
// .bss each for the recorded and the loaded stream. Never wraps: a stream that
// silently lost its oldest ticks would restore the wrong values and still look
// complete.
//
// Raised from 262,144. A Freeman Kingmaker start left to run unattended until
// the AI mounts its own assault takes a long time -- a Freeman AI escalates
// slowly, with fewer resources, and it has to promote first. Two hours of game
// time was not a safe ceiling for that, and a recording that hits the cap
// loses cursor entries (`prngDropped` above 0).
static const uint32_t MAX_PRNG_TICKS = REPLAY_PRNG_CAPACITY;

// The phase record is diagnostic only -- it exists for the `prngcur1` /
// `S2Governor` question, not for replay -- so it keeps the older, smaller cap
// rather than costing another 10 MB. Overflow shows up as `phaseDropped`, which
// no acceptance check reads.
static const uint32_t MAX_PHASE_TICKS = 262144;

static ReplayPrngEntry s_record[MAX_PRNG_TICKS];
static uint32_t s_recordCount = 0;
static ReplayMonasteryClock s_recordClocks[REPLAY_CLOCK_MAX], s_streamClocks[REPLAY_CLOCK_MAX];
static uint32_t s_recordClockCount = 0, s_streamClockCount = 0;
static bool s_clocksCaptured = false, s_clocksPending = false;
static ReplayPrngEntry s_stream[MAX_PRNG_TICKS];
static uint32_t s_streamCount = 0;
static uint32_t s_streamPos = 0;
static uint32_t s_streamEpoch = 0;

// Render gates (prng stream v3). Simulation code that draws
// iterator 1 only when something was RENDERED since the last update: the
// render state differs between a recording and any playback, so the number
// of in-tick draws did too, and prngGuard's tick-entry restore cannot cover
// a draw made inside the tick. The gate inputs are captured at the entry of
// every simulation update while recording and written back at the same point
// during playback. Rendering only happens between updates, so a gate's value
// at update entry is the whole render-side input to that update.
//
//   Sheep: draw method vt+0x5c (FUN_0052fbc0) sets the byte below to 1; the
//   in-tick bleat check FUN_0052fd40 decrements the global bleat cooldown only
//   when it is set (and clears it), then draws iterator 1 at cooldown 0.
//   KB section 8.1f.
//
// One byte per job entry: bit k = the Sheep flag at the entry of the job's
// k-th simulation update (MP runs about five per job). Updates past the 8th
// in one job are neither captured nor restored (counted in s_gateOverflow).
static const uintptr_t SHEEP_DRAWN_FLAG_RVA = 0xeb6b08;   // VA 0x12b6b08, byte
static uint8_t s_recordGates[MAX_PRNG_TICKS];
static uint8_t s_streamGates[MAX_PRNG_TICKS];
static bool s_streamHasGates = false;
static uint32_t s_gatePos = 0;          // playback scan position in s_stream
static bool s_gateHaveRel = false;
static uint32_t s_gateRel = 0, s_gateSub = 0;
static uint8_t s_pendingGates = 0;      // recording: bits of the job not yet appended
static bool s_curGatesValid = false;    // playback: s_curGates belongs to s_gateRel
static uint8_t s_curGates = 0;
static uint32_t s_gatesApplied = 0, s_gateOverflow = 0;

#pragma pack(push, 1)
struct PrngStepEntry {
    uint32_t relativeJob, substep;
    uint16_t pre1, start1, end1, pre0, start0, end0;
    uint8_t netCounter, valid; // valid bits: iterator 1, iterator 0
    uint16_t reserved;
};
struct PrngStepHeader {
    char magic[8];
    uint32_t version, entrySize, count, epoch, dropped;
};
#pragma pack(pop)
static_assert(sizeof(PrngStepEntry) == 24, "PRNG step entry layout");
static_assert(sizeof(PrngStepHeader) == 28, "PRNG step header layout");
static const uint32_t MAX_PRNG_STEPS = 65536;
static PrngStepEntry s_steps[MAX_PRNG_STEPS], s_step = {};
static uint32_t s_stepCount = 0, s_stepDropped = 0;
static bool s_stepOpen = false, s_haveStep = false;

void prngStepsReset() {
    s_stepCount = s_stepDropped = 0;
    s_stepOpen = s_haveStep = false;
    s_step = {};
}

static uintptr_t s_exeBase = 0;
static uint32_t s_installed = 0;
static bool s_applying = false;

static uint32_t s_invocations = 0;
static uint32_t s_distinctTicks = 0;
static uint32_t s_duplicateTicks = 0;
static uint32_t s_dropped = 0;
static uint32_t s_badState = 0;
static uint32_t s_applied = 0;
static uint32_t s_missing = 0;
static uint32_t s_outsideWindow = 0;
static uint32_t s_skipped = 0;
static uint32_t s_preApplyMismatch = 0;
static int32_t s_worstDelta = 0;

// Iterator 0. Restore is OFF unless explicitly armed; see prngGuard.h.
static bool s_applyIter0 = false;
static uint32_t s_iter0Applied = 0;
static uint32_t s_iter0PreMismatch = 0;
static uint32_t s_iter0BetweenTickMoves = 0;
static uint32_t s_iter0TickGaps = 0;
static bool s_haveIter0End = false;
static uint16_t s_iter0LastEnd = 0;
static uint16_t s_openStart0 = 0;
static uint16_t s_simEndCursor0 = 0;

// The tick currently between the two hooks.
static bool s_openTick = false;
static uint32_t s_openRelative = 0;
static uint16_t s_openStart = 0;
static bool s_haveLastRelative = false;
static uint32_t s_lastRelative = 0;

// Phase probe state. `s_openCursor` is the validated iterator-1 pointer for the
// tick currently open, cached at entry so a phase hook costs one load and one
// store; the hooks never validate and never write.
static uint32_t s_phaseInstalled = 0;
static bool s_simBoundaryInstalled = false;
static volatile uint16_t *s_openCursor = NULL;
static uint16_t s_phaseCursors[PHASE_SITES];
static ReplayPhaseEntry s_phaseRecord[MAX_PHASE_TICKS];
static uint32_t s_phaseCount = 0;
static uint32_t s_phaseDropped = 0;


// Round-2 state. `s_pendingVtable` is the element whose virtual call is
// currently in flight; the draws it made are charged when the next element head
// or loop boundary samples the cursor again.
static uint32_t s_sitesInstalled = 0;
static PrngSiteBucket s_buckets[MAX_SITE_BUCKETS];
static uint32_t s_bucketCount = 0;
static uint32_t s_bucketOverflow = 0;
static uint32_t s_unattributed[RGN_COUNT];
static uintptr_t s_pendingVtable = 0;
static uint32_t s_pendingRegion = RGN_OUTSIDE;
static uint16_t s_pendingCursor = 0;
static uint32_t s_region = RGN_OUTSIDE;

// The most recent completed tick, published to the fingerprint.
static bool s_haveSimEnd = false;
static uint32_t s_simEndRelative = 0;
static uint16_t s_simEndCursor = 0;

// Trampoline state must have external linkage: MinGW GAS cannot resolve local
// static symbols named from naked inline assembly.
uintptr_t g_prngGuardEntryReturn = 0;
uintptr_t g_prngGuardDoneReturn = 0;
uintptr_t g_prngGuardFxFlag = 0;
uintptr_t g_monasteryClockReturn = 0;
uintptr_t g_prngPhaseEcx[PHASE_SITES] = {0};
uintptr_t g_prngPhaseReturn[PHASE_SITES] = {0};
// D element head, D loop-2 boundary, D loop-3 boundary, E element head.
uintptr_t g_prngSiteReturn[5] = {0};   // [4] = phase G element head

// Resolves the live iterator-1 cursor, validating the same invariant the
// fingerprint validates: both RandomIters must point at the Game's own
// Dragonfly::Random. A failure here means the Game is not constructed or the
// layout assumption is wrong, and the only safe response is to do nothing.
static uint16_t *iter1Cursor() {
    if (!s_exeBase) {
        return NULL;
    }
    const uintptr_t game = *(volatile uintptr_t *)(s_exeBase + GAME_PTR_RVA);
    if (!game) {
        return NULL;
    }
    const uintptr_t table = *(volatile uintptr_t *)(game + ITER1_TABLE_OFF);
    if (table != game + GAME_RANDOM_OFF) {
        return NULL;
    }
    return (uint16_t *)(game + ITER1_CURSOR_OFF);
}

// Same validation for RandomIter #0: its table pointer must also address the
// Game's own Dragonfly::Random.
static uint16_t *iter0Cursor() {
    if (!s_exeBase) {
        return NULL;
    }
    const uintptr_t game = *(volatile uintptr_t *)(s_exeBase + GAME_PTR_RVA);
    if (!game) {
        return NULL;
    }
    const uintptr_t table = *(volatile uintptr_t *)(game + ITER0_TABLE_OFF);
    if (table != game + GAME_RANDOM_OFF) {
        return NULL;
    }
    return (uint16_t *)(game + ITER0_CURSOR_OFF);
}


static const uintptr_t MONASTERY_VTABLE_RVA = 0x59e904;

static bool buildingSlots(const uintptr_t **slots, uint32_t *count) {
    const uintptr_t *header = (const uintptr_t *)(s_exeBase + 0x6e5f40 + 8);
    if (!observerReadable(header, 8) || header[1] < header[0] ||
        (header[1] - header[0]) % 4 || (header[1] - header[0]) / 4 > 65536) return false;
    *count = (uint32_t)((header[1] - header[0]) / 4);
    *slots = (const uintptr_t *)header[0];
    return !*count || observerReadable(*slots, *count * 4);
}

static void epochClocks() {
    if (!s_clocksPending && (s_clocksCaptured || !replayRecorderIsRecording())) return;
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;
    if (!buildingSlots(&slots, &count)) {
        if (replayRecorderIsRecording() && !s_clocksCaptured) ++s_dropped;
        return;
    }
    if (s_clocksPending) {
        for (uint32_t i = 0; i < s_streamClockCount; ++i) {
            const ReplayMonasteryClock &e = s_streamClocks[i];
            const uintptr_t b = e.slot < count ? slots[e.slot] : 0;
            if (b && observerReadable((const void *)b, 0x168) &&
                *(const uintptr_t *)b == s_exeBase + MONASTERY_VTABLE_RVA &&
                *(const uint32_t *)(b + 0x9c) == ((uint32_t)e.serial << 16 | e.slot)) {
                *(uint32_t *)(b + 0x164) = e.clock;
            }
        }
        s_clocksPending = false;
    }
    if (!s_clocksCaptured && replayRecorderIsRecording()) {
        s_clocksCaptured = true;
        s_recordClockCount = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uintptr_t b = slots[i];
            if (!b || !observerReadable((const void *)b, 0x168) ||
                *(const uintptr_t *)b != s_exeBase + MONASTERY_VTABLE_RVA) continue;
            if (s_recordClockCount == REPLAY_CLOCK_MAX ||
                *(const uint16_t *)(b + 0x9c) != i) {
                ++s_dropped;
                return;
            }
            s_recordClocks[s_recordClockCount++] = {(uint16_t)i,
                *(const uint16_t *)(b + 0x9e), *(const uint32_t *)(b + 0x164)};
        }
    }
}

extern "C" void prngGuardTickEntry() {
    s_stepOpen = false;
    const uint32_t epoch = replayRecorderEpochTick();
    if (!epoch) {
        return;
    }
    const uint32_t now = replayTraceCurrentSimulationTick();
    if (now < epoch) {
        return;
    }
    const uint32_t relative = now - epoch;
    epochClocks();
    ++s_invocations;

    // Render gates: every simulation update, duplicates of a job included.
    {
        const uint32_t sub = (s_gateHaveRel && relative == s_gateRel) ? s_gateSub + 1 : 0;
        s_gateHaveRel = true;
        s_gateRel = relative;
        s_gateSub = sub;
        volatile uint8_t *flag = (volatile uint8_t *)(s_exeBase + SHEEP_DRAWN_FLAG_RVA);
        if (sub >= 8) {
            ++s_gateOverflow;
        } else {
            if (replayRecorderIsRecording()) {
                const uint8_t bit = (uint8_t)((*flag ? 1u : 0u) << sub);
                if (sub == 0) {
                    s_pendingGates = bit;   // appended with the job's entry at SimDone
                } else if (s_recordCount && s_record[s_recordCount - 1].relativeTick == relative) {
                    s_recordGates[s_recordCount - 1] |= bit;
                }
            }
            if (s_applying && s_streamHasGates) {
                if (sub == 0) {
                    while (s_gatePos < s_streamCount && s_stream[s_gatePos].relativeTick < relative) {
                        ++s_gatePos;
                    }
                    s_curGatesValid = s_gatePos < s_streamCount &&
                                      s_stream[s_gatePos].relativeTick == relative;
                    if (s_curGatesValid) s_curGates = s_streamGates[s_gatePos];
                }
                if (s_curGatesValid) {
                    *flag = (uint8_t)((s_curGates >> sub) & 1u);
                    ++s_gatesApplied;
                }
            }
        }
    }

    // Independent witness: observe every update, including those the legacy
    // per-job guard skips. Capture before and after restoration separately.
    if (replayRecorderIsRecording()) {
        const uint32_t substep = s_haveStep && s_step.relativeJob == relative
                                     ? s_step.substep + 1 : 0;
        s_step = {};
        s_step.relativeJob = relative;
        s_step.substep = substep;
        s_step.netCounter = *(uint8_t *)(s_exeBase + 0x17e6a34);
        uint16_t *c1 = iter1Cursor(), *c0 = iter0Cursor();
        if (c1) { s_step.pre1 = s_step.start1 = *c1; s_step.valid |= 1; }
        if (c0) { s_step.pre0 = s_step.start0 = *c0; s_step.valid |= 2; }
        s_stepOpen = s_haveStep = true;
    }

    // One capture and at most one restore per relative job tick. MP normally
    // runs five simulation updates per job (one at byte-counter rollover).
    // Restoring again would rewind the cursor into the job's own draws.
    if (s_haveLastRelative && relative == s_lastRelative) {
        ++s_duplicateTicks;
        return;
    }
    s_haveLastRelative = true;
    s_lastRelative = relative;
    ++s_distinctTicks;

    uint16_t *cursor = iter1Cursor();
    if (!cursor) {
        ++s_badState;
        s_openTick = false;
        return;
    }

    if (s_applying) {
        while (s_streamPos < s_streamCount &&
               s_stream[s_streamPos].relativeTick < relative) {
            ++s_streamPos;
            ++s_skipped;
        }
        if (s_streamPos < s_streamCount &&
            s_stream[s_streamPos].relativeTick == relative) {
            const uint16_t want = s_stream[s_streamPos].startCursor;
            if (*cursor != want) {
                ++s_preApplyMismatch;
                // Signed 16-bit distance: the cursor is a wrapping uint16, so
                // "live minus recorded" is only meaningful modulo 0x10000.
                const int32_t delta = (int32_t)(int16_t)(uint16_t)(*cursor - want);
                const int32_t magnitude = delta < 0 ? -delta : delta;
                const int32_t worst = s_worstDelta < 0 ? -s_worstDelta : s_worstDelta;
                if (magnitude > worst) {
                    s_worstDelta = delta;
                }
            }
            *cursor = want;
            ++s_applied;
            ++s_streamPos;
        } else if (s_streamCount &&
                   relative >= s_stream[0].relativeTick &&
                   relative <= s_stream[s_streamCount - 1].relativeTick) {
            // Only a gap INSIDE the recorded window is a defect. Playback
            // normally starts before and runs past the reference's window, and
            // counting those ticks would make every clean run look broken.
            ++s_missing;
        } else {
            ++s_outsideWindow;
        }
    }

    // Iterator 0. The between-tick test comes first and is measured against the
    // PRE-restore value. MP gaps include the remaining unsampled simulation
    // updates in a job; movement alone cannot identify a frame-driven consumer.
    uint16_t *cursor0 = iter0Cursor();
    if (cursor0) {
        if (s_haveIter0End) {
            ++s_iter0TickGaps;
            if (*cursor0 != s_iter0LastEnd) {
                ++s_iter0BetweenTickMoves;
            }
        }
        if (s_applying && s_applyIter0 && s_streamPos > 0 &&
            s_stream[s_streamPos - 1].relativeTick == relative) {
            const uint16_t want0 = s_stream[s_streamPos - 1].startCursor0;
            if (*cursor0 != want0) {
                ++s_iter0PreMismatch;
            }
            *cursor0 = want0;
            ++s_iter0Applied;
        }
        s_openStart0 = *cursor0;
    }

    s_openTick = true;
    s_openRelative = relative;
    s_openStart = *cursor;
    if (s_stepOpen) {
        s_step.start1 = *cursor;
        if (cursor0) s_step.start0 = *cursor0;
    }

    // Phase probe: a tick that never reaches a boundary must not inherit the
    // previous tick's sample, so seed every phase with the entry cursor. A
    // phase that genuinely ran overwrites it.
    s_openCursor = (volatile uint16_t *)cursor;
    s_pendingVtable = 0;
    s_pendingCursor = s_openStart;
    s_region = RGN_OUTSIDE;
    for (size_t i = 0; i < PHASE_SITES; ++i) {
        s_phaseCursors[i] = s_openStart;
    }
}


// Per-tick site log: the census above is cumulative, which cannot
// say WHICH class made a tick's draws differ between a recording and a
// playback. Every charge is also logged as {relative tick, key, draws}; key is
// the element's static vtable VA, or 0xF000000r for region r when no element
// was pending. Written as prngsitelog.txt beside prngsites.txt.
struct PrngSiteLogEntry {
    uint32_t relative;
    uint32_t key;
    uint32_t draws;
};
static const uint32_t MAX_SITE_LOG = 262144;
static PrngSiteLogEntry s_siteLog[MAX_SITE_LOG];
static uint32_t s_siteLogCount = 0, s_siteLogDropped = 0;

static void prngSiteLog(uint32_t key, uint32_t draws) {
    if (s_siteLogCount < MAX_SITE_LOG) {
        PrngSiteLogEntry &e = s_siteLog[s_siteLogCount++];
        e.relative = s_openRelative;
        e.key = key;
        e.draws = draws;
    } else {
        ++s_siteLogDropped;
    }
}

// Charges everything drawn since the last sample to the pending element, or to
// the current region when no element is pending (a draw made by the loop body
// itself rather than by a pooled object's virtual call).
static void prngSiteFlush() {
    if (!s_openTick || !s_openCursor) {
        s_pendingVtable = 0;
        return;
    }
    const uint16_t now = *s_openCursor;
    const uint16_t delta = (uint16_t)(now - s_pendingCursor);
    s_pendingCursor = now;
    if (!delta) {
        s_pendingVtable = 0;
        return;
    }
    if (!s_pendingVtable) {
        const uint32_t r = s_region < RGN_COUNT ? s_region : (uint32_t)RGN_OUTSIDE;
        s_unattributed[r] += delta;
        prngSiteLog(0xF0000000u | r, delta);
        return;
    }
    prngSiteLog((uint32_t)(s_pendingVtable - s_exeBase + 0x400000), delta);
    for (uint32_t i = 0; i < s_bucketCount; ++i) {
        if (s_buckets[i].vtable == s_pendingVtable &&
            s_buckets[i].region == s_pendingRegion) {
            s_buckets[i].draws += delta;
            ++s_buckets[i].hits;
            s_pendingVtable = 0;
            return;
        }
    }
    if (s_bucketCount < MAX_SITE_BUCKETS) {
        PrngSiteBucket &b = s_buckets[s_bucketCount++];
        b.vtable = s_pendingVtable;
        b.region = s_pendingRegion;
        b.draws = delta;
        b.hits = 1;
        b.visits = 0;
    } else {
        ++s_bucketOverflow;
    }
    s_pendingVtable = 0;
}

// Counts an element walk even when it draws nothing, so a class with many
// visits and no draws is distinguishable from one that is never pooled.
static void prngSiteVisit(uintptr_t vtable) {
    for (uint32_t i = 0; i < s_bucketCount; ++i) {
        if (s_buckets[i].vtable == vtable && s_buckets[i].region == s_region) {
            ++s_buckets[i].visits;
            return;
        }
    }
    if (s_bucketCount < MAX_SITE_BUCKETS) {
        PrngSiteBucket &b = s_buckets[s_bucketCount++];
        b.vtable = vtable;
        b.region = s_region;
        b.draws = 0;
        b.hits = 0;
        b.visits = 1;
    } else {
        ++s_bucketOverflow;
    }
}

extern "C" void prngSiteElement(void *object) {
    prngSiteFlush();
    if (!s_openTick || !object) {
        return;
    }
    const uintptr_t vtable = *(uintptr_t *)object;
    prngSiteVisit(vtable);
    s_pendingVtable = vtable;
    s_pendingRegion = s_region;
}

extern "C" void prngSiteLoop2() {
    prngSiteFlush();
    s_region = RGN_D2;
}

extern "C" void prngSiteLoop3() {
    prngSiteFlush();
    s_region = RGN_D3;
}

// Called from the phase samplers so a region change also closes the previous
// element out. Phase 2 opens D, phase 3 opens E, phase 4 leaves both.
static void prngSiteRegion(uint32_t region) {
    if (!s_sitesInstalled) {
        return;
    }
    prngSiteFlush();
    s_region = region;
}

// The six phase samplers. Deliberately branch-free apart from the open-tick
// guard: they run once per subsystem per tick on the simulation thread.
extern "C" void prngPhaseSample0() {
    if (s_openTick && s_openCursor) {
        s_phaseCursors[0] = *s_openCursor;
    }
}

extern "C" void prngPhaseSample1() {
    if (s_openTick && s_openCursor) {
        s_phaseCursors[1] = *s_openCursor;
    }
}

extern "C" void prngPhaseSample2() {
    if (s_openTick && s_openCursor) {
        s_phaseCursors[2] = *s_openCursor;
    }
    prngSiteRegion(RGN_D1);
}

extern "C" void prngPhaseSample3() {
    if (s_openTick && s_openCursor) {
        s_phaseCursors[3] = *s_openCursor;
    }
    prngSiteRegion(RGN_E);
}

extern "C" void prngPhaseSample4() {
    if (s_openTick && s_openCursor) {
        s_phaseCursors[4] = *s_openCursor;
    }
    prngSiteRegion(RGN_OUTSIDE);
}

extern "C" void prngPhaseSample5() {
    if (s_openTick && s_openCursor) {
        s_phaseCursors[5] = *s_openCursor;
    }
}

// Also drives the fingerprint sampler. SIM_DONE follows the emitter manager,
// whose render-dependent iterator-1 draws are presentation only. When the phase
// boundary is installed, witness iterator 1 at entry to that manager (phase 5).
// Keep the live cursor untouched; the next update's entry restore handles it.

extern "C" void prngGuardSimDone() {
    // Tick-exact all-Bakery smoke observation for the economy policy.  This
    // must precede every early return below so it remains active when PRNG
    // recording/playback is disarmed (the normal economy-policy case).
    if (s_stepOpen) {
        s_stepOpen = false;
        uint16_t *c1 = iter1Cursor(), *c0 = iter0Cursor();
        if (c1) s_step.end1 = *c1; else s_step.valid &= ~1u;
        if (c0) s_step.end0 = *c0; else s_step.valid &= ~2u;
        if (replayRecorderIsRecording()) {
            if (s_stepCount < MAX_PRNG_STEPS) s_steps[s_stepCount++] = s_step;
            else ++s_stepDropped;
        }
    }
    if (!s_openTick) {
        replayRecorderSimulationTick();
        return;
    }
    s_openTick = false;

    uint16_t *cursor = iter1Cursor();
    if (!cursor) {
        ++s_badState;
        replayRecorderSimulationTick();
        return;
    }

    s_haveSimEnd = true;
    s_simEndRelative = s_openRelative;
    s_simEndCursor = s_simBoundaryInstalled ? s_phaseCursors[5] : *cursor;

    uint16_t *cursor0 = iter0Cursor();
    if (cursor0) {
        s_simEndCursor0 = *cursor0;
        s_iter0LastEnd = *cursor0;
        s_haveIter0End = true;
    }

    if (replayRecorderIsRecording()) {
        if (s_recordCount >= MAX_PRNG_TICKS) {
            ++s_dropped;
        } else {
            s_recordGates[s_recordCount] = s_pendingGates;
            ReplayPrngEntry &e = s_record[s_recordCount++];
            e.relativeTick = s_openRelative;
            e.startCursor = s_openStart;
            e.simEndCursor = s_simEndCursor;
            e.startCursor0 = s_openStart0;
            e.simEndCursor0 = s_simEndCursor0;
        }
        if (s_phaseInstalled) {
            if (s_phaseCount >= MAX_PHASE_TICKS) {
                ++s_phaseDropped;
            } else {
                ReplayPhaseEntry &p = s_phaseRecord[s_phaseCount++];
                p.relativeTick = s_openRelative;
                p.startCursor = s_openStart;
                for (size_t i = 0; i < PHASE_SITES; ++i) {
                    p.phaseCursor[i] = s_phaseCursors[i];
                }
                p.simEndCursor = *cursor;
            }
        }
    }
    s_openCursor = NULL;

    replayRecorderSimulationTick();
}

// Re-emits `fld dword ptr [ecx+8]` and `inc dword ptr [ecx+0xc]`, both
// position-independent, then resumes at the site + 6.
__declspec(naked) static void prngGuardEntryHook() {
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "call _prngGuardTickEntry\n\t"
        "popfl\n\t"
        "popal\n\t"
        "flds 0x8(%ecx)\n\t"
        "incl 0xc(%ecx)\n\t"
        "jmp *_g_prngGuardEntryReturn\n\t");
}

// Re-emits `cmp dword ptr [<fx flag>], 0` through a relocated pointer, because
// the stock instruction carries an absolute address. EAX is saved and restored
// around it, and `pop` does not disturb the flags the following `jz` reads.
__declspec(naked) static void prngGuardDoneHook() {
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "call _prngGuardSimDone\n\t"
        "popfl\n\t"
        "popal\n\t"
        "pushl %eax\n\t"
        "movl _g_prngGuardFxFlag, %eax\n\t"
        "cmpl $0, (%eax)\n\t"
        "popl %eax\n\t"
        "jmp *_g_prngGuardDoneReturn\n\t");
}

__declspec(naked) static void monasteryClockHook() {
    __asm__ volatile(
        "movl %eax, 0x160(%esi)\n\t" // stock store; EAX was zeroed just before it
        "movl %eax, 0x164(%esi)\n\t"
        "jmp *_g_monasteryClockReturn\n\t");
}


// The presentation boundary is part of replay in both builds, independently
// of the optional diagnostic phase/census hooks.
__declspec(naked) static void prngPhaseHook5() {
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "call _prngPhaseSample5\n\t"
        "popfl\n\t"
        "popal\n\t"
        "movl _g_prngPhaseEcx+20, %ecx\n\t"
        "jmp *_g_prngPhaseReturn+20\n\t");
}


void installPrngGuard() {
    s_exeBase = (uintptr_t)GetModuleHandleA(NULL);
    if (!s_exeBase) {
        return;
    }
    uint8_t *entry = (uint8_t *)(s_exeBase + SIM_TICK_ENTRY_RVA);
    uint8_t *done = (uint8_t *)(s_exeBase + SIM_TICK_DONE_RVA);

    const uintptr_t fxFlag = s_exeBase + FX_FLAG_RVA;
    uint8_t doneStock[DONE_STOCK_LEN];
    doneStock[0] = 0x83;
    doneStock[1] = 0x3d;
    memcpy(doneStock + 2, &fxFlag, 4);
    doneStock[6] = 0x00;

    uint8_t boundaryStock[PHASE_STOCK_LEN] = {0xb9};
    const uintptr_t boundaryImm = s_exeBase + PHASE_SITE_IMM_RVA[5];
    memcpy(boundaryStock + 1, &boundaryImm, 4);
    uint8_t *boundary = (uint8_t *)(s_exeBase + PHASE_SITE_RVA[5]);
    uint8_t *monastery = (uint8_t *)(s_exeBase + MONASTERY_CLOCK_RVA);

    // All-or-nothing: an unknown executable keeps all four stock sites.
    if (memcmp(entry, ENTRY_STOCK, sizeof(ENTRY_STOCK)) != 0 ||
        memcmp(done, doneStock, DONE_STOCK_LEN) != 0 ||
        memcmp(boundary, boundaryStock, PHASE_STOCK_LEN) != 0 ||
        memcmp(monastery, MONASTERY_CLOCK_STOCK, sizeof(MONASTERY_CLOCK_STOCK)) != 0) {
        return;
    }

    g_prngGuardEntryReturn = (uintptr_t)entry + sizeof(ENTRY_STOCK);
    g_prngGuardDoneReturn = (uintptr_t)done + DONE_STOCK_LEN;
    g_prngGuardFxFlag = fxFlag;
    g_prngPhaseEcx[5] = boundaryImm;
    g_prngPhaseReturn[5] = (uintptr_t)boundary + PHASE_STOCK_LEN;
    g_monasteryClockReturn = (uintptr_t)monastery + sizeof(MONASTERY_CLOCK_STOCK);

    installHook(entry, reinterpret_cast<void *>(prngGuardEntryHook),
                sizeof(ENTRY_STOCK));
    installHook(done, reinterpret_cast<void *>(prngGuardDoneHook),
                DONE_STOCK_LEN);
    installHook(boundary, reinterpret_cast<void *>(prngPhaseHook5), PHASE_STOCK_LEN);
    installHook(monastery, reinterpret_cast<void *>(monasteryClockHook), sizeof(MONASTERY_CLOCK_STOCK));
    s_simBoundaryInstalled = true;
    s_installed = 0x3;
}

uint32_t prngPhaseInstalled() {
    return s_phaseInstalled;
}

bool prngGuardSimEndCursor(uint16_t *cursorOut, uint32_t *relativeTickOut) {
    if (!s_haveSimEnd) {
        return false;
    }
    if (cursorOut) {
        *cursorOut = s_simEndCursor;
    }
    if (relativeTickOut) {
        *relativeTickOut = s_simEndRelative;
    }
    return true;
}

uint32_t prngGuardControl(uint32_t mode) {
    if (mode == 0) {
        prngStepsReset();
        s_recordCount = 0;
        s_recordClockCount = 0;
        s_clocksCaptured = false;
        s_dropped = 0;
        s_pendingGates = 0;
        s_gateHaveRel = false;
        s_gateOverflow = 0;
        s_invocations = 0;
        s_distinctTicks = 0;
        s_duplicateTicks = 0;
        s_badState = 0;
        s_haveLastRelative = false;
        s_openTick = false;
        s_haveIter0End = false;
        s_iter0BetweenTickMoves = 0;
        s_iter0TickGaps = 0;
        s_phaseCount = 0;
        s_phaseDropped = 0;
        s_bucketCount = 0;
        s_siteLogCount = 0;
        s_siteLogDropped = 0;
        s_bucketOverflow = 0;
        s_pendingVtable = 0;
        s_region = RGN_OUTSIDE;
        for (uint32_t i = 0; i < RGN_COUNT; ++i) {
            s_unattributed[i] = 0;
        }
        s_openCursor = NULL;
        return s_recordCount;
    }
    if (mode == 3 || mode == 4) {
        s_applyIter0 = (mode == 4);
        s_iter0Applied = 0;
        s_iter0PreMismatch = 0;
        return s_applyIter0 ? 1u : 0u;
    }
    if (mode == 2) {
        s_streamClockCount = 0;
        s_clocksPending = false;
        s_applying = false;
        s_streamCount = 0;
        s_streamPos = 0;
        s_streamEpoch = 0;
        s_applied = 0;
        s_missing = 0;
        s_outsideWindow = 0;
        s_skipped = 0;
        s_preApplyMismatch = 0;
        s_worstDelta = 0;
        return 0;
    }
    return s_recordCount;
}

size_t prngGuardStatus(void *out, size_t cap) {
    if (!out || cap < sizeof(PrngGuardStatus)) {
        return 0;
    }
    PrngGuardStatus status = {};
    status.magic = 0x31475250u; // "PRG1"
    status.version = 1;
    status.installed = (uint16_t)s_installed;
    status.invocations = s_invocations;
    status.distinctTicks = s_distinctTicks;
    status.duplicateTicks = s_duplicateTicks;
    status.recorded = s_recordCount;
    status.droppedTicks = s_dropped;
    status.badState = s_badState;
    status.streamEntries = s_streamCount;
    status.streamEpoch = s_streamEpoch;
    status.applying = s_applying ? 1u : 0u;
    status.applied = s_applied;
    status.missing = s_missing;
    status.outsideWindow = s_outsideWindow;
    status.skipped = s_skipped;
    status.preApplyMismatch = s_preApplyMismatch;
    status.worstDelta = s_worstDelta;
    status.lastRelative = s_simEndRelative;
    status.lastStart = s_openStart;
    status.lastSimEnd = s_simEndCursor;
    status.iter0Applying = s_applyIter0 ? 1u : 0u;
    status.iter0Applied = s_iter0Applied;
    status.iter0PreMismatch = s_iter0PreMismatch;
    status.iter0BetweenTickMoves = s_iter0BetweenTickMoves;
    status.iter0TickGaps = s_iter0TickGaps;
    status.lastStart0 = s_openStart0;
    status.lastSimEnd0 = s_simEndCursor0;
    memcpy(out, &status, sizeof(status));
    return sizeof(status);
}

// Writes the phase stream beside the iterator stream. `<dir>\prng.bin` becomes
// `<dir>\prngphase.bin`; any other path just gets ".phase" appended. Failure is
// silent and non-fatal: the phase stream is a diagnostic, and losing it must
// never make a container look un-saved.
static void prngPhaseSave(const char *path) {
    if (!s_phaseInstalled || !s_phaseCount) {
        return;
    }
    char out[544];
    const size_t len = strlen(path);
    static const char kTail[] = "prng.bin";
    const size_t tail = sizeof(kTail) - 1;
    if (len + 16 >= sizeof(out)) {
        return;
    }
    if (len >= tail && strcmp(path + len - tail, kTail) == 0) {
        memcpy(out, path, len - tail);
        strcpy(out + len - tail, "prngphase.bin");
    } else {
        memcpy(out, path, len);
        strcpy(out + len, ".phase");
    }

    HANDLE file = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    ReplayPhaseFileHeader header = {};
    memcpy(header.magic, "SH2RPHS", 8);
    header.version = 1;
    header.entrySize = sizeof(ReplayPhaseEntry);
    header.entryCount = s_phaseCount;
    header.epochTick = replayRecorderDataEpochTick();
    header.firstRelative = s_phaseRecord[0].relativeTick;
    header.lastRelative = s_phaseRecord[s_phaseCount - 1].relativeTick;
    header.droppedTicks = s_phaseDropped;
    DWORD written = 0;
    if (WriteFile(file, &header, sizeof(header), &written, NULL) &&
        written == sizeof(header)) {
        const uint32_t bytes = s_phaseCount * sizeof(ReplayPhaseEntry);
        WriteFile(file, s_phaseRecord, bytes, &written, NULL);
    }
    CloseHandle(file);
}

// Writes the round-2 attribution table beside the phase stream, as text: this
// is read by a human and by a one-off script, not by the diff tool.
static void prngSitesSave(const char *path) {
    if (!s_sitesInstalled) {
        return;
    }
    char out[544];
    const size_t len = strlen(path);
    static const char kTail[] = "prng.bin";
    const size_t tail = sizeof(kTail) - 1;
    if (len + 16 >= sizeof(out)) {
        return;
    }
    if (len >= tail && strcmp(path + len - tail, kTail) == 0) {
        memcpy(out, path, len - tail);
        strcpy(out + len - tail, "prngsites.txt");
    } else {
        memcpy(out, path, len);
        strcpy(out + len, ".sites");
    }
    HANDLE file = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    char line[256];
    DWORD written = 0;
    int n = _snprintf(line, sizeof(line),
                      "# in-tick RandomIter#1 draws by pooled-element vtable\r\n"
                      "# columns: vtableVA draws region hits visits; region 0=D loop1 1=D loop2 2=D loop3 3=E\r\n"
                      "# imageBase %08lx  buckets %lu  overflow %lu\r\n"
                      "# unattributed D1 %lu  D2 %lu  D3 %lu  E %lu  outside %lu\r\n",
                      (unsigned long)s_exeBase, (unsigned long)s_bucketCount,
                      (unsigned long)s_bucketOverflow,
                      (unsigned long)s_unattributed[RGN_D1],
                      (unsigned long)s_unattributed[RGN_D2],
                      (unsigned long)s_unattributed[RGN_D3],
                      (unsigned long)s_unattributed[RGN_E],
                      (unsigned long)s_unattributed[RGN_OUTSIDE]);
    if (n > 0) {
        WriteFile(file, line, (DWORD)n, &written, NULL);
    }
    for (uint32_t i = 0; i < s_bucketCount; ++i) {
        n = _snprintf(line, sizeof(line), "%08lx\t%lu\t%lu\t%lu\t%lu\r\n",
                      (unsigned long)(s_buckets[i].vtable - s_exeBase + 0x400000),
                      (unsigned long)s_buckets[i].draws,
                      (unsigned long)s_buckets[i].region,
                      (unsigned long)s_buckets[i].hits,
                      (unsigned long)s_buckets[i].visits);
        if (n > 0) {
            WriteFile(file, line, (DWORD)n, &written, NULL);
        }
    }
    CloseHandle(file);

    // Per-tick log beside it: "prngsites.txt" -> "prngsitelog.txt".
    const size_t olen = strlen(out);
    if (olen >= 4 && strcmp(out + olen - 4, ".txt") == 0) {
        strcpy(out + olen - 4, "log.txt");
    } else {
        strcat(out, ".log");
    }
    file = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    n = _snprintf(line, sizeof(line),
                  "# per-tick in-tick RandomIter#1 draws: relTick key draws; key = static vtable VA,"
                  " or f000000r = unattributed region r\r\n# entries %lu dropped %lu\r\n",
                  (unsigned long)s_siteLogCount, (unsigned long)s_siteLogDropped);
    if (n > 0) {
        WriteFile(file, line, (DWORD)n, &written, NULL);
    }
    for (uint32_t i = 0; i < s_siteLogCount; ++i) {
        n = _snprintf(line, sizeof(line), "%lu\t%08lx\t%lu\r\n",
                      (unsigned long)s_siteLog[i].relative, (unsigned long)s_siteLog[i].key,
                      (unsigned long)s_siteLog[i].draws);
        if (n > 0) {
            WriteFile(file, line, (DWORD)n, &written, NULL);
        }
    }
    CloseHandle(file);
}

static bool prngStepsSave(const char *path) {
    char sibling[512];
    const size_t pathLength = strlen(path);
    if (pathLength >= sizeof(sibling)) return false;
    memcpy(sibling, path, pathLength + 1);
    size_t len = pathLength;
    while (len && sibling[len - 1] != '\\' && sibling[len - 1] != '/') --len;
    static const char name[] = "prngsteps.bin";
    if (len + sizeof(name) > sizeof(sibling)) return false;
    memcpy(sibling + len, name, sizeof(name));
    HANDLE file = CreateFileA(sibling, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    PrngStepHeader header = {};
    memcpy(header.magic, "SH2RSTEP", 8);
    header.version = 1;
    header.entrySize = sizeof(PrngStepEntry);
    header.count = s_stepCount;
    header.epoch = replayRecorderDataEpochTick();
    header.dropped = s_stepDropped;
    DWORD written = 0;
    const uint32_t bytes = s_stepCount * sizeof(PrngStepEntry);
    bool ok = WriteFile(file, &header, sizeof(header), &written, NULL) && written == sizeof(header);
    ok = ok && (!bytes || (WriteFile(file, s_steps, bytes, &written, NULL) && written == bytes));
    CloseHandle(file);
    return ok;
}

uint32_t prngGuardSave(const char *path) {
    if (!path || !*path || !s_recordCount) {
        return 0;
    }
    prngPhaseSave(path);
    prngSitesSave(path);
    if (!prngStepsSave(path)) return 0;
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }

    ReplayPrngFileHeader header = {};
    memcpy(header.magic, "SH2RPRN", 8);
    header.version = 4;
    header.entrySize = sizeof(ReplayPrngEntry);
    header.entryCount = s_recordCount;
    header.epochTick = replayRecorderDataEpochTick();
    header.firstRelative = s_record[0].relativeTick;
    header.lastRelative = s_record[s_recordCount - 1].relativeTick;
    header.droppedTicks = s_dropped;

    DWORD written = 0;
    uint32_t total = 0;
    if (WriteFile(file, &header, sizeof(header), &written, NULL) &&
        written == sizeof(header)) {
        total += written;
        const uint32_t bytes = s_recordCount * sizeof(ReplayPrngEntry);
        if (WriteFile(file, s_record, bytes, &written, NULL) && written == bytes &&
            WriteFile(file, s_recordGates, s_recordCount, &written, NULL) &&
            written == s_recordCount) {
            total += bytes + s_recordCount;
            const uint32_t footer[2] = {REPLAY_CLOCK_MAGIC, s_recordClockCount};
            const uint32_t clockBytes = s_recordClockCount * sizeof(ReplayMonasteryClock);
            if (WriteFile(file, footer, sizeof(footer), &written, NULL) && written == sizeof(footer) &&
                (!clockBytes || (WriteFile(file, s_recordClocks, clockBytes, &written, NULL) && written == clockBytes))) {
                total += sizeof(footer) + clockBytes;
            } else total = 0;
        } else {
            total = 0;
        }
    }
    CloseHandle(file);
    return total;
}

static uint32_t armStream(const ReplayPrngFileHeader &header);

uint32_t prngGuardLoad(const char *path) {
    if (!path || !*path) {
        return 0;
    }
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }

    const DWORD bytes = GetFileSize(file, nullptr);
    const uint32_t maximum = sizeof(ReplayPrngFileHeader) + MAX_PRNG_TICKS * 13 +
                             8 + REPLAY_CLOCK_MAX * sizeof(ReplayMonasteryClock);
    void *buffer = bytes >= sizeof(ReplayPrngFileHeader) && bytes <= maximum ?
                   HeapAlloc(GetProcessHeap(), 0, bytes) : nullptr;
    DWORD got = 0;
    const bool ok = buffer && ReadFile(file, buffer, bytes, &got, nullptr) && got == bytes;
    CloseHandle(file);
    const uint32_t result = ok ? prngGuardLoadBytes(buffer, bytes) : 0;
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    if (!result) prngGuardControl(2);
    return result;
}

uint32_t prngGuardLoadBytes(const void *data, uint32_t bytes) {
    prngGuardControl(2);
    ReplayPrngFileHeader header = {};
    if (!data || bytes < sizeof(header)) {
        return 0;
    }
    memcpy(&header, data, sizeof(header));
    const uint32_t gateBytes = header.version >= 3 ? header.entryCount : 0;
    const bool ok = memcmp(header.magic, "SH2RPRN", 8) == 0 &&
                    (header.version >= 2 && header.version <= 4) &&
                    header.entrySize == sizeof(ReplayPrngEntry) && header.entryCount > 0 &&
                    header.entryCount <= MAX_PRNG_TICKS && header.droppedTicks == 0 &&
                    bytes - sizeof(header) >=
                        header.entryCount * sizeof(ReplayPrngEntry) + gateBytes;
    if (!ok) {
        s_streamCount = 0;
        s_streamHasGates = false;
        return 0;
    }
    const uint8_t *entries = (const uint8_t *)data + sizeof(header);
    const uint32_t end = sizeof(header) + header.entryCount * sizeof(ReplayPrngEntry) + gateBytes;
    if (header.version == 4) {
        uint32_t footer[2] = {};
        if (bytes - end < sizeof(footer)) return 0;
        memcpy(footer, (const uint8_t *)data + end, sizeof(footer));
        if (footer[0] != REPLAY_CLOCK_MAGIC || footer[1] > REPLAY_CLOCK_MAX ||
            bytes - end - sizeof(footer) != footer[1] * sizeof(ReplayMonasteryClock)) return 0;
        s_streamClockCount = footer[1];
        memcpy(s_streamClocks, (const uint8_t *)data + end + sizeof(footer),
               s_streamClockCount * sizeof(ReplayMonasteryClock));
        // Duplicate/unsorted identities cannot silently write the same clock twice.
        for (uint32_t i = 1; i < s_streamClockCount; ++i) {
            if (s_streamClocks[i - 1].slot >= s_streamClocks[i].slot) {
                s_streamClockCount = 0;
                return 0;
            }
        }
        s_clocksPending = true;
    } else if (bytes != end) return 0;
    memcpy(s_stream, entries, header.entryCount * sizeof(ReplayPrngEntry));
    s_streamHasGates = gateBytes != 0;
    if (s_streamHasGates) {
        memcpy(s_streamGates, entries + header.entryCount * sizeof(ReplayPrngEntry), gateBytes);
    }
    const uint32_t result = armStream(header);
    if (!result) prngGuardControl(2);
    return result;
}

void prngGuardRecordView(const ReplayPrngEntry **entries, uint32_t *count,
                         uint32_t *dropped) {
    *entries = s_record;
    *count = s_recordCount;
    *dropped = s_dropped;
}

void prngGuardRecordClocks(const ReplayMonasteryClock **entries, uint32_t *count) {
    *entries = s_recordClocks;
    *count = s_recordClockCount;
}

const uint8_t *prngGuardRecordGates() {
    return s_recordGates;
}

void prngGuardGateStats(uint32_t *applied, uint32_t *overflow, bool *streamHasGates) {
    *applied = s_gatesApplied;
    *overflow = s_gateOverflow;
    *streamHasGates = s_streamHasGates;
}

static uint32_t armStream(const ReplayPrngFileHeader &header) {
    // The stream has to be strictly increasing in relative tick, or the
    // forward-only walk in the entry hook would restore out of order.
    for (uint32_t i = 1; i < header.entryCount; ++i) {
        if (s_stream[i].relativeTick <= s_stream[i - 1].relativeTick) {
            s_streamCount = 0;
            return 0;
        }
    }

    s_streamCount = header.entryCount;
    s_streamEpoch = header.epochTick;
    s_streamPos = 0;
    s_gatePos = 0;
    s_gateHaveRel = false;
    s_curGatesValid = false;
    s_gatesApplied = 0;
    s_applied = 0;
    s_missing = 0;
    s_outsideWindow = 0;
    s_skipped = 0;
    s_preApplyMismatch = 0;
    s_worstDelta = 0;
    s_iter0Applied = 0;
    s_iter0PreMismatch = 0;
    // A stream is loaded before its match starts, so any duplicate-tick memory
    // belongs to an earlier match in this process.
    s_haveLastRelative = false;
    s_applying = true;
    return s_streamCount;
}
