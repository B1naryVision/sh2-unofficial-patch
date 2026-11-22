#include "keyframeSerial.h"

#include "../core/exeBase.h"
#include "../core/hook.h"

#include <cstring>
#include <windows.h>

// GlowWorm::Actor::serialize, version-2 path: `fld dword [edi+0xdc]` starts
// the float-to-whole-number round trip of the path progress, which ends with
// the `fstp` at 0x7896fb; 0x789701 is the next field. edi = actor, esi =
// stream. Nothing after 0x789701 reads eax, ecx, edx or the flags, and the only
// branch into the block is its own `jns` at 0x7896f3.
static const uintptr_t PATH_SITE_RVA = 0x3896b8;
static const uintptr_t PATH_SKIP_RVA = 0x389701;
static const uint8_t PATH_SITE_STOCK[] = {0xd9, 0x87, 0xdc, 0x00, 0x00, 0x00};

// Every handle resolver a serializer calls (a census of the serializer shape
// over the whole exe, grouped by callee): thiscall (classId, index, serial),
// `ret 0xc`, pure reads, no branch into the first bytes. Four start
// `push ebp; mov ebp,esp; movzx eax,word [ebp+0xc]`, the element one
// `push ebp; mov ebp,esp; mov edx,[ecx+0xc]`.
static const uint8_t PROLOGUE_MOVZX[] = {0x55, 0x8b, 0xec, 0x0f, 0xb7, 0x45, 0x0c};
static const uint8_t PROLOGUE_MOVEDX[] = {0x55, 0x8b, 0xec, 0x8b, 0x51, 0x0c};

enum Resolver { ACTOR, BUILDING, FLY, CLAN, ELEMENT, RESOLVERS };

struct ResolverSite {
    uintptr_t rva;
    const uint8_t *stock;
    size_t length;
    uint32_t serializers; // serializer-shaped callers, for the record
};

static const ResolverSite RESOLVER_SITES[RESOLVERS] = {
    {0x369360, PROLOGUE_MOVZX, sizeof(PROLOGUE_MOVZX), 107}, // ActorMgr, 0xae8728
    {0x368550, PROLOGUE_MOVZX, sizeof(PROLOGUE_MOVZX), 41}, // BuildingMgr, 0xae5f40
    {0x36a000, PROLOGUE_MOVZX, sizeof(PROLOGUE_MOVZX), 12}, // FlyMgr, 0xae8800
    {0x374e20, PROLOGUE_MOVZX, sizeof(PROLOGUE_MOVZX), 22}, // ClanMgr, 0x107937c
    {0x367e40, PROLOGUE_MOVEDX, sizeof(PROLOGUE_MOVEDX), 6}, // ElementMgr, 0xae5e74
};

// 1.5 MB, only during a save; a late-match save logs about 8,000.
static const uint32_t LOG_MAX = 1u << 17;

struct HandleEntry {
    uint32_t address;
    uint32_t cached;
    uint32_t id;
};

// Not static: GAS names them from the naked hooks.
extern "C" {
volatile uint32_t g_keyframeSaving = 0;
// 0 stock, 1 raw float bits (save and load), 2 stock with the live value
// logged first and put back after the save (save only).
volatile uint32_t g_keyframePathMode = 0;
uintptr_t g_keyframePathResume = 0;
uintptr_t g_keyframePathSkip = 0;
uintptr_t g_keyframeResume[RESOLVERS] = {};
void keyframePathHook();
void keyframeResolveHook0();
void keyframeResolveHook1();
void keyframeResolveHook2();
void keyframeResolveHook3();
void keyframeResolveHook4();
void keyframePathLog(uint32_t actor);
uint32_t keyframeResolveFilter(
    uint32_t ret, uint32_t result, uint32_t ebp, uint32_t ebx, uint32_t esi, uint32_t edi
);
}

static bool s_installed = false;
static uintptr_t s_textBegin = 0, s_textEnd = 0;
static DWORD s_thread = 0;
static HandleEntry *s_log = nullptr;
// Read, never written: what a handle that does not resolve points at for the
// rest of the save, so that a stray read finds zeros rather than faulting.
static uint32_t s_unresolved[64];
static uint32_t s_logCount = 0;
static bool s_overflow = false;
static bool s_rawPath = true;

struct PathEntry {
    uint32_t address;
    uint32_t value;
};

static const uint32_t PATH_LOG_MAX = 1u << 16; // one per actor slot
static PathEntry *s_pathLog = nullptr;
static uint32_t s_pathCount = 0;

// Mode 1: the stream's io (vtable +4, thiscall (void *, size)) on the float
// itself, then on to the next field. Mode 2: the live float is logged, then the
// stock round trip runs. Otherwise the overwritten `fld`. The flags are dead
// here, and the x87 stack is empty (a call just returned).
__declspec(naked) void keyframePathHook() {
    __asm__ volatile("cmpl $1, _g_keyframePathMode\n\t"
                     "je 1f\n\t"
                     "cmpl $2, _g_keyframePathMode\n\t"
                     "jne 2f\n\t"
                     "pushl %eax\n\t"
                     "pushl %ecx\n\t"
                     "pushl %edx\n\t"
                     "pushl %edi\n\t"
                     "call _keyframePathLog\n\t"
                     "addl $4, %esp\n\t"
                     "popl %edx\n\t"
                     "popl %ecx\n\t"
                     "popl %eax\n\t"
                     "2:\n\t"
                     "flds 0xdc(%edi)\n\t"
                     "jmp *_g_keyframePathResume\n\t"
                     "1:\n\t"
                     "movl (%esi), %edx\n\t"
                     "pushl $4\n\t"
                     "leal 0xdc(%edi), %eax\n\t"
                     "pushl %eax\n\t"
                     "movl %esi, %ecx\n\t"
                     "call *4(%edx)\n\t"
                     "jmp *_g_keyframePathSkip\n\t");
}

// Saving: run the stock resolver on a copy of the arguments, then let the
// filter log the handle the caller is about to write. ecx (the manager) is
// untouched until the stock body runs. The caller's ebx, esi, edi and ebp are
// still its own here, which is what lets the filter find that handle.
// `prologue` re-emits the overwritten bytes, `index` picks the resume address.
#define KEYFRAME_RESOLVE_HOOK(name, prologue, index)                                               \
    __declspec(naked) void name() {                                                                \
        __asm__ volatile("cmpl $0, _g_keyframeSaving\n\t"                                          \
                         "jne 1f\n\t" prologue "jmp *_g_keyframeResume+" #index "\n\t"             \
                         "1:\n\t"                                                                  \
                         "pushl 0xc(%esp)\n\t"                                                     \
                         "pushl 0xc(%esp)\n\t"                                                     \
                         "pushl 0xc(%esp)\n\t"                                                     \
                         "call 2f\n\t"                                                             \
                         "pushl %edi\n\t"                                                          \
                         "pushl %esi\n\t"                                                          \
                         "pushl %ebx\n\t"                                                          \
                         "pushl %ebp\n\t"                                                          \
                         "pushl %eax\n\t"                                                          \
                         "pushl 0x14(%esp)\n\t"                                                    \
                         "call _keyframeResolveFilter\n\t"                                         \
                         "addl $0x18, %esp\n\t"                                                    \
                         "ret $0xc\n\t"                                                            \
                         "2:\n\t" prologue "jmp *_g_keyframeResume+" #index "\n\t");               \
    }

#define MOVZX_PROLOGUE "pushl %ebp\n\tmovl %esp, %ebp\n\tmovzwl 0xc(%ebp), %eax\n\t"
#define MOVEDX_PROLOGUE "pushl %ebp\n\tmovl %esp, %ebp\n\tmovl 0xc(%ecx), %edx\n\t"

// The resume table holds 4-byte entries: the index is a byte offset.
KEYFRAME_RESOLVE_HOOK(keyframeResolveHook0, MOVZX_PROLOGUE, 0)
KEYFRAME_RESOLVE_HOOK(keyframeResolveHook1, MOVZX_PROLOGUE, 4)
KEYFRAME_RESOLVE_HOOK(keyframeResolveHook2, MOVZX_PROLOGUE, 8)
KEYFRAME_RESOLVE_HOOK(keyframeResolveHook3, MOVZX_PROLOGUE, 12)
KEYFRAME_RESOLVE_HOOK(keyframeResolveHook4, MOVEDX_PROLOGUE, 16)

static void (*const RESOLVE_HOOKS[RESOLVERS])() = {
    keyframeResolveHook0, keyframeResolveHook1, keyframeResolveHook2,
    keyframeResolveHook3, keyframeResolveHook4,
};

// `mov [reg+disp], eax` (89 /0) at p: its length, base register and
// displacement, or 0 when it is anything else.
static int decodeStoreEax(const uint8_t *p, int *rm, int32_t *disp) {
    if (p[0] != 0x89) {
        return 0;
    }

    const int mod = p[1] >> 6;
    const int reg = (p[1] >> 3) & 7;
    *rm = p[1] & 7;

    if (reg != 0 || mod == 3 || *rm == 4) {
        return 0;
    }

    if (mod == 0) {
        if (*rm == 5) {
            return 0;
        }

        *disp = 0;
        return 2;
    }

    if (mod == 1) {
        *disp = (int8_t)p[2];
        return 3;
    }

    memcpy(disp, p + 2, sizeof(*disp));
    return 6;
}

// The handle serializer's shape after the resolve, at the return address:
//   mov [h], eax / test eax, eax / jne next / mov [h+4], eax / next:
// with h in a callee-saved register. Returns h, or 0 for any other caller.
static uintptr_t handleAt(uint32_t ret, uint32_t ebp, uint32_t ebx, uint32_t esi, uint32_t edi) {
    if (ret < s_textBegin || ret + 16 > s_textEnd) {
        return 0;
    }

    const uint8_t *p = (const uint8_t *)ret;
    int rm = 0, rm2 = 0;
    int32_t disp = 0, disp2 = 0;
    const int first = decodeStoreEax(p, &rm, &disp);

    if (!first || p[first] != 0x85 || p[first + 1] != 0xc0 || p[first + 2] != 0x75) {
        return 0;
    }

    const int second = decodeStoreEax(p + first + 4, &rm2, &disp2);

    if (!second || rm2 != rm || disp2 != disp + 4) {
        return 0;
    }

    if ((int8_t)p[first + 3] != second) {
        return 0; // the jne must skip exactly the id store
    }

    uint32_t base = 0;

    switch (rm) {
    case 3:
        base = ebx;
        break;
    case 5:
        base = ebp;
        break;
    case 6:
        base = esi;
        break;
    case 7:
        base = edi;
        break;
    default:
        return 0;
    }

    return (uintptr_t)(base + (uint32_t)disp);
}

void keyframePathLog(uint32_t actor) {
    if (!s_pathLog || s_pathCount >= PATH_LOG_MAX) {
        s_overflow = true;
        return;
    }

    s_pathLog[s_pathCount].address = actor + 0xdc;
    s_pathLog[s_pathCount].value = *(const uint32_t *)(uintptr_t)(actor + 0xdc);
    ++s_pathCount;
}

uint32_t keyframeResolveFilter(
    uint32_t ret, uint32_t result, uint32_t ebp, uint32_t ebx, uint32_t esi, uint32_t edi
) {
    if (GetCurrentThreadId() != s_thread) {
        return result;
    }

    const uintptr_t handle = handleAt(ret, ebp, ebx, esi, edi);

    if (!handle) {
        return result; // not a handle serializer: the stock answer
    }

    const uint32_t cached = *(const uint32_t *)handle;

    // A temporary in a serializer's frame is gone by the time the save
    // returns, and putting it back would write into whatever stack is there
    // then; it is not logged.
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    const bool onStack = handle >= (uintptr_t)tib->StackLimit && handle < (uintptr_t)tib->StackBase;

    if (!onStack) {
        if (s_logCount >= LOG_MAX) {
            // Cannot be put back: stock behaviour from here, keyframe discarded.
            s_overflow = true;
            return result;
        }

        s_log[s_logCount].address = (uint32_t)handle;
        s_log[s_logCount].cached = cached;
        s_log[s_logCount].id = *(const uint32_t *)(handle + 4);
        ++s_logCount;
    }

    if (result) {
        return result;
    }

    // Does not resolve: anything non-zero skips the id store, so the stale id
    // is written as the live world holds it. Keep the cached pointer the live
    // world already had; the log puts it back in any case.
    if (cached) {
        return cached;
    }

    return (uint32_t)(uintptr_t)s_unresolved;
}

void keyframeSerialSetRawPath(bool raw) { s_rawPath = raw; }

bool keyframeSerialRawPath() { return s_rawPath; }

bool keyframeSaveBegin(bool *rawPath) {
    if (!s_installed || g_keyframeSaving) {
        return false;
    }

    if (!s_pathLog) {
        s_pathLog = (PathEntry *)VirtualAlloc(
            NULL, PATH_LOG_MAX * sizeof(PathEntry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
        );
    }

    if (!s_log) {
        s_log = (HandleEntry *)VirtualAlloc(
            NULL, LOG_MAX * sizeof(HandleEntry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
        );
    }

    if (!s_log || !s_pathLog) {
        return false;
    }

    s_logCount = 0;
    s_pathCount = 0;
    s_overflow = false;
    s_thread = GetCurrentThreadId();
    g_keyframePathMode = s_rawPath ? 1 : 2;
    *rawPath = s_rawPath;
    g_keyframeSaving = 1;
    return true;
}

bool keyframeSaveEnd(uint32_t *handles) {
    g_keyframeSaving = 0;
    g_keyframePathMode = 0;

    if (handles) {
        *handles = s_logCount;
    }

    if (!s_log) {
        return false;
    }

    // Newest first, so a handle written twice ends on its value before both.
    for (uint32_t i = s_logCount; i > 0; --i) {
        uint32_t *handle = (uint32_t *)(uintptr_t)s_log[i - 1].address;
        handle[0] = s_log[i - 1].cached;
        handle[1] = s_log[i - 1].id;
    }

    for (uint32_t i = s_pathCount; i > 0; --i) {
        *(uint32_t *)(uintptr_t)s_pathLog[i - 1].address = s_pathLog[i - 1].value;
    }

    s_logCount = 0;
    s_pathCount = 0;
    VirtualFree(s_log, 0, MEM_RELEASE);
    s_log = nullptr;
    VirtualFree(s_pathLog, 0, MEM_RELEASE);
    s_pathLog = nullptr;
    return !s_overflow;
}

void keyframeLoadBegin(bool rawPath) {
    if (s_installed && rawPath) {
        g_keyframePathMode = 1;
    }
}

void keyframeLoadEnd() { g_keyframePathMode = 0; }

bool keyframeSerialInstalled() { return s_installed; }

void installKeyframeSerial() {
    const uintptr_t base = exeBase();
    uint8_t *path = (uint8_t *)(base + PATH_SITE_RVA);

    // All or nothing: a keyframe needs every one of them.
    if (memcmp(path, PATH_SITE_STOCK, sizeof(PATH_SITE_STOCK)) != 0) {
        return;
    }

    for (const ResolverSite &r : RESOLVER_SITES) {
        if (memcmp((const void *)(base + r.rva), r.stock, r.length) != 0) {
            return;
        }
    }

    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);

    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_CNT_CODE) {
            s_textBegin = base + sec[i].VirtualAddress;
            s_textEnd = s_textBegin + sec[i].Misc.VirtualSize;
            break;
        }
    }

    if (!s_textBegin) {
        return;
    }

    g_keyframePathResume = (uintptr_t)path + sizeof(PATH_SITE_STOCK);
    g_keyframePathSkip = base + PATH_SKIP_RVA;
    installHook(path, (void *)keyframePathHook, sizeof(PATH_SITE_STOCK));

    for (int i = 0; i < RESOLVERS; ++i) {
        const ResolverSite &r = RESOLVER_SITES[i];
        g_keyframeResume[i] = base + r.rva + r.length;
        installHook((void *)(base + r.rva), (void *)RESOLVE_HOOKS[i], r.length);
    }
    s_installed = true;
}
