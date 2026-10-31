#include "replaySession.h"

#include "../core/config.h"
#include "../core/exeBase.h"
#include "../core/frameTick.h"
#include "../core/gameWString.h"
#include "../core/hook.h"
#include "../patches/endgameStats/killTracker.h"
#include "prngGuard.h"
#include "replayCommon.h"
#include "replayFormat.h"
#include "replayPlayer.h"
#include "replayRecorder.h"
#include "replayRetime.h"
#include "replayTrace.h"
#include "replayViewer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <windows.h>

// See replaySession.h for the contract. Ghidra VAs use ImageBase 0x400000;
// everything below is an RVA applied to the live module base.

// void __thiscall World::LoadGame(std::wstring, int), RVA 0x2ac60 (callers:
// main-menu save list 0x67d4bc, pause-menu load 0x65ec...).
// Stock MSVC prologue `push ebp; mov ebp,esp; push -1` (55 8b ec 6a ff),
// re-emitted byte-exactly by the trampoline. The by-value wstring occupies
// [esp+4, esp+0x20) at entry.
static const uintptr_t LOAD_GAME_RVA = 0x2ac60;
static const uint8_t LOAD_GAME_STOCK[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};

// Periodic sidecar rewrite. The whole file is rebuilt each time, so the
// interval grows by FLUSH_MS per FLUSH_SCALE_BYTES already written (10 s up to
// 4 MB, 40 s at the 16 MB of a two-hour 8-player match).
static const DWORD FLUSH_MS = 10000;
static const uint32_t FLUSH_SCALE_BYTES = 4u * 1024u * 1024u;
static const DWORD FLUSH_MAX_MS = 60000;
static const uint32_t ARM_DELAY_FRAMES = 30;

// Headroom left in each recording buffer when recordingFull() stops the
// recording: far more than one frame's jobs can add.
static const uint32_t HEADROOM_EVENTS = 4096;
static const uint32_t HEADROOM_LOG_BYTES = 512u * 1024u;
static const uint32_t HEADROOM_PRNG_JOBS = 4096;

// A replay that ended on a victory/defeat screen plays into the game's own
// screen instead of holding at endRelative. The hold stays as a fallback in
// case the outcome does not reproduce, this many jobs past the recorded end
// (the screen activates a job or so after the last recorded job).
static const uint32_t OUTCOME_HOLD_SLACK = 64;

// ── State ───────────────────────────────────────────────────────────────────

enum SessionMode : uint32_t {
    SESSION_IDLE = 0,
    SESSION_RECORDING = 1,
    SESSION_PLAYBACK = 2,
};

// Off until mid-match checkpoints are validated across more live matches
// (docs/features/multiplayer-replays.md, Known Issues).
static bool s_recordMp = false;
static bool s_recordSp = false;
static int s_maxReplays = REPLAY_KEEP_DEFAULT;
static uint32_t s_mode = SESSION_IDLE;
static bool s_prevInGame = false;
// Every replay module installed: recording is refused otherwise, because a
// recording that this build could not play back is worse than none.
static bool s_canRecord = false;

// Recording.
static wchar_t s_savePath[MAX_PATH] = L"";
static wchar_t s_sidecarPath[MAX_PATH] = L"";
static bool s_armConfirmed = false;
static bool s_exitWritten = false;
static uint64_t s_checkpointHash = 0;
static uint32_t s_checkpointSize = 0;
static uint64_t s_recordedUtc = 0;
static uint32_t s_mpPhase = 0;
static uint32_t s_netMode = 0;
static DWORD s_lastFlush = 0;
// The writer lock (see "Periodic writes on a worker thread").
static volatile LONG s_writing = 0;
static volatile LONG s_lastFlushBytes = 0;

// Playback.
static uint32_t s_endRelative = 0;
static uint32_t s_playEndReason = 0; // the sidecar's REPLAY_END_*

// Victory/defeat screen activation (replaySessionNoteOutcome), 1 win 2 lose.
// The screen hook only latches it; the frame tick acts on it.
static volatile LONG s_pendingOutcome = 0;
// The victory/defeat screen clears World+4, and its OnActivate is not ordered
// against our frame tick, so a fall of World+4 waits this many frames for the
// outcome before it is treated as a plain exit. 0 = no fall pending.
static const uint32_t OUTCOME_GRACE_FRAMES = 30;
static uint32_t s_fallFrames = 0;

// ── Small helpers ───────────────────────────────────────────────────────────

static void exeIdentity(uint32_t *imageSize, uint32_t *timeStamp) {
    const uintptr_t base = exeBase();
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    *imageSize = nt->OptionalHeader.SizeOfImage;
    *timeStamp = nt->FileHeader.TimeDateStamp;
}

static uint64_t nowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
}

// FNV-1a 64 of a whole file; false when it cannot be read. Game thread only.
static bool hashFile(const wchar_t *path, uint64_t *hash, uint32_t *size) {
    HANDLE f = CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL
    );

    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }

    static uint8_t chunk[65536];
    uint64_t h = 14695981039346656037ull;
    uint32_t total = 0;
    DWORD got = 0;
    bool ok = true;

    for (;;) {
        if (!ReadFile(f, chunk, sizeof(chunk), &got, NULL)) {
            ok = false;
            break;
        }

        if (!got) {
            break;
        }

        for (DWORD i = 0; i < got; ++i) {
            h ^= chunk[i];
            h *= 1099511628211ull;
        }

        total += got;
    }

    CloseHandle(f);
    *hash = h;
    *size = total;
    return ok && total > 0;
}

static bool endsWithSh2(const wchar_t *path) {
    const size_t n = wcslen(path);
    return n > 4 && path[n - 4] == L'.' && (path[n - 3] | 0x20) == L's' &&
           (path[n - 2] | 0x20) == L'h' && path[n - 1] == L'2';
}

// `x.sh2` -> `x.s2replay`. Deliberately not `.sh2...`: with 8.3 names a
// `*.sh2` search would also match `x.sh2replay` and list it as a save.
static bool sidecarFor(const wchar_t *save, wchar_t *out, size_t cap) {
    static const wchar_t EXT[] = L".s2replay";
    const size_t n = wcslen(save);

    if (!endsWithSh2(save) || n - 4 + (sizeof(EXT) / sizeof(EXT[0])) > cap) {
        return false;
    }

    memcpy(out, save, (n - 4) * sizeof(wchar_t));
    memcpy(out + n - 4, EXT, sizeof(EXT));
    return true;
}

// <Documents>\Stronghold 2\Saves, resolved through shell32 at runtime (the DLL
// imports only kernel32/user32/gdi32, and shell32 is already in the process).
static bool savesDirectory(wchar_t *out, size_t cap) {
    typedef HRESULT(WINAPI * GetFolderFn)(HWND, int, HANDLE, DWORD, LPWSTR);
    HMODULE shell = GetModuleHandleA("shell32.dll");

    if (!shell) {
        shell = LoadLibraryA("shell32.dll");
    }

    GetFolderFn getFolder =
        shell ? (GetFolderFn)(void *)GetProcAddress(shell, "SHGetFolderPathW") : nullptr;
    wchar_t docs[MAX_PATH];

    if (!getFolder || getFolder(NULL, 5 /* CSIDL_PERSONAL */, NULL, 0, docs) != 0) {
        return false;
    }

    static const wchar_t TAIL[] = L"\\Stronghold 2\\Saves";
    const size_t n = wcslen(docs);

    if (n + sizeof(TAIL) / sizeof(TAIL[0]) > cap) {
        return false;
    }

    memcpy(out, docs, n * sizeof(wchar_t));
    memcpy(out + n, TAIL, sizeof(TAIL));
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

// 32 random bits from the OS generator (advapi32!SystemFunction036, resolved
// at runtime so the import table does not grow), else a mix of clocks and ids.
static uint32_t randomWord() {
    typedef BOOLEAN(WINAPI * GenRandomFn)(PVOID, ULONG);
    HMODULE advapi = LoadLibraryA("advapi32.dll");
    GenRandomFn gen =
        advapi ? (GenRandomFn)(void *)GetProcAddress(advapi, "SystemFunction036") : nullptr;
    uint32_t value = 0;

    if (gen && gen(&value, sizeof(value))) {
        return value;
    }

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const uint64_t mix[] = {
        (uint64_t)counter.QuadPart, nowFileTime(), GetTickCount(),
        (uint64_t)GetCurrentProcessId() << 32 | GetCurrentThreadId(), (uint64_t)(uintptr_t)&value
    };
    return replayFnv32(REPLAY_FNV32_SEED, mix, sizeof(mix));
}

// Identifies replays this install recorded, so retention never deletes one that
// was copied in: a received file keeps its sender's write time and would look
// oldest. Random, generated once and kept in [replay] RecorderId; when the ini
// cannot be written it lasts for this process only, which errs toward keeping
// files. Never 0. Game thread only.
static uint32_t localRecorderId() {
    static uint32_t s_id = 0;

    if (s_id) {
        return s_id;
    }

    char text[16] = "";

    if (configString("replay", "RecorderId", text, sizeof(text))) {
        s_id = (uint32_t)strtoul(text, nullptr, 16);
    }

    if (!s_id) {
        do {
            s_id = randomWord();
        } while (!s_id);

        snprintf(text, sizeof(text), "%08lx", (unsigned long)s_id);
        configSetString("replay", "RecorderId", text);
    }

    return s_id;
}

// `replay-1.31.26-905pm-k3`: local date and 12-hour time, plus two random
// base-36 characters so two matches started in the same minute never collide.
static bool newReplayPath(wchar_t *out, size_t cap) {
    wchar_t dir[MAX_PATH];

    if (!savesDirectory(dir, MAX_PATH)) {
        return false;
    }

    SYSTEMTIME t;
    GetLocalTime(&t);
    const int hour12 = t.wHour % 12 == 0 ? 12 : t.wHour % 12;
    static const wchar_t DIGITS[] = L"0123456789abcdefghijklmnopqrstuvwxyz";
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    uint32_t seed = (uint32_t)counter.QuadPart ^ GetTickCount() ^ (GetCurrentProcessId() << 16);

    for (int attempt = 0; attempt < 64; ++attempt) {
        seed = seed * 1664525u + 1013904223u;
        const wchar_t a = DIGITS[(seed >> 8) % 36], b = DIGITS[(seed >> 20) % 36];
        _snwprintf(
            out, cap, L"%ls\\replay-%u.%u.%02u-%u%02u%ls-%lc%lc.sh2", dir, t.wMonth, t.wDay,
            t.wYear % 100, hour12, t.wMinute, t.wHour < 12 ? L"am" : L"pm", a, b
        );
        out[cap - 1] = 0;
        wchar_t side[MAX_PATH];

        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES && sidecarFor(out, side, MAX_PATH) &&
            GetFileAttributesW(side) == INVALID_FILE_ATTRIBUTES) {
            return true;
        }
    }

    return false;
}

// Keeps at most `keep` of this install's replays in Saves, deleting the oldest
// first. A replay is a `replay-*.s2replay` sidecar and the `.sh2` save beside
// it, and only one whose header carries localRecorderId() is a candidate: a
// replay someone sent, an older-format one, or anything unreadable is never
// touched. Age is the header's recording time, not a file time, which a copy
// preserves. Nothing without a sidecar is ever touched either, so an ordinary
// save that happens to be named `replay-...` is safe.
static void pruneReplays(int keep) {
    wchar_t dir[MAX_PATH];

    if (keep < 0 || !savesDirectory(dir, MAX_PATH) || wcslen(dir) + 40 > MAX_PATH) {
        return;
    }

    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\replay-*.s2replay", dir);
    pattern[MAX_PATH - 1] = 0;

    struct Entry {
        wchar_t name[MAX_PATH];
        uint64_t age;
    };
    static const int MAX_ENTRIES = 512;
    Entry *entries = (Entry *)VirtualAlloc(
        NULL, MAX_ENTRIES * sizeof(Entry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    );

    if (!entries) {
        return;
    }

    const uint32_t me = localRecorderId();
    int count = 0;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);

    if (find == INVALID_HANDLE_VALUE) {
        VirtualFree(entries, 0, MEM_RELEASE);
        return;
    }

    do {
        const size_t n = wcslen(fd.cFileName);
        static const size_t EXT = 9; // ".s2replay"

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n <= EXT ||
            _wcsicmp(fd.cFileName + n - EXT, L".s2replay") != 0 || count >= MAX_ENTRIES ||
            wcslen(dir) + n + 2 > MAX_PATH) {
            continue;
        }

        wchar_t side[MAX_PATH];
        _snwprintf(side, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        side[MAX_PATH - 1] = 0;
        HANDLE f = CreateFileW(
            side, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL
        );

        if (f == INVALID_HANDLE_VALUE) {
            continue;
        }

        ReplayBundleHeader h = {};
        DWORD got = 0;
        const bool read = ReadFile(f, &h, sizeof(h), &got, NULL) && got == sizeof(h);
        CloseHandle(f);

        if (!read || memcmp(h.magic, "S2REPLAY", 8) != 0 || h.headerSize != sizeof(h) ||
            !(h.flags & REPLAY_BUNDLE_RECORDER_ID) || h.reserved[1] != me) {
            continue;
        }

        Entry &e = entries[count];
        memcpy(e.name, fd.cFileName, (n - EXT) * sizeof(wchar_t));
        e.name[n - EXT] = 0;
        e.age = h.recordedUtc;
        ++count;
    } while (FindNextFileW(find, &fd));

    FindClose(find);

    while (count > keep) {
        int oldest = 0;

        for (int i = 1; i < count; ++i) {
            if (entries[i].age < entries[oldest].age) {
                oldest = i;
            }
        }

        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%ls\\%ls.sh2", dir, entries[oldest].name);
        path[MAX_PATH - 1] = 0;
        DeleteFileW(path);
        _snwprintf(path, MAX_PATH, L"%ls\\%ls.s2replay", dir, entries[oldest].name);
        path[MAX_PATH - 1] = 0;
        DeleteFileW(path);
        entries[oldest] = entries[--count];
    }

    VirtualFree(entries, 0, MEM_RELEASE);
}

// ── Recording buffers ───────────────────────────────────────────────────────

static void releaseRecordingBuffers() {
    replayRecorderRelease();
    replayTraceRelease();
    prngGuardReleaseRecord();
    replayRetimeRelease();
}

// All or none: the command log, the execution witness, the iterator-1 record
// and the retimer's working storage.
static bool acquireRecordingBuffers() {
    if (replayRecorderAcquire() && replayTraceAcquire() && prngGuardAcquireRecord() &&
        replayRetimeAcquire()) {
        return true;
    }

    releaseRecordingBuffers();
    return false;
}

// ── Sidecar writer ──────────────────────────────────────────────────────────

// Everything a sidecar is built from, taken on the game thread. The recording
// buffers are append-only and their counts are published after the data they
// cover, so a writer on another thread that reads only up to these counts
// never sees a torn entry. (The last iterator-1 entry's render-gate byte and
// later-update cursors can still grow while that job's later updates run; a
// periodic sidecar may lack them, the final one never does.)
struct BundleInput {
    ReplayRecorderLogView log;
    const ReplayBoundaryEntry *boundaries;
    uint32_t boundaryCount, boundaryDropped, hooks;
    const ReplayPrngEntry *prng;
    uint32_t prngCount, prngDropped;
    const uint8_t *gates;
    const ReplayPrngUpdates *updates;
    const ReplayMonasteryClock *clocks;
    uint32_t clockCount;
    const uint32_t *tile;
    uint32_t tileWords, tileLists;
    bool haveTile;
    const ReplayFlyField *fly;
    uint32_t flyCount;
    bool haveFly;
    uint32_t now;
    uint32_t iter0Before, iter0After;
    bool complete;
    uint32_t endReason;
    uint64_t checkpointHash;
    uint32_t checkpointSize, mpPhase, netMode;
    uint64_t recordedUtc;
    uint32_t recorderId;
    wchar_t sidecarPath[MAX_PATH];
};

static void takeSnapshot(bool complete, uint32_t endReason, BundleInput *in) {
    replayRecorderLogView(&in->log);
    replayTraceBoundaryView(&in->boundaries, &in->boundaryCount, &in->boundaryDropped, &in->hooks);
    prngGuardRecordView(&in->prng, &in->prngCount, &in->prngDropped);
    in->gates = prngGuardRecordGates();
    in->updates = prngGuardRecordUpdates();
    prngGuardRecordClocks(&in->clocks, &in->clockCount);
    in->haveTile = replayTileOrderView(&in->tile, &in->tileWords, &in->tileLists);
    in->haveFly = replayFlyStateView(&in->fly, &in->flyCount);
    in->now = replayTraceCurrentSimulationTick();
    replayRecorderIter0AtSave(&in->iter0Before, &in->iter0After);
    in->complete = complete;
    in->endReason = endReason;
    in->checkpointHash = s_checkpointHash;
    in->checkpointSize = s_checkpointSize;
    in->mpPhase = s_mpPhase;
    in->netMode = s_netMode;
    in->recordedUtc = s_recordedUtc;
    in->recorderId = localRecorderId();
    memcpy(in->sidecarPath, s_sidecarPath, sizeof(in->sidecarPath));
}

static uint32_t minLimit(uint32_t limit, uint32_t tick) {
    return !limit || tick < limit ? tick : limit;
}

struct BundleWriter {
    HANDLE file;
    uint32_t hash;
    uint32_t bytes;
    bool ok;
    uint8_t stage[65536];
    uint32_t staged;
};
static BundleWriter s_writer;
static ReplayRetimeResult s_retime;

static void writerFlushStage(BundleWriter &w) {
    if (!w.staged || !w.ok) {
        w.staged = 0;
        return;
    }

    DWORD wrote = 0;
    w.ok = WriteFile(w.file, w.stage, w.staged, &wrote, NULL) && wrote == w.staged;
    w.staged = 0;
}

static void writerPut(BundleWriter &w, const void *data, uint32_t bytes) {
    w.hash = replayFnv32(w.hash, data, bytes);
    w.bytes += bytes;
    const uint8_t *p = (const uint8_t *)data;

    while (bytes && w.ok) {
        uint32_t room = sizeof(w.stage) - w.staged;

        if (!room) {
            writerFlushStage(w);
            continue;
        }

        const uint32_t take = bytes < room ? bytes : room;
        memcpy(w.stage + w.staged, p, take);
        w.staged += take;
        p += take;
        bytes -= take;
    }
}

static void writerSection(BundleWriter &w, const char *tag, uint32_t bytes) {
    const ReplayBundleSection s = {replayTag(tag), bytes};
    writerPut(w, &s, sizeof(s));
}

// Writes the whole sidecar to `<path>.tmp` and renames it over the previous
// one, so a crash mid-write leaves the last good sidecar in place. The caller
// holds the writer lock.
static bool writeBundle(const BundleInput &in) {
    const bool durable = in.complete || in.endReason != REPLAY_END_RECORDING;
    const ReplayRecorderLogView &log = in.log;

    // The last job the sidecar can be exact through, 0 = the whole recording.
    // recordingFull() ends the recording before any buffer fills; these cuts
    // are the fallback, so a lost tail never costs the rest of the match.
    uint32_t limit = 0;

    if (log.dropped) {
        limit = minLimit(
            limit, log.firstDroppedTick > log.epochTick ? log.firstDroppedTick - 1 : log.epochTick
        );
    }

    if (in.prngDropped && in.prngCount) {
        limit = minLimit(limit, log.epochTick + in.prng[in.prngCount - 1].relativeTick);
    }

    if (in.endReason == REPLAY_END_CAPACITY) {
        // Cut before the commands still in flight, so the replay ends exact.
        limit = minLimit(limit, in.now > log.epochTick ? in.now - 1 : log.epochTick);
    }

    replayRetime(
        log.bytes, log.used, log.epochTick, in.boundaries, in.boundaryCount, in.boundaryDropped,
        in.hooks, limit, &s_retime
    );
    const bool truncated = s_retime.endTick != 0;

    ReplayBundleHeader h = {};
    memcpy(h.magic, "S2REPLAY", 8);
    h.version = 1;
    h.headerSize = sizeof(h);
    h.endReason = in.endReason;
    h.checkpointHash = in.checkpointHash;
    h.checkpointSize = in.checkpointSize;
    h.mpPhase = in.mpPhase;
    h.netMode = in.netMode;
    h.classCount = log.classCount;
    h.epochTick = log.epochTick;
    // The last job the simulation actually ran. The job counter keeps advancing
    // under a single-player pause (ESC menu) while nothing is simulated, so it
    // overshoots; the iterator-1 record is one entry per simulated job.
    h.endRelative = in.prngCount ? in.prng[in.prngCount - 1].relativeTick
                                 : (in.now > log.epochTick ? in.now - log.epochTick : 0);

    if (truncated) {
        const uint32_t end =
            s_retime.endTick > log.epochTick ? s_retime.endTick - log.epochTick : 0;
        h.endRelative = end < h.endRelative ? end : h.endRelative;
        h.cutCommands = s_retime.cutEvents;
    }

    h.commandCount = s_retime.events;
    h.injectedCount = s_retime.injected;
    h.retimedCount = s_retime.retimed;
    h.unretimedCount = s_retime.unretimed;
    h.execPlans = s_retime.planJobs;
    exeIdentity(&h.exeImageSize, &h.exeTimeStamp);
    h.recordedUtc = in.recordedUtc;
    h.updatedUtc = nowFileTime();
    const bool prngUsable = in.prngCount && (!in.prngDropped || truncated);
    h.flags = (in.complete ? REPLAY_BUNDLE_COMPLETE : 0) |
              (s_retime.planWords ? REPLAY_BUNDLE_RETIMED : 0) |
              (in.netMode == 2 ? REPLAY_BUNDLE_MP_TIMING : 0) |
              (in.haveTile ? REPLAY_BUNDLE_TILE_ORDER : 0) | (prngUsable ? REPLAY_BUNDLE_PRNG : 0) |
              (truncated ? REPLAY_BUNDLE_TRUNCATED : 0);

    if (in.iter0Before <= 0xffff && in.iter0After <= 0xffff) {
        h.flags |= REPLAY_BUNDLE_ITER0_AT_SAVE;
        h.reserved[0] = in.iter0Before | (in.iter0After << 16);
    }

    h.flags |= REPLAY_BUNDLE_RECORDER_ID;
    h.reserved[1] = in.recorderId;

    wchar_t tmp[MAX_PATH + 8];
    const size_t n = wcslen(in.sidecarPath);
    memcpy(tmp, in.sidecarPath, n * sizeof(wchar_t));
    memcpy(tmp + n, L".tmp", 5 * sizeof(wchar_t));
    BundleWriter &w = s_writer;
    w.file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    w.ok = w.file != INVALID_HANDLE_VALUE;
    w.hash = REPLAY_FNV32_SEED;
    w.bytes = 0;
    w.staged = 0;

    if (w.ok) {
        DWORD wrote = 0;
        w.ok = WriteFile(w.file, &h, sizeof(h), &wrote, NULL) && wrote == sizeof(h);
    }

    // CMDS: the command log in playback order.
    ReplayLogFileHeader ch = {};
    memcpy(ch.magic, "SH2RCMD1", 8);
    ch.version = 2;
    ch.recordSize = sizeof(ReplayEventRecord);
    ch.eventCount = s_retime.events;
    ch.byteCount = s_retime.bytes;
    ch.firstTick = s_retime.firstTick;
    ch.lastTick = s_retime.lastTick;
    // A truncated log is complete up to its end: the arena's losses are past it.
    ch.droppedEvents = truncated ? 0 : log.dropped;
    ch.classCount = log.classCount;
    ch.epochTick = log.epochTick;
    uint32_t sections = 1;
    writerSection(w, "CMDS", sizeof(ch) + s_retime.bytes);
    writerPut(w, &ch, sizeof(ch));

    for (uint32_t i = 0; i < s_retime.events && w.ok; ++i) {
        const ReplayRetimeRef &ref = replayRetimeRefs()[i];
        ReplayEventRecord e;
        memcpy(&e, log.bytes + ref.offset, sizeof(e));
        e.simulationTick = ref.newTick;
        writerPut(w, &e, sizeof(e));
        writerPut(w, log.bytes + ref.offset + sizeof(e), e.payloadLength);
    }

    if (prngUsable) {
        ReplayPrngFileHeader ph = {};
        memcpy(ph.magic, "SH2RPRN", 8);
        ph.version = REPLAY_PRNG_VERSION;
        ph.entrySize = sizeof(ReplayPrngEntry);
        ph.entryCount = in.prngCount;
        ph.epochTick = log.epochTick;
        ph.firstRelative = in.prng[0].relativeTick;
        ph.lastRelative = in.prng[in.prngCount - 1].relativeTick;
        const uint32_t footer[2] = {REPLAY_CLOCK_MAGIC, in.clockCount};
        const uint32_t perEntry = sizeof(ReplayPrngEntry) + 1 + sizeof(ReplayPrngUpdates);
        writerSection(
            w, "PRNG",
            sizeof(ph) + in.prngCount * perEntry + sizeof(footer) +
                in.clockCount * sizeof(ReplayMonasteryClock)
        );
        writerPut(w, &ph, sizeof(ph));
        writerPut(w, in.prng, in.prngCount * sizeof(ReplayPrngEntry));
        writerPut(w, in.gates, in.prngCount);
        writerPut(w, in.updates, in.prngCount * sizeof(ReplayPrngUpdates));
        writerPut(w, footer, sizeof(footer));
        writerPut(w, in.clocks, in.clockCount * sizeof(ReplayMonasteryClock));
        ++sections;
    }

    if (in.haveTile) {
        const uint32_t th[3] = {REPLAY_TILE_MAGIC, 1, in.tileLists};
        writerSection(w, "TILE", sizeof(th) + in.tileWords * 4);
        writerPut(w, th, sizeof(th));
        writerPut(w, in.tile, in.tileWords * 4);
        ++sections;
    }

    if (in.haveFly && in.flyCount) {
        const uint32_t fh[3] = {REPLAY_FLY_MAGIC, 1, in.flyCount};
        writerSection(w, "FLYS", sizeof(fh) + in.flyCount * sizeof(ReplayFlyField));
        writerPut(w, fh, sizeof(fh));
        writerPut(w, in.fly, in.flyCount * sizeof(ReplayFlyField));
        ++sections;
    }

    if (s_retime.planJobs) {
        writerSection(w, "EXOR", s_retime.planWords * 4);
        writerPut(w, replayRetimePlanWords(), s_retime.planWords * 4);
        ++sections;
    }

    writerFlushStage(w);

    if (w.ok) {
        h.sectionCount = sections;
        h.bodyBytes = w.bytes;
        h.bodyHash = w.hash;
        DWORD wrote = 0;
        // Only a final write is forced to disk. A periodic one only has to
        // survive the process, and the OS cache does.
        w.ok = SetFilePointer(w.file, 0, NULL, FILE_BEGIN) == 0 &&
               WriteFile(w.file, &h, sizeof(h), &wrote, NULL) && wrote == sizeof(h) &&
               (!durable || FlushFileBuffers(w.file));
    }

    if (w.file != INVALID_HANDLE_VALUE) {
        CloseHandle(w.file);
    }

    const bool ok = w.ok && MoveFileExW(
                                tmp, in.sidecarPath,
                                MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0)
                            );

    if (!ok) {
        DeleteFileW(tmp);
    } else {
        InterlockedExchange(&s_lastFlushBytes, (LONG)(sizeof(h) + w.bytes));
    }

    return ok;
}

// ── Periodic writes on a worker thread ──────────────────────────────────────
//
// A periodic rewrite retimes the whole log, sorts up to 262k commands and
// writes up to 16 MB, and in lockstep a long game-thread frame stalls every
// peer. So the game thread only takes the snapshot, and a worker builds and
// writes the file.
//
// s_writing is the one lock over the sidecar writer, its working storage and
// the recording buffers' lifetime. A periodic flush takes it on the game thread
// and the worker drops it when the file is written; a final write, freeing the
// buffers, and the crash filter all take it first, so none of them can overlap
// a write in progress.

static BundleInput s_job;
static HANDLE s_workerWake = NULL;

static bool tryLockWriter() { return InterlockedCompareExchange(&s_writing, 1, 0) == 0; }

static void lockWriter() {
    while (!tryLockWriter()) {
        Sleep(1);
    }
}

static void unlockWriter() { InterlockedExchange(&s_writing, 0); }

static DWORD WINAPI writerThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(s_workerWake, INFINITE);
        writeBundle(s_job);
        unlockWriter();
    }
}

static bool ensureWorker() {
    if (s_workerWake) {
        return true;
    }

    HANDLE wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE thread = wake ? CreateThread(NULL, 0, writerThread, NULL, 0, NULL) : NULL;

    if (!thread) {
        if (wake) {
            CloseHandle(wake);
        }

        return false;
    }

    SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(thread);
    s_workerWake = wake;
    return true;
}

// A periodic snapshot of a match still in progress. False when a write is
// already running; the caller tries again next frame.
static bool flushPeriodic() {
    if (!tryLockWriter()) {
        return false;
    }

    takeSnapshot(false, REPLAY_END_RECORDING, &s_job);

    if (!ensureWorker()) {
        writeBundle(s_job);
        unlockWriter();
        return true;
    }

    SetEvent(s_workerWake); // the worker unlocks
    return true;
}

// A final write on the game thread. The caller holds the writer lock.
static void flushFinalLocked(bool complete, uint32_t endReason) {
    takeSnapshot(complete, endReason, &s_job);
    writeBundle(s_job);
}

// ── Recording lifecycle ─────────────────────────────────────────────────────

static void finishRecording(uint32_t reason) {
    if (s_mode != SESSION_RECORDING) {
        return;
    }

    // A match that ends before its arm fired must not leave the arm for the
    // next match, or for a playback.
    replayRecorderCancelAutoArm();
    lockWriter();

    if (s_armConfirmed) {
        replayRecorderStop();
        flushFinalLocked(true, reason);
    }

    releaseRecordingBuffers();
    s_mode = SESSION_IDLE;
    s_armConfirmed = false;
    unlockWriter();
}

static void beginRecording() {
    if (!s_canRecord || !(replayTraceSessionIsNetworked() ? s_recordMp : s_recordSp)) {
        return;
    }

    if (!newReplayPath(s_savePath, MAX_PATH) || !sidecarFor(s_savePath, s_sidecarPath, MAX_PATH)) {
        return;
    }

    if (!acquireRecordingBuffers()) {
        return;
    }

    if (!replayRecorderAutoArm(s_savePath, ARM_DELAY_FRAMES)) {
        releaseRecordingBuffers();
        return;
    }

    s_armConfirmed = false;
    s_exitWritten = false;
    s_recordedUtc = nowFileTime();
    s_mode = SESSION_RECORDING;
}

// True when a recording buffer is about to fill: the command log, the
// admission/execution witness, the retimer's event table or the iterator-1
// record. The recording then ends cleanly (END_CAPACITY) and the replay keeps
// the match up to here, exactly timed, rather than losing its tail. At the
// rates of a 5-player test match that is past two hours for 8 players.
static bool recordingFull() {
    ReplayRecorderLogView log = {};
    replayRecorderLogView(&log);
    const ReplayBoundaryEntry *b = nullptr;
    uint32_t witness = 0, witnessDropped = 0, hooks = 0;
    replayTraceBoundaryView(&b, &witness, &witnessDropped, &hooks);
    const ReplayPrngEntry *prng = nullptr;
    uint32_t jobs = 0, jobsDropped = 0;
    prngGuardRecordView(&prng, &jobs, &jobsDropped);
    return log.dropped || witnessDropped || jobsDropped ||
           log.events >= REPLAY_MAX_EVENTS - HEADROOM_EVENTS ||
           log.used + HEADROOM_LOG_BYTES > REPLAY_LOG_CAPACITY ||
           witness + 2 * HEADROOM_EVENTS > REPLAY_BOUNDARY_CAPACITY ||
           jobs + HEADROOM_PRNG_JOBS > REPLAY_PRNG_CAPACITY;
}

static void recordingTick() {
    replayRecorderFrame();

    if (!s_armConfirmed) {
        const ReplayArmState arm = replayRecorderArmState();

        if (arm == REPLAY_ARM_PENDING) {
            return;
        }

        if (arm != REPLAY_ARM_ARMED ||
            !hashFile(s_savePath, &s_checkpointHash, &s_checkpointSize)) {
            finishRecording(REPLAY_END_STOPPED); // the checkpoint failed: no replay
            return;
        }

        s_armConfirmed = true;
        replayRecorderEpochTiming(&s_mpPhase, &s_netMode);
        // Only now that this match will produce a replay, so an abandoned arm
        // never costs the oldest one.
        pruneReplays(s_maxReplays - 1);
        s_lastFlush = GetTickCount();
        flushPeriodic();
        return;
    }

    if (!replayRecorderIsRecording()) {
        finishRecording(REPLAY_END_STOPPED);
        return;
    }

    if (recordingFull()) {
        finishRecording(REPLAY_END_CAPACITY);
        return;
    }

    DWORD interval = FLUSH_MS * (1 + (uint32_t)s_lastFlushBytes / FLUSH_SCALE_BYTES);

    if (interval > FLUSH_MAX_MS) {
        interval = FLUSH_MAX_MS;
    }

    if (GetTickCount() - s_lastFlush >= interval && flushPeriodic()) {
        s_lastFlush = GetTickCount();
    }
}

// ── Playback lifecycle ──────────────────────────────────────────────────────

static void clearPlaybackState() {
    replayPlayerUnload();
    prngGuardClearStream();
    replayPlayerClearTileOrder();
    replayPlayerClearExecOrder();
    replayPlayerClearFlyState();
    replayPlayerSetIter0AtSave(0xffffffffu);
}

static void stopPlayback() {
    clearPlaybackState();
    replayViewerSetEnded(false);
    replayViewerSetReplayName(L"");
    s_mode = SESSION_IDLE;
}

static void playbackTick() {
    uint32_t rel = 0;
    const bool outcome = s_playEndReason == REPLAY_END_WIN || s_playEndReason == REPLAY_END_LOSE;
    const uint32_t hold = s_endRelative + (outcome ? OUTCOME_HOLD_SLACK : 0);

    if (s_endRelative && !replayViewerEnded() && replayPlayerPlaybackJob(&rel) && rel >= hold) {
        replayViewerSetEnded(true);
    }
}

// The match ended. Recording: the replay ends here (on a victory/defeat screen,
// right there rather than when the player leaves it). Playback: stop the
// player, which drops the spectator guard, so the end screen, the menu and the
// stat overlay are usable again. `outcome` 0 means no end screen.
static void endMatch(LONG outcome) {
    if (s_mode == SESSION_RECORDING) {
        uint32_t reason = REPLAY_END_LEFT_MATCH;

        if (outcome == 1) {
            reason = REPLAY_END_WIN;
        } else if (outcome == 2) {
            reason = REPLAY_END_LOSE;
        }

        finishRecording(reason);
    } else if (s_mode == SESSION_PLAYBACK) {
        stopPlayback();
    }
}

static uint32_t loadReplayFile(const wchar_t *path, uint8_t **data) {
    HANDLE f = CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL
    );

    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }

    const DWORD size = GetFileSize(f, NULL);
    *data = nullptr;

    if (size != INVALID_FILE_SIZE && size >= sizeof(ReplayBundleHeader)) {
        *data = (uint8_t *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    DWORD got = 0;
    const bool read = *data && ReadFile(f, *data, size, &got, NULL) && got == size;
    CloseHandle(f);

    if (!read && *data) {
        VirtualFree(*data, 0, MEM_RELEASE);
        *data = nullptr;
    }

    return read ? size : 0;
}

// Arms playback from the sidecar beside `savePath`. False for an ordinary save,
// or a sidecar that is refused for any reason; the save then loads untouched.
static bool armReplay(const wchar_t *savePath) {
    wchar_t side[MAX_PATH];
    uint8_t *data = nullptr;
    const uint32_t size = sidecarFor(savePath, side, MAX_PATH) ? loadReplayFile(side, &data) : 0;

    if (!size) {
        return false;
    }

    ReplayBundleHeader h;
    uint32_t imageSize = 0, timeStamp = 0;
    exeIdentity(&imageSize, &timeStamp);
    uint64_t hash = 0;
    uint32_t saveSize = 0;
    const uint8_t *body = data + sizeof(ReplayBundleHeader);
    const uint8_t *sec = nullptr;
    uint32_t secBytes = 0;
    bool armed = false;

    if (replayValidateBundle(data, size, &h) && h.exeImageSize == imageSize &&
        h.exeTimeStamp == timeStamp && hashFile(savePath, &hash, &saveSize) &&
        hash == h.checkpointHash && saveSize == h.checkpointSize &&
        !replayTraceSessionIsNetworked() &&
        replayFindSection(body, h.bodyBytes, h.sectionCount, replayTag("CMDS"), &sec, &secBytes) &&
        replayPlayerLoadBytes(sec, secBytes)) {
        // The optional sections: absent or refused, that state is not restored.
        if (replayFindSection(
                body, h.bodyBytes, h.sectionCount, replayTag("PRNG"), &sec, &secBytes
            )) {
            prngGuardLoadBytes(sec, secBytes);
        }

        if (replayFindSection(
                body, h.bodyBytes, h.sectionCount, replayTag("TILE"), &sec, &secBytes
            )) {
            replayPlayerLoadTileOrderBytes(sec, secBytes);
        }

        if (replayFindSection(
                body, h.bodyBytes, h.sectionCount, replayTag("EXOR"), &sec, &secBytes
            )) {
            replayPlayerLoadExecOrderBytes(sec, secBytes);
        }

        if (replayFindSection(
                body, h.bodyBytes, h.sectionCount, replayTag("FLYS"), &sec, &secBytes
            )) {
            replayPlayerLoadFlyStateBytes(sec, secBytes);
        }

        replayPlayerSetIter0AtSave(
            (h.flags & REPLAY_BUNDLE_ITER0_AT_SAVE) ? h.reserved[0] >> 16 : 0xffffffffu
        );
        const bool mp = (h.flags & REPLAY_BUNDLE_MP_TIMING) != 0;
        armed = replayPlayerControl(mp ? 2 : 1, h.mpPhase & 0xff) == 2;
    }

    if (armed) {
        s_endRelative = h.endRelative;
        s_playEndReason = h.endReason;
    } else {
        clearPlaybackState();
    }

    VirtualFree(data, 0, MEM_RELEASE);
    return armed;
}

// Replay name for the viewer title: the file name without directory or `.sh2`.
static void setViewerName(const wchar_t *path) {
    const wchar_t *name = path;

    for (const wchar_t *p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            name = p + 1;
        }
    }

    wchar_t buf[64];
    size_t n = 0;

    for (; name[n] && n + 1 < 64; ++n) {
        buf[n] = name[n];
    }

    buf[n] = 0;

    if (n > 4 && endsWithSh2(buf)) {
        buf[n - 4] = 0;
    }

    replayViewerSetReplayName(buf);
}

extern "C" void replaySessionOnLoadGame(const GameWString *arg) {
    // Whatever was running is being replaced. Close it out first.
    if (s_mode == SESSION_RECORDING) {
        finishRecording(REPLAY_END_REPLACED);
    } else if (s_mode == SESSION_PLAYBACK) {
        stopPlayback();
    }

    // The next in-match frame is a new match even if World+4 never dropped (a
    // load from the pause menu keeps it set).
    s_prevInGame = false;
    s_fallFrames = 0;
    InterlockedExchange(&s_pendingOutcome, 0);
    // Kills are counted only during playback, and every playback starts with a
    // load: a pause-menu load never passes the main menu's reset.
    resetKillCounts();
    const wchar_t *text = gameWStringText(arg);

    if (!text || arg->size == 0 || arg->size >= MAX_PATH) {
        return;
    }

    wchar_t path[MAX_PATH];
    memcpy(path, text, arg->size * sizeof(wchar_t));
    path[arg->size] = 0;
    // The save list may hand over a bare file name; resolve it in Saves.
    wchar_t full[MAX_PATH];

    if (!wcschr(path, L'\\') && !wcschr(path, L'/')) {
        wchar_t dir[MAX_PATH];

        if (savesDirectory(dir, MAX_PATH) && wcslen(dir) + wcslen(path) + 2 < MAX_PATH) {
            _snwprintf(full, MAX_PATH, L"%ls\\%ls", dir, path);
            full[MAX_PATH - 1] = 0;
            memcpy(path, full, sizeof(full));
        }
    }

    if (armReplay(path)) {
        s_mode = SESSION_PLAYBACK;
        setViewerName(path);
        replayViewerSetEnded(false);
    }
}

uintptr_t g_replaySessionLoadReturn = 0;

__declspec(naked) static void loadGameHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     // 32 (pushal) + 4 (pushfl) + 4 (return address)
                     "leal 40(%esp), %eax\n\t"
                     "pushl %eax\n\t"
                     "call _replaySessionOnLoadGame\n\t"
                     "addl $4, %esp\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $-1\n\t"
                     "jmp *_g_replaySessionLoadReturn\n\t");
}

// ── Frame tick ──────────────────────────────────────────────────────────────

static LPTOP_LEVEL_EXCEPTION_FILTER s_prevFilter = nullptr;
static bool s_filterInstalled = false;

// A crash mid-recording still leaves the match up to the crash. This runs the
// full sidecar build -- retiming, a qsort over our own buffers and file I/O --
// inside an unhandled-exception filter, which is deliberate: it touches only
// the recording buffers (no game heap), and the snapshot reads only up to the
// published counts, so a game thread that is still appending cannot tear it.
// What it cannot rule out is a crash that corrupted those buffers, in which
// case the write fails or produces a sidecar that its own hash refuses; the
// previous periodic sidecar then stays on disk. A write already running on
// the worker is left to finish instead.
static LONG WINAPI sessionCrashFilter(EXCEPTION_POINTERS *ep) {
    if (tryLockWriter()) {
        if (s_mode == SESSION_RECORDING && s_armConfirmed) {
            flushFinalLocked(false, REPLAY_END_CRASH);
        }

        unlockWriter();
    }

    return s_prevFilter ? s_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static void sessionTick() {
    // Installed here rather than at DLL attach: the game's CRT startup, and
    // any earlier-registered tick, set theirs first, and this one chains to
    // whatever it replaces.
    if (!s_filterInstalled) {
        s_filterInstalled = true;
        s_prevFilter = SetUnhandledExceptionFilter(sessionCrashFilter);
    }

    if (!replayInGame()) {
        if (s_prevInGame) {
            // Left the match: menu exit, a desync drop to the menu, or the
            // victory/defeat screen. Give the screen's OnActivate a few frames
            // to report which it was.
            s_fallFrames = 1;
        }

        s_prevInGame = false;

        if (s_fallFrames) {
            const LONG outcome = InterlockedExchange(&s_pendingOutcome, 0);

            if (outcome || ++s_fallFrames > OUTCOME_GRACE_FRAMES) {
                endMatch(outcome);
                s_fallFrames = 0;
            }
        }

        return;
    }

    if (s_fallFrames) {
        // Back in a match inside the grace window (a load straight from the
        // menu): close the previous one out as a plain exit first.
        endMatch(0);
        s_fallFrames = 0;
    }

    if (!s_prevInGame) {
        s_prevInGame = true;
        InterlockedExchange(&s_pendingOutcome, 0);

        if (s_mode == SESSION_IDLE) {
            beginRecording();
        }
    }

    const LONG outcome = InterlockedExchange(&s_pendingOutcome, 0);

    if (outcome) {
        endMatch(outcome);
    }

    if (s_mode == SESSION_RECORDING) {
        recordingTick();
    } else if (s_mode == SESSION_PLAYBACK) {
        playbackTick();
    }
}

// ── Public ──────────────────────────────────────────────────────────────────

void replaySessionNoteOutcome(bool won) { InterlockedExchange(&s_pendingOutcome, won ? 1 : 2); }

void replaySessionOnWindowClosing() {
    if (s_mode == SESSION_RECORDING && s_armConfirmed && !s_exitWritten) {
        s_exitWritten = true;
        lockWriter();
        flushFinalLocked(false, REPLAY_END_PROCESS_EXIT);
        unlockWriter();
    }
}

bool replaySessionRecordMultiplayer() { return s_recordMp; }

void replaySessionSetRecordMultiplayer(bool on) { s_recordMp = on; }

int replaySessionMaxReplays() { return s_maxReplays; }

void replaySessionSetMaxReplays(int keep) {
    if (keep < REPLAY_KEEP_MIN) {
        keep = REPLAY_KEEP_MIN;
    } else if (keep > REPLAY_KEEP_MAX) {
        keep = REPLAY_KEEP_MAX;
    }

    s_maxReplays = keep;
}

void installReplaySession() {
    s_recordMp = configInt("replay", "RecordMultiplayer", 0) != 0;
    s_recordSp = configInt("replay", "RecordSinglePlayer", 0) != 0;
    replaySessionSetMaxReplays(configInt("replay", "MaxReplays", REPLAY_KEEP_DEFAULT));

    uint8_t *site = (uint8_t *)(exeBase() + LOAD_GAME_RVA);

    if (memcmp(site, LOAD_GAME_STOCK, sizeof(LOAD_GAME_STOCK)) != 0 || !replayPlayerInstalled() ||
        !replayTraceInstalled() || !prngGuardInstalled()) {
        return; // a replay this build could not play is not recorded either
    }

    if (!registerFrameTick(sessionTick)) {
        return;
    }

    g_replaySessionLoadReturn = (uintptr_t)site + sizeof(LOAD_GAME_STOCK);
    installHook(site, reinterpret_cast<void *>(loadGameHook), sizeof(LOAD_GAME_STOCK));
    s_canRecord = true;
}
