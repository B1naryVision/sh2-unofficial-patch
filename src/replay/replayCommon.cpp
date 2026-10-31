#include "replayCommon.h"

#include "../core/readableMemory.h"

#include <cstring>
#include <cwchar>
#include <windows.h>

// Ghidra VAs use ImageBase 0x400000; everything below is an RVA.
static const uintptr_t WORLD_RVA = 0xd781d0;
static const uintptr_t IAT_SCREEN_INSTANCE = 0x500930; // static Screen&
static const uintptr_t IAT_SCREEN_IS_POPUP_ON = 0x500730; // bool()
static const uintptr_t ACTOR_MGR_RVA = 0x6e8728;

static uintptr_t exeBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL)); }

static inline uint32_t load32(uintptr_t address) {
    return *reinterpret_cast<const volatile uint32_t *>(address);
}

bool replayInGame() {
    const uintptr_t inGame = exeBase() + WORLD_RVA + 4;
    return observerReadable(reinterpret_cast<const void *>(inGame), 4) && load32(inGame) != 0;
}

// ── Screen popup ────────────────────────────────────────────────────────────

static uintptr_t importedCode(uintptr_t iatRva) {
    const uintptr_t slot = exeBase() + iatRva;
    if (!observerReadable(reinterpret_cast<const void *>(slot), 4)) {
        return 0;
    }
    const uintptr_t fn = load32(slot);
    MEMORY_BASIC_INFORMATION mbi;
    if (!fn || VirtualQuery(reinterpret_cast<const void *>(fn), &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY))) {
        return 0;
    }
    return fn;
}

bool replayScreenPopupOn() {
    typedef void *(__cdecl * ScreenInstanceFn)();
    typedef bool(__attribute__((thiscall)) * ScreenPredicateFn)(void *self);
    const uintptr_t instance = importedCode(IAT_SCREEN_INSTANCE);
    const uintptr_t isPopupOn = importedCode(IAT_SCREEN_IS_POPUP_ON);
    if (!instance || !isPopupOn) {
        return false;
    }
    void *screen = reinterpret_cast<ScreenInstanceFn>(instance)();
    return screen && reinterpret_cast<ScreenPredicateFn>(isPopupOn)(screen);
}

// ── Tile order ──────────────────────────────────────────────────────────────

static const uint32_t TILE_ORDER_WORDS = 65536;
static const uint32_t TILE_ORDER_ACTORS = 8192;
static uint32_t s_tileOrder[TILE_ORDER_WORDS];
static uint32_t s_tileOrderWords = 0; // 0 = not captured
static uint32_t s_tileOrderLists = 0;

void replayCaptureTileOrder() {
    s_tileOrderWords = 0;
    s_tileOrderLists = 0;
    const uintptr_t header = exeBase() + ACTOR_MGR_RVA + 8;
    if (!observerReadable(reinterpret_cast<const void *>(header), 8)) {
        return;
    }
    const uintptr_t begin = load32(header), end = load32(header + 4);
    if (end < begin || ((end - begin) & 3u) != 0) {
        return;
    }
    const uint32_t count = static_cast<uint32_t>((end - begin) / 4);
    if (count > TILE_ORDER_ACTORS ||
        (count && !observerReadable(reinterpret_cast<const void *>(begin), count * 4u))) {
        return;
    }
    static uintptr_t objects[TILE_ORDER_ACTORS];
    static uint32_t slotOf[TILE_ORDER_ACTORS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t object = load32(begin + 4 * i);
        if (object && observerReadable(reinterpret_cast<const void *>(object), 0x90)) {
            objects[n] = object;
            slotOf[n] = i;
            ++n;
        }
    }
    uint32_t at = 0;
    static const uint32_t offsets[2] = {0x74, 0x80};
    for (uint32_t kind = 0; kind < 2; ++kind) {
        const uint32_t off = offsets[kind];
        for (uint32_t h = 0; h < n; ++h) {
            const uintptr_t headNode = objects[h] + off;
            // A chain head: no other actor's `next` points at this node.
            bool referenced = false;
            for (uint32_t j = 0; j < n && !referenced; ++j) {
                referenced = load32(objects[j] + off + 4) == headNode;
            }
            if (referenced || load32(headNode) == 0) {
                continue;
            }
            uint32_t chain[64];
            uint32_t length = 0;
            for (uintptr_t node = headNode; node && length < 64;) {
                uint32_t member = n;
                for (uint32_t j = 0; j < n; ++j) {
                    if (objects[j] + off == node) {
                        member = j;
                        break;
                    }
                }
                if (member == n) {
                    break;
                }
                chain[length++] = slotOf[member];
                node = load32(node + 4);
            }
            if (length < 2) {
                continue;
            }
            if (at + 2 + length > TILE_ORDER_WORDS) {
                return; // leave uncaptured rather than write a partial table
            }
            s_tileOrder[at++] = kind;
            s_tileOrder[at++] = length;
            memcpy(s_tileOrder + at, chain, length * 4u);
            at += length;
            ++s_tileOrderLists;
        }
    }
    s_tileOrderWords = at ? at : 1; // captured, possibly zero lists
}

bool replayTileOrderView(const uint32_t **words, uint32_t *wordCount, uint32_t *lists) {
    if (!s_tileOrderWords) {
        return false;
    }
    *words = s_tileOrder;
    *wordCount = s_tileOrderLists ? s_tileOrderWords : 0;
    *lists = s_tileOrderLists;
    return true;
}

uint32_t replayTileOrderSave(const char *path) {
    if (!s_tileOrderWords) {
        return 0;
    }
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    const uint32_t header[3] = {0x44524F54u, 1, s_tileOrderLists};
    const uint32_t words = s_tileOrderLists ? s_tileOrderWords : 0;
    DWORD wrote = 0, wrote2 = 0;
    const bool ok = WriteFile(f, header, sizeof(header), &wrote, NULL) &&
                    (!words || WriteFile(f, s_tileOrder, words * 4u, &wrote2, NULL));
    CloseHandle(f);
    return ok ? wrote + wrote2 : 0;
}

// ── Projectile damage ────────────────────────────────────────────────────────

static const uintptr_t FLY_MGR_RVA = 0x6e8800; // tick phase 3: mov ecx,0xae8800; call 0x76a140
static const uint32_t FLY_SLOTS_MAX = 65536;
// {class vtable RVA, damage offset}: Arrow ctor 0xcea70 sets +0xd4 = 750.0,
// Axe 0xd4b20 +0xcc, Spear 0xd5bd0 +0xd0; none of the three serializes it.
static const uint32_t FLY_FIELDS[][2] = {{0x5a882c, 0xd4}, {0x5a8f34, 0xcc}, {0x5a8ff4, 0xd0}};
static ReplayFlyField s_fly[REPLAY_FLY_MAX];
static uint32_t s_flyCount = 0;
static bool s_flyCaptured = false;

bool replayFlyFieldKnown(uint32_t vtableRva, uint32_t offset) {
    for (const auto &f : FLY_FIELDS) {
        if (f[0] == vtableRva && f[1] == offset) {
            return true;
        }
    }
    return false;
}

bool replayFlySlots(const uintptr_t **slots, uint32_t *count) {
    const uintptr_t header = exeBase() + FLY_MGR_RVA + 8;
    if (!observerReadable(reinterpret_cast<const void *>(header), 8)) {
        return false;
    }
    const uintptr_t begin = load32(header), end = load32(header + 4);
    if (end < begin || ((end - begin) & 3u) != 0 || (end - begin) / 4 > FLY_SLOTS_MAX) {
        return false;
    }
    const uint32_t n = static_cast<uint32_t>((end - begin) / 4);
    if (n && !observerReadable(reinterpret_cast<const void *>(begin), n * 4u)) {
        return false;
    }
    *slots = reinterpret_cast<const uintptr_t *>(begin);
    *count = n;
    return true;
}

void replayCaptureFlyState() {
    s_flyCount = 0;
    s_flyCaptured = false;
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;
    if (!replayFlySlots(&slots, &count)) {
        return;
    }
    const uintptr_t base = exeBase();
    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t fly = slots[i];
        if (!fly || !observerReadable(reinterpret_cast<const void *>(fly), 0xe0)) {
            continue;
        }
        const uint32_t vt = load32(fly) - static_cast<uint32_t>(base);
        for (const auto &f : FLY_FIELDS) {
            if (f[0] != vt) {
                continue;
            }
            if (s_flyCount == REPLAY_FLY_MAX) {
                s_flyCount = 0;
                return; // leave uncaptured rather than keep a partial set
            }
            ReplayFlyField &e = s_fly[s_flyCount++];
            e.slot = static_cast<uint16_t>(i);
            e.serial = *reinterpret_cast<const volatile uint16_t *>(fly + 0x7a);
            e.vtableRva = vt;
            e.offset = f[1];
            e.value = load32(fly + f[1]);
        }
    }
    s_flyCaptured = true;
}

bool replayFlyStateView(const ReplayFlyField **entries, uint32_t *count) {
    if (!s_flyCaptured) {
        return false;
    }
    *entries = s_fly;
    *count = s_flyCount;
    return true;
}

// ── Pending lockstep commands ────────────────────────────────────────────────

static const uintptr_t NET_MGR_RVA = 0x17e6a30;
static const uint32_t END_JOB_VTABLE_RVA = 0x5f32b0;
static const uint32_t QUEUE_JOB_ITEMS_MAX = 4096;

uint32_t replayNetQueuedCommands() {
    const uint32_t unknown = 0xffffffffu;
    const uintptr_t base = exeBase();
    const uintptr_t mgr = base + NET_MGR_RVA;
    if (!observerReadable(reinterpret_cast<const void *>(mgr + 0x10), 0x14)) {
        return unknown;
    }
    const uintptr_t storageBegin = load32(mgr + 0x10), storageEnd = load32(mgr + 0x14);
    uintptr_t cursor = load32(mgr + 0x18);
    const uint32_t count = load32(mgr + 0x20);
    if (!count) {
        return 0;
    }
    if (storageEnd <= storageBegin || (storageEnd - storageBegin) % 0x18 != 0 ||
        count > (storageEnd - storageBegin) / 0x18 || cursor < storageBegin ||
        cursor >= storageEnd || (cursor - storageBegin) % 0x18 != 0 ||
        !observerReadable(reinterpret_cast<const void *>(storageBegin), storageEnd - storageBegin)) {
        return unknown;
    }
    uint32_t pending = 0;
    for (uint32_t j = 0; j < count; ++j, cursor += 0x18) {
        if (cursor == storageEnd) {
            cursor = storageBegin;
        }
        const uintptr_t b = load32(cursor), e = load32(cursor + 4);
        if (e < b || ((e - b) & 3u) != 0 || (e - b) / 4 > QUEUE_JOB_ITEMS_MAX ||
            (e > b && !observerReadable(reinterpret_cast<const void *>(b), e - b))) {
            return unknown;
        }
        for (uintptr_t p = b; p < e; p += 4) {
            const uintptr_t chore = load32(p);
            if (!chore || !observerReadable(reinterpret_cast<const void *>(chore), 4)) {
                return unknown;
            }
            if (load32(chore) != base + END_JOB_VTABLE_RVA) {
                ++pending;
            }
        }
    }
    return pending;
}

static const uintptr_t BUILDING_PLACER_RVA = 0xdb8c10;
static const uint32_t PLACEMENT_RECORD_SIZE = 0x10;

uint32_t replayPendingPlacements() {
    const uintptr_t placer = exeBase() + BUILDING_PLACER_RVA;
    if (!observerReadable(reinterpret_cast<const void *>(placer + 0x60), 8)) {
        return 0xffffffffu;
    }
    const uintptr_t begin = load32(placer + 0x60), end = load32(placer + 0x64);
    if (end < begin || (end - begin) % PLACEMENT_RECORD_SIZE != 0) {
        return 0xffffffffu;
    }
    return (uint32_t)((end - begin) / PLACEMENT_RECORD_SIZE);
}

// ── Checkpoint save ─────────────────────────────────────────────────────────
//
// SaveGame, 158 bytes in and before any serialization, announces "saving" to
// the game-state machine:
//
//   0042a37d  push eax                        ; eax = 6
//   0042a37e  mov  ecx, 0xa892a8              ; state-machine object
//   0042a383  mov  [0xa892b0], eax
//   0042a388  mov  [0xa892ac], eax
//   0042a38d  call 0x40c2a0                   ; SetState(6), ends in `ret 4`
//
// Its virtual dispatch tears a networked session down to the lobby while the
// save itself completes. The push and the call are removed together (callee
// cleans 4 bytes). Signature is opcodes only: the three imm32 are relocated.

static const uintptr_t SAVE_GAME_RVA = 0x2a2e0;         // thiscall, ret 0x1c, bool in al
static const uintptr_t WSTRING_COPY_CTOR_RVA = 0x1d120; // thiscall(dest), 1 stack arg
static const uintptr_t SAVE_KIND_FLAG_RVA = 0xe46594;   // stock handler clears it first
static const uintptr_t STATE_BLOCK_RVA = 0x2a37d;
static const size_t STATE_BLOCK_LEN = 21;

struct SigByte {
    size_t off;
    uint8_t val;
};
static const SigByte STATE_SIG[] = {
    {0, 0x50},  {1, 0xb9},  {6, 0xa3},  {11, 0xa3}, {16, 0xe8},
    {17, 0x0e}, {18, 0x1f}, {19, 0xfe}, {20, 0xff},
};
static uint8_t s_origBytes[STATE_BLOCK_LEN];
static bool s_origCaptured = false;

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

static bool captureOriginal(uintptr_t base) {
    if (s_origCaptured) {
        return true;
    }
    const uint8_t *p = reinterpret_cast<const uint8_t *>(base + STATE_BLOCK_RVA);
    for (size_t i = 0; i < sizeof(STATE_SIG) / sizeof(STATE_SIG[0]); ++i) {
        if (p[STATE_SIG[i].off] != STATE_SIG[i].val) {
            return false;
        }
    }
    memcpy(s_origBytes, p, STATE_BLOCK_LEN);
    s_origCaptured = true;
    return true;
}

static bool setStateSuppression(uintptr_t base, int mode, bool on) {
    if (mode == 0) {
        return true;
    }
    if (!captureOriginal(base)) {
        return false;
    }
    uint8_t *p = reinterpret_cast<uint8_t *>(base + STATE_BLOCK_RVA);
    DWORD old = 0;
    if (!VirtualProtect(p, STATE_BLOCK_LEN, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    if (!on) {
        memcpy(p, s_origBytes, STATE_BLOCK_LEN);
    } else if (mode == 2) {
        memset(p, 0x90, STATE_BLOCK_LEN);
    } else {
        p[0] = 0x90;             // push eax
        memset(p + 16, 0x90, 5); // call 0x40c2a0
    }
    FlushInstructionCache(GetCurrentProcess(), p, STATE_BLOCK_LEN);
    VirtualProtect(p, STATE_BLOCK_LEN, old, &old);
    return true;
}

// The ESC-menu handler's sequence (VA 0x683e99..0x683ebb): build the by-value
// wstring in place with the game's copy-ctor (its allocator owns the buffer
// SaveGame destroys), then call SaveGame, which pops its 0x1c-byte argument.
static bool callSaveGame(uintptr_t base, const wchar_t *path) {
    GameWString src;
    src.u.ptr = const_cast<wchar_t *>(path);
    src.size = static_cast<uint32_t>(wcslen(path));
    src.res = src.size + 1;
    if (src.res < 8) {
        src.res = 8; // force the heap-pointer read path
    }
    *reinterpret_cast<volatile uint8_t *>(base + SAVE_KIND_FLAG_RVA) = 0;
    void *srcPtr = &src;
    const uintptr_t manager = base + WORLD_RVA;
    const uintptr_t ctor = base + WSTRING_COPY_CTOR_RVA;
    const uintptr_t saveFn = base + SAVE_GAME_RVA;
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

bool replayCheckpointSave(const wchar_t *path, int suppressState) {
    if (!path || !path[0]) {
        return false;
    }
    const uintptr_t base = exeBase();
    if (suppressState != 0 && !setStateSuppression(base, suppressState, true)) {
        return false; // wrong build: refuse rather than patch blindly
    }
    const bool ok = callSaveGame(base, path);
    if (suppressState != 0) {
        setStateSuppression(base, suppressState, false);
    }
    return ok;
}
