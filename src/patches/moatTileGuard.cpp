#include "moatTileGuard.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// A soldier filling a moat keeps the tile it is working on at +0x3b8. An outlaw
// that spots a target while walking there throws at it from an interrupt pushed
// over the fill-in-moat script, after its attack starter has already reset that
// tile to "none". When the script resumes, its work step indexes the tile-flag
// table with the reset value, about 52 MB past the table: a crash where nothing
// is mapped there, a garbage read that steers the unit where something is. This
// sends a script holding a tile that is not on the map down the stock "tile
// already filled" path, which looks for another tile nearby. A fix that changes
// the simulation, so it is switched on per match by patches/matchRules.cpp only
// when every player has it. Offsets and the trail live in
// docs/bugs/outlaw-moat-crash.md.

// Soldier::handleMessage, script 0x7d7 (fill in moat), step 1, right after the
// step's early-exit test (0x4e3490): lea ebx,[esi+0x3b8]. ecx and the flags are dead here.
static const uintptr_t SITE_RVA = 0xfa229;
static const uintptr_t RESUME_RVA = 0xfa22f; // push ebx: adds the tile to the moat list
static const uintptr_t MOVE_ON_RVA = 0xfa315; // reset the tile, look for another one

static const uint8_t SITE_STOCK[6] = {0x8d, 0x9e, 0xb8, 0x03, 0x00, 0x00};

static const int CAVE_LEN = 26;

// A packed tile is ((y * 256 + x) << 8) | layer, so it is on the map exactly when
// it is below 1 << 24. The "no tile" value (0xfffeff00) is not.
static const uint8_t CAVE_TEMPLATE[CAVE_LEN] = {
    0x8d, 0x9e, 0xb8, 0x03, 0x00, 0x00, // lea ebx, [esi+0x3b8]
    0x8b, 0x0b, // mov ecx, [ebx]
    0x81, 0xf9, 0x00, 0x00, 0x00, 0x01, // cmp ecx, 0x1000000
    0x72, 0x05, // jb on_map
    0xe9, 0x00, 0x00, 0x00, 0x00, // jmp move_on
    0xe9, 0x00, 0x00, 0x00, 0x00 // on_map: jmp resume
};

static const int CAVE_MOVE_ON_DISP = 0x11;
static const int CAVE_RESUME_DISP = 0x16;

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

// The move-on target must still begin the stock reset: mov edx,[abs32] (the
// "no tile" constant, base-relocated, so the operand is not compared) followed
// by mov [esi+0x3b8],edx.
static bool moveOnIsStock(uintptr_t base) {
    static const uint8_t LOAD[2] = {0x8b, 0x15};
    static const uint8_t STORE[6] = {0x89, 0x96, 0xb8, 0x03, 0x00, 0x00};
    const uint8_t *target = (const uint8_t *)(base + MOVE_ON_RVA);

    if (memcmp(target, LOAD, sizeof(LOAD)) != 0) {
        return false;
    }

    return memcmp(target + 6, STORE, sizeof(STORE)) == 0;
}

bool installMoatTileGuard() {
    uintptr_t base = gameBase();

    if (memcmp((void *)(base + SITE_RVA), SITE_STOCK, sizeof(SITE_STOCK)) != 0) {
        return false;
    }

    if (!moveOnIsStock(base)) {
        return false;
    }

    s_cave =
        (uint8_t *)VirtualAlloc(NULL, CAVE_LEN, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    if (!s_cave) {
        return false;
    }

    memcpy(s_cave, CAVE_TEMPLATE, CAVE_LEN);
    writeRel32(
        s_cave + CAVE_MOVE_ON_DISP, (uintptr_t)s_cave + CAVE_MOVE_ON_DISP + 4, base + MOVE_ON_RVA
    );
    writeRel32(
        s_cave + CAVE_RESUME_DISP, (uintptr_t)s_cave + CAVE_RESUME_DISP + 4, base + RESUME_RVA
    );

    memset(s_sitePatched, 0x90, sizeof(s_sitePatched));
    s_sitePatched[0] = 0xe9;
    writeRel32(s_sitePatched + 1, base + SITE_RVA + 5, (uintptr_t)s_cave);

    s_ready = true;

    return true;
}

// Called on the simulation thread only, so no soldier is half-way through the
// site while its bytes are rewritten.
void moatTileGuardSetEnabled(bool enabled) {
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

bool moatTileGuardAvailable() { return s_ready; }
