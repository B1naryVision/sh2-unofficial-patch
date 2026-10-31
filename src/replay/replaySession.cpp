#include "replaySession.h"

#include "choreCodec.h"
#include "prngGuard.h"
#include "replayCommon.h"
#include "replayPlayer.h"
#include "replayRecorder.h"
#include "replayRetime.h"
#include "replayTrace.h"
#include "replayViewer.h"
#include "../core/config.h"
#include "../core/frameTick.h"
#include "../core/hook.h"

#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <windows.h>

// See replaySession.h for the contract. Ghidra VAs use ImageBase 0x400000;
// everything below is an RVA applied to the live module base.

static const uintptr_t WORLD_IN_GAME_RVA = 0xd781d0 + 4; // World+4, "a game is running"

// void __thiscall World::LoadGame(std::wstring, int), RVA 0x2ac60 (callers:
// main-menu save list 0x67d4bc, pause-menu load 0x65ec...).
// Stock MSVC prologue `push ebp; mov ebp,esp; push -1` (55 8b ec 6a ff),
// re-emitted byte-exactly by the trampoline. The by-value wstring occupies
// [esp+4, esp+0x20) at entry.
static const uintptr_t LOAD_GAME_RVA = 0x2ac60;
static const uint8_t LOAD_GAME_STOCK[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};

// Periodic sidecar rewrite. The whole file is rebuilt each time, so the
// interval grows by FLUSH_MS per FLUSH_SCALE_BYTES already written (10 s up to
// 4 MB, 40 s at the 16 MB of a two-hour 8-player match): the rewrite runs on
// the game thread, and in lockstep a long frame stalls every player.
static const DWORD FLUSH_MS = 10000;
static const uint32_t FLUSH_SCALE_BYTES = 4u * 1024u * 1024u;
static const DWORD FLUSH_MAX_MS = 60000;
// Recorder fingerprint witness interval for session recordings (jobs). The
// witness is diagnostic; 64 keeps its cost negligible.
static uint32_t s_fingerprintInterval = 64;
static const uint32_t ARM_DELAY_FRAMES = 30;

// Sidecar format. Little-endian, packed; sections follow the header as
// {tag, bytes, data}. The body hash covers every byte after the header, so a
// torn or foreign file is refused rather than half-replayed.
#pragma pack(push, 1)
struct ReplayBundleHeader {
    char magic[8];           // "S2REPLAY"
    uint32_t version;        // 1
    uint32_t headerSize;
    uint32_t flags;          // BUNDLE_*
    uint32_t endReason;      // END_*
    uint64_t checkpointHash; // FNV-1a 64 of the .sh2 bytes
    uint32_t checkpointSize;
    uint32_t sectionCount;
    uint32_t bodyBytes;
    uint32_t bodyHash;       // FNV-1a 32 of the body
    uint32_t mpPhase;        // NetGameMgr+4 byte at the checkpoint
    uint32_t netMode;        // 2 = recorded in a networked session
    uint32_t classCount;
    uint32_t epochTick;
    uint32_t endRelative;    // last job the recording saw, relative to the epoch
    uint32_t commandCount;
    uint32_t injectedCount;
    uint32_t retimedCount;
    // Injected commands never executed live. They are left out of CMDS.
    uint32_t unretimedCount;
    uint32_t execPlans;
    uint32_t exeImageSize;
    uint32_t exeTimeStamp;
    uint64_t recordedUtc;    // FILETIME
    uint64_t updatedUtc;     // FILETIME of this write
    uint32_t cutCommands;    // BUNDLE_TRUNCATED: recorded commands left out past the end
    uint32_t reserved[7];
};
struct ReplayBundleSection {
    uint32_t tag;
    uint32_t bytes;
};
#pragma pack(pop)
static_assert(sizeof(ReplayBundleHeader) == 144, "replay bundle header changed");

static const uint32_t BUNDLE_COMPLETE = 1u << 0;    // written when the match ended
static const uint32_t BUNDLE_RETIMED = 1u << 1;     // execution schedule applied
static const uint32_t BUNDLE_MP_TIMING = 1u << 2;   // replay with MP job cadence
static const uint32_t BUNDLE_TILE_ORDER = 1u << 3;
static const uint32_t BUNDLE_PRNG = 1u << 4;
// The recording went on past what the sidecar keeps (a buffer filled up):
// commands are exact through endRelative and the replay ends there.
static const uint32_t BUNDLE_TRUNCATED = 1u << 5;
// reserved[0] = RandomIter #0 cursor before (low 16) / after (high 16) the
// checkpoint save; playback's first job should start from "after".
static const uint32_t BUNDLE_ITER0_AT_SAVE = 1u << 6;

enum : uint32_t {
    END_RECORDING = 0,   // periodic snapshot of a match still in progress
    END_LEFT_MATCH = 1,  // the match ended (menu exit, desync drop, game over exit)
    END_STOPPED = 2,     // the recorder was stopped (tooling)
    END_PROCESS_EXIT = 3,
    END_CRASH = 4,
    END_REPLACED = 5,    // a save was loaded over the recorded match
    END_WIN = 6,         // the local player's victory screen came up
    END_LOSE = 7,        // the local player's defeat screen came up
    END_CAPACITY = 8,    // the recording buffers were nearly full (see recordingFull)
};

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

static uint32_t tag4(const char *s) {
    return (uint32_t)(uint8_t)s[0] | (uint32_t)(uint8_t)s[1] << 8 |
           (uint32_t)(uint8_t)s[2] << 16 | (uint32_t)(uint8_t)s[3] << 24;
}

// ── State ───────────────────────────────────────────────────────────────────

static bool s_recordMp = true;
static bool s_recordSp = false;
static int s_maxReplays = REPLAY_KEEP_DEFAULT;
static uint32_t s_mode = REPLAY_SESSION_IDLE;
static bool s_prevInGame = false;
static uint32_t s_hooks = 0;

// Recording.
static wchar_t s_savePath[MAX_PATH] = L"";
static wchar_t s_sidecarPath[MAX_PATH] = L"";
static char s_sidecarUtf8[MAX_PATH * 3] = "";
static bool s_armConfirmed = false;
static uint64_t s_checkpointHash = 0;
static uint32_t s_checkpointSize = 0;
static uint64_t s_recordedUtc = 0;
static uint32_t s_mpPhase = 0;
static uint32_t s_netMode = 0;
static DWORD s_lastFlush = 0;
static volatile LONG s_writing = 0;

// Playback.
static uint32_t s_endRelative = 0;
static uint32_t s_playEndReason = 0;   // the sidecar's END_*
static bool s_sessionPlayback = false; // armed by our load hook (vs. tooling)

// Victory/defeat screen activation (replaySessionNoteOutcome), 1 win 2 lose.
// The screen hook only latches it; the frame tick acts on it.
static volatile LONG s_pendingOutcome = 0;
// Why the last playback finished: 0 not yet, 1 end hold, 2 win, 3 lose screen,
// 4 left the match without an end screen.
static uint32_t s_playbackEnded = 0;
// The victory/defeat screen clears World+4, and its OnActivate is not ordered
// against our frame tick, so a fall of World+4 waits this many frames for the
// outcome before it is treated as a plain exit. 0 = no fall pending.
static const uint32_t OUTCOME_GRACE_FRAMES = 30;
static uint32_t s_fallFrames = 0;

// Status.
static uint32_t s_recordings = 0, s_flushes = 0, s_flushFailures = 0;
static uint32_t s_lastFlushBytes = 0, s_lastFlushMs = 0, s_lastEndReason = 0;
static uint32_t s_lastRetimed = 0, s_lastUnretimed = 0, s_lastExecPlans = 0;
static uint32_t s_lastRetimeFail = 0;
static uint32_t s_commandBudget = 0; // tooling: stop recording after this many commands
static uint32_t s_loads = 0, s_lastLoadResult = REPLAY_LOAD_NONE, s_playbacks = 0;

// ── Small helpers ───────────────────────────────────────────────────────────

static uintptr_t exeBase() { return (uintptr_t)GetModuleHandleA(NULL); }

static bool inGame() {
    return *(const volatile uint32_t *)(exeBase() + WORLD_IN_GAME_RVA) != 0;
}

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

static uint32_t fnv32(uint32_t hash, const void *data, uint32_t bytes) {
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

// FNV-1a 64 of a whole file; false when it cannot be read.
static bool hashFile(const wchar_t *path, uint64_t *hash, uint32_t *size) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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

static size_t wlen(const wchar_t *s) {
    size_t n = 0;
    while (s[n]) {
        ++n;
    }
    return n;
}

static bool endsWithSh2(const wchar_t *path) {
    const size_t n = wlen(path);
    return n > 4 && path[n - 4] == L'.' && (path[n - 3] | 0x20) == L's' &&
           (path[n - 2] | 0x20) == L'h' && path[n - 1] == L'2';
}

// `x.sh2` -> `x.s2replay`. Deliberately not `.sh2...`: with 8.3 names a
// `*.sh2` search would also match `x.sh2replay` and list it as a save.
static bool sidecarFor(const wchar_t *save, wchar_t *out, size_t cap) {
    static const wchar_t EXT[] = L".s2replay";
    const size_t n = wlen(save);
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
    GetFolderFn getFolder = shell ? (GetFolderFn)(void *)GetProcAddress(shell, "SHGetFolderPathW") : nullptr;
    wchar_t docs[MAX_PATH];
    if (!getFolder || getFolder(NULL, 5 /* CSIDL_PERSONAL */, NULL, 0, docs) != 0) {
        return false;
    }
    static const wchar_t TAIL[] = L"\\Stronghold 2\\Saves";
    const size_t n = wlen(docs);
    if (n + sizeof(TAIL) / sizeof(TAIL[0]) > cap) {
        return false;
    }
    memcpy(out, docs, n * sizeof(wchar_t));
    memcpy(out + n, TAIL, sizeof(TAIL));
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
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
        _snwprintf(out, cap, L"%ls\\replay-%u.%u.%02u-%u%02u%ls-%lc%lc.sh2", dir, t.wMonth, t.wDay,
                   t.wYear % 100, hour12, t.wMinute, t.wHour < 12 ? L"am" : L"pm", a, b);
        out[cap - 1] = 0;
        wchar_t side[MAX_PATH];
        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES &&
            sidecarFor(out, side, MAX_PATH) && GetFileAttributesW(side) == INVALID_FILE_ATTRIBUTES) {
            return true;
        }
    }
    return false;
}

// Keeps at most `keep` replays in Saves, deleting the oldest first. A replay
// is a `replay-*.s2replay` sidecar and the `.sh2` save beside it; its age is
// the save's last write (the checkpoint, written once at match start), or the
// sidecar's when the save is gone. Nothing without a sidecar is ever touched,
// so an ordinary save that happens to be named `replay-...` is safe.
static void pruneReplays(int keep) {
    wchar_t dir[MAX_PATH];
    if (keep < 0 || !savesDirectory(dir, MAX_PATH) || wlen(dir) + 40 > MAX_PATH) {
        return;
    }
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\replay-*.s2replay", dir);
    pattern[MAX_PATH - 1] = 0;

    struct Entry {
        wchar_t name[MAX_PATH];
        uint64_t age;
    };
    static Entry entries[512];
    int count = 0;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const size_t n = wlen(fd.cFileName);
        static const size_t EXT = 9; // ".s2replay"
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n <= EXT ||
            _wcsicmp(fd.cFileName + n - EXT, L".s2replay") != 0 || count >= 512 ||
            wlen(dir) + n + 2 > MAX_PATH) {
            continue;
        }
        Entry &e = entries[count];
        memcpy(e.name, fd.cFileName, (n - EXT) * sizeof(wchar_t));
        e.name[n - EXT] = 0;
        e.age = (uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
        wchar_t save[MAX_PATH];
        _snwprintf(save, MAX_PATH, L"%ls\\%ls.sh2", dir, e.name);
        save[MAX_PATH - 1] = 0;
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(save, GetFileExInfoStandard, &a)) {
            e.age = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
        }
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
}

static void toUtf8(const wchar_t *in, char *out, int cap) {
    if (WideCharToMultiByte(CP_UTF8, 0, in, -1, out, cap, NULL, NULL) <= 0) {
        out[0] = 0;
    }
}

// ── Retiming (replayRetime.cpp) ───────────────────────────────────────────

static ReplayRetimeResult s_retime;

static void buildSchedule(const ReplayRecorderLogView &log, uint32_t limitTick) {
    const ReplayBoundaryEntry *b = nullptr;
    uint32_t count = 0, dropped = 0, hooks = 0;
    replayTraceBoundaryView(&b, &count, &dropped, &hooks);
    replayRetime(log.bytes, log.used, log.epochTick, b, count, dropped, hooks, limitTick,
                 &s_retime);
    s_lastRetimeFail = s_retime.failReason;
}

static uint32_t minLimit(uint32_t limit, uint32_t tick) {
    return !limit || tick < limit ? tick : limit;
}

// ── Sidecar writer ──────────────────────────────────────────────────────────

struct BundleWriter {
    HANDLE file;
    uint32_t hash;
    uint32_t bytes;
    bool ok;
    uint8_t stage[65536];
    uint32_t staged;
};
static BundleWriter s_writer;

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
    w.hash = fnv32(w.hash, data, bytes);
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
    const ReplayBundleSection s = {tag4(tag), bytes};
    writerPut(w, &s, sizeof(s));
}

// Writes the whole sidecar to `<path>.tmp` and renames it over the previous
// one, so a crash mid-write leaves the last good sidecar in place.
static bool writeBundle(bool complete, uint32_t endReason) {
    if (InterlockedCompareExchange(&s_writing, 1, 0) != 0) {
        return false;
    }
    const DWORD started = GetTickCount();
    const bool durable = complete || endReason != END_RECORDING;
    ReplayRecorderLogView log = {};
    replayRecorderLogView(&log);
    const ReplayPrngEntry *prng = nullptr;
    uint32_t prngCount = 0, prngDropped = 0;
    prngGuardRecordView(&prng, &prngCount, &prngDropped);

    // The last job the sidecar can be exact through, 0 = the whole recording.
    // recordingFull() ends the recording before any buffer fills; these cuts
    // are the fallback, so a lost tail never costs the rest of the match.
    uint32_t limit = 0;
    if (log.dropped) {
        limit = minLimit(limit, log.firstDroppedTick > log.epochTick ? log.firstDroppedTick - 1
                                                                     : log.epochTick);
    }
    if (prngDropped && prngCount) {
        limit = minLimit(limit, log.epochTick + prng[prngCount - 1].relativeTick);
    }
    if (endReason == END_CAPACITY) {
        // Cut before the commands still in flight, so the replay ends exact.
        const uint32_t now = replayTraceCurrentSimulationTick();
        limit = minLimit(limit, now > log.epochTick ? now - 1 : log.epochTick);
    }
    buildSchedule(log, limit);
    const bool truncated = s_retime.endTick != 0;
    const uint32_t *tile = nullptr;
    uint32_t tileWords = 0, tileLists = 0;
    const bool haveTile = replayTileOrderView(&tile, &tileWords, &tileLists);

    ReplayBundleHeader h = {};
    memcpy(h.magic, "S2REPLAY", 8);
    h.version = 1;
    h.headerSize = sizeof(h);
    h.endReason = endReason;
    h.checkpointHash = s_checkpointHash;
    h.checkpointSize = s_checkpointSize;
    h.mpPhase = s_mpPhase;
    h.netMode = s_netMode;
    h.classCount = log.classCount;
    h.epochTick = log.epochTick;
    // The last job the simulation actually ran. The job counter itself keeps
    // advancing under a single-player pause (ESC menu) while nothing is
    // simulated, so it overshoots; the iterator-1 record is one entry per
    // simulated job.
    const uint32_t now = replayTraceCurrentSimulationTick();
    h.endRelative = prngCount ? prng[prngCount - 1].relativeTick
                              : (now > log.epochTick ? now - log.epochTick : 0);
    if (truncated) {
        const uint32_t end = s_retime.endTick > log.epochTick ? s_retime.endTick - log.epochTick : 0;
        h.endRelative = end < h.endRelative ? end : h.endRelative;
        h.cutCommands = s_retime.cutEvents;
    }
    h.commandCount = s_retime.events;
    h.injectedCount = s_retime.injected;
    h.retimedCount = s_retime.retimed;
    h.unretimedCount = s_retime.unretimed;
    h.execPlans = s_retime.planJobs;
    exeIdentity(&h.exeImageSize, &h.exeTimeStamp);
    h.recordedUtc = s_recordedUtc;
    h.updatedUtc = nowFileTime();
    h.flags = (complete ? BUNDLE_COMPLETE : 0) | (s_retime.planWords ? BUNDLE_RETIMED : 0) |
              (s_netMode == 2 ? BUNDLE_MP_TIMING : 0) | (haveTile ? BUNDLE_TILE_ORDER : 0) |
              (prngCount && (!prngDropped || truncated) ? BUNDLE_PRNG : 0) |
              (truncated ? BUNDLE_TRUNCATED : 0);
    uint32_t iter0Before = 0, iter0After = 0;
    replayRecorderIter0AtSave(&iter0Before, &iter0After);
    if (iter0Before <= 0xffff && iter0After <= 0xffff) {
        h.flags |= BUNDLE_ITER0_AT_SAVE;
        h.reserved[0] = iter0Before | (iter0After << 16);
    }

    wchar_t tmp[MAX_PATH + 8];
    const size_t n = wlen(s_sidecarPath);
    memcpy(tmp, s_sidecarPath, n * sizeof(wchar_t));
    memcpy(tmp + n, L".tmp", 5 * sizeof(wchar_t));
    BundleWriter &w = s_writer;
    w.file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    w.ok = w.file != INVALID_HANDLE_VALUE;
    w.hash = 2166136261u;
    w.bytes = 0;
    w.staged = 0;
    if (w.ok) {
        DWORD wrote = 0;
        w.ok = WriteFile(w.file, &h, sizeof(h), &wrote, NULL) && wrote == sizeof(h);
    }

    // CMDS: a commands.bin image in playback order.
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
        ReplayEventRecord e;
        memcpy(&e, log.bytes + replayRetimeRefs()[i].offset, sizeof(e));
        e.simulationTick = replayRetimeRefs()[i].newTick;
        writerPut(w, &e, sizeof(e));
        writerPut(w, log.bytes + replayRetimeRefs()[i].offset + sizeof(e), e.payloadLength);
    }
    if (h.flags & BUNDLE_PRNG) {
        ReplayPrngFileHeader ph = {};
        memcpy(ph.magic, "SH2RPRN", 8);
        ph.version = 4;
        ph.entrySize = sizeof(ReplayPrngEntry);
        ph.entryCount = prngCount;
        ph.epochTick = log.epochTick;
        ph.firstRelative = prng[0].relativeTick;
        ph.lastRelative = prng[prngCount - 1].relativeTick;
        const ReplayMonasteryClock *clocks = nullptr;
        uint32_t clockCount = 0;
        prngGuardRecordClocks(&clocks, &clockCount);
        const uint32_t footer[2] = {REPLAY_CLOCK_MAGIC, clockCount};
        writerSection(w, "PRNG", sizeof(ph) + prngCount * (sizeof(ReplayPrngEntry) + 1) +
                      sizeof(footer) + clockCount * sizeof(ReplayMonasteryClock));
        writerPut(w, &ph, sizeof(ph));
        writerPut(w, prng, prngCount * sizeof(ReplayPrngEntry));
        writerPut(w, prngGuardRecordGates(), prngCount);
        writerPut(w, footer, sizeof(footer));
        writerPut(w, clocks, clockCount * sizeof(ReplayMonasteryClock));
        ++sections;
    }
    if (haveTile) {
        const uint32_t th[3] = {0x44524F54u, 1, tileLists};
        writerSection(w, "TILE", sizeof(th) + tileWords * 4);
        writerPut(w, th, sizeof(th));
        writerPut(w, tile, tileWords * 4);
        ++sections;
    }
    const ReplayFlyField *fly = nullptr;
    uint32_t flyCount = 0;
    if (replayFlyStateView(&fly, &flyCount) && flyCount) {
        const uint32_t fh[3] = {REPLAY_FLY_MAGIC, 1, flyCount};
        writerSection(w, "FLYS", sizeof(fh) + flyCount * sizeof(ReplayFlyField));
        writerPut(w, fh, sizeof(fh));
        writerPut(w, fly, flyCount * sizeof(ReplayFlyField));
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
        // Only the final write is forced to disk. A periodic one only has to
        // survive the process, and the OS cache does; a sync of a 16 MB
        // sidecar every few seconds would stall the lockstep for everyone.
        w.ok = SetFilePointer(w.file, 0, NULL, FILE_BEGIN) == 0 &&
               WriteFile(w.file, &h, sizeof(h), &wrote, NULL) && wrote == sizeof(h) &&
               (!durable || FlushFileBuffers(w.file));
    }
    if (w.file != INVALID_HANDLE_VALUE) {
        CloseHandle(w.file);
    }
    bool ok = w.ok && MoveFileExW(tmp, s_sidecarPath,
                                  MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0));
    if (!ok) {
        DeleteFileW(tmp);
        ++s_flushFailures;
    } else {
        ++s_flushes;
        s_lastFlushBytes = sizeof(h) + w.bytes;
    }
    s_lastFlushMs = GetTickCount() - started;
    s_lastRetimed = s_retime.retimed;
    s_lastUnretimed = s_retime.unretimed;
    s_lastExecPlans = s_retime.planJobs;
    InterlockedExchange(&s_writing, 0);
    return ok;
}

// ── Recording lifecycle ─────────────────────────────────────────────────────

static void finishRecording(uint32_t reason) {
    if (s_mode != REPLAY_SESSION_RECORDING) {
        return;
    }
    if (s_armConfirmed) {
        if (replayRecorderIsRecording()) {
            replayRecorderControl(0);
        }
        writeBundle(true, reason);
        ++s_recordings;
        s_lastEndReason = reason;
    }
    s_mode = REPLAY_SESSION_IDLE;
    s_armConfirmed = false;
}

static void beginRecording() {
    const char *explicitPath = replayRecorderAutoArmPath();
    if (explicitPath) {
        // Tooling armed this match (REPLAY_AUTOARM): adopt it, so the reference
        // container's checkpoint also becomes loadable in game.
        if (MultiByteToWideChar(CP_UTF8, 0, explicitPath, -1, s_savePath, MAX_PATH) <= 0) {
            return;
        }
    } else {
        const bool networked = replayTraceSessionIsNetworked();
        if (!(networked ? s_recordMp : s_recordSp)) {
            return;
        }
        // Make room first, so the new match brings the count up to the limit.
        pruneReplays(s_maxReplays - 1);
        if (!newReplayPath(s_savePath, MAX_PATH)) {
            return;
        }
        char utf8[MAX_PATH * 3];
        toUtf8(s_savePath, utf8, sizeof(utf8));
        if (!utf8[0] ||
            !replayRecorderAutoArm(utf8, s_fingerprintInterval, ARM_DELAY_FRAMES, 0xffffffffu)) {
            return;
        }
    }
    if (!sidecarFor(s_savePath, s_sidecarPath, MAX_PATH)) {
        return;
    }
    toUtf8(s_sidecarPath, s_sidecarUtf8, sizeof(s_sidecarUtf8));
    s_armConfirmed = false;
    s_recordedUtc = nowFileTime();
    s_mode = REPLAY_SESSION_RECORDING;
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
    uint32_t maxEvents = REPLAY_MAX_EVENTS - HEADROOM_EVENTS;
    if (s_commandBudget && s_commandBudget < maxEvents) {
        maxEvents = s_commandBudget;
    }
    return log.dropped || witnessDropped || jobsDropped || log.events >= maxEvents ||
           log.used + HEADROOM_LOG_BYTES > REPLAY_LOG_CAPACITY ||
           witness + 2 * HEADROOM_EVENTS > REPLAY_BOUNDARY_CAPACITY ||
           jobs + HEADROOM_PRNG_JOBS > REPLAY_PRNG_CAPACITY;
}

static void recordingTick() {
    ReplayRecorderStatus st = {};
    replayRecorderStatus(&st, sizeof(st));
    if (!s_armConfirmed) {
        if (st.autoArmPending) {
            return;
        }
        if (!st.autoArmDone || !st.autoArmSaved ||
            !hashFile(s_savePath, &s_checkpointHash, &s_checkpointSize)) {
            s_mode = REPLAY_SESSION_IDLE; // the checkpoint failed: no replay
            return;
        }
        s_armConfirmed = true;
        s_mpPhase = st.epochNetCounter;
        s_netMode = st.epochNetMode;
        s_lastFlush = GetTickCount();
        writeBundle(false, END_RECORDING);
        return;
    }
    if (!replayRecorderIsRecording()) {
        finishRecording(END_STOPPED);
        return;
    }
    if (recordingFull()) {
        finishRecording(END_CAPACITY);
        return;
    }
    DWORD interval = FLUSH_MS * (1 + s_lastFlushBytes / FLUSH_SCALE_BYTES);
    if (interval > FLUSH_MAX_MS) {
        interval = FLUSH_MAX_MS;
    }
    if (GetTickCount() - s_lastFlush >= interval) {
        s_lastFlush = GetTickCount();
        writeBundle(false, END_RECORDING);
    }
}

// ── Playback lifecycle ──────────────────────────────────────────────────────

static void stopPlayback() {
    replayPlayerControl(0);
    if (s_sessionPlayback) {
        prngGuardControl(2);
        replayPlayerLoadTileOrder("");
        replayPlayerLoadExecOrder("");
        replayPlayerClearFlyState();
        replayPlayerSetIter0AtSave(0xffffffffu);
    }
    replayViewerSetEnded(false);
    replayViewerSetReplayName(L"");
    s_sessionPlayback = false;
    s_mode = REPLAY_SESSION_IDLE;
}

static void playbackTick() {
    uint32_t rel = 0;
    const uint32_t hold = s_endRelative +
        ((s_playEndReason == END_WIN || s_playEndReason == END_LOSE) ? OUTCOME_HOLD_SLACK : 0);
    if (s_sessionPlayback && s_endRelative && !replayViewerEnded() &&
        replayPlayerPlaybackJob(&rel) && rel >= hold) {
        replayViewerSetEnded(true);
        s_playbackEnded = 1;
    }
}

// The victory/defeat screen came up (latched by replaySessionNoteOutcome).
// Recording: the match is over for the local player, so the replay ends right
// here rather than whenever they leave the screen. Playback of a session
// replay: the replay reached its own end screen. Stop the player, which drops
// the spectator guard, so the screen (and the stat overlay) is usable.
// Tooling-armed playback is left running to its own end.
// `outcome` 0 means the match was left without an end screen.
static void endMatch(LONG outcome, bool leftMatch) {
    if (s_mode == REPLAY_SESSION_RECORDING) {
        finishRecording(outcome == 1 ? END_WIN : outcome == 2 ? END_LOSE : END_LEFT_MATCH);
    } else if (s_mode == REPLAY_SESSION_PLAYBACK && (s_sessionPlayback || leftMatch)) {
        const bool session = s_sessionPlayback;
        stopPlayback();
        if (session) {
            s_playbackEnded = outcome == 1 ? 2 : outcome == 2 ? 3 : 4;
        }
    }
}

static void outcomeTick() {
    const LONG outcome = InterlockedExchange(&s_pendingOutcome, 0);
    if (outcome) {
        endMatch(outcome, false);
    }
}

static bool findSection(const uint8_t *body, uint32_t bytes, uint32_t sections, const char *tag,
                        const uint8_t **data, uint32_t *size) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < sections; ++i) {
        if (at + sizeof(ReplayBundleSection) > bytes) {
            return false;
        }
        ReplayBundleSection s;
        memcpy(&s, body + at, sizeof(s));
        at += sizeof(s);
        if (s.bytes > bytes - at) {
            return false;
        }
        if (s.tag == tag4(tag)) {
            *data = body + at;
            *size = s.bytes;
            return true;
        }
        at += s.bytes;
    }
    return false;
}

static uint32_t armReplay(const wchar_t *savePath) {
    wchar_t side[MAX_PATH];
    if (!sidecarFor(savePath, side, MAX_PATH)) {
        return REPLAY_LOAD_NOT_REPLAY;
    }
    HANDLE f = CreateFileW(side, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return REPLAY_LOAD_NOT_REPLAY;
    }
    const DWORD size = GetFileSize(f, NULL);
    uint8_t *data = size != INVALID_FILE_SIZE && size >= sizeof(ReplayBundleHeader)
                        ? (uint8_t *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
                        : nullptr;
    DWORD got = 0;
    const bool read = data && ReadFile(f, data, size, &got, NULL) && got == size;
    CloseHandle(f);
    uint32_t result = REPLAY_LOAD_BAD_SIDECAR;
    ReplayBundleHeader h = {};
    uint32_t imageSize = 0, timeStamp = 0;
    exeIdentity(&imageSize, &timeStamp);
    uint64_t hash = 0;
    uint32_t saveSize = 0;
    const uint8_t *body = data ? data + sizeof(h) : nullptr;
    const uint8_t *sec = nullptr;
    uint32_t secBytes = 0;
    if (read) {
        memcpy(&h, data, sizeof(h));
    }
    if (!read || memcmp(h.magic, "S2REPLAY", 8) != 0 || h.version != 1 ||
        h.headerSize != sizeof(h) || h.bodyBytes != size - sizeof(h) ||
        fnv32(2166136261u, body, h.bodyBytes) != h.bodyHash) {
        result = REPLAY_LOAD_BAD_SIDECAR;
    } else if (h.exeImageSize != imageSize || h.exeTimeStamp != timeStamp) {
        result = REPLAY_LOAD_EXE;
    } else if (!hashFile(savePath, &hash, &saveSize) || hash != h.checkpointHash ||
               saveSize != h.checkpointSize) {
        result = REPLAY_LOAD_CHECKPOINT;
    } else if (replayTraceSessionIsNetworked()) {
        result = REPLAY_LOAD_NETWORKED;
    } else if (!findSection(body, h.bodyBytes, h.sectionCount, "CMDS", &sec, &secBytes) ||
               !replayPlayerLoadBytes(sec, secBytes)) {
        result = REPLAY_LOAD_COMMANDS;
    } else {
        if (!(findSection(body, h.bodyBytes, h.sectionCount, "PRNG", &sec, &secBytes) &&
              prngGuardLoadBytes(sec, secBytes))) {
            prngGuardControl(2);
        }
        if (!(findSection(body, h.bodyBytes, h.sectionCount, "TILE", &sec, &secBytes) &&
              replayPlayerLoadTileOrderBytes(sec, secBytes) != TILE_ORDER_REFUSED)) {
            replayPlayerLoadTileOrder("");
        }
        if (!(findSection(body, h.bodyBytes, h.sectionCount, "EXOR", &sec, &secBytes) &&
              replayPlayerLoadExecOrderBytes(sec, secBytes) != EXEC_ORDER_REFUSED)) {
            replayPlayerLoadExecOrder("");
        }
        // Absent in older sidecars: nothing to restore.
        if (!(findSection(body, h.bodyBytes, h.sectionCount, "FLYS", &sec, &secBytes) &&
              replayPlayerLoadFlyStateBytes(sec, secBytes) != FLY_STATE_REFUSED)) {
            replayPlayerClearFlyState();
        }
        // Absent in older sidecars (flag 0x40): the load's own cursor stays.
        replayPlayerSetIter0AtSave((h.flags & BUNDLE_ITER0_AT_SAVE) ? h.reserved[0] >> 16
                                                                    : 0xffffffffu);
        const bool mp = (h.flags & BUNDLE_MP_TIMING) != 0;
        const uint32_t state = replayPlayerControl(mp ? 2 : 1, h.mpPhase & 0xff, true);
        if (state != 2 && state != 5) {
            prngGuardControl(2);
            replayPlayerLoadTileOrder("");
            replayPlayerLoadExecOrder("");
            replayPlayerClearFlyState();
            replayPlayerSetIter0AtSave(0xffffffffu);
            result = REPLAY_LOAD_CONTROL;
        } else {
            result = REPLAY_LOAD_ARMED;
            s_endRelative = h.endRelative;
            s_playEndReason = h.endReason;
            s_checkpointHash = h.checkpointHash;
        }
    }
    if (data) {
        VirtualFree(data, 0, MEM_RELEASE);
    }
    return result;
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

// MSVC std::wstring as the game passes it by value:
// a 16-byte SSO union, then size at +0x10 and capacity at +0x14.
namespace { // internal linkage: several modules define this layout
struct GameWString {
    union {
        wchar_t buf[8];
        wchar_t *ptr;
    } u;
    uint32_t size;
    uint32_t res;
};
} // namespace

extern "C" void replaySessionOnLoadGame(const GameWString *arg) {
    ++s_loads;
    // Whatever was running is being replaced. Close it out first.
    if (s_mode == REPLAY_SESSION_RECORDING) {
        finishRecording(END_REPLACED);
    } else if (s_mode == REPLAY_SESSION_PLAYBACK) {
        stopPlayback();
    }
    // The next in-match frame is a new match even if World+4 never dropped (a
    // load from the pause menu keeps it set).
    replayRecorderResetEpoch();
    s_prevInGame = false;
    s_fallFrames = 0;
    s_playbackEnded = 0;
    InterlockedExchange(&s_pendingOutcome, 0);

    if (!arg || arg->size == 0 || arg->size >= MAX_PATH) {
        s_lastLoadResult = REPLAY_LOAD_NOT_REPLAY;
        return;
    }
    const wchar_t *text = arg->res >= 8 ? arg->u.ptr : arg->u.buf;
    wchar_t path[MAX_PATH];
    memcpy(path, text, arg->size * sizeof(wchar_t));
    path[arg->size] = 0;
    // The save list may hand over a bare file name; resolve it in Saves.
    wchar_t full[MAX_PATH];
    if (!wcschr(path, L'\\') && !wcschr(path, L'/')) {
        wchar_t dir[MAX_PATH];
        if (savesDirectory(dir, MAX_PATH) && wlen(dir) + wlen(path) + 2 < MAX_PATH) {
            _snwprintf(full, MAX_PATH, L"%ls\\%ls", dir, path);
            full[MAX_PATH - 1] = 0;
            memcpy(path, full, sizeof(full));
        }
    }
    s_lastLoadResult = armReplay(path);
    if (s_lastLoadResult == REPLAY_LOAD_ARMED) {
        ++s_playbacks;
        s_sessionPlayback = true;
        s_mode = REPLAY_SESSION_PLAYBACK;
        setViewerName(path);
        replayViewerSetEnded(false);
        wchar_t side[MAX_PATH];
        if (sidecarFor(path, side, MAX_PATH)) {
            toUtf8(side, s_sidecarUtf8, sizeof(s_sidecarUtf8));
        }
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

static LONG WINAPI sessionCrashFilter(EXCEPTION_POINTERS *ep) {
    if (s_mode == REPLAY_SESSION_RECORDING && s_armConfirmed) {
        writeBundle(false, END_CRASH);
    }
    return s_prevFilter ? s_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static void sessionTick() {
    // Installed here rather than at DLL attach: the game's CRT startup, and
    // any earlier-registered tick, set theirs first, and
    // this one chains to whatever it replaces.
    if (!s_filterInstalled) {
        s_filterInstalled = true;
        s_prevFilter = SetUnhandledExceptionFilter(sessionCrashFilter);
        s_hooks |= 2;
    }

    const bool now = inGame();
    if (!now) {
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
                endMatch(outcome, true);
                replayRecorderResetEpoch();
                s_fallFrames = 0;
            }
        }
        return;
    }
    if (s_fallFrames) {
        // Back in a match inside the grace window (a load straight from the
        // menu): close the previous one out as a plain exit first.
        endMatch(0, true);
        replayRecorderResetEpoch();
        s_fallFrames = 0;
    }
    if (!s_prevInGame) {
        s_prevInGame = true;
        InterlockedExchange(&s_pendingOutcome, 0);
        if (replayPlayerArmed()) {
            s_mode = REPLAY_SESSION_PLAYBACK; // ours, or armed by tooling
        } else if (s_mode == REPLAY_SESSION_IDLE) {
            beginRecording();
        }
    }
    outcomeTick();
    if (s_mode == REPLAY_SESSION_RECORDING) {
        recordingTick();
    } else if (s_mode == REPLAY_SESSION_PLAYBACK) {
        playbackTick();
    }
}

// ── Public ──────────────────────────────────────────────────────────────────

void replaySessionNoteOutcome(bool won) {
    InterlockedExchange(&s_pendingOutcome, won ? 1 : 2);
}

void replaySessionOnProcessExit() {
    if (s_mode == REPLAY_SESSION_RECORDING && s_armConfirmed) {
        writeBundle(false, END_PROCESS_EXIT);
    }
}

void replaySessionConfigure(bool recordMultiplayer, bool recordSinglePlayer,
                            uint32_t fingerprintInterval) {
    s_recordMp = recordMultiplayer;
    s_recordSp = recordSinglePlayer;
    if (fingerprintInterval) {
        s_fingerprintInterval = fingerprintInterval;
    }
}

void replaySessionSetCommandBudget(uint32_t commands) {
    s_commandBudget = commands;
}

void replaySessionGetStatus(ReplaySessionStatus *out) {
    out->mode = s_mode;
    out->recordMultiplayer = s_recordMp ? 1 : 0;
    out->recordSinglePlayer = s_recordSp ? 1 : 0;
    out->hooks = s_hooks;
    out->recordings = s_recordings;
    out->flushes = s_flushes;
    out->flushFailures = s_flushFailures;
    out->lastFlushBytes = s_lastFlushBytes;
    out->lastFlushMs = s_lastFlushMs;
    out->lastEndReason = s_lastEndReason;
    out->lastRetimed = s_lastRetimed;
    out->lastUnretimed = s_lastUnretimed;
    out->lastExecPlans = s_lastExecPlans;
    out->lastRetimeFail = s_lastRetimeFail;
    out->loads = s_loads;
    out->lastLoadResult = s_lastLoadResult;
    out->playbacks = s_playbacks;
    out->endRelative = s_endRelative;
    out->ended = s_playbackEnded ? s_playbackEnded : (replayViewerEnded() ? 1u : 0u);
    out->checkpointHashLo = (uint32_t)s_checkpointHash;
    out->checkpointHashHi = (uint32_t)(s_checkpointHash >> 32);
}

bool replaySessionRecordMultiplayer() {
    return s_recordMp;
}

void replaySessionSetRecordMultiplayer(bool on) {
    s_recordMp = on;
}

int replaySessionMaxReplays() {
    return s_maxReplays;
}

void replaySessionSetMaxReplays(int keep) {
    s_maxReplays = keep < REPLAY_KEEP_MIN ? REPLAY_KEEP_MIN
                   : keep > REPLAY_KEEP_MAX ? REPLAY_KEEP_MAX : keep;
}

const char *replaySessionSidecarPath() {
    return s_sidecarUtf8;
}

void installReplaySession() {
    s_recordMp = configInt("replay", "RecordMultiplayer", 1) != 0;
    s_recordSp = configInt("replay", "RecordSinglePlayer", 0) != 0;
    replaySessionSetMaxReplays(configInt("replay", "MaxReplays", REPLAY_KEEP_DEFAULT));
    const int interval = configInt("replay", "FingerprintInterval", 64);
    s_fingerprintInterval = interval > 0 ? (uint32_t)interval : 64;

    const uintptr_t base = exeBase();
    uint8_t *site = (uint8_t *)(base + LOAD_GAME_RVA);
    if (memcmp(site, LOAD_GAME_STOCK, sizeof(LOAD_GAME_STOCK)) == 0) {
        g_replaySessionLoadReturn = (uintptr_t)site + sizeof(LOAD_GAME_STOCK);
        installHook(site, reinterpret_cast<void *>(loadGameHook), sizeof(LOAD_GAME_STOCK));
        s_hooks |= 1;
    }
    registerFrameTick(sessionTick);
}
