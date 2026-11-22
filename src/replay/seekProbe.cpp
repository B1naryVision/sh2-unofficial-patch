#include "seekProbe.h"

#ifndef DEBUG

void installSeekProbe() {}

#else

#include "../core/exeBase.h"
#include "../core/frameTick.h"
#include "../core/hotkey.h"
#include "../core/observerContext.h"
#include "../core/readableMemory.h"
#include "../patches/endgameStats/gameOffsets.h"
#include "keyframeSerial.h"
#include "prngGuard.h"
#include "replayCommon.h"
#include "replayKeyframes.h"
#include "replayPlayer.h"
#include "replaySeeker.h"
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
static const int KEY_KEYFRAMES = VK_F10;

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

// A path for the log, as UTF-8: replay names carry player names, and a narrow
// %ls goes through the C locale and mangles anything outside ASCII.
static const char *utf8(const wchar_t *text) {
    static char buf[MAX_PATH * 3];

    if (!text || !WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, (int)sizeof(buf), NULL, NULL)) {
        return "?";
    }

    return buf;
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
    uint32_t frame; // probe frame counter when the job ran
    uint32_t dtUs; // since the previous job's pump
};

// Frames seen by the probe's frame tick. Jobs of one frame share a number, so
// the first job of a frame carries the frame's drawing and the rest only the
// simulation.
static uint32_t s_frame = 0;
static int64_t s_lastJobQpc = 0;

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

static void traceMarker(uint8_t kind, uint32_t cursor);

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
    traceMarker(1, e.iter0); // TRACE_PUMP
    e.hash = h;
    e.worldTick = worldTick();
    e.frame = s_frame;
    // QueryPerformanceCounter reads the clock without touching the FPU.
    const int64_t now = qpcNow();
    const int64_t dt = s_lastJobQpc ? qpcMicros(now - s_lastJobQpc) : 0;
    e.dtUs = dt > 0xffffffffll ? 0xffffffffu : (uint32_t)dt;
    s_lastJobQpc = now;
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
            fprintf(
                s_log, "J %u %u %u %08x %u %u %u\n", s_run, e.rel, e.iter0, e.hash, e.worldTick,
                e.frame, e.dtUs
            );
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
            "RUN %u epoch=%u worldTick=%u phase=%u va=%uMB path=%s", s_run, epoch, worldTick(),
            *(const volatile uint8_t *)(exeBase() + JOB_PHASE_RVA), addressSpaceMb(), utf8(path)
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

// The playback check (prngGuard) and the seeker's repairs, logged when they
// change: an exact playback reports no mismatch over every job it checked.
static uint32_t s_verifyLoggedJob = 0xffffffffu, s_verifyLoggedRepairs = 0;
static uint32_t currentRel();

static void verifyLog() {
    uint32_t job = 0, checked = 0;
    const bool mismatch = prngGuardVerifyMismatch(&job, &checked);
    const uint32_t repairs = replaySeekerRepairs();

    if (mismatch && job != s_verifyLoggedJob) {
        s_verifyLoggedJob = job;
        logLine("VERIFY mismatch run=%u job=%u checked=%u", s_run, job, checked);
    }

    if (!mismatch) {
        s_verifyLoggedJob = 0xffffffffu;
    }

    if (repairs != s_verifyLoggedRepairs) {
        s_verifyLoggedRepairs = repairs;
        logLine("VERIFY repair run=%u repairs=%u rel=%u", s_run, repairs, currentRel());
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

    // The player's skip owns the same update override.
    if (replaySeekerActive(nullptr)) {
        stopTurbo("skip");
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

// ── Keyframes ───────────────────────────────────────────────────────────────

static uint32_t s_keyframesLogged = 0;

static void keyframeLog() {
    uint32_t captures = 0, job = 0, saveUs = 0, bytes = 0, handles = 0;
    replayKeyframesDebugLast(&captures, &job, &saveUs, &bytes, &handles);

    if (captures == s_keyframesLogged) {
        return;
    }

    s_keyframesLogged = captures;
    logLine(
        "KEYFRAME run=%u job=%u save=%u.%03ums bytes=%u handles=%u path=%s va=%uMB", s_run, job,
        saveUs / 1000, saveUs % 1000, bytes, handles, keyframeSerialRawPath() ? "raw" : "stock",
        addressSpaceMb()
    );
}

// ── Skips ───────────────────────────────────────────────────────────────────
//
// The player's skip (replaySeeker), observed: how fast it runs, every 2 s and in
// total.

static bool s_skipSeen = false;
static int64_t s_skipStartQpc = 0, s_skipWinQpc = 0;
static uint32_t s_skipRun = 0; // the run the skip's counts belong to
static uint64_t s_skipWinSubsystems = 0, s_skipWinEmitters = 0;
static uint32_t s_skipWinUpdates = 0;

// Of the window's wall time: the tick body's subsystems, the emitter manager,
// and the update count; the rest of the frame is everything else (the world
// update outside the tick body, drawing, the window).
static void skipWindowStart() {
    prngGuardDebugTimes(&s_skipWinSubsystems, &s_skipWinEmitters, &s_skipWinUpdates);
}
static uint32_t s_skipStartRel = 0, s_skipWinRel = 0, s_skipWinFrames = 0;

static int particleGate() { return *(volatile uint32_t *)(exeBase() + FX_FLAG_RVA) ? 1 : 0; }

static void skipFrame() {
    uint32_t target = 0;
    const bool active = replaySeekerActive(&target);
    const int64_t now = qpcNow();
    const uint32_t rel = currentRel();

    if (active && !s_skipSeen) {
        s_skipSeen = true;
        s_skipStartQpc = s_skipWinQpc = now;
        s_skipStartRel = s_skipWinRel = rel;
        s_skipWinFrames = 0;
        s_skipRun = s_run;
        skipWindowStart();
        logLine(
            "SKIP start run=%u rel=%u target=%u%s fx=%d", s_run, rel, target,
            target < rel ? " (back)" : "", particleGate()
        );
        return;
    }

    // A rewind reloaded the replay: a new run, counting from its job 0. The
    // time until here is the wait for a quiet job plus the load.
    if (active && s_run != s_skipRun) {
        logLine(
            "SKIP rewind run=%u reloaded after %lldms, now to target=%u", s_run,
            (long long)(qpcMicros(now - s_skipStartQpc) / 1000), target
        );
        s_skipRun = s_run;
        s_skipStartQpc = s_skipWinQpc = now;
        s_skipStartRel = s_skipWinRel = rel;
        s_skipWinFrames = 0;
        skipWindowStart();
        return;
    }

    if (!active && s_skipSeen) {
        s_skipSeen = false;
        const int64_t us = qpcMicros(now - s_skipStartQpc);
        uint32_t jobs = 0;

        // A rewind to 0:00 ends at the reload, before any new run is seen.
        if (s_run == s_skipRun && rel >= s_skipStartRel) {
            jobs = rel - s_skipStartRel;
        }
        uint64_t perSec = 0;

        if (us > 0) {
            perSec = (uint64_t)jobs * 1000000 / (uint64_t)us;
        }

        logLine(
            "SKIP end run=%u rel=%u jobs=%u wall=%lldms jobs/s=%llu", s_run, rel, jobs,
            (long long)(us / 1000), (unsigned long long)perSec
        );
        return;
    }

    if (!active) {
        return;
    }

    ++s_skipWinFrames;
    const int64_t winUs = qpcMicros(now - s_skipWinQpc);

    if (winUs < TURBO_REPORT_US) {
        return;
    }

    const uint32_t jobs = rel - s_skipWinRel;
    uint64_t subsystems = 0, emitters = 0;
    uint32_t updates = 0;
    prngGuardDebugTimes(&subsystems, &emitters, &updates);
    const uint64_t subUs = (uint64_t)qpcMicros((int64_t)(subsystems - s_skipWinSubsystems));
    const uint64_t emitUs = (uint64_t)qpcMicros((int64_t)(emitters - s_skipWinEmitters));
    logLine(
        "SKIP rel=%u jobs/s=%llu frames=%u frame=%lldus fx=%d va=%uMB "
        "updates=%u subsystems=%llu%% emitters=%llu%% rest=%llu%%",
        rel, (unsigned long long)((uint64_t)jobs * 1000000 / (uint64_t)winUs), s_skipWinFrames,
        (long long)(winUs / (s_skipWinFrames ? s_skipWinFrames : 1)), particleGate(),
        addressSpaceMb(), updates - s_skipWinUpdates,
        (unsigned long long)(subUs * 100 / (uint64_t)winUs),
        (unsigned long long)(emitUs * 100 / (uint64_t)winUs),
        (unsigned long long)(100 - (subUs + emitUs) * 100 / (uint64_t)winUs)
    );
    skipWindowStart();
    s_skipWinQpc = now;
    s_skipWinRel = rel;
    s_skipWinFrames = 0;
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

// ── What a save changes ─────────────────────────────────────────────────────
//
// A keyframe save broke the playback it was taken from (2026-11-15). F9 now
// copies the exe's writable sections and the first DIFF_OBJECT_BYTES of every
// actor, building and projectile before the save, and logs what the save
// changed: exe addresses as VAs at 0x400000, object fields by class (RTTI) and
// offset with how many objects changed there. The save itself is the only code
// that runs in between, apart from the save-kind byte replayCheckpointSave
// clears first (VA 0x1246594).

static const uint32_t DIFF_OBJECT_BYTES = 0x800;
static const uint32_t DIFF_OBJECTS_MAX = 24000;
static const uint32_t DIFF_RANGES_MAX = 8;
static const uint32_t DIFF_EXE_LINES = 120;
static const uint32_t DIFF_GONE_LINES = 20;
static const uint32_t DIFF_FIELD_LINES = 100;
static const uint32_t DIFF_TABLE = 8192; // power of two
static const uintptr_t DIFF_MANAGERS[] = {
    0x6e8728, 0x6e5f40, 0x6e8800
}; // actors, buildings, projectiles

struct DiffField {
    uint32_t vtable;
    uint32_t offset;
    uint32_t count;
    uint32_t before;
    uint32_t after;
};

static uint8_t *s_diffCopy = nullptr;
static size_t s_diffCopyBytes = 0;
static uintptr_t s_diffRangeStart[DIFF_RANGES_MAX];
static uint32_t s_diffRangeSize[DIFF_RANGES_MAX];
static uint32_t s_diffRanges = 0;
static uintptr_t s_diffObjects[DIFF_OBJECTS_MAX];
static uint32_t s_diffObjectCount = 0;
static DiffField s_diffTable[DIFF_TABLE];

static void diffCollect(uintptr_t managerRva) {
    const uintptr_t header = exeBase() + managerRva + 8;

    if (!observerReadable(header, 8)) {
        return;
    }

    const uintptr_t begin = *(const uint32_t *)header, end = *(const uint32_t *)(header + 4);

    if (end < begin || (end - begin) % 4 || !observerReadable(begin, end - begin)) {
        return;
    }

    for (uintptr_t at = begin; at < end && s_diffObjectCount < DIFF_OBJECTS_MAX; at += 4) {
        const uintptr_t object = *(const uint32_t *)at;

        if (object && observerReadable(object, DIFF_OBJECT_BYTES)) {
            s_diffObjects[s_diffObjectCount++] = object;
        }
    }
}

static bool diffBegin() {
    const uintptr_t base = exeBase();
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    size_t bytes = 0;
    s_diffRanges = 0;
    s_diffObjectCount = 0;

    for (int i = 0; i < nt->FileHeader.NumberOfSections && s_diffRanges < DIFF_RANGES_MAX; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) {
            continue;
        }

        s_diffRangeStart[s_diffRanges] = base + sec[i].VirtualAddress;
        s_diffRangeSize[s_diffRanges] = sec[i].Misc.VirtualSize;
        bytes += sec[i].Misc.VirtualSize;
        ++s_diffRanges;
    }

    for (uintptr_t mgr : DIFF_MANAGERS) {
        diffCollect(mgr);
    }

    bytes += (size_t)s_diffObjectCount * DIFF_OBJECT_BYTES;
    s_diffCopy = (uint8_t *)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!s_diffCopy) {
        logLine("DIFF refused: could not allocate %u bytes", (unsigned)bytes);
        return false;
    }

    s_diffCopyBytes = bytes;
    uint8_t *at = s_diffCopy;

    for (uint32_t r = 0; r < s_diffRanges; ++r) {
        memcpy(at, (const void *)s_diffRangeStart[r], s_diffRangeSize[r]);
        at += s_diffRangeSize[r];
    }

    for (uint32_t o = 0; o < s_diffObjectCount; ++o) {
        memcpy(at, (const void *)s_diffObjects[o], DIFF_OBJECT_BYTES);
        at += DIFF_OBJECT_BYTES;
    }

    return true;
}

// The RTTI class name of an object's vtable, or "?".
static const char *diffClassName(uint32_t vtable) {
    if (!observerReadable(vtable - 4, 4)) {
        return "?";
    }

    const uintptr_t col = *(const uint32_t *)(vtable - 4);

    if (!observerReadable(col, 16)) {
        return "?";
    }

    const uintptr_t td = *(const uint32_t *)(col + 12);

    if (!observerReadable(td + 8, 64)) {
        return "?";
    }

    return (const char *)(td + 8);
}

static void diffNoteField(uint32_t vtable, uint32_t offset, uint32_t before, uint32_t after) {
    uint32_t h = (vtable * 2654435761u) ^ (offset * 40503u);

    for (uint32_t probe = 0; probe < DIFF_TABLE; ++probe) {
        DiffField &f = s_diffTable[(h + probe) & (DIFF_TABLE - 1)];

        if (f.count && (f.vtable != vtable || f.offset != offset)) {
            continue;
        }

        if (!f.count) {
            f.vtable = vtable;
            f.offset = offset;
            f.before = before;
            f.after = after;
        }

        ++f.count;
        return;
    }
}

static void diffAbandon() {
    if (s_diffCopy) {
        VirtualFree(s_diffCopy, 0, MEM_RELEASE);
    }

    s_diffCopy = nullptr;
    s_diffCopyBytes = 0;
}

static void diffEnd() {
    if (!s_diffCopy) {
        return;
    }

    const uintptr_t base = exeBase();
    const uint8_t *copy = s_diffCopy;
    uint32_t exeLines = 0, exeBytes = 0;

    for (uint32_t r = 0; r < s_diffRanges; ++r) {
        const uint32_t *now = (const uint32_t *)s_diffRangeStart[r];
        const uint32_t *then = (const uint32_t *)copy;
        const uint32_t words = s_diffRangeSize[r] / 4;
        uint32_t i = 0;

        while (i < words) {
            if (now[i] == then[i]) {
                ++i;
                continue;
            }

            // A run of changed words, merged across gaps of up to 8 words.
            const uint32_t first = i;
            uint32_t last = i;

            for (uint32_t j = i + 1; j < words && j <= last + 8; ++j) {
                if (now[j] != then[j]) {
                    last = j;
                }
            }

            exeBytes += (last - first + 1) * 4;

            if (exeLines++ < DIFF_EXE_LINES) {
                const uintptr_t va = s_diffRangeStart[r] + first * 4 - base + 0x400000;
                logLine(
                    "DIFF exe va=0x%x len=%u first %08x->%08x", (unsigned)va,
                    (last - first + 1) * 4, then[first], now[first]
                );
            }

            i = last + 1;
        }

        copy += s_diffRangeSize[r];
    }

    memset(s_diffTable, 0, sizeof(s_diffTable));
    uint32_t changedObjects = 0, gone = 0, goneLines = 0;

    for (uint32_t o = 0; o < s_diffObjectCount; ++o) {
        const uint32_t *now = (const uint32_t *)s_diffObjects[o];
        const uint32_t *then = (const uint32_t *)copy;
        bool changed = false;

        if (now[0] != then[0]) {
            if (goneLines < DIFF_GONE_LINES) {
                logLine(
                    "DIFF gone %s vt=0x%x obj=%p now vt=%08x", diffClassName(then[0]),
                    (unsigned)(then[0] - base + 0x400000), (void *)s_diffObjects[o], now[0]
                );
                ++goneLines;
            }

            ++gone;
            changedObjects += 1;
            copy += DIFF_OBJECT_BYTES;
            continue; // its fields are no longer the object's
        }

        for (uint32_t k = 0; k < DIFF_OBJECT_BYTES / 4; ++k) {
            if (now[k] != then[k]) {
                diffNoteField(then[0], k * 4, then[k], now[k]);
                changed = true;
            }
        }

        changedObjects += changed ? 1 : 0;
        copy += DIFF_OBJECT_BYTES;
    }

    logLine(
        "DIFF summary: exe runs=%u bytes=%u, objects %u of %u changed, %u of them gone", exeLines,
        exeBytes, changedObjects, s_diffObjectCount, gone
    );

    for (uint32_t line = 0; line < DIFF_FIELD_LINES; ++line) {
        DiffField *best = nullptr;

        for (DiffField &f : s_diffTable) {
            if (f.count && (!best || f.count > best->count)) {
                best = &f;
            }
        }

        if (!best) {
            break;
        }

        logLine(
            "DIFF field %s vt=0x%x +0x%x objects=%u e.g. %08x->%08x", diffClassName(best->vtable),
            (unsigned)(best->vtable - base + 0x400000), best->offset, best->count, best->before,
            best->after
        );
        best->count = 0;
    }

    diffAbandon();
}

// F9 saves in keyframe mode (keyframeSerial.h), Ctrl+F9 as a stock save, so
// the diff shows what each one changes.
static bool s_lastKeyframeMode = false;
static bool s_lastKeyframeRaw = false;

static void saveKeyframe(bool keyframeMode) {
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
    const bool diffing = diffBegin();

    bool raw = false;

    if (keyframeMode && !keyframeSaveBegin(&raw)) {
        logLine("SAVE refused: keyframe mode is not available");
        diffAbandon();
        return;
    }

    const int64_t t0 = qpcNow();
    const bool ok = replayCheckpointSave(path, 2);
    const int64_t us = qpcMicros(qpcNow() - t0);
    uint32_t handles = 0;
    bool complete = true;

    if (keyframeMode) {
        complete = keyframeSaveEnd(&handles);
    }
    const uint32_t iter0After = readCursor(ITER0_CURSOR_OFF);
    const uint32_t iter1After = readCursor(ITER1_CURSOR_OFF);

    if (diffing) {
        diffEnd();
    }

    // Put both cursors back, so a later divergence cannot be the save's draws.
    if (iter0After != iter0Before) {
        writeCursor(ITER0_CURSOR_OFF, iter0Before);
    }

    if (iter1After != iter1Before) {
        writeCursor(ITER1_CURSOR_OFF, iter1Before);
    }

    logLine(
        "SAVE run=%u rel=%u mode=%s ok=%d complete=%d handles=%u time=%lld.%03lldms bytes=%u "
        "queued=%u placements=%u iter0 %u->%u iter1 %u->%u va %uMB->%uMB path=%s",
        s_run, rel, keyframeMode ? "keyframe" : "stock", ok ? 1 : 0, complete ? 1 : 0, handles,
        (long long)(us / 1000), (long long)(us % 1000), fileBytes(path), queued, placements,
        iter0Before, iter0After, iter1Before, iter1After, vaBefore, addressSpaceMb(), utf8(path)
    );

    if (ok) {
        memcpy(s_lastKeyframe, path, sizeof(s_lastKeyframe));
        s_lastKeyframeMode = keyframeMode;
        s_lastKeyframeRaw = raw;
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
    const bool keyframe = !rearms && s_lastKeyframeMode;

    if (keyframe) {
        keyframeLoadBegin(s_lastKeyframeRaw);
    }

    const bool ok = replayLoadSave(path);

    if (keyframe) {
        keyframeLoadEnd();
    }

    bool started = false;

    // Before this frame's update loop can run (see replayPlayerStartNow).
    if (ok && rearms) {
        started = replayPlayerStartNow();
    }

    const int64_t us = qpcMicros(qpcNow() - t0);
    logLine(
        "%s ok=%d started=%d time=%lld.%03lldms queued %u->%u placements %u->%u va %uMB->%uMB "
        "path=%s",
        what, ok ? 1 : 0, started ? 1 : 0, (long long)(us / 1000), (long long)(us % 1000),
        queuedBefore, replayNetQueuedCommands(), placementsBefore, replayPendingPlacements(),
        vaBefore, addressSpaceMb(), utf8(path)
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

// ── Round trip ──────────────────────────────────────────────────────────────
//
// Shift+F10: what a keyframe loses. At the next quiet job, copies every object
// of the five managers (by slot) and the player table, takes a keyframe there,
// restores it at once (before any update), and compares the loaded world with
// the copy, slot by slot. A word is reported when it differs and is not a
// pointer that merely moved (both values readable and their top 12 bits
// different). Also the exe's writable sections, by the same rule. The run then
// carries on from the restored world, so the bracket check shows whether it
// stays in step.

struct RoundTripSet {
    const char *name;
    uintptr_t rva; // a manager (slot vector at +8/+0xc) or, for players, the table
    bool table;
};

static const RoundTripSet ROUND_TRIP_SETS[] = {
    {"actor", 0x6e8728, false}, {"building", 0x6e5f40, false}, {"fly", 0x6e8800, false},
    {"clan", 0xc7937c, false},  {"element", 0x6e5e74, false},  {"player", 0x6e8bd8, true},
};
static const uint32_t ROUND_TRIP_SETS_N = sizeof(ROUND_TRIP_SETS) / sizeof(ROUND_TRIP_SETS[0]);
static const uint32_t ROUND_TRIP_PLAYERS = 32;
static const uint32_t ROUND_TRIP_OBJECTS_MAX = 160000;
static const uint32_t ROUND_TRIP_OBJECT_MAX_BYTES = 0x2000;
static const uint32_t ROUND_TRIP_ARENA = 96u << 20;
static const uint32_t ROUND_TRIP_LINES = 120;

// An actor's actions are separate allocations: each entry of its stack
// (actor+0x158..+0x15c) and the last finished one (+0x168) is compared as an
// object of its own, keyed by owner slot and position.
static const uint32_t ACTOR_STACK_BEGIN_OFF = 0x158;
static const uint32_t ACTOR_STACK_END_OFF = 0x15c;
static const uint32_t ACTOR_LAST_ACTION_OFF = 0x168;
static const uint32_t ACTOR_STACK_MAX = 32;
static const uint16_t SUB_SELF = 0;
static const uint16_t SUB_LAST = 0xffff;

struct RoundTripObject {
    uint8_t set;
    uint16_t sub; // SUB_SELF, 1 + stack index, or SUB_LAST
    uint32_t slot;
    uint32_t vtable;
    uint32_t bytes;
    uint32_t offset; // into the arena
};

static RoundTripObject *s_rtObjects = nullptr;
static uint32_t s_rtCount = 0;
static uint8_t *s_rtArena = nullptr;
static uint32_t s_rtUsed = 0;
static uint8_t *s_rtExe = nullptr;
static bool s_roundTripPending = false;
static uint32_t s_roundTripWait = 0;

static HANDLE crtHeap() {
    static HANDLE heap = nullptr;
    static bool tried = false;

    if (!tried) {
        tried = true;
        HMODULE crt = GetModuleHandleA("msvcr100.dll");
        typedef intptr_t(__cdecl * GetHeap)();
        GetHeap get = crt ? (GetHeap)(void *)GetProcAddress(crt, "_get_heap_handle") : nullptr;
        heap = get ? (HANDLE)get() : nullptr;
    }

    return heap;
}

static uint32_t objectBytes(uintptr_t object) {
    const HANDLE heap = crtHeap();
    SIZE_T size = (SIZE_T)-1;

    if (heap) {
        size = HeapSize(heap, 0, (const void *)object);
    }

    if (size == (SIZE_T)-1 || size < 8) {
        size = 0x400;
    }

    if (size > ROUND_TRIP_OBJECT_MAX_BYTES) {
        size = ROUND_TRIP_OBJECT_MAX_BYTES;
    }

    return (uint32_t)(size & ~(SIZE_T)3);
}

static bool roundTripSlots(const RoundTripSet &set, const uint32_t **slots, uint32_t *count) {
    const uintptr_t at = exeBase() + set.rva;

    if (set.table) {
        *slots = (const uint32_t *)at;
        *count = ROUND_TRIP_PLAYERS;
        return true;
    }

    if (!observerReadable(at + 8, 8)) {
        return false;
    }

    const uintptr_t begin = *(const uint32_t *)(at + 8), end = *(const uint32_t *)(at + 12);

    if (end < begin || (end - begin) % 4 || !observerReadable(begin, end - begin)) {
        return false;
    }

    *slots = (const uint32_t *)begin;
    *count = (uint32_t)((end - begin) / 4);
    return true;
}

static void roundTripFree() {
    if (s_rtObjects) {
        VirtualFree(s_rtObjects, 0, MEM_RELEASE);
    }

    if (s_rtArena) {
        VirtualFree(s_rtArena, 0, MEM_RELEASE);
    }

    if (s_rtExe) {
        VirtualFree(s_rtExe, 0, MEM_RELEASE);
    }

    s_rtObjects = nullptr;
    s_rtArena = nullptr;
    s_rtExe = nullptr;
    s_rtCount = 0;
    s_rtUsed = 0;
}

// The action at `sub` of the actor at `owner`, or 0.
static uintptr_t actorAction(uintptr_t owner, uint16_t sub) {
    if (!observerReadable(
            owner + ACTOR_STACK_BEGIN_OFF, ACTOR_LAST_ACTION_OFF + 4 - ACTOR_STACK_BEGIN_OFF
        )) {
        return 0;
    }

    if (sub == SUB_LAST) {
        return *(const uint32_t *)(owner + ACTOR_LAST_ACTION_OFF);
    }

    const uintptr_t begin = *(const uint32_t *)(owner + ACTOR_STACK_BEGIN_OFF);
    const uintptr_t end = *(const uint32_t *)(owner + ACTOR_STACK_END_OFF);
    const uint32_t index = (uint32_t)sub - 1;

    if (end < begin || (end - begin) % 4 || (end - begin) / 4 <= index ||
        !observerReadable(begin + index * 4, 4)) {
        return 0;
    }

    return *(const uint32_t *)(begin + index * 4);
}

static uint32_t actorStackDepth(uintptr_t owner) {
    if (!observerReadable(owner + ACTOR_STACK_BEGIN_OFF, 8)) {
        return 0;
    }

    const uintptr_t begin = *(const uint32_t *)(owner + ACTOR_STACK_BEGIN_OFF);
    const uintptr_t end = *(const uint32_t *)(owner + ACTOR_STACK_END_OFF);

    if (end < begin || (end - begin) % 4) {
        return 0;
    }

    return (uint32_t)((end - begin) / 4);
}

static bool roundTripCopy(uint8_t set, uint16_t sub, uint32_t slot, uintptr_t object) {
    if (!object || s_rtCount >= ROUND_TRIP_OBJECTS_MAX || !observerReadable(object, 8)) {
        return false;
    }

    const uint32_t bytes = objectBytes(object);

    if (s_rtUsed + bytes > ROUND_TRIP_ARENA || !observerReadable(object, bytes)) {
        return false;
    }

    RoundTripObject &o = s_rtObjects[s_rtCount++];
    o.set = set;
    o.sub = sub;
    o.slot = slot;
    o.vtable = *(const uint32_t *)object;
    o.bytes = bytes;
    o.offset = s_rtUsed;
    memcpy(s_rtArena + s_rtUsed, (const void *)object, bytes);
    s_rtUsed += bytes;
    return true;
}

static bool roundTripSnapshot() {
    s_rtObjects = (RoundTripObject *)VirtualAlloc(
        NULL, ROUND_TRIP_OBJECTS_MAX * sizeof(RoundTripObject), MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );
    s_rtArena =
        (uint8_t *)VirtualAlloc(NULL, ROUND_TRIP_ARENA, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    size_t exeBytes = 0;
    const uintptr_t base = exeBase();
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    s_diffRanges = 0;

    for (int i = 0; i < nt->FileHeader.NumberOfSections && s_diffRanges < DIFF_RANGES_MAX; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) {
            s_diffRangeStart[s_diffRanges] = base + sec[i].VirtualAddress;
            s_diffRangeSize[s_diffRanges] = sec[i].Misc.VirtualSize;
            exeBytes += sec[i].Misc.VirtualSize;
            ++s_diffRanges;
        }
    }

    s_rtExe = (uint8_t *)VirtualAlloc(NULL, exeBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!s_rtObjects || !s_rtArena || !s_rtExe) {
        roundTripFree();
        return false;
    }

    uint8_t *at = s_rtExe;

    for (uint32_t r = 0; r < s_diffRanges; ++r) {
        memcpy(at, (const void *)s_diffRangeStart[r], s_diffRangeSize[r]);
        at += s_diffRangeSize[r];
    }

    for (uint32_t k = 0; k < ROUND_TRIP_SETS_N; ++k) {
        const uint32_t *slots = nullptr;
        uint32_t count = 0;

        if (!roundTripSlots(ROUND_TRIP_SETS[k], &slots, &count)) {
            continue;
        }

        for (uint32_t i = 0; i < count && s_rtCount < ROUND_TRIP_OBJECTS_MAX; ++i) {
            const uintptr_t object = slots[i];

            if (!roundTripCopy((uint8_t)k, SUB_SELF, i, object) || k != 0) {
                continue;
            }

            const uint32_t depth = actorStackDepth(object);

            for (uint32_t d = 0; d < depth && d < ACTOR_STACK_MAX; ++d) {
                roundTripCopy(0, (uint16_t)(1 + d), i, actorAction(object, (uint16_t)(1 + d)));
            }

            roundTripCopy(0, SUB_LAST, i, actorAction(object, SUB_LAST));
        }
    }

    return true;
}

// A pointer that merely moved with the reload.
static bool movedPointer(uint32_t a, uint32_t b) {
    return (a >> 20) != (b >> 20) && a >= 0x10000 && b >= 0x10000 && observerReadable(a, 4) &&
           observerReadable(b, 4);
}

// Mother (vtable RVA 0x5b22a4): every differing word of each one, and a
// summary of its actions before and after. Her idle walk is the first thing a
// restored keyframe does differently.
static const uint32_t MOTHER_VTABLE_RVA = 0x5b22a4;
static const uint32_t MOTHER_LINES = 400;

static void actionSummary(char *out, size_t size, const uint32_t *a) {
    snprintf(
        out, size, "%s step=%u start=%u res=%d b14=%u flags=%08x b1c=%u id=%u", diffClassName(a[0]),
        a[2], a[3], (int)a[4], a[5] & 0xff, a[6], a[7] & 0xff,
        a[0] == (uint32_t)(exeBase() + 0x5f47b0) ? a[9] : 0
    );
}

static void motherActions(
    const char *when, uint32_t slot, const RoundTripObject *objs, uint32_t n, bool now,
    uint32_t *lines
) {
    char text[200];

    for (uint32_t i = 0; i < n && *lines < MOTHER_LINES; ++i) {
        const RoundTripObject &o = objs[i];

        if (o.set != 0 || o.slot != slot || o.sub == SUB_SELF) {
            continue;
        }

        const uint32_t *a = (const uint32_t *)(s_rtArena + o.offset);

        if (now) {
            const uint32_t *slots = nullptr;
            uint32_t count = 0;

            if (!roundTripSlots(ROUND_TRIP_SETS[0], &slots, &count) || slot >= count) {
                continue;
            }

            const uintptr_t at = actorAction(slots[slot], o.sub);

            if (!at || !observerReadable(at, 0x28)) {
                logLine("MOTHER %s slot=%u sub=%u none", when, slot, o.sub);
                ++*lines;
                continue;
            }

            a = (const uint32_t *)at;
        }

        actionSummary(text, sizeof(text), a);
        logLine("MOTHER %s slot=%u sub=%u %s", when, slot, o.sub, text);
        ++*lines;
    }
}

static void roundTripMothers() {
    const uint32_t vt = (uint32_t)(exeBase() + MOTHER_VTABLE_RVA);
    uint32_t lines = 0, mothers = 0;
    const uint32_t *slots = nullptr;
    uint32_t count = 0;

    if (!roundTripSlots(ROUND_TRIP_SETS[0], &slots, &count)) {
        return;
    }

    for (uint32_t n = 0; n < s_rtCount && lines < MOTHER_LINES; ++n) {
        const RoundTripObject &o = s_rtObjects[n];

        if (o.set != 0 || o.sub != SUB_SELF || o.vtable != vt || o.slot >= count ||
            !slots[o.slot] || !observerReadable(slots[o.slot], o.bytes)) {
            continue;
        }

        ++mothers;
        const uint32_t *then = (const uint32_t *)(s_rtArena + o.offset);
        const uint32_t *cur = (const uint32_t *)slots[o.slot];
        const uint32_t thenDepth =
            (then[ACTOR_STACK_END_OFF / 4] - then[ACTOR_STACK_BEGIN_OFF / 4]) / 4;
        logLine(
            "MOTHER slot=%u bytes=0x%x state=%u depth %u->%u last %08x/%08x idle=%d->%d", o.slot,
            o.bytes, then[0x20 / 4], thenDepth, actorStackDepth(slots[o.slot]),
            then[ACTOR_LAST_ACTION_OFF / 4], cur[ACTOR_LAST_ACTION_OFF / 4], (int)then[0x2e0 / 4],
            (int)cur[0x2e0 / 4]
        );
        ++lines;

        for (uint32_t k = 1; k < o.bytes / 4 && lines < MOTHER_LINES; ++k) {
            if (cur[k] != then[k]) {
                logLine("MOTHER slot=%u +0x%x %08x->%08x", o.slot, k * 4, then[k], cur[k]);
                ++lines;
            }
        }

        motherActions("then", o.slot, s_rtObjects, s_rtCount, false, &lines);
        motherActions("now ", o.slot, s_rtObjects, s_rtCount, true, &lines);
    }

    logLine("MOTHER count=%u", mothers);
}

static void roundTripCompare(uint32_t job) {
    const uintptr_t base = exeBase();
    memset(s_diffTable, 0, sizeof(s_diffTable));
    uint32_t missing = 0, extra = 0, retyped = 0, differing = 0, compared = 0, lines = 0;
    uint32_t actionLines = 0;

    for (uint32_t n = 0; n < s_rtCount; ++n) {
        const RoundTripObject &o = s_rtObjects[n];
        const uint32_t *slots = nullptr;
        uint32_t count = 0;
        uintptr_t now = 0;

        if (roundTripSlots(ROUND_TRIP_SETS[o.set], &slots, &count) && o.slot < count) {
            now = slots[o.slot];
        }

        if (now && o.sub != SUB_SELF) {
            now = actorAction(now, o.sub);
        }

        if (!now || !observerReadable(now, o.bytes)) {
            ++missing;

            if (lines < 20) {
                logLine(
                    "ROUNDTRIP missing %s slot=%u sub=%u %s", ROUND_TRIP_SETS[o.set].name, o.slot,
                    o.sub, diffClassName(o.vtable)
                );
                ++lines;
            }

            continue;
        }

        const uint32_t *then = (const uint32_t *)(s_rtArena + o.offset);
        const uint32_t *cur = (const uint32_t *)now;

        if (cur[0] != then[0]) {
            ++retyped;

            if (lines < 40) {
                logLine(
                    "ROUNDTRIP retyped %s slot=%u sub=%u %s -> %s", ROUND_TRIP_SETS[o.set].name,
                    o.slot, o.sub, diffClassName(then[0]), diffClassName(cur[0])
                );
                ++lines;
            }

            continue;
        }

        ++compared;
        bool any = false;

        for (uint32_t k = 1; k < o.bytes / 4; ++k) {
            if (cur[k] != then[k] && !movedPointer(then[k], cur[k])) {
                diffNoteField(then[0], k * 4, then[k], cur[k]);

                // Actions individually, with their owner: few, and each one
                // names a timer or a step the load changed.
                if (o.sub != SUB_SELF && actionLines < ROUND_TRIP_LINES) {
                    const uintptr_t owner = slots[o.slot];
                    logLine(
                        "ROUNDTRIP action owner=%u %s sub=%u %s +0x%x %08x->%08x", o.slot,
                        observerReadable(owner, 4) ? diffClassName(*(const uint32_t *)owner) : "?",
                        o.sub, diffClassName(then[0]), k * 4, then[k], cur[k]
                    );
                    ++actionLines;
                }

                any = true;
            }
        }

        differing += any ? 1 : 0;
    }

    // Stack depths the load changed (an action lost or added).
    for (uint32_t n = 0; n < s_rtCount; ++n) {
        const RoundTripObject &o = s_rtObjects[n];

        if (o.set != 0 || o.sub != SUB_SELF) {
            continue;
        }

        const uint32_t *slots = nullptr;
        uint32_t count = 0;

        if (!roundTripSlots(ROUND_TRIP_SETS[0], &slots, &count) || o.slot >= count ||
            !slots[o.slot]) {
            continue;
        }

        const uint32_t *then = (const uint32_t *)(s_rtArena + o.offset);
        uint32_t thenDepth = 0;

        if (o.bytes > ACTOR_STACK_END_OFF &&
            then[ACTOR_STACK_END_OFF / 4] >= then[ACTOR_STACK_BEGIN_OFF / 4]) {
            thenDepth = (then[ACTOR_STACK_END_OFF / 4] - then[ACTOR_STACK_BEGIN_OFF / 4]) / 4;
        }

        const uint32_t nowDepth = actorStackDepth(slots[o.slot]);

        if (nowDepth != thenDepth && actionLines < ROUND_TRIP_LINES) {
            logLine(
                "ROUNDTRIP stack owner=%u %s depth %u->%u", o.slot, diffClassName(o.vtable),
                thenDepth, nowDepth
            );
            ++actionLines;
        }
    }

    // Objects the load brought that the live world did not have.
    for (uint32_t k = 0; k < ROUND_TRIP_SETS_N; ++k) {
        const uint32_t *slots = nullptr;
        uint32_t count = 0;

        if (!roundTripSlots(ROUND_TRIP_SETS[k], &slots, &count)) {
            continue;
        }

        for (uint32_t i = 0; i < count; ++i) {
            if (!slots[i]) {
                continue;
            }

            bool known = false;

            for (uint32_t n = 0; n < s_rtCount && !known; ++n) {
                known = s_rtObjects[n].set == k && s_rtObjects[n].slot == i &&
                        s_rtObjects[n].sub == SUB_SELF;
            }

            if (!known) {
                ++extra;

                if (lines < 60 && observerReadable(slots[i], 4)) {
                    logLine(
                        "ROUNDTRIP extra %s slot=%u %s", ROUND_TRIP_SETS[k].name, i,
                        diffClassName(*(const uint32_t *)slots[i])
                    );
                    ++lines;
                }
            }
        }
    }

    logLine(
        "ROUNDTRIP summary job=%u objects=%u compared=%u differing=%u missing=%u extra=%u "
        "retyped=%u",
        job, s_rtCount, compared, differing, missing, extra, retyped
    );

    for (uint32_t line = 0; line < ROUND_TRIP_LINES; ++line) {
        DiffField *best = nullptr;

        for (DiffField &f : s_diffTable) {
            if (f.count && (!best || f.count > best->count)) {
                best = &f;
            }
        }

        if (!best) {
            break;
        }

        logLine(
            "ROUNDTRIP field %s vt=0x%x +0x%x objects=%u e.g. %08x->%08x",
            diffClassName(best->vtable), (unsigned)(best->vtable - base + 0x400000), best->offset,
            best->count, best->before, best->after
        );
        best->count = 0;
    }

    // The exe's own globals.
    const uint8_t *copy = s_rtExe;
    uint32_t exeLines = 0;

    for (uint32_t r = 0; r < s_diffRanges; ++r) {
        const uint32_t *now = (const uint32_t *)s_diffRangeStart[r];
        const uint32_t *then = (const uint32_t *)copy;
        const uint32_t words = s_diffRangeSize[r] / 4;

        for (uint32_t i = 0; i < words && exeLines < ROUND_TRIP_LINES; ++i) {
            if (now[i] == then[i] || movedPointer(then[i], now[i])) {
                continue;
            }

            uint32_t last = i;

            while (last + 1 < words && now[last + 1] != then[last + 1] &&
                   !movedPointer(then[last + 1], now[last + 1])) {
                ++last;
            }

            const uintptr_t va = s_diffRangeStart[r] + i * 4 - base + 0x400000;
            logLine(
                "ROUNDTRIP exe va=0x%x len=%u first %08x->%08x", (unsigned)va, (last - i + 1) * 4,
                then[i], now[i]
            );
            ++exeLines;
            i = last;
        }

        copy += s_diffRangeSize[r];
    }
}

static void roundTripFrame() {
    if (!s_roundTripPending) {
        return;
    }

    uint32_t target = 0;

    if (replayNetQueuedCommands() != 0 || replayPendingPlacements() != 0 || replayScreenPopupOn() ||
        replaySeekerActive(&target)) {
        if (++s_roundTripWait >= QUIET_WAIT_FRAMES) {
            s_roundTripPending = false;
            logLine("ROUNDTRIP gave up waiting for a quiet job");
        }

        return;
    }

    s_roundTripPending = false;
    stopTurbo("roundtrip");

    if (!roundTripSnapshot()) {
        logLine("ROUNDTRIP refused: could not allocate");
        return;
    }

    uint32_t job = 0;

    if (!replayKeyframesCaptureNow(&job)) {
        logLine("ROUNDTRIP refused: no keyframe could be taken");
        roundTripFree();
        return;
    }

    bool loaded = false;
    const int64_t t0 = qpcNow();
    const bool ok = replayKeyframeRestore(job, &loaded);
    logLine(
        "ROUNDTRIP job=%u restore ok=%d loaded=%d time=%lldms objects=%u bytes=%u", job, ok ? 1 : 0,
        loaded ? 1 : 0, (long long)(qpcMicros(qpcNow() - t0) / 1000), s_rtCount, s_rtUsed
    );

    if (ok) {
        roundTripMothers();
        roundTripCompare(job);
    }

    roundTripFree();
}

// ── Crash report ────────────────────────────────────────────────────────────
//
// Where a crash happened, in the log: exception, registers, and every dword
// within 8 KB above esp that points into the exe's code (return addresses, with
// some noise), as VAs at the preferred base. Chains to the filter it replaced.

static LPTOP_LEVEL_EXCEPTION_FILTER s_prevCrashFilter = nullptr;
static bool s_crashFilterInstalled = false;

static uint32_t exeVa(uint32_t value) {
    const uintptr_t base = exeBase();

    if (value >= base + TEXT_BEGIN_RVA && value < base + TEXT_END_RVA) {
        return (uint32_t)(value - base + 0x400000);
    }

    return 0;
}

static LONG WINAPI probeCrashFilter(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const CONTEXT *cx = ep->ContextRecord;
    uint32_t access = 0, kind = 0;

    if (er->NumberParameters >= 2) {
        kind = (uint32_t)er->ExceptionInformation[0];
        access = (uint32_t)er->ExceptionInformation[1];
    }

    logLine(
        "CRASH run=%u rel=%u code=%08x eip=%08x (va %08x) access %s %08x", s_run, s_jobRel,
        (unsigned)er->ExceptionCode, (unsigned)cx->Eip, exeVa(cx->Eip),
        kind == 1 ? "write" : (kind == 8 ? "exec" : "read"), access
    );
    logLine(
        "CRASH eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
        (unsigned)cx->Eax, (unsigned)cx->Ebx, (unsigned)cx->Ecx, (unsigned)cx->Edx,
        (unsigned)cx->Esi, (unsigned)cx->Edi, (unsigned)cx->Ebp, (unsigned)cx->Esp
    );

    char line[512];
    int at = 0;
    uint32_t found = 0;
    const uint32_t regs[6] = {(uint32_t)cx->Eax, (uint32_t)cx->Ebx, (uint32_t)cx->Ecx,
                              (uint32_t)cx->Edx, (uint32_t)cx->Esi, (uint32_t)cx->Edi};
    static const char *const REG_NAMES[6] = {"eax", "ebx", "ecx", "edx", "esi", "edi"};

    // What each register points at, with the class of an object it may be.
    for (int r = 0; r < 6; ++r) {
        if (regs[r] < 0x10000 || !observerReadable(regs[r], 0x20)) {
            continue;
        }

        const uint32_t *w = (const uint32_t *)(uintptr_t)regs[r];
        logLine(
            "CRASH [%s] %08x %08x %08x %08x %08x %08x %08x %08x %s", REG_NAMES[r], w[0], w[1], w[2],
            w[3], w[4], w[5], w[6], w[7], diffClassName(w[0])
        );
    }

    for (uint32_t off = 0; off < 0x2000 && found < 40; off += 4) {
        const uintptr_t slot = cx->Esp + off;

        if (!observerReadable(slot, 4)) {
            break;
        }

        const uint32_t va = exeVa(*(const uint32_t *)slot);

        if (!va) {
            continue;
        }

        at += snprintf(line + at, sizeof(line) - at, " +%x:%08x", off, va);
        ++found;

        if (found % 8 == 0) {
            logLine("CRASH stack%s", line);
            at = 0;
        }
    }

    if (at) {
        logLine("CRASH stack%s", line);
    }

    if (s_log) {
        fflush(s_log);
    }

    return s_prevCrashFilter ? s_prevCrashFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// ── Tunnel watch ────────────────────────────────────────────────────────────
//
// Two playbacks crashed in TunnelSetTargetChore after a keyframe load: the
// TunnelHole's second base (+0xe8, the pathfinder callback, vtable VA
// 0x9a8474) held 0x00c20ada instead of its vtable. Every frame, every
// TunnelHole in the building manager is checked, and the first four get a
// hardware write watchpoint on that word on the game thread, which names the
// instruction that overwrites it. Hits are stored by the exception handler and
// logged on the next frame tick.

static const uintptr_t BUILDING_MGR_RVA = 0x6e5f40;
static const uintptr_t TUNNEL_VTABLE_RVA = 0x5a847c;
static const uintptr_t TUNNEL_CALLBACK_VTABLE_RVA = 0x5a8474;
static const uint32_t TUNNEL_CALLBACK_OFF = 0xe8;
static const uint32_t TUNNEL_BYTES = 0x160;
static const DWORD ARM_WATCH_CODE = 0xe0db0001;
static const uint32_t WATCH_HITS_MAX = 32;
static const uint32_t WATCH_STACK = 16;

struct WatchHit {
    uint32_t eip, slot, address, value, frame, rel;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp;
    uint32_t stack[WATCH_STACK];
};

static uintptr_t s_watchAddr[4] = {};
static uintptr_t s_armAddr[4] = {};
static uint32_t s_armLen[4] = {3, 3, 3, 3}; // Dr7 LEN: 1 = two bytes, 3 = four
static bool s_traceArmed = false;
static void traceRecord(const CONTEXT *cx, uint32_t bits);
static DWORD s_gameThread = 0;
static volatile LONG s_watchHitCount = 0;
static uint32_t s_watchHitsLogged = 0;
static WatchHit s_watchHits[WATCH_HITS_MAX];
static uintptr_t s_tunnelSeen[64] = {};
static uint32_t s_tunnelSeenCount = 0;
static uintptr_t s_tunnelBad[64] = {};
static uint32_t s_tunnelBadCount = 0;

static LONG CALLBACK tunnelWatchHandler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;

    if (er->ExceptionCode == ARM_WATCH_CODE) {
        DWORD dr7 = 0;
        cx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        cx->Dr0 = s_armAddr[0];
        cx->Dr1 = s_armAddr[1];
        cx->Dr2 = s_armAddr[2];
        cx->Dr3 = s_armAddr[3];

        for (int n = 0; n < 4; ++n) {
            if (s_armAddr[n]) {
                // Local enable, break on write, s_armLen bytes.
                dr7 |= 1u << (2 * n);
                dr7 |= 1u << (16 + 4 * n);
                dr7 |= s_armLen[n] << (18 + 4 * n);
            }
        }

        cx->Dr7 = dr7;
        cx->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (er->ExceptionCode != EXCEPTION_SINGLE_STEP || (cx->Dr6 & 0xf) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const uint32_t bits = (uint32_t)(cx->Dr6 & 0xf);
    cx->Dr6 = 0;

    if (s_traceArmed) {
        traceRecord(cx, bits);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    const LONG n = InterlockedIncrement(&s_watchHitCount) - 1;

    if (n < (LONG)WATCH_HITS_MAX) {
        WatchHit &h = s_watchHits[n];
        uint32_t slot = 0;

        while (slot < 3 && !(bits & (1u << slot))) {
            ++slot;
        }

        h.slot = slot;
        h.address = (uint32_t)s_watchAddr[slot];
        h.value = h.address ? *(const uint32_t *)(uintptr_t)h.address : 0;
        h.eip = (uint32_t)cx->Eip;
        h.frame = s_frame;
        h.rel = s_jobRel;
        h.eax = (uint32_t)cx->Eax;
        h.ebx = (uint32_t)cx->Ebx;
        h.ecx = (uint32_t)cx->Ecx;
        h.edx = (uint32_t)cx->Edx;
        h.esi = (uint32_t)cx->Esi;
        h.edi = (uint32_t)cx->Edi;
        h.ebp = (uint32_t)cx->Ebp;
        h.esp = (uint32_t)cx->Esp;
        uint32_t found = 0;

        for (uint32_t off = 0; off < 0x1000 && found < WATCH_STACK; off += 4) {
            const uintptr_t at = cx->Esp + off;

            if (!observerReadable(at, 4)) {
                break;
            }

            const uint32_t va = exeVa(*(const uint32_t *)at);

            if (va) {
                h.stack[found++] = va;
            }
        }

        while (found < WATCH_STACK) {
            h.stack[found++] = 0;
        }
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

static void tunnelLogObject(const char *tag, uintptr_t t) {
    char line[160];

    for (uint32_t off = 0; off < TUNNEL_BYTES; off += 0x20) {
        int at = 0;

        for (uint32_t k = 0; k < 0x20; k += 4) {
            at += snprintf(line + at, sizeof(line) - at, " %08x", *(const uint32_t *)(t + off + k));
        }

        logLine("%s +%03x%s", tag, off, line);
    }
}

static void tunnelWatchFrame() {
    if (!s_gameThread) {
        s_gameThread = GetCurrentThreadId();
        AddVectoredExceptionHandler(1, tunnelWatchHandler);
    }

    const LONG hits = s_watchHitCount;

    while (s_watchHitsLogged < (uint32_t)hits && s_watchHitsLogged < WATCH_HITS_MAX) {
        const WatchHit &h = s_watchHits[s_watchHitsLogged++];
        logLine(
            "TUNNEL write frame=%u rel=%u dr%u addr=%08x now=%08x eip=%08x (va %08x)", h.frame,
            h.rel, h.slot, h.address, h.value, h.eip, exeVa(h.eip)
        );
        logLine(
            "TUNNEL write eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
            h.eax, h.ebx, h.ecx, h.edx, h.esi, h.edi, h.ebp, h.esp
        );
        char line[256];
        int at = 0;

        for (uint32_t k = 0; k < WATCH_STACK && h.stack[k]; ++k) {
            at += snprintf(line + at, sizeof(line) - at, " %08x", h.stack[k]);
        }

        logLine("TUNNEL write stack%s", line);
    }

    const uintptr_t base = exeBase();
    const uintptr_t mgr = base + BUILDING_MGR_RVA;

    if (!observerReadable(mgr + 8, 8)) {
        return;
    }

    const uintptr_t begin = *(const uint32_t *)(mgr + 8), end = *(const uint32_t *)(mgr + 12);

    if (end < begin || (end - begin) % 4 || !observerReadable(begin, end - begin)) {
        return;
    }

    uintptr_t now[64];
    uint32_t count = 0;

    for (uintptr_t p = begin; p < end && count < 64; p += 4) {
        const uintptr_t t = *(const uint32_t *)p;

        if (!t || !observerReadable(t, TUNNEL_BYTES) ||
            *(const uint32_t *)t != base + TUNNEL_VTABLE_RVA) {
            continue;
        }

        now[count++] = t;
        const uint32_t cb = *(const uint32_t *)(t + TUNNEL_CALLBACK_OFF);
        bool reported = false;

        for (uint32_t k = 0; k < s_tunnelBadCount && !reported; ++k) {
            reported = s_tunnelBad[k] == t;
        }

        if (cb != base + TUNNEL_CALLBACK_VTABLE_RVA && !reported) {
            logLine(
                "TUNNEL corrupt frame=%u rel=%u obj=%08x id=%08x +0xe8=%08x", s_frame, currentRel(),
                (unsigned)t, *(const uint32_t *)(t + 0x9c), cb
            );
            tunnelLogObject("TUNNEL bytes", t);

            if (s_tunnelBadCount < 64) {
                s_tunnelBad[s_tunnelBadCount++] = t;
            }
        }
    }

    // The draw trace holds the debug registers while it runs.
    if (s_traceArmed) {
        return;
    }

    bool changed = count != s_tunnelSeenCount;

    for (uint32_t k = 0; k < count && !changed; ++k) {
        changed = now[k] != s_tunnelSeen[k];
    }

    if (!changed) {
        return;
    }

    char line[400];
    int at = 0;

    for (uint32_t k = 0; k < count && k < 12; ++k) {
        const uintptr_t t = now[k];
        at += snprintf(
            line + at, sizeof(line) - at, " %08x/id=%08x/%s", (unsigned)t,
            *(const uint32_t *)(t + 0x9c),
            *(const uint32_t *)(t + TUNNEL_CALLBACK_OFF) == base + TUNNEL_CALLBACK_VTABLE_RVA
                ? "ok"
                : "BAD"
        );
    }

    logLine("TUNNEL set frame=%u rel=%u n=%u%s", s_frame, currentRel(), count, line);
    memcpy(s_tunnelSeen, now, count * sizeof(uintptr_t));
    s_tunnelSeenCount = count;
    s_tunnelBadCount = 0;

    // The game thread only: debug registers are per thread.
    if (GetCurrentThreadId() != s_gameThread) {
        return;
    }

    for (int n = 0; n < 4; ++n) {
        s_armAddr[n] = n < (int)count ? now[n] + TUNNEL_CALLBACK_OFF : 0;
        s_armLen[n] = 3;
    }

    RaiseException(ARM_WATCH_CODE, 0, 0, nullptr);

    for (int n = 0; n < 4; ++n) {
        s_watchAddr[n] = s_armAddr[n];
    }

    logLine(
        "TUNNEL watch dr0=%08x dr1=%08x dr2=%08x dr3=%08x", (unsigned)s_watchAddr[0],
        (unsigned)s_watchAddr[1], (unsigned)s_watchAddr[2], (unsigned)s_watchAddr[3]
    );
}

// ── Draw trace ──────────────────────────────────────────────────────────────
//
// Ctrl+Shift+F10: where a restored keyframe first behaves differently. At the
// next quiet job J a keyframe is taken and every simulation draw of both
// random iterators is recorded for TRACE_JOBS jobs (a write watchpoint on each
// cursor; the draw is inlined at every call site, so the trapping EIP is the
// drawing code). Then the keyframe is restored and the same jobs are recorded
// again. The first draw that differs, with the code that made it and its
// callers, names the code whose input the load lost. Only draws between update
// entry and SIM_END count: the emitters and rendering draw iterator 1 on a
// frame schedule.

enum TracePhase { TRACE_OFF, TRACE_WAIT, TRACE_LIVE, TRACE_RESTORED };
static const uint32_t TRACE_JOBS = 40;
// A request made before this job starts the trace here instead (the keyframe
// at 5130 of replay 634pm-5n diverged at 5133).
static const uint32_t TRACE_START_JOB = 5125;
static const uint32_t TRACE_MAX = 1u << 17;
static const uintptr_t TRACE_GAME_PTR_RVA = 0x6e8bcc;
static const uint32_t TRACE_CURSOR_OFF[2] = {0x40078, 0x40080};

struct TraceHit {
    uint8_t kind; // TRACE_DRAW or a marker
    uint8_t iter;
    uint8_t phase; // the MP cadence byte
    uint8_t inSim; // between update entry and SIM_END
    uint32_t worldTick;
    uint32_t rel; // the job the last pump announced
    uint32_t eip; // exe VA, or 0x80000000 | raw EIP outside the exe
    uint32_t caller[3]; // exe VAs
    uint32_t cursor; // after the draw; for a pump, iterator 0's cursor
    uint32_t esi, edi, ebx, ecx; // the drawing code's registers (its `this`, usually)
};

static const uint8_t TRACE_DRAW = 0;
static const uint8_t TRACE_PUMP = 1;
static const uint8_t TRACE_LOAD_BEGIN = 2;
static const uint8_t TRACE_LOAD_END = 3;

// Live, then one restore of the same keyframe (3: a second restore, to tell
// leftover memory from a load apart from state the keyframe carries).
static const int TRACE_PASSES = 2;
static TraceHit *s_trace[TRACE_PASSES] = {};
static volatile LONG s_traceCount[TRACE_PASSES] = {};
static volatile LONG s_traceDropped[TRACE_PASSES] = {};
static volatile int s_tracePhase = TRACE_OFF;
static int s_traceBuf = -1; // where hits go, -1 = nowhere
static uint32_t s_traceJob = 0;
static uint32_t s_traceWait = 0;
static uintptr_t s_traceCursor[2] = {0, 0};

static TraceHit *traceSlot(int buf) {
    const LONG n = InterlockedIncrement(&s_traceCount[buf]) - 1;

    if (n >= (LONG)TRACE_MAX) {
        InterlockedIncrement(&s_traceDropped[buf]);
        return nullptr;
    }

    TraceHit *h = &s_trace[buf][n];
    memset(h, 0, sizeof(*h));
    h->worldTick = worldTick();
    h->phase = *(const volatile uint8_t *)(exeBase() + JOB_PHASE_RVA);
    h->inSim = (uint8_t)g_prngGuardInSimulation;
    h->rel = s_jobRel;
    return h;
}

static void traceMarker(uint8_t kind, uint32_t cursor) {
    const int buf = s_traceBuf;

    if (buf < 0) {
        return;
    }

    TraceHit *h = traceSlot(buf);

    if (h) {
        h->kind = kind;
        h->cursor = cursor;
    }
}

static void traceRecord(const CONTEXT *cx, uint32_t bits) {
    const int buf = s_traceBuf;

    if (buf < 0) {
        return;
    }

    uint32_t eip = exeVa((uint32_t)cx->Eip);

    if (!eip) {
        eip = 0x80000000u | (uint32_t)cx->Eip;
    }

    for (uint32_t iter = 0; iter < 2; ++iter) {
        if (!(bits & (1u << iter))) {
            continue;
        }

        TraceHit *h = traceSlot(buf);

        if (!h) {
            continue;
        }

        h->kind = TRACE_DRAW;
        h->eip = eip;
        h->iter = (uint8_t)iter;
        h->cursor = *(const uint16_t *)s_traceCursor[iter];
        h->esi = (uint32_t)cx->Esi;
        h->edi = (uint32_t)cx->Edi;
        h->ebx = (uint32_t)cx->Ebx;
        h->ecx = (uint32_t)cx->Ecx;
        const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
        uint32_t found = 0;

        for (uintptr_t at = cx->Esp;
             at + 4 <= (uintptr_t)tib->StackBase && found < 3 && at < cx->Esp + 0x800; at += 4) {
            const uint32_t va = exeVa(*(const uint32_t *)at);

            if (va) {
                h->caller[found++] = va;
            }
        }
    }
}

static bool traceArm(bool on) {
    for (int n = 0; n < 4; ++n) {
        s_armAddr[n] = 0;
        s_armLen[n] = 3;
    }

    if (on) {
        const uintptr_t game = *(const uint32_t *)(exeBase() + TRACE_GAME_PTR_RVA);

        if (!game) {
            return false;
        }

        for (int k = 0; k < 2; ++k) {
            s_traceCursor[k] = game + TRACE_CURSOR_OFF[k];
            s_armAddr[k] = s_traceCursor[k];
            s_armLen[k] = 1;
        }
    }

    s_traceArmed = on;
    RaiseException(ARM_WATCH_CODE, 0, 0, nullptr);

    for (int n = 0; n < 4; ++n) {
        s_watchAddr[n] = s_armAddr[n];
    }

    if (!on) {
        s_tunnelSeenCount = 0xffffffffu; // the tunnel watch re-arms next frame
    }

    return true;
}

static bool simDraw(const TraceHit &h) {
    return h.kind == TRACE_DRAW && h.inSim && !(h.eip & 0x80000000u);
}

static bool sameDraw(const TraceHit &a, const TraceHit &b) {
    return a.worldTick == b.worldTick && a.iter == b.iter && a.eip == b.eip &&
           a.cursor == b.cursor && a.caller[0] == b.caller[0];
}

static void traceLogHit(const char *tag, uint32_t i, const TraceHit &h) {
    logLine(
        "TRACE %s[%u] tick=%u rel=%u ph=%u it%u cur=%u eip=%08x callers %08x %08x %08x", tag, i,
        h.worldTick, h.rel, h.phase, h.iter, h.cursor, h.eip, h.caller[0], h.caller[1], h.caller[2]
    );
}

static uint32_t traceCount(int buf) {
    const LONG n = s_traceCount[buf];

    if (n > (LONG)TRACE_MAX) {
        return TRACE_MAX;
    }

    return (uint32_t)n;
}

// Every actor, at the entry of the first REC_UPDATES simulation updates of
// both passes: the whole object (0x300 bytes), each action of its stack and
// its last finished action (+0x168), raw, and its cluster when it has one.
static const uint32_t REC_ACTOR_BYTES = 0x300;
static const uint32_t REC_ACTION_BYTES = 0x60;
static const uint32_t REC_ACTIONS = 5; // stack entries 0..3, then the last finished one
static const uint32_t REC_CLUSTER_BYTES = 0xc8;
static const uint32_t ACTOR_CLUSTER_OFF = 0xb0;
static const uint32_t REC_UPDATES = 8;
static const uint32_t MOTHER_RECORDS_MAX = 16000;

struct MotherRecord {
    uint32_t worldTick;
    uint32_t slot;
    uint32_t depth;
    uint32_t actor; // its address, to match a draw's registers
    uint32_t words[REC_ACTOR_BYTES / 4];
    uint32_t actions[REC_ACTIONS][REC_ACTION_BYTES / 4];
    uint32_t clusterWords[REC_CLUSTER_BYTES / 4];
};

static MotherRecord *s_mothers[TRACE_PASSES] = {};
static uint32_t s_motherCount[TRACE_PASSES] = {};
static uint32_t s_motherUpdates[TRACE_PASSES] = {};

static void copyWords(uint32_t *to, uintptr_t from, uint32_t bytes) {
    if (!from || !observerReadable(from, bytes)) {
        return;
    }

    const uint32_t *src = (const uint32_t *)from;

    for (uint32_t k = 0; k < bytes / 4; ++k) {
        to[k] = src[k];
    }
}

static void motherObserve() {
    const int buf = s_traceBuf;

    if (buf < 0 || !s_mothers[buf] || s_motherUpdates[buf] >= REC_UPDATES) {
        return;
    }

    ++s_motherUpdates[buf];
    const uint32_t *slots = nullptr;
    uint32_t count = 0;

    if (!roundTripSlots(ROUND_TRIP_SETS[0], &slots, &count)) {
        return;
    }

    const uint32_t tick = worldTick();

    for (uint32_t i = 0; i < count && s_motherCount[buf] < MOTHER_RECORDS_MAX; ++i) {
        const uintptr_t actor = slots[i];

        if (!actor || !observerReadable(actor, REC_ACTOR_BYTES)) {
            continue;
        }

        MotherRecord &r = s_mothers[buf][s_motherCount[buf]++];
        memset(&r, 0, sizeof(r));
        r.worldTick = tick;
        r.slot = i;
        r.actor = (uint32_t)actor;
        r.depth = actorStackDepth(actor);
        copyWords(r.words, actor, REC_ACTOR_BYTES);

        for (uint32_t d = 0; d < r.depth && d + 1 < REC_ACTIONS; ++d) {
            copyWords(r.actions[d], actorAction(actor, (uint16_t)(1 + d)), REC_ACTION_BYTES);
        }

        copyWords(r.actions[REC_ACTIONS - 1], actorAction(actor, SUB_LAST), REC_ACTION_BYTES);
        copyWords(
            r.clusterWords, *(const uint32_t *)(actor + ACTOR_CLUSTER_OFF), REC_CLUSTER_BYTES
        );
    }
}

static bool motherAlloc() {
    for (int k = 0; k < TRACE_PASSES; ++k) {
        s_mothers[k] = (MotherRecord *)VirtualAlloc(
            NULL, MOTHER_RECORDS_MAX * sizeof(MotherRecord), MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE
        );
        s_motherCount[k] = 0;
        s_motherUpdates[k] = 0;

        if (!s_mothers[k]) {
            return false;
        }
    }

    g_prngGuardEntryObserver = motherObserve;
    return true;
}

static void motherFree() {
    g_prngGuardEntryObserver = nullptr;

    for (int k = 0; k < TRACE_PASSES; ++k) {
        if (s_mothers[k]) {
            VirtualFree(s_mothers[k], 0, MEM_RELEASE);
            s_mothers[k] = nullptr;
        }
    }
}

// Both buffers next to the log, for offline analysis: u32 count, then records.
static void traceDump() {
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = n && n < MAX_PATH ? strrchr(path, '\\') : nullptr;

    if (!slash || (slash - path) + sizeof("\\seek_mothers_restored2.bin") > MAX_PATH) {
        return;
    }

    static const char *const NAMES[3] = {
        "seek_trace_live.bin", "seek_trace_restored.bin", "seek_trace_restored2.bin"
    };
    static const char *const MOTHER_NAMES[3] = {
        "seek_mothers_live.bin", "seek_mothers_restored.bin", "seek_mothers_restored2.bin"
    };

    for (int k = 0; k < TRACE_PASSES; ++k) {
        strcpy(slash + 1, NAMES[k]);
        FILE *f = fopen(path, "wb");

        if (!f) {
            continue;
        }

        const uint32_t count = traceCount(k);
        const uint32_t size = sizeof(TraceHit);
        fwrite(&size, 4, 1, f);
        fwrite(&count, 4, 1, f);
        fwrite(s_trace[k], sizeof(TraceHit), count, f);
        fclose(f);

        if (!s_mothers[k]) {
            continue;
        }

        strcpy(slash + 1, MOTHER_NAMES[k]);
        f = fopen(path, "wb");

        if (!f) {
            continue;
        }

        const uint32_t msize = sizeof(MotherRecord);
        fwrite(&msize, 4, 1, f);
        fwrite(&s_motherCount[k], 4, 1, f);
        fwrite(s_mothers[k], sizeof(MotherRecord), s_motherCount[k], f);
        fclose(f);
    }
}

static void traceCompare() {
    const uint32_t na = traceCount(0);
    const uint32_t nb = traceCount(1);
    const TraceHit *a = s_trace[0], *b = s_trace[1];
    logLine(
        "TRACE done job=%u live=%u restored=%u dropped=%ld/%ld", s_traceJob, na, nb,
        (long)s_traceDropped[0], (long)s_traceDropped[1]
    );
    traceDump();

    // Simulation draws only, from the first world tick both recorded one in.
    uint32_t i = 0, j = 0, fromA = 0, fromB = 0;

    while (fromA < na && !simDraw(a[fromA])) {
        ++fromA;
    }

    while (fromB < nb && !simDraw(b[fromB])) {
        ++fromB;
    }

    if (fromA == na || fromB == nb) {
        return;
    }

    uint32_t from = a[fromA].worldTick;

    if (b[fromB].worldTick > from) {
        from = b[fromB].worldTick;
    }

    logLine(
        "TRACE first sim draw tick live=%u restored=%u, comparing from tick %u", a[fromA].worldTick,
        b[fromB].worldTick, from
    );
    i = fromA;
    j = fromB;

    for (;;) {
        while (i < na && (!simDraw(a[i]) || a[i].worldTick < from)) {
            ++i;
        }

        while (j < nb && (!simDraw(b[j]) || b[j].worldTick < from)) {
            ++j;
        }

        if (i == na || j == nb || !sameDraw(a[i], b[j])) {
            break;
        }

        ++i;
        ++j;
    }

    if (i == na || j == nb) {
        logLine("TRACE identical simulation draws from tick %u to the end of a trace", from);
        return;
    }

    logLine("TRACE first difference at tick %u / %u", a[i].worldTick, b[j].worldTick);
    uint32_t shown = 0;

    for (uint32_t k = i >= 12 ? i - 12 : 0; k < na && shown < 30; ++k) {
        if (a[k].kind != TRACE_DRAW || a[k].inSim) {
            traceLogHit(a[k].kind == TRACE_DRAW ? "live" : "live-mark", k, a[k]);
            ++shown;
        }
    }

    shown = 0;

    for (uint32_t k = j >= 12 ? j - 12 : 0; k < nb && shown < 30; ++k) {
        if (b[k].kind != TRACE_DRAW || b[k].inSim) {
            traceLogHit(b[k].kind == TRACE_DRAW ? "restored" : "restored-mark", k, b[k]);
            ++shown;
        }
    }
}

static void traceFree() {
    motherFree();

    for (int k = 0; k < TRACE_PASSES; ++k) {
        if (s_trace[k]) {
            VirtualFree(s_trace[k], 0, MEM_RELEASE);
            s_trace[k] = nullptr;
        }
    }
}

static void traceFrame() {
    const int phase = s_tracePhase;

    if (phase == TRACE_OFF) {
        return;
    }

    uint32_t target = 0;

    if (phase == TRACE_WAIT) {
        // Requested early: wait, without counting, for the job to aim at.
        if (currentRel() < TRACE_START_JOB) {
            return;
        }

        if (replayNetQueuedCommands() != 0 || replayPendingPlacements() != 0 ||
            replayScreenPopupOn() || replaySeekerActive(&target)) {
            if (++s_traceWait >= QUIET_WAIT_FRAMES) {
                s_tracePhase = TRACE_OFF;
                logLine("TRACE gave up waiting for a quiet job");
            }

            return;
        }

        stopTurbo("trace");

        for (int k = 0; k < TRACE_PASSES; ++k) {
            s_trace[k] = (TraceHit *)VirtualAlloc(
                NULL, TRACE_MAX * sizeof(TraceHit), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
            );
            s_traceCount[k] = 0;
            s_traceDropped[k] = 0;
        }

        uint32_t job = 0;

        if (!s_trace[0] || !s_trace[TRACE_PASSES - 1] || !motherAlloc() ||
            !replayKeyframesCaptureNow(&job)) {
            logLine("TRACE refused: no keyframe or no memory");
            traceFree();
            s_tracePhase = TRACE_OFF;
            return;
        }

        s_traceJob = job;

        if (!traceArm(true)) {
            traceFree();
            s_tracePhase = TRACE_OFF;
            return;
        }

        s_tracePhase = TRACE_LIVE;
        s_traceBuf = 0;
        logLine("TRACE live from job=%u for %u jobs", job, TRACE_JOBS);
        return;
    }

    if (currentRel() < s_traceJob + TRACE_JOBS) {
        return;
    }

    if (phase == TRACE_LIVE || (phase == TRACE_RESTORED && s_traceBuf + 1 < TRACE_PASSES)) {
        // As the seeker does: a load keeps the lockstep queue, so a command
        // queued here would run in the restored world too.
        if (replayNetQueuedCommands() != 0 || replayPendingPlacements() != 0 ||
            replayScreenPopupOn()) {
            return;
        }

        traceArm(false);
        const int next = s_traceBuf + 1;
        s_traceBuf = next;
        s_tracePhase = TRACE_WAIT;
        traceMarker(TRACE_LOAD_BEGIN, 0);
        bool loaded = false;
        const bool ok = replayKeyframeRestore(s_traceJob, &loaded);
        traceMarker(TRACE_LOAD_END, readCursor(ITER0_CURSOR_OFF));

        if (!ok || !traceArm(true)) {
            logLine("TRACE restore failed ok=%d loaded=%d", ok ? 1 : 0, loaded ? 1 : 0);
            s_traceBuf = -1;
            traceArm(false);
            traceFree();
            s_tracePhase = TRACE_OFF;
            return;
        }

        s_tracePhase = TRACE_RESTORED;
        uint32_t saved = 0, foreign = 0, rebuilt = 0, skipped = 0;
        replayKeyframesDebugChains(&saved, &foreign, &rebuilt, &skipped);
        uint32_t cSaved = 0, cRestored = 0, cSkipped = 0;
        replayKeyframesDebugClusters(&cSaved, &cRestored, &cSkipped);
        logLine("TRACE clusters saved=%u restored=%u skipped=%u", cSaved, cRestored, cSkipped);
        logLine(
            "TRACE restored job=%u pass=%d, recording again; chains saved=%u foreign=%u "
            "rebuilt=%u skipped=%u",
            s_traceJob, next, saved, foreign, rebuilt, skipped
        );
        return;
    }

    traceArm(false);
    s_traceBuf = -1;
    s_tracePhase = TRACE_OFF;
    traceCompare();
    traceFree();
    MessageBeep(MB_ICONASTERISK); // the tester can stop now
}

static void probeTick() {
    // Formatting, file and engine calls below; the frame tick is mid-function.
    ObserverFpScope fp;

    if (!s_log) {
        openLog();
    }

    if (!s_crashFilterInstalled) {
        s_crashFilterInstalled = true;
        s_prevCrashFilter = SetUnhandledExceptionFilter(probeCrashFilter);
    }

    drainPrints();
    drainDraws();

    // So a copy taken mid-session never ends in a torn line.
    if (s_log) {
        fflush(s_log);
    }

    ++s_frame;
    tunnelWatchFrame();
    keyframeLog();
    trackRuns();
    verifyLog();
    turboFrame();
    skipFrame();
    maintainWatch();
    // Runs without focus: a trace in progress finishes and writes its files
    // even when the player switches windows.
    if (replayInGame()) {
        traceFrame();
    }

    const bool focused = gameWindowFocused();
    const bool watchKey = pressed(KEY_WATCH);
    const bool turboKey = pressed(KEY_TURBO);
    const bool keyframeKey = pressed(KEY_KEYFRAME);
    const bool restartKey = pressed(KEY_RESTART);
    const bool keyframesKey = pressed(KEY_KEYFRAMES);

    if (!focused || !spectatorGuardActive() || !replayInGame()) {
        return;
    }

    // F10 toggles automatic capture; Ctrl+F10 how keyframe files hold path
    // progress (raw float bits, or stock with the live value put back);
    // Shift+F10 runs the round trip.
    if (keyframesKey) {
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

        if (shift && ctrl) {
            if (s_tracePhase == TRACE_OFF) {
                s_tracePhase = TRACE_WAIT;
                s_traceWait = 0;
                logLine("TRACE requested run=%u rel=%u", s_run, currentRel());
            }
        } else if (shift) {
            s_roundTripPending = true;
            s_roundTripWait = 0;
            logLine("ROUNDTRIP requested run=%u rel=%u", s_run, currentRel());
        } else if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
            keyframeSerialSetRawPath(!keyframeSerialRawPath());
            logLine("KEYFRAMES path progress %s", keyframeSerialRawPath() ? "raw" : "stock");
        } else {
            replayKeyframesSetEnabled(!replayKeyframesEnabled());
            logLine("KEYFRAMES automatic capture %s", replayKeyframesEnabled() ? "on" : "off");
        }
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
            saveKeyframe((GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0);
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
    roundTripFrame();
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
