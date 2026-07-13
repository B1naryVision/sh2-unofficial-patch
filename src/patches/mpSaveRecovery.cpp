#include "mpSaveRecovery.h"
#include "../core/frameTick.h"
#include "endgameStats/session.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;

// See docs/features/mp-save-recovery.md for how these were found.
static const uintptr_t SAVE_GAME_RVA =
    0x2a2e0; // SaveGame(std::wstring path); thiscall, ret 0x1c, bool in al
static const uintptr_t WSTRING_COPY_CTOR_RVA =
    0x1d120; // game's wstring copy-ctor; thiscall(dest), 1 stack arg
static const uintptr_t GET_SAVES_FOLDER_RVA =
    0x28880; // builds "<Documents>\Stronghold 2\Saves\" into out
static const uintptr_t SAVE_MANAGER_RVA =
    0xd781d0; // global save/profile manager (`this` for the above)
static const uintptr_t SAVE_KIND_FLAG_RVA =
    0xe46594; // byte: stock save handler clears it before saving
static const uintptr_t LOCAL_PLAYER_RVA = 0x6e8c60; // pointer to the local player object

static const DWORD SAVE_INTERVAL_MS = 3 * 60 * 1000;
static const size_t KEEP_LATEST_MAPS = 2;
static const wchar_t RECOVERY_PREFIX[] = L"MP_Recovery_";

// MSVC (VC7/VC8) std::wstring: 16-byte SSO union, then size, then capacity.
// Capacity >= 8 means the union holds a heap pointer instead of inline chars.
struct GameWString {
    union {
        wchar_t buf[8];
        wchar_t *ptr;
    } u;
    uint32_t size;
    uint32_t res;
};

typedef GameWString *(__attribute__((thiscall)) * GetSavesFolderFn)(void *self, GameWString *out);

static wchar_t s_savePath[MAX_PATH]; // full path of the .sh2 the game writes for us
static wchar_t s_mapsDir[MAX_PATH];
static wchar_t s_statusPath[MAX_PATH];
static bool s_pathsReady = false;
static HANDLE s_wakeEvent = nullptr;
static HANDLE s_worker = nullptr;

static volatile LONG s_wantStatus = 0; // worker: rewrite the status file
static volatile LONG s_wantCopy = 0; // worker: process a fresh recovery save (unused in diag mode)

// Diagnostic samples of candidate "is-networked-game" globals (see the
// DIAGNOSTIC MODE note below), written to the status file by the worker.
static volatile LONG s_diagGate = -1;
static volatile LONG s_diagNetA = -1;
static volatile LONG s_diagNetB = -1;
static volatile LONG s_diagNetC = -1;
static volatile LONG s_diagModeChar = -1;

static const wchar_t *gameStringChars(const GameWString *s) {
    if (s->res >= 8) {
        return s->u.ptr;
    }

    return s->u.buf;
}

static bool resolvePaths() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    GameWString folder;
    std::memset(&folder, 0, sizeof(folder));

    GetSavesFolderFn getSavesFolder = (GetSavesFolderFn)(base + GET_SAVES_FOLDER_RVA);
    getSavesFolder((void *)(base + SAVE_MANAGER_RVA), &folder);

    const wchar_t *chars = gameStringChars(&folder);

    if (!chars || folder.size == 0 || folder.size > folder.res || folder.size > MAX_PATH - 32) {
        return false;
    }

    std::memcpy(s_savePath, chars, folder.size * sizeof(wchar_t));
    s_savePath[folder.size] = L'\0';
    wcscat(s_savePath, L"MP_Recovery.sh2");
    // The folder wstring's heap buffer is deliberately leaked (once per
    // process): freeing it safely would need the game's allocator.

    wchar_t exePath[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exePath, MAX_PATH);

    if (n == 0 || n >= MAX_PATH) {
        return false;
    }

    wchar_t *slash = wcsrchr(exePath, L'\\');

    if (!slash) {
        return false;
    }

    slash[1] = L'\0';

    if (wcslen(exePath) > MAX_PATH - 8) {
        return false;
    }

    wcscpy(s_mapsDir, exePath);
    wcscat(s_mapsDir, L"maps");

    wcscpy(s_statusPath, exePath);
    wcscat(s_statusPath, L"mp_save_recovery_status.txt");

    return true;
}

// Byte-for-byte re-creation of the stock ESC-menu save handler's call
// sequence (VA 0x683e99..0x683ebb): construct the by-value wstring argument
// in place with the game's own copy-ctor (so the game's allocator owns the
// buffer that SaveGame later destroys), then call SaveGame, which pops its
// own 0x1c-byte argument.
[[maybe_unused]] static bool callSaveGame(const wchar_t *path) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    GameWString src;
    src.u.ptr = const_cast<wchar_t *>(path);
    src.size = (uint32_t)wcslen(path);
    src.res = src.size + 1;

    if (src.res < 8) {
        src.res = 8; // force the heap-pointer read path
    }

    *(volatile uint8_t *)(base + SAVE_KIND_FLAG_RVA) = 0;

    void *srcPtr = &src;
    uintptr_t manager = base + SAVE_MANAGER_RVA;
    uintptr_t ctor = base + WSTRING_COPY_CTOR_RVA;
    uintptr_t saveFn = base + SAVE_GAME_RVA;
    uint8_t ok = 0;

    __asm__ volatile("subl $0x1c, %%esp\n\t"
                     "movl %%esp, %%ecx\n\t"
                     "pushl %[src]\n\t"
                     "call *%[ctor]\n\t"
                     "movl %[mgr], %%ecx\n\t"
                     "call *%[save]\n\t"
                     : "=a"(ok), [src] "+d"(srcPtr)
                     : [ctor] "S"(ctor), [mgr] "b"(manager), [save] "D"(saveFn)
                     : "ecx", "cc", "memory");

    return ok != 0;
}

static void pruneOldRecoveryMaps(const fs::path &mapsDir) {
    std::vector<fs::path> recoveries;
    std::error_code ec;

    for (fs::directory_iterator it(mapsDir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path &p = it->path();
        std::wstring name = p.filename().wstring();

        if (name.rfind(RECOVERY_PREFIX, 0) == 0 && p.extension() == L".s2m") {
            recoveries.push_back(p);
        }
    }

    if (recoveries.size() <= KEEP_LATEST_MAPS) {
        return;
    }

    // Zero-padded timestamps make lexicographic order chronological.
    std::sort(recoveries.begin(), recoveries.end());

    for (size_t i = 0; i + KEEP_LATEST_MAPS < recoveries.size(); ++i) {
        std::error_code removeEc;
        fs::remove(recoveries[i], removeEc);
    }
}

static void processRecoverySave() {
    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t name[64];
    std::swprintf(
        name, 64, L"%ls%04u%02u%02u_%02u%02u.s2m", RECOVERY_PREFIX, st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute
    );

    fs::path src = s_savePath;
    fs::path dst = fs::path(s_mapsDir) / name;
    bool copied = false;

    for (int attempt = 0; attempt < 10 && !copied; ++attempt) {
        if (attempt > 0) {
            Sleep(500);
        }

        std::error_code ec;

        if (!fs::exists(src, ec) || ec) {
            continue;
        }

        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        copied = !ec;
    }

    if (!copied) {
        return;
    }

    pruneOldRecoveryMaps(fs::path(s_mapsDir));
}

static void writeStatusFile() {
    char text[256];
    std::snprintf(
        text, sizeof(text),
        "gate_byte_0xdb8483=%ld\n"
        "net_a_0x1245745=%ld\n"
        "net_b_0x11b845c=%ld\n"
        "net_c_0x1246305=%ld\n"
        "mode_char=%ld\n",
        (long)s_diagGate, (long)s_diagNetA, (long)s_diagNetB, (long)s_diagNetC, (long)s_diagModeChar
    );

    HANDLE file = CreateFileW(
        s_statusPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(file, text, (DWORD)std::strlen(text), &written, NULL);
    CloseHandle(file);
}

static DWORD WINAPI recoveryWorkerProc(LPVOID) {
    for (;;) {
        WaitForSingleObject(s_wakeEvent, INFINITE);

        if (InterlockedExchange(&s_wantCopy, 0)) {
            Sleep(1500); // let the engine finish flushing the save to disk
            processRecoverySave();
        }

        if (InterlockedExchange(&s_wantStatus, 0)) {
            writeStatusFile();
        }
    }

    return 0;
}

static bool ensureWorker() {
    if (s_worker) {
        return true;
    }

    s_wakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

    if (!s_wakeEvent) {
        return false;
    }

    s_worker = CreateThread(NULL, 0, recoveryWorkerProc, NULL, 0, NULL);

    if (!s_worker) {
        CloseHandle(s_wakeEvent);
        s_wakeEvent = nullptr;
        return false;
    }

    return true;
}

// ── DIAGNOSTIC MODE ───────────────────────────────────────────────────────────
// Live SaveGame kicks a networked match to the lobby (it performs a game-state
// transition the netcode won't tolerate; the save file is written fine but the
// session ends). So this build calls NO save at all — it is guaranteed not to
// disturb a match. Instead it samples candidate "is-networked-game" globals
// while in a game and writes them to the status file. Play one single-player
// skirmish and one multiplayer match, then send the two status files: the byte
// that differs between them is the real MP discriminator we need to gate on.
static const uintptr_t DIAG_GATE_BYTE_RVA = 0xdb8483; // menu gate (known 0 in MP; SP value unknown)
static const uintptr_t DIAG_NET_A_RVA = 0x1245745; // network-manager flag serialized into saves
static const uintptr_t DIAG_NET_B_RVA = 0x11b845c; // set from a network-object call at VA 0x6caeea
static const uintptr_t DIAG_NET_C_RVA = 0x1246305; // written by save/end-of-game handlers
static const uintptr_t DIAG_MODE_CHAR_OFF = 0x40284; // game-mode char k/h/w/p/f, off local player

// Runs once per frame on the game/sim thread. Samples only; never saves. The
// status file is refreshed every few seconds (not just once) so it captures
// the steady mid-game values, in case a network flag settles after the first
// in-game frame.
static void saveRecoveryTick() {
    static bool wasActive = false;
    static DWORD lastDump = 0;

    if (!sessionInGame()) {
        wasActive = false;
        return;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    s_diagGate = *(volatile uint8_t *)(base + DIAG_GATE_BYTE_RVA);
    s_diagNetA = *(volatile uint8_t *)(base + DIAG_NET_A_RVA);
    s_diagNetB = *(volatile uint8_t *)(base + DIAG_NET_B_RVA);
    s_diagNetC = *(volatile uint8_t *)(base + DIAG_NET_C_RVA);

    uintptr_t localPlayer = *(volatile uintptr_t *)(base + LOCAL_PLAYER_RVA);

    if (localPlayer != 0) {
        s_diagModeChar = *(volatile uint8_t *)(localPlayer + DIAG_MODE_CHAR_OFF);
    }

    if (!s_pathsReady) {
        s_pathsReady = resolvePaths();

        if (!s_pathsReady) {
            return;
        }
    }

    DWORD now = GetTickCount();

    if (!wasActive || now - lastDump >= 5000) {
        wasActive = true;
        lastDump = now;
        InterlockedExchange(&s_wantStatus, 1);

        if (ensureWorker()) {
            SetEvent(s_wakeEvent);
        }
    }
}

void installMpSaveRecovery() { registerFrameTick(saveRecoveryTick); }
