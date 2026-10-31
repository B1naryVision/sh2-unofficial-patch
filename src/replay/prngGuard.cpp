#include "prngGuard.h"

#include "../core/exeBase.h"
#include "../core/hook.h"
#include "../core/readableMemory.h"
#include "replayFormat.h"
#include "replayPlayer.h"
#include "replayRecorder.h"
#include "replayTrace.h"

#include <cstring>
#include <windows.h>

// Ghidra VAs use ImageBase 0x400000; runtime addresses are module base + RVA.
static const uintptr_t SIM_TICK_ENTRY_RVA = 0x36aca0;
static const uintptr_t SIM_TICK_DONE_RVA = 0x36acff;
static const uintptr_t SIM_END_RVA = 0x36acf5; // mov ecx, <emitter manager>
static const uintptr_t SIM_END_IMM_RVA = 0x6e88c8; // the emitter manager (FUN_0076a610)
static const uintptr_t FX_FLAG_RVA = 0xc800b0;
static const uintptr_t GAME_PTR_RVA = 0x6e8bcc;
// Monastery's prayer-stage clock is neither constructed nor serialized by the
// stock game (ctor 0x4739b0, serializer 0x4740a0). Heap leftovers can therefore
// trigger a simulation RNG draw at a different update after the same save load.
// Live play zeroes it itself on entering stage 2 (0x474339) before any read, so
// the store is only needed while a replay loads and is skipped otherwise.
static const uintptr_t MONASTERY_CLOCK_RVA = 0x739ba;
static const uint8_t MONASTERY_CLOCK_STOCK[] = {0x89, 0x86, 0x60, 0x01, 0x00, 0x00};
static const uintptr_t MONASTERY_VTABLE_RVA = 0x59e904;
static const uintptr_t BUILDING_MGR_RVA = 0x6e5f40;

static const uintptr_t GAME_RANDOM_OFF = 0x70;
static const uintptr_t ITER1_TABLE_OFF = 0x4007c;
static const uintptr_t ITER1_CURSOR_OFF = 0x40080;
static const uintptr_t ITER0_TABLE_OFF = 0x40074;
static const uintptr_t ITER0_CURSOR_OFF = 0x40078;

// fld dword ptr [ecx+8] ; inc dword ptr [ecx+0xc]
static const uint8_t ENTRY_STOCK[] = {0xd9, 0x41, 0x08, 0xff, 0x41, 0x0c};
// cmp dword ptr [<fx flag>], 0 and mov ecx, <emitter manager>: both operands
// are absolute, so the expected bytes are rebuilt from the live base.
static const size_t DONE_STOCK_LEN = 7;
static const size_t SIM_END_STOCK_LEN = 5;

// Never wraps: a stream that silently lost its oldest ticks would restore the
// wrong values and still look complete.
static const uint32_t MAX_PRNG_TICKS = REPLAY_PRNG_CAPACITY;

// Recording. Allocated by prngGuardAcquireRecord.
static ReplayPrngEntry *s_record = nullptr;
static uint8_t *s_recordGates = nullptr;
static uint32_t s_recordCount = 0;
static uint32_t s_dropped = 0;
static ReplayMonasteryClock s_recordClocks[REPLAY_CLOCK_MAX];
static uint32_t s_recordClockCount = 0;
static bool s_clocksCaptured = false;

// Playback. Allocated by prngGuardLoadBytes.
static ReplayPrngEntry *s_stream = nullptr;
static uint8_t *s_streamGates = nullptr;
static uint32_t s_streamCount = 0;
static uint32_t s_streamPos = 0;
static bool s_streamHasGates = false;
static bool s_applying = false;
static ReplayMonasteryClock s_streamClocks[REPLAY_CLOCK_MAX];
static uint32_t s_streamClockCount = 0;
static bool s_clocksPending = false;

// Render gates. Simulation code that draws iterator 1 only when something was
// RENDERED since the last update: the render state differs between a
// recording and any playback, so the number of in-tick draws did too, and the
// tick-entry restore cannot cover a draw made inside the tick. The gate inputs
// are captured at the entry of every simulation update while recording and
// written back at the same point during playback. Rendering only happens
// between updates, so a gate's value at update entry is the whole render-side
// input to that update.
//
//   Sheep: draw method vt+0x5c (FUN_0052fbc0) sets the byte below to 1; the
//   in-tick bleat check FUN_0052fd40 decrements the global bleat cooldown only
//   when it is set (and clears it), then draws iterator 1 at cooldown 0.
//
// One byte per job entry: bit k = the Sheep flag at the entry of the job's
// k-th simulation update (MP runs about five per job). Updates past the 8th in
// one job are neither captured nor restored.
static const uintptr_t SHEEP_DRAWN_FLAG_RVA = 0xeb6b08; // VA 0x12b6b08, byte
static uint32_t s_gatePos = 0; // playback scan position in s_stream
static bool s_gateHaveRel = false;
static uint32_t s_gateRel = 0, s_gateSub = 0;
static uint8_t s_pendingGates = 0; // recording: bits of the job not yet appended
static bool s_curGatesValid = false; // playback: s_curGates belongs to s_gateRel
static uint8_t s_curGates = 0;

static uintptr_t s_exeBase = 0;
static bool s_installed = false;

// The job currently between the entry and SIM_DONE hooks.
static bool s_openTick = false;
static uint32_t s_openRelative = 0;
static uint16_t s_openStart = 0;
static uint16_t s_openStart0 = 0;
static uint16_t s_simEndSample = 0;
static bool s_haveLastRelative = false;
static uint32_t s_lastRelative = 0;

// Trampoline state must have external linkage: MinGW GAS cannot resolve local
// static symbols named from naked inline assembly.
uintptr_t g_prngGuardEntryReturn = 0;
uintptr_t g_prngGuardDoneReturn = 0;
uintptr_t g_prngGuardFxFlag = 0;
uintptr_t g_prngSimEndEcx = 0;
uintptr_t g_prngSimEndReturn = 0;
uintptr_t g_monasteryClockReturn = 0;

// Resolves a live iterator cursor, validating that its RandomIter points at the
// Game's own Dragonfly::Random. A failure means the Game is not constructed or
// the layout assumption is wrong, and the only safe response is to do nothing.
static uint16_t *iterCursor(uintptr_t tableOff, uintptr_t cursorOff) {
    const uintptr_t game = *(volatile uintptr_t *)(s_exeBase + GAME_PTR_RVA);

    if (!game) {
        return NULL;
    }

    const uintptr_t table = *(volatile uintptr_t *)(game + tableOff);

    if (table != game + GAME_RANDOM_OFF) {
        return NULL;
    }

    return (uint16_t *)(game + cursorOff);
}

static uint16_t *iter1Cursor() { return iterCursor(ITER1_TABLE_OFF, ITER1_CURSOR_OFF); }

static uint16_t *iter0Cursor() { return iterCursor(ITER0_TABLE_OFF, ITER0_CURSOR_OFF); }

// The epoch the relative clock is measured from: the recording's checkpoint
// tick while recording, the playback's own first in-match tick while a stream
// is loaded, otherwise none and every hook returns at once.
static uint32_t activeEpoch() {
    if (replayRecorderIsRecording()) {
        return replayRecorderEpochTick();
    }

    if (s_applying) {
        return replayPlayerPlaybackEpoch();
    }

    return 0;
}

static bool buildingSlots(const uintptr_t **slots, uint32_t *count) {
    const uintptr_t *header = (const uintptr_t *)(s_exeBase + BUILDING_MGR_RVA + 8);

    if (!observerReadable(header, 8) || header[1] < header[0] || (header[1] - header[0]) % 4 ||
        (header[1] - header[0]) / 4 > 65536) {
        return false;
    }

    *count = (uint32_t)((header[1] - header[0]) / 4);
    *slots = (const uintptr_t *)header[0];
    return !*count || observerReadable(*slots, *count * 4);
}

static void epochClocks(bool recording) {
    if (!s_clocksPending && (s_clocksCaptured || !recording)) {
        return;
    }

    const uintptr_t *slots = nullptr;
    uint32_t count = 0;

    if (!buildingSlots(&slots, &count)) {
        if (recording && !s_clocksCaptured) {
            ++s_dropped;
        }

        return;
    }

    if (s_clocksPending) {
        for (uint32_t i = 0; i < s_streamClockCount; ++i) {
            const ReplayMonasteryClock &e = s_streamClocks[i];
            const uintptr_t b = e.slot < count ? slots[e.slot] : 0;

            if (b && observerReadable(b, 0x168) &&
                *(const uintptr_t *)b == s_exeBase + MONASTERY_VTABLE_RVA &&
                *(const uint32_t *)(b + 0x9c) == ((uint32_t)e.serial << 16 | e.slot)) {
                *(uint32_t *)(b + 0x164) = e.clock;
            }
        }

        s_clocksPending = false;
    }

    if (!s_clocksCaptured && recording) {
        s_clocksCaptured = true;
        s_recordClockCount = 0;

        for (uint32_t i = 0; i < count; ++i) {
            const uintptr_t b = slots[i];

            if (!b || !observerReadable(b, 0x168) ||
                *(const uintptr_t *)b != s_exeBase + MONASTERY_VTABLE_RVA) {
                continue;
            }

            if (s_recordClockCount == REPLAY_CLOCK_MAX || *(const uint16_t *)(b + 0x9c) != i) {
                ++s_dropped;
                return;
            }

            s_recordClocks[s_recordClockCount++] = {
                (uint16_t)i, *(const uint16_t *)(b + 0x9e), *(const uint32_t *)(b + 0x164)
            };
        }
    }
}

// Every simulation update, duplicates of a job included.
static void renderGates(uint32_t relative, bool recording) {
    const uint32_t sub = (s_gateHaveRel && relative == s_gateRel) ? s_gateSub + 1 : 0;
    s_gateHaveRel = true;
    s_gateRel = relative;
    s_gateSub = sub;

    if (sub >= 8) {
        return;
    }

    volatile uint8_t *flag = (volatile uint8_t *)(s_exeBase + SHEEP_DRAWN_FLAG_RVA);

    if (recording) {
        const uint8_t bit = (uint8_t)((*flag ? 1u : 0u) << sub);

        if (sub == 0) {
            s_pendingGates = bit; // appended with the job's entry at SIM_DONE
        } else if (s_recordCount && s_record[s_recordCount - 1].relativeTick == relative) {
            s_recordGates[s_recordCount - 1] |= bit;
        }
    }

    if (s_applying && s_streamHasGates) {
        if (sub == 0) {
            while (s_gatePos < s_streamCount && s_stream[s_gatePos].relativeTick < relative) {
                ++s_gatePos;
            }

            s_curGatesValid =
                s_gatePos < s_streamCount && s_stream[s_gatePos].relativeTick == relative;

            if (s_curGatesValid) {
                s_curGates = s_streamGates[s_gatePos];
            }
        }

        if (s_curGatesValid) {
            *flag = (uint8_t)((s_curGates >> sub) & 1u);
        }
    }
}

extern "C" void prngGuardTickEntry() {
    const uint32_t epoch = activeEpoch();

    if (!epoch) {
        return;
    }

    const uint32_t now = replayTraceCurrentSimulationTick();

    if (now < epoch) {
        return;
    }

    const uint32_t relative = now - epoch;
    const bool recording = replayRecorderIsRecording() && s_record;
    epochClocks(recording);
    renderGates(relative, recording);

    // One capture and at most one restore per relative job. MP normally runs
    // five simulation updates per job (one at byte-counter rollover); restoring
    // again would rewind the cursor into the job's own draws.
    if (s_haveLastRelative && relative == s_lastRelative) {
        return;
    }

    s_haveLastRelative = true;
    s_lastRelative = relative;
    uint16_t *cursor = iter1Cursor();

    if (!cursor) {
        s_openTick = false;
        return;
    }

    if (s_applying) {
        while (s_streamPos < s_streamCount && s_stream[s_streamPos].relativeTick < relative) {
            ++s_streamPos;
        }

        if (s_streamPos < s_streamCount && s_stream[s_streamPos].relativeTick == relative) {
            *cursor = s_stream[s_streamPos].startCursor;
            ++s_streamPos;
        }
    }

    uint16_t *cursor0 = iter0Cursor();
    s_openStart0 = cursor0 ? *cursor0 : 0;
    s_openTick = true;
    s_openRelative = relative;
    s_openStart = *cursor;
    // A job that never reaches SIM_END must not inherit the previous sample.
    s_simEndSample = s_openStart;
}

extern "C" void prngGuardSimEnd() {
    if (!s_openTick) {
        return;
    }

    const uint16_t *cursor = iter1Cursor();

    if (cursor) {
        s_simEndSample = *cursor;
    }
}

// The live cursor is left untouched; the next update's entry restore handles it.
extern "C" void prngGuardSimDone() {
    if (!s_openTick) {
        return;
    }

    s_openTick = false;

    if (!replayRecorderIsRecording() || !s_record) {
        return;
    }

    if (s_recordCount >= MAX_PRNG_TICKS) {
        ++s_dropped;
        return;
    }

    const uint16_t *cursor0 = iter0Cursor();
    s_recordGates[s_recordCount] = s_pendingGates;
    ReplayPrngEntry &e = s_record[s_recordCount];
    e.relativeTick = s_openRelative;
    e.startCursor = s_openStart;
    e.simEndCursor = s_simEndSample;
    e.startCursor0 = s_openStart0;
    e.simEndCursor0 = cursor0 ? *cursor0 : 0;
    // Published after the entry, for the crash filter's reader.
    __atomic_store_n(&s_recordCount, s_recordCount + 1, __ATOMIC_RELEASE);
}

// Re-emits `fld dword ptr [ecx+8]` and `inc dword ptr [ecx+0xc]`, both
// position-independent, then resumes at the site + 6.
__declspec(naked) static void prngGuardEntryHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "call _prngGuardTickEntry\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "flds 0x8(%ecx)\n\t"
                     "incl 0xc(%ecx)\n\t"
                     "jmp *_g_prngGuardEntryReturn\n\t");
}

// Re-emits `mov ecx, <emitter manager>` through the relocated immediate
// captured at install time.
__declspec(naked) static void prngGuardSimEndHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "call _prngGuardSimEnd\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "movl _g_prngSimEndEcx, %ecx\n\t"
                     "jmp *_g_prngSimEndReturn\n\t");
}

// Re-emits `cmp dword ptr [<fx flag>], 0` through a relocated pointer, because
// the stock instruction carries an absolute address. EAX is saved and restored
// around it, and `pop` does not disturb the flags the following `jz` reads.
__declspec(naked) static void prngGuardDoneHook() {
    __asm__ volatile("pushal\n\t"
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

extern "C" bool prngGuardZeroMonasteryClock() { return replayPlayerArmed(); }

// Only a replay being loaded may touch the clock: a live match, recorded or
// not, keeps the stock constructor exactly.
__declspec(naked) static void monasteryClockHook() {
    __asm__ volatile("movl %eax, 0x160(%esi)\n\t" // stock store; EAX was zeroed just before it
                     "pushfl\n\t"
                     "pushl %eax\n\t"
                     "pushl %ecx\n\t"
                     "pushl %edx\n\t"
                     "call _prngGuardZeroMonasteryClock\n\t"
                     "testb %al, %al\n\t"
                     "popl %edx\n\t"
                     "popl %ecx\n\t"
                     "popl %eax\n\t"
                     "jz 1f\n\t"
                     "movl %eax, 0x164(%esi)\n\t"
                     "1:\n\t"
                     "popfl\n\t"
                     "jmp *_g_monasteryClockReturn\n\t");
}

void installPrngGuard() {
    s_exeBase = exeBase();

    if (!s_exeBase) {
        return;
    }

    uint8_t *entry = (uint8_t *)(s_exeBase + SIM_TICK_ENTRY_RVA);
    uint8_t *done = (uint8_t *)(s_exeBase + SIM_TICK_DONE_RVA);
    uint8_t *simEnd = (uint8_t *)(s_exeBase + SIM_END_RVA);
    uint8_t *monastery = (uint8_t *)(s_exeBase + MONASTERY_CLOCK_RVA);

    const uintptr_t fxFlag = s_exeBase + FX_FLAG_RVA;
    uint8_t doneStock[DONE_STOCK_LEN];
    doneStock[0] = 0x83;
    doneStock[1] = 0x3d;
    memcpy(doneStock + 2, &fxFlag, 4);
    doneStock[6] = 0x00;

    const uintptr_t simEndImm = s_exeBase + SIM_END_IMM_RVA;
    uint8_t simEndStock[SIM_END_STOCK_LEN] = {0xb9};
    memcpy(simEndStock + 1, &simEndImm, 4);

    // All-or-nothing: an unknown executable keeps all four stock sites.
    if (memcmp(entry, ENTRY_STOCK, sizeof(ENTRY_STOCK)) != 0 ||
        memcmp(done, doneStock, DONE_STOCK_LEN) != 0 ||
        memcmp(simEnd, simEndStock, SIM_END_STOCK_LEN) != 0 ||
        memcmp(monastery, MONASTERY_CLOCK_STOCK, sizeof(MONASTERY_CLOCK_STOCK)) != 0) {
        return;
    }

    g_prngGuardEntryReturn = (uintptr_t)entry + sizeof(ENTRY_STOCK);
    g_prngGuardDoneReturn = (uintptr_t)done + DONE_STOCK_LEN;
    g_prngGuardFxFlag = fxFlag;
    g_prngSimEndEcx = simEndImm;
    g_prngSimEndReturn = (uintptr_t)simEnd + SIM_END_STOCK_LEN;
    g_monasteryClockReturn = (uintptr_t)monastery + sizeof(MONASTERY_CLOCK_STOCK);

    installHook(entry, reinterpret_cast<void *>(prngGuardEntryHook), sizeof(ENTRY_STOCK));
    installHook(done, reinterpret_cast<void *>(prngGuardDoneHook), DONE_STOCK_LEN);
    installHook(simEnd, reinterpret_cast<void *>(prngGuardSimEndHook), SIM_END_STOCK_LEN);
    installHook(
        monastery, reinterpret_cast<void *>(monasteryClockHook), sizeof(MONASTERY_CLOCK_STOCK)
    );
    s_installed = true;
}

bool prngGuardInstalled() { return s_installed; }

bool prngGuardAcquireRecord() {
    if (!s_record) {
        s_record = (ReplayPrngEntry *)VirtualAlloc(
            NULL, MAX_PRNG_TICKS * sizeof(ReplayPrngEntry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
        );
    }

    if (s_record && !s_recordGates) {
        s_recordGates =
            (uint8_t *)VirtualAlloc(NULL, MAX_PRNG_TICKS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    if (!s_record || !s_recordGates) {
        prngGuardReleaseRecord();
        return false;
    }

    return true;
}

void prngGuardResetRecord() {
    __atomic_store_n(&s_recordCount, 0u, __ATOMIC_RELEASE);
    s_recordClockCount = 0;
    s_clocksCaptured = false;
    s_dropped = 0;
    s_pendingGates = 0;
    s_gateHaveRel = false;
    s_haveLastRelative = false;
    s_openTick = false;
}

void prngGuardReleaseRecord() {
    prngGuardResetRecord();

    if (s_record) {
        VirtualFree(s_record, 0, MEM_RELEASE);
        s_record = nullptr;
    }

    if (s_recordGates) {
        VirtualFree(s_recordGates, 0, MEM_RELEASE);
        s_recordGates = nullptr;
    }
}

void prngGuardClearStream() {
    s_applying = false;
    s_streamCount = 0;
    s_streamPos = 0;
    s_streamHasGates = false;
    s_streamClockCount = 0;
    s_clocksPending = false;

    if (s_stream) {
        VirtualFree(s_stream, 0, MEM_RELEASE);
        s_stream = nullptr;
    }

    if (s_streamGates) {
        VirtualFree(s_streamGates, 0, MEM_RELEASE);
        s_streamGates = nullptr;
    }
}

uint32_t prngGuardLoadBytes(const void *data, uint32_t bytes) {
    prngGuardClearStream();
    ReplayPrngLayout layout;

    if (!replayValidatePrng(data, bytes, MAX_PRNG_TICKS, &layout)) {
        return 0;
    }

    const uint32_t count = layout.header.entryCount;
    s_stream = (ReplayPrngEntry *)VirtualAlloc(
        NULL, count * sizeof(ReplayPrngEntry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    );
    s_streamGates =
        layout.gates
            ? (uint8_t *)VirtualAlloc(NULL, count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
            : nullptr;

    if (!s_stream || (layout.gates && !s_streamGates)) {
        prngGuardClearStream();
        return 0;
    }

    memcpy(s_stream, layout.entries, count * sizeof(ReplayPrngEntry));

    if (layout.gates) {
        memcpy(s_streamGates, layout.gates, count);
    }

    memcpy(s_streamClocks, layout.clocks, layout.clockCount * sizeof(ReplayMonasteryClock));
    s_streamClockCount = layout.clockCount;
    s_clocksPending = layout.clockCount != 0;
    s_streamHasGates = layout.gates != nullptr;
    s_streamCount = count;
    s_streamPos = 0;
    s_gatePos = 0;
    s_gateHaveRel = false;
    s_curGatesValid = false;
    // A stream is loaded before its match starts, so any duplicate-tick memory
    // belongs to an earlier match in this process.
    s_haveLastRelative = false;
    s_applying = true;
    return s_streamCount;
}

void prngGuardRecordView(const ReplayPrngEntry **entries, uint32_t *count, uint32_t *dropped) {
    *entries = s_record;
    *count = __atomic_load_n(&s_recordCount, __ATOMIC_ACQUIRE);
    *dropped = s_dropped;
}

void prngGuardRecordClocks(const ReplayMonasteryClock **entries, uint32_t *count) {
    *entries = s_recordClocks;
    *count = s_recordClockCount;
}

const uint8_t *prngGuardRecordGates() { return s_recordGates; }
