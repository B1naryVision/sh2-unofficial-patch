#include "seekProbe.h"

#ifndef DEBUG

void installSeekProbe() {}

#else

#include "../core/exeBase.h"
#include "../core/frameTick.h"
#include "../core/hotkey.h"
#include "../core/observerContext.h"
#include "../patches/endgameStats/gameOffsets.h"
#include "replayCommon.h"
#include "replayPlayer.h"
#include "replaySession.h"
#include "replayViewer.h"
#include "spectatorGuard.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <windows.h>

static const int KEY_TURBO = VK_F8;
static const int KEY_KEYFRAME = VK_F9;
static const int KEY_RESTART = VK_F11;

static const uintptr_t GAME_PTR_RVA = 0x6e8bcc;
static const uintptr_t ITER0_CURSOR_OFF = 0x40078;
static const uintptr_t ITER1_CURSOR_OFF = 0x40080;
static const uintptr_t WORLD_TICK_RVA = 0x6e8bd4; // +0xc of the tick object 0xae8bc8
static const uintptr_t JOB_PHASE_RVA = 0x17e6a34; // NetGameMgr+4, the MP cadence byte
static const uintptr_t PACER_DEBT_RVA = 0xe4651c; // World+0xce34c
static const uintptr_t FX_FLAG_RVA = 0xc800b0; // particle system gate

// Turbo aims for frames of this length, so the window keeps pumping messages
// and the camera stays usable while hundreds of updates run per frame.
static const int64_t TURBO_FRAME_US = 100000;
static const uint32_t TURBO_N_START = 20;
static const uint32_t TURBO_N_MAX = 20000;
static const int64_t TURBO_REPORT_US = 2000000;
static const uint32_t LATE_VA_FRAMES = 300;

static FILE *s_log = nullptr;
static int64_t s_qpcFreq = 0;

static int64_t qpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static int64_t qpcMicros(int64_t delta) { return delta * 1000000 / s_qpcFreq; }

static uint32_t addressSpaceMb() {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);

    if (!GlobalMemoryStatusEx(&ms)) {
        return 0;
    }

    return (uint32_t)((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20);
}

static void logLine(const char *fmt, ...) {
    if (!s_log) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    fputc('\n', s_log);
    fflush(s_log);
}

static void openLog() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);

    if (!n || n >= MAX_PATH) {
        return;
    }

    char *slash = strrchr(path, '\\');

    if (!slash || (slash - path) + sizeof("\\seek_probe.txt") > MAX_PATH) {
        return;
    }

    strcpy(slash + 1, "seek_probe.txt");
    s_log = fopen(path, "a");
    SYSTEMTIME st;
    GetLocalTime(&st);
    logLine(
        "# seek probe session %04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond
    );
}

static uintptr_t gameObject() { return *(const volatile uintptr_t *)(exeBase() + GAME_PTR_RVA); }

static uint32_t readCursor(uintptr_t off) {
    const uintptr_t game = gameObject();

    if (!game) {
        return 0xffffffffu;
    }

    return *(const volatile uint16_t *)(game + off);
}

static void writeCursor(uintptr_t off, uint32_t value) {
    const uintptr_t game = gameObject();

    if (game && value <= 0xffff) {
        *(volatile uint16_t *)(game + off) = (uint16_t)value;
    }
}

// ── Per-job fingerprint (pump context: float-free, no OS calls) ────────────

struct JobPrint {
    uint32_t rel;
    uint32_t iter0;
    uint32_t hash;
    uint32_t worldTick;
};

static uint32_t worldTick() { return *(const volatile uint32_t *)(exeBase() + WORLD_TICK_RVA); }

static const uint32_t RING_SIZE = 16384;
static JobPrint s_ring[RING_SIZE];
static uint32_t s_ringWrite = 0;
static uint32_t s_ringRead = 0;

static uint32_t fnv(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        h = (h ^ ((v >> (i * 8)) & 0xff)) * 16777619u;
    }

    return h;
}

static const uintptr_t PRINT_FIELDS[] = {
    OFF_COLOR, OFF_HONOR,      OFF_TROOPS,    OFF_SIEGE,          OFF_CASTLE,
    OFF_GOLD,  OFF_POPULARITY, OFF_INC_TRADE, OFF_INC_TAX_CASTLE, OFF_INC_TAX_ESTATES,
};

// The job the next iterator-0 draws belong to: draws between the pump of job J
// and the pump of J + 1 are J's, matching what a `J` line's cursor covers.
static volatile uint32_t s_jobRel = 0;

static void observeJob(uint32_t rel) {
    s_jobRel = rel;
    const uintptr_t base = exeBase();
    const uintptr_t *table = playerTable(base);
    uint32_t h = 2166136261u;

    for (int slot = 0; slot < PLAYER_TABLE_SLOTS; ++slot) {
        const uintptr_t p = table[slot];

        if (!p) {
            continue;
        }

        h = fnv(h, (uint32_t)slot);

        for (uintptr_t off : PRINT_FIELDS) {
            h = fnv(h, *(const volatile uint32_t *)(p + off));
        }

        for (int k = 0; k < 8; ++k) {
            h = fnv(h, *(const volatile uint32_t *)(p + OFF_HON_BASE + 4 * k));
        }
    }

    JobPrint &e = s_ring[s_ringWrite % RING_SIZE];
    e.rel = rel;
    e.iter0 = readCursor(ITER0_CURSOR_OFF);
    e.hash = h;
    e.worldTick = worldTick();
    ++s_ringWrite;
}

// ── Runs ────────────────────────────────────────────────────────────────────

static uint32_t s_run = 0;
static uint32_t s_runEpoch = 0;
static int64_t s_restartQpc = 0; // a restart or load is waiting for its first frame
static uint32_t s_lateVaFrames = 0;

static void drainPrints() {
    if (s_ringWrite - s_ringRead > RING_SIZE) {
        logLine("DROPPED %u job prints", s_ringWrite - s_ringRead - RING_SIZE);
        s_ringRead = s_ringWrite - RING_SIZE;
    }

    for (; s_ringRead != s_ringWrite; ++s_ringRead) {
        const JobPrint &e = s_ring[s_ringRead % RING_SIZE];

        if (s_log) {
            fprintf(s_log, "J %u %u %u %08x %u\n", s_run, e.rel, e.iter0, e.hash, e.worldTick);
        }
    }
}

static void trackRuns() {
    const uint32_t epoch = replayPlayerPlaybackEpoch();

    if (epoch && epoch != s_runEpoch) {
        s_runEpoch = epoch;
        ++s_run;
        const wchar_t *path = replaySessionPlaybackPath();
        logLine(
            "RUN %u epoch=%u worldTick=%u phase=%u va=%uMB path=%ls", s_run, epoch, worldTick(),
            *(const volatile uint8_t *)(exeBase() + JOB_PHASE_RVA), addressSpaceMb(),
            path ? path : L"?"
        );

        if (s_restartQpc) {
            logLine(
                "RUN %u epoch reached %lld ms after the load call", s_run,
                (long long)(qpcMicros(qpcNow() - s_restartQpc) / 1000)
            );
            s_restartQpc = 0;
        }
    }

    if (s_lateVaFrames && --s_lateVaFrames == 0) {
        logLine("VA %u frames after the load: %uMB", LATE_VA_FRAMES, addressSpaceMb());
    }
}

// ── Iterator-0 write watch ──────────────────────────────────────────────────
//
// A hardware data breakpoint (DR0, write, 2 bytes) on the iterator-0 cursor of
// the game thread. Every draw traps once, after the write. A draw is "in the
// simulation" when the update loop's return address into FUN_00421e00 is on the
// stack; anything else is a draw the frame schedule can move between jobs.
// The debug registers are set from our own vectored handler, on an exception
// the frame tick raises for that purpose, so no second thread is involved.

static const int KEY_WATCH = VK_F7;
static const DWORD WATCH_ARM_CODE = 0xe05eec01;
static const uintptr_t LOOP_RET_TICK_RVA = 0x21eba; // after NetGameMgr::tick
static const uintptr_t LOOP_RET_UPDATE_RVA = 0x21ec7; // after S2Game::update
static const uintptr_t SINGLE_RET_TICK_RVA = 0x21efd; // the one-update path
static const uintptr_t TEXT_BEGIN_RVA = 0x1000;
static const uintptr_t TEXT_END_RVA = 0x500000;
static const uint32_t CHAIN_MAX = 8;
static const uintptr_t CHAIN_SCAN_BYTES = 0x4000;

enum DrawFlags : uint8_t {
    DRAW_IN_SIM = 1,
    DRAW_OUTSIDE_EXE = 2, // `site` is an absolute address, not an RVA
};

struct DrawEvent {
    uint32_t run;
    uint32_t rel;
    uint32_t worldTick;
    uint32_t site;
    uint8_t flags;
};

static const uint32_t DRAW_RING = 65536;
static DrawEvent s_draws[DRAW_RING];
static uint32_t s_drawWrite = 0;
static uint32_t s_drawRead = 0;

struct OutSite {
    uint32_t site;
    uint32_t chain[CHAIN_MAX];
    bool logged;
};

static const uint32_t OUT_SITES_MAX = 64;
static OutSite s_outSites[OUT_SITES_MAX];
static uint32_t s_outSiteCount = 0;

static bool s_watchOn = false;
static uintptr_t s_watchWant = 0; // address the next arm exception writes to DR0
static uintptr_t s_watchArmed = 0;
static PVOID s_veh = nullptr;

static bool isCallReturn(uintptr_t base, uintptr_t value) {
    if (value < base + TEXT_BEGIN_RVA + 6 || value >= base + TEXT_END_RVA) {
        return false;
    }

    const uint8_t *p = (const uint8_t *)value;
    return p[-5] == 0xe8 || p[-6] == 0xff || p[-3] == 0xff || p[-2] == 0xff;
}

static void noteOutSite(uintptr_t base, uint32_t site, uintptr_t esp, uintptr_t top) {
    for (uint32_t i = 0; i < s_outSiteCount; ++i) {
        if (s_outSites[i].site == site) {
            return;
        }
    }

    if (s_outSiteCount >= OUT_SITES_MAX) {
        return;
    }

    OutSite &o = s_outSites[s_outSiteCount++];
    o.site = site;
    o.logged = false;
    uint32_t n = 0;
    uintptr_t end = esp + CHAIN_SCAN_BYTES;

    if (end > top) {
        end = top;
    }

    for (uintptr_t at = esp; at + 4 <= end && n < CHAIN_MAX; at += 4) {
        const uintptr_t value = *(const uintptr_t *)at;

        if (isCallReturn(base, value)) {
            o.chain[n++] = (uint32_t)(value - base);
        }
    }

    for (; n < CHAIN_MAX; ++n) {
        o.chain[n] = 0;
    }
}

static void recordDraw(uintptr_t eip, uintptr_t esp) {
    const uintptr_t base = exeBase();
    const uintptr_t top = (uintptr_t)((const NT_TIB *)NtCurrentTeb())->StackBase;
    const uintptr_t r1 = base + LOOP_RET_TICK_RVA, r2 = base + LOOP_RET_UPDATE_RVA;
    const uintptr_t r3 = base + SINGLE_RET_TICK_RVA;
    uint8_t flags = 0;

    for (uintptr_t at = esp & ~(uintptr_t)3; at + 4 <= top; at += 4) {
        const uintptr_t value = *(const uintptr_t *)at;

        if (value == r1 || value == r2 || value == r3) {
            flags |= DRAW_IN_SIM;
            break;
        }
    }

    uint32_t site = (uint32_t)eip;

    if (eip >= base + TEXT_BEGIN_RVA && eip < base + TEXT_END_RVA) {
        site = (uint32_t)(eip - base);
    } else {
        flags |= DRAW_OUTSIDE_EXE;
    }

    if (!(flags & DRAW_IN_SIM)) {
        noteOutSite(base, site, esp, top);
    }

    DrawEvent &e = s_draws[s_drawWrite % DRAW_RING];
    e.run = s_run;
    e.rel = s_jobRel;
    e.worldTick = worldTick();
    e.site = site;
    e.flags = flags;
    ++s_drawWrite;
}

static LONG CALLBACK watchHandler(EXCEPTION_POINTERS *ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT *cx = ep->ContextRecord;

    if (code == WATCH_ARM_CODE) {
        cx->Dr0 = s_watchWant;
        cx->Dr6 = 0;
        // L0, RW0 = 01 (data write), LEN0 = 01 (2 bytes).
        cx->Dr7 = s_watchWant ? (1u | (1u << 16) | (1u << 18)) : 0;
        cx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code != EXCEPTION_SINGLE_STEP || !(cx->Dr6 & 1)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    cx->Dr6 = 0;
    recordDraw(cx->Eip, cx->Esp);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void armWatch(uintptr_t address) {
    if (!s_veh) {
        s_veh = AddVectoredExceptionHandler(1, watchHandler);

        if (!s_veh) {
            logLine("WATCH refused: no vectored handler");
            return;
        }
    }

    s_watchWant = address;
    RaiseException(WATCH_ARM_CODE, 0, 0, nullptr);
    s_watchArmed = address;
}

// Follows the Game object across loads, and drops the breakpoint whenever the
// watch is off or no playback is running.
static void maintainWatch() {
    uintptr_t want = 0;
    const uintptr_t game = gameObject();

    if (s_watchOn && game && spectatorGuardActive()) {
        want = game + ITER0_CURSOR_OFF;
    }

    if (want != s_watchArmed) {
        armWatch(want);
    }
}

static void drainDraws() {
    if (s_drawWrite - s_drawRead > DRAW_RING) {
        logLine("DROPPED %u draws", s_drawWrite - s_drawRead - DRAW_RING);
        s_drawRead = s_drawWrite - DRAW_RING;
    }

    for (; s_drawRead != s_drawWrite; ++s_drawRead) {
        const DrawEvent &e = s_draws[s_drawRead % DRAW_RING];

        if (s_log) {
            fprintf(s_log, "D %u %u %u %x %u\n", e.run, e.rel, e.worldTick, e.site, e.flags);
        }
    }

    for (uint32_t i = 0; i < s_outSiteCount; ++i) {
        OutSite &o = s_outSites[i];

        if (o.logged) {
            continue;
        }

        o.logged = true;
        logLine(
            "OUTSITE %x chain %x %x %x %x %x %x %x %x", o.site, o.chain[0], o.chain[1], o.chain[2],
            o.chain[3], o.chain[4], o.chain[5], o.chain[6], o.chain[7]
        );
    }
}

// ── Turbo ───────────────────────────────────────────────────────────────────

enum TurboMode : uint32_t {
    TURBO_OFF = 0,
    TURBO_ON = 1,
    TURBO_NO_FX = 2,
};

static uint32_t s_turbo = TURBO_OFF;
static uint32_t s_turboN = 0;
static int64_t s_lastFrameQpc = 0;
static bool s_fxSaved = false;
static uint32_t s_fxValue = 0;
static int64_t s_winStartQpc = 0, s_turboStartQpc = 0;
static uint32_t s_winStartRel = 0, s_turboStartRel = 0;
static uint64_t s_winUpdates = 0, s_turboUpdates = 0;
static uint32_t s_winFrames = 0;

static uint32_t currentRel() {
    uint32_t rel = 0;
    replayPlayerPlaybackJob(&rel);
    return rel;
}

static void stopTurbo(const char *why) {
    if (s_turbo == TURBO_OFF) {
        return;
    }

    const uintptr_t base = exeBase();
    replayViewerSetUpdateOverride(0);
    *(volatile int32_t *)(base + PACER_DEBT_RVA) = 0;

    if (s_fxSaved) {
        *(volatile uint32_t *)(base + FX_FLAG_RVA) = s_fxValue;
        s_fxSaved = false;
    }

    const int64_t us = qpcMicros(qpcNow() - s_turboStartQpc);
    const uint32_t jobs = currentRel() - s_turboStartRel;
    uint64_t jobsPerSec = 0;

    if (us > 0) {
        jobsPerSec = (uint64_t)jobs * 1000000 / (uint64_t)us;
    }

    logLine(
        "TURBO off (%s) rel=%u jobs=%u wall=%lldms jobs/s=%llu updatesSet=%llu", why, currentRel(),
        jobs, (long long)(us / 1000), (unsigned long long)jobsPerSec,
        (unsigned long long)s_turboUpdates
    );
    s_turbo = TURBO_OFF;
}

static void startTurbo(uint32_t mode) {
    const uintptr_t base = exeBase();
    s_turbo = mode;
    s_turboN = TURBO_N_START;
    s_lastFrameQpc = s_winStartQpc = s_turboStartQpc = qpcNow();
    s_winStartRel = s_turboStartRel = currentRel();
    s_winUpdates = s_turboUpdates = 0;
    s_winFrames = 0;

    if (mode == TURBO_NO_FX && !s_fxSaved) {
        s_fxValue = *(volatile uint32_t *)(base + FX_FLAG_RVA);
        s_fxSaved = true;
        *(volatile uint32_t *)(base + FX_FLAG_RVA) = 0;
    }

    replayViewerSetUpdateOverride(s_turboN);
    logLine("TURBO on fx=%d rel=%u", mode == TURBO_NO_FX ? 0 : 1, s_turboStartRel);
}

static void turboFrame() {
    if (s_turbo == TURBO_OFF) {
        return;
    }

    if (!spectatorGuardActive() || !replayInGame()) {
        stopTurbo("playback left");
        return;
    }

    if (replayViewerEnded()) {
        stopTurbo("end of recording");
        return;
    }

    const int64_t now = qpcNow();
    const int64_t frameUs = qpcMicros(now - s_lastFrameQpc);
    s_lastFrameQpc = now;

    if (replayViewerMenuOpen()) {
        return;
    }

    // The count set last frame is what ran in the frame that just ended.
    s_winUpdates += s_turboN;
    s_turboUpdates += s_turboN;
    ++s_winFrames;

    if (frameUs > 0) {
        uint64_t next = (uint64_t)s_turboN * (uint64_t)TURBO_FRAME_US / (uint64_t)frameUs;

        if (next > (uint64_t)s_turboN * 2) {
            next = (uint64_t)s_turboN * 2;
        }

        if (next < s_turboN / 2) {
            next = s_turboN / 2;
        }

        if (next < 1) {
            next = 1;
        }

        if (next > TURBO_N_MAX) {
            next = TURBO_N_MAX;
        }

        s_turboN = (uint32_t)next;
    }

    replayViewerSetUpdateOverride(s_turboN);
    const int64_t winUs = qpcMicros(now - s_winStartQpc);

    if (winUs < TURBO_REPORT_US) {
        return;
    }

    const uint32_t rel = currentRel();
    const uint32_t jobs = rel - s_winStartRel;
    logLine(
        "TURBO fx=%d rel=%u N=%u frames=%u wall=%lldms jobs=%u jobs/s=%llu updates/s=%llu "
        "frame=%lldus va=%uMB",
        s_turbo == TURBO_NO_FX ? 0 : 1, rel, s_turboN, s_winFrames, (long long)(winUs / 1000), jobs,
        (unsigned long long)((uint64_t)jobs * 1000000 / (uint64_t)winUs),
        (unsigned long long)(s_winUpdates * 1000000 / (uint64_t)winUs),
        (long long)(winUs / (s_winFrames ? s_winFrames : 1)), addressSpaceMb()
    );
    s_winStartQpc = now;
    s_winStartRel = rel;
    s_winUpdates = 0;
    s_winFrames = 0;
}

// ── Save and load probes ────────────────────────────────────────────────────

static wchar_t s_lastKeyframe[MAX_PATH] = L"";

static bool keyframePath(uint32_t rel, wchar_t *out, size_t cap) {
    wchar_t dir[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, dir);

    if (!n || n + 32 >= MAX_PATH) {
        return false;
    }

    wcscat(dir, L"sh2up-seekprobe");
    CreateDirectoryW(dir, NULL);
    _snwprintf(out, cap, L"%ls\\kf-%u.sh2", dir, rel);
    out[cap - 1] = 0;
    return true;
}

static uint32_t fileBytes(const wchar_t *path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        return 0;
    }

    return fa.nFileSizeLow;
}

static void saveKeyframe() {
    uint32_t rel = 0;

    if (!replayPlayerPlaybackJob(&rel) || replayScreenPopupOn()) {
        logLine("SAVE refused: no playback, or a popup is up");
        return;
    }

    wchar_t path[MAX_PATH];

    if (!keyframePath(rel, path, MAX_PATH)) {
        logLine("SAVE refused: no temp path");
        return;
    }

    const uint32_t queued = replayNetQueuedCommands();
    const uint32_t placements = replayPendingPlacements();
    const uint32_t vaBefore = addressSpaceMb();
    const uint32_t iter0Before = readCursor(ITER0_CURSOR_OFF);
    const uint32_t iter1Before = readCursor(ITER1_CURSOR_OFF);
    const int64_t t0 = qpcNow();
    const bool ok = replayCheckpointSave(path, 2);
    const int64_t us = qpcMicros(qpcNow() - t0);
    const uint32_t iter0After = readCursor(ITER0_CURSOR_OFF);
    const uint32_t iter1After = readCursor(ITER1_CURSOR_OFF);

    // Put both cursors back, so a later divergence cannot be the save's draws.
    if (iter0After != iter0Before) {
        writeCursor(ITER0_CURSOR_OFF, iter0Before);
    }

    if (iter1After != iter1Before) {
        writeCursor(ITER1_CURSOR_OFF, iter1Before);
    }

    logLine(
        "SAVE run=%u rel=%u ok=%d time=%lld.%03lldms bytes=%u queued=%u placements=%u "
        "iter0 %u->%u iter1 %u->%u va %uMB->%uMB path=%ls",
        s_run, rel, ok ? 1 : 0, (long long)(us / 1000), (long long)(us % 1000), fileBytes(path),
        queued, placements, iter0Before, iter0After, iter1Before, iter1After, vaBefore,
        addressSpaceMb(), path
    );

    if (ok) {
        memcpy(s_lastKeyframe, path, sizeof(s_lastKeyframe));
    }
}

// `rearms`: the load is a replay save, so a new run follows and the time to its
// epoch is logged too.
static void timedLoad(const char *what, const wchar_t *path, bool rearms) {
    stopTurbo("load");
    const uint32_t queuedBefore = replayNetQueuedCommands();
    const uint32_t placementsBefore = replayPendingPlacements();
    const uint32_t vaBefore = addressSpaceMb();
    const int64_t t0 = qpcNow();
    const bool ok = replayLoadSave(path);
    bool started = false;

    // Before this frame's update loop can run (see replayPlayerStartNow).
    if (ok && rearms) {
        started = replayPlayerStartNow();
    }

    const int64_t us = qpcMicros(qpcNow() - t0);
    logLine(
        "%s ok=%d started=%d time=%lld.%03lldms queued %u->%u placements %u->%u va %uMB->%uMB "
        "path=%ls",
        what, ok ? 1 : 0, started ? 1 : 0, (long long)(us / 1000), (long long)(us % 1000),
        queuedBefore, replayNetQueuedCommands(), placementsBefore, replayPendingPlacements(),
        vaBefore, addressSpaceMb(), path
    );

    if (ok) {
        s_restartQpc = rearms ? t0 : 0;
        s_lateVaFrames = LATE_VA_FRAMES;
    }
}

static void restartReplay(const char *what) {
    const wchar_t *playing = replaySessionPlaybackPath();

    if (!playing) {
        logLine("%s refused: no playback", what);
        return;
    }

    // The session rewrites its own copy inside the load.
    wchar_t path[MAX_PATH];
    memcpy(path, playing, sizeof(path));
    timedLoad(what, path, true);
}

// F11 waits for a frame with nothing in flight: no command queued for a later
// job and no deferred placement. Whatever is in flight when the world is
// replaced may survive into the new run.
static const uint32_t QUIET_WAIT_FRAMES = 600;
static bool s_quietRestartPending = false;
static uint32_t s_quietWaitFrames = 0;

static void quietRestartFrame() {
    if (!s_quietRestartPending) {
        return;
    }

    if (replayNetQueuedCommands() == 0 && replayPendingPlacements() == 0 &&
        !replayScreenPopupOn()) {
        s_quietRestartPending = false;
        logLine("RESTART after %u frames of waiting for a quiet job", s_quietWaitFrames);
        restartReplay("RESTART");
        return;
    }

    if (++s_quietWaitFrames >= QUIET_WAIT_FRAMES) {
        s_quietRestartPending = false;
        logLine(
            "RESTART gave up after %u frames: queued=%u placements=%u (paused?)", s_quietWaitFrames,
            replayNetQueuedCommands(), replayPendingPlacements()
        );
    }
}

static void loadKeyframe() {
    if (!s_lastKeyframe[0]) {
        logLine("LOADKF refused: no keyframe saved yet");
        return;
    }

    timedLoad("LOADKF", s_lastKeyframe, false);
}

// ── Frame tick ──────────────────────────────────────────────────────────────

static bool s_keyWasDown[256];

static bool pressed(int vk) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !s_keyWasDown[vk];
    s_keyWasDown[vk] = down;
    return edge;
}

static void probeTick() {
    // Formatting, file and engine calls below; the frame tick is mid-function.
    ObserverFpScope fp;

    if (!s_log) {
        openLog();
    }

    drainPrints();
    drainDraws();

    // So a copy taken mid-session never ends in a torn line.
    if (s_log) {
        fflush(s_log);
    }

    trackRuns();
    turboFrame();
    maintainWatch();
    const bool focused = gameWindowFocused();
    const bool watchKey = pressed(KEY_WATCH);
    const bool turboKey = pressed(KEY_TURBO);
    const bool keyframeKey = pressed(KEY_KEYFRAME);
    const bool restartKey = pressed(KEY_RESTART);

    if (!focused || !spectatorGuardActive() || !replayInGame()) {
        return;
    }

    if (watchKey) {
        s_watchOn = !s_watchOn;
        logLine("WATCH %s run=%u rel=%u", s_watchOn ? "on" : "off", s_run, currentRel());
        maintainWatch();
    }

    // F8 toggles turbo; Shift+F8 starts it without particles.
    if (turboKey) {
        if (s_turbo != TURBO_OFF) {
            stopTurbo("key");
        } else if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            startTurbo(TURBO_NO_FX);
        } else {
            startTurbo(TURBO_ON);
        }
    }

    if (keyframeKey) {
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            loadKeyframe();
        } else {
            saveKeyframe();
        }
    }

    // F11 restarts at the next quiet job; Shift+F11 at once (the old way).
    if (restartKey) {
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            restartReplay("RESTART-NOW");
        } else {
            s_quietRestartPending = true;
            s_quietWaitFrames = 0;
        }
    }

    quietRestartFrame();
}

void installSeekProbe() {
    LARGE_INTEGER f;

    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) {
        return;
    }

    s_qpcFreq = f.QuadPart;

    if (!replayPlayerInstalled() || !replayViewerInstalled()) {
        return;
    }

    replayPlayerSetJobObserver(observeJob);
    registerFrameTick(probeTick);
}

#endif
