#include "deadShieldPassThrough.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Every projectile asks S2Game's blocker list (Mantlets and Cats) whether a
// shield catches it before testing anything else, and that lookup never checks
// health, so a dead shield keeps catching shots until its death animation ends
// and it leaves the list. This skips a blocker the damage sink already counts as
// dead. A match rule: it changes the simulation, so it is switched on and off per
// match by patches/matchRules.cpp. Site, bytes and the cave layout are in
// docs/features/dead-shield-pass-through.md.

// S2Game::findBlocker (RVA 0x21f10), right after the entry's actor resolved:
//   mov ebx,[esi+0x58] / mov eax,[ebp+0x1c]
static const uintptr_t SITE_RVA = 0x21fdb;
static const uintptr_t RESUME_RVA = 0x21fe1; // cmp eax,ebx: the friendly test
static const uintptr_t SKIP_RVA = 0x22144; // next entry, where the friendly skip goes

static const uint8_t SITE_STOCK[6] = {0x8b, 0x5e, 0x58, 0x8b, 0x45, 0x1c};

static const int CAVE_LEN = 31;

// The sink's own "already dead" test (max health non-zero, health <= 0), on the
// raw float bits so the cave stays off the x87 stack.
static const uint8_t CAVE_TEMPLATE[CAVE_LEN] = {
    0xf7, 0x46, 0x48, 0xff, 0xff, 0xff, 0x7f, // test dword [esi+0x48], 0x7fffffff
    0x74, 0x0b, // jz alive
    0x83, 0x7e, 0x4c, 0x00, // cmp dword [esi+0x4c], 0
    0x7f, 0x05, // jg alive
    0xe9, 0x00, 0x00, 0x00, 0x00, // jmp skip
    0x8b, 0x5e, 0x58, // alive: mov ebx, [esi+0x58]
    0x8b, 0x45, 0x1c, // mov eax, [ebp+0x1c]
    0xe9, 0x00, 0x00, 0x00, 0x00 // jmp resume
};

static const int CAVE_SKIP_DISP = 0x10;
static const int CAVE_RESUME_DISP = 0x1b;

static uint8_t *s_cave = nullptr;
static uint8_t s_sitePatched[6];
static bool s_ready = false;
static bool s_enabled = false;

static uintptr_t gameBase() { return (uintptr_t)GetModuleHandleA(NULL); }

static void writeRel32(uint8_t *at, uintptr_t from, uintptr_t to) {
    int32_t disp = (int32_t)(to - from);

    memcpy(at, &disp, sizeof(disp));
}

static void writeCode(uintptr_t address, const uint8_t *bytes, size_t len) {
    DWORD oldProtect = 0;

    VirtualProtect((void *)address, len, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void *)address, bytes, len);
    VirtualProtect((void *)address, len, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void *)address, len);
}

bool installDeadShieldPassThrough() {
    uintptr_t base = gameBase();

    if (memcmp((void *)(base + SITE_RVA), SITE_STOCK, sizeof(SITE_STOCK)) != 0) {
        return false;
    }

    s_cave =
        (uint8_t *)VirtualAlloc(NULL, CAVE_LEN, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    if (!s_cave) {
        return false;
    }

    memcpy(s_cave, CAVE_TEMPLATE, CAVE_LEN);
    writeRel32(s_cave + CAVE_SKIP_DISP, (uintptr_t)s_cave + CAVE_SKIP_DISP + 4, base + SKIP_RVA);
    writeRel32(
        s_cave + CAVE_RESUME_DISP, (uintptr_t)s_cave + CAVE_RESUME_DISP + 4, base + RESUME_RVA
    );

    memset(s_sitePatched, 0x90, sizeof(s_sitePatched));
    s_sitePatched[0] = 0xe9;
    writeRel32(s_sitePatched + 1, base + SITE_RVA + 5, (uintptr_t)s_cave);

    s_ready = true;

    return true;
}

// Called on the simulation thread only, so no projectile is half-way through
// the lookup while its bytes are rewritten.
void deadShieldPassThroughSetEnabled(bool enabled) {
    uintptr_t base = gameBase();

    if (!s_ready || enabled == s_enabled) {
        return;
    }

    if (enabled) {
        writeCode(base + SITE_RVA, s_sitePatched, sizeof(s_sitePatched));
    } else {
        writeCode(base + SITE_RVA, SITE_STOCK, sizeof(SITE_STOCK));
    }

    s_enabled = enabled;
}

bool deadShieldPassThroughAvailable() { return s_ready; }
