#include "ballistaAutoFire.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Field ballistae (FireBallista::think, RVA 0x180c30) auto-target and fire like
// tower ballistae. A match rule: it changes the simulation, so it is switched on
// and off per match by patches/matchRules.cpp and never installed on its own.
// Sites, bytes and the cave layout are in docs/features/ballista-auto-fire.md.

static const uintptr_t SITE_GATE_RVA = 0x180c48; // deployment-flag gate -> jmp to cave
static const uintptr_t SITE_PLAYER_RVA = 0x180c71; // human-player je -> NOPs
static const uintptr_t SITE_DEAD_RVA = 0x180c92; // dead [this+0xb4] je -> jmp short
static const uintptr_t SITE_CALL_RVA = 0x180ef9; // cached-target call -> fresh search

static const uintptr_t MANUAL_HANDLER_RVA = 0x180f39;
static const uintptr_t AUTO_FIRE_RVA = 0x180c55; // the player check after the gate
static const uintptr_t CACHED_TARGET_RVA = 0x17f8c0;
static const uintptr_t FRESH_SEARCH_RVA = 0x177b90; // Ballista's target search
static const uintptr_t IS_ALIVE_RVA = 0x15ed0; // S2Actor::isAlive
static const uintptr_t CAN_ENGAGE_SLOT_RVA = 0x5c1294; // FireBallista vtable +0x180
static const uintptr_t CAN_ENGAGE_RVA = 0x1778d0; // shared with Ballista

static const uint8_t GATE_STOCK[13] = {0x80, 0xbe, 0x08, 0x03, 0x00, 0x00, 0x00,
                                       0x0f, 0x85, 0xe4, 0x02, 0x00, 0x00};
static const uint8_t PLAYER_STOCK[6] = {0x0f, 0x84, 0xc2, 0x02, 0x00, 0x00};
static const uint8_t DEAD_STOCK[1] = {0x74};

// isAlive must be the stock `[ecx+0x98] == 0 && [ecx+0x20] != 3` before the
// cave calls it.
static const uint8_t IS_ALIVE_STOCK[13] = {0x83, 0xb9, 0x98, 0x00, 0x00, 0x00, 0x00,
                                           0x75, 0x0c, 0x83, 0x79, 0x20, 0x03};

static const int CAVE_LEN = 118;

// Keeps a held target only while the engine's own canEngage (vtable +0x180:
// resolves, in range, line of sight) and isAlive accept it. Otherwise the target
// is released and +0x352 cleared, so the tick gate searches again this tick.
// isAlive gets the cached pointer at +0x194, as the first version of this rule
// did: that is the behaviour recordings and every other client run, so it must
// not change under the same rule bit. Only when the cache is null does the cave
// resolve the id at +0x198 itself, the way canEngage does. A load restores only
// the id, so after loading a save in which a ballista holds a target the cache
// is null while the id resolves; the first version passed that null to isAlive
// and crashed (2026-11-15). A fresh resolve for every check changed which
// targets were kept and broke the playback of a recorded match (2026-11-21).
static const uint8_t CAVE_TEMPLATE[CAVE_LEN] = {
    0x83, 0xbe, 0x98, 0x01, 0x00, 0x00, 0x00, // 00 cmp dword [esi+0x198], 0
    0x74, 0x68, // 07 je auto_fire
    0xff, 0xb6, 0x98, 0x01, 0x00, 0x00, // 09 push dword [esi+0x198]
    0xff, 0xb6, 0x94, 0x01, 0x00, 0x00, // 0f push dword [esi+0x194]
    0x6a, 0x00, // 15 push 0
    0x8b, 0x06, // 17 mov eax, [esi]
    0x8b, 0xce, // 19 mov ecx, esi
    0xff, 0x90, 0x80, 0x01, 0x00, 0x00, // 1b call [eax+0x180]
    0x84, 0xc0, // 21 test al, al
    0x74, 0x38, // 23 je release
    0x8b, 0x8e, 0x94, 0x01, 0x00, 0x00, // 25 mov ecx, [esi+0x194]
    0x85, 0xc9, // 2b test ecx, ecx
    0x75, 0x20, // 2d jne alive
    0x8b, 0x86, 0x98, 0x01, 0x00, 0x00, // 2f mov eax, [esi+0x198]
    0x50, // 35 push eax
    0xc1, 0xe8, 0x10, // 36 shr eax, 16
    0x50, // 39 push eax
    0x68, 0x00, 0x00, 0x00, 0x00, // 3a push <actor handle class id>
    0xb9, 0x00, 0x00, 0x00, 0x00, // 3f mov ecx, <actor manager>
    0xe8, 0x00, 0x00, 0x00, 0x00, // 44 call resolve
    0x85, 0xc0, // 49 test eax, eax
    0x74, 0x10, // 4b je release
    0x8b, 0xc8, // 4d mov ecx, eax
    0xe8, 0x00, 0x00, 0x00, 0x00, // 4f alive: call S2Actor::isAlive
    0x84, 0xc0, // 54 test al, al
    0x74, 0x05, // 56 je release
    0xe9, 0x00, 0x00, 0x00, 0x00, // 58 jmp manual handler
    0x31, 0xc0, // 5d release: xor eax, eax
    0x89, 0x86, 0x94, 0x01, 0x00, 0x00, // 5f mov [esi+0x194], eax
    0x89, 0x86, 0x98, 0x01, 0x00, 0x00, // 65 mov [esi+0x198], eax
    0x88, 0x86, 0x52, 0x03, 0x00, 0x00, // 6b mov [esi+0x352], al
    0xe9, 0x00, 0x00, 0x00, 0x00 // 71 auto_fire: jmp back to the player check
};

static const int CAVE_CLASS_IMM = 0x3b;
static const int CAVE_MANAGER_IMM = 0x40;
static const int CAVE_RESOLVE_DISP = 0x45;
static const int CAVE_IS_ALIVE_DISP = 0x50;
static const int CAVE_MANUAL_DISP = 0x59;
static const int CAVE_RETURN_DISP = 0x72;

// canEngage's own resolve (RVA 0x1778e9): push <class id>; mov ecx, <actor
// manager>; call ActorMgr::resolve. Both immediates are base-relocated, so the
// cave copies them from the loaded code instead of rebuilding them.
static const uintptr_t CAN_ENGAGE_RESOLVE_RVA = 0x1778e9;
static const uintptr_t RESOLVE_RVA = 0x369360;
static const uintptr_t ACTOR_CLASS_VA = 0x12b0e98;
static const uintptr_t ACTOR_MANAGER_VA = 0xae8728;

static uint8_t *s_cave = nullptr;
static uint8_t s_gatePatched[13];
static uint8_t s_callStock[5];
static uint8_t s_callPatched[5];
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

bool installBallistaAutoFire() {
    uintptr_t base = gameBase();

    // The call's stock bytes encode the distance to its target, so rebuild them
    // rather than trusting a literal.
    s_callStock[0] = 0xe8;
    writeRel32(s_callStock + 1, base + SITE_CALL_RVA + 5, base + CACHED_TARGET_RVA);

    // All-or-nothing: every site must hold the stock bytes before any is touched.
    if (memcmp((void *)(base + SITE_GATE_RVA), GATE_STOCK, sizeof(GATE_STOCK)) != 0) {
        return false;
    }

    if (memcmp((void *)(base + SITE_PLAYER_RVA), PLAYER_STOCK, sizeof(PLAYER_STOCK)) != 0) {
        return false;
    }

    if (memcmp((void *)(base + SITE_DEAD_RVA), DEAD_STOCK, sizeof(DEAD_STOCK)) != 0) {
        return false;
    }

    if (memcmp((void *)(base + SITE_CALL_RVA), s_callStock, sizeof(s_callStock)) != 0) {
        return false;
    }

    if (memcmp((void *)(base + IS_ALIVE_RVA), IS_ALIVE_STOCK, sizeof(IS_ALIVE_STOCK)) != 0) {
        return false;
    }

    // The cave calls canEngage through the vtable; the slot is a relocated
    // pointer, so compare it against the rebuilt address.
    if (*(uint32_t *)(base + CAN_ENGAGE_SLOT_RVA) != (uint32_t)(base + CAN_ENGAGE_RVA)) {
        return false;
    }

    // The resolve the cave repeats: canEngage's, operand for operand.
    const uint8_t *resolve = (const uint8_t *)(base + CAN_ENGAGE_RESOLVE_RVA);
    uint32_t classId = 0, manager = 0;
    int32_t call = 0;
    memcpy(&classId, resolve + 1, sizeof(classId));
    memcpy(&manager, resolve + 6, sizeof(manager));
    memcpy(&call, resolve + 11, sizeof(call));

    if (resolve[0] != 0x68 || resolve[5] != 0xb9 || resolve[10] != 0xe8 ||
        classId != (uint32_t)(base + ACTOR_CLASS_VA - 0x400000) ||
        manager != (uint32_t)(base + ACTOR_MANAGER_VA - 0x400000) ||
        (uintptr_t)(resolve + 15) + call != base + RESOLVE_RVA) {
        return false;
    }

    s_cave =
        (uint8_t *)VirtualAlloc(NULL, CAVE_LEN, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    if (!s_cave) {
        return false;
    }

    memcpy(s_cave, CAVE_TEMPLATE, CAVE_LEN);
    memcpy(s_cave + CAVE_CLASS_IMM, &classId, sizeof(classId));
    memcpy(s_cave + CAVE_MANAGER_IMM, &manager, sizeof(manager));
    writeRel32(
        s_cave + CAVE_RESOLVE_DISP, (uintptr_t)s_cave + CAVE_RESOLVE_DISP + 4, base + RESOLVE_RVA
    );
    writeRel32(
        s_cave + CAVE_IS_ALIVE_DISP, (uintptr_t)s_cave + CAVE_IS_ALIVE_DISP + 4, base + IS_ALIVE_RVA
    );
    writeRel32(
        s_cave + CAVE_MANUAL_DISP, (uintptr_t)s_cave + CAVE_MANUAL_DISP + 4,
        base + MANUAL_HANDLER_RVA
    );
    writeRel32(
        s_cave + CAVE_RETURN_DISP, (uintptr_t)s_cave + CAVE_RETURN_DISP + 4, base + AUTO_FIRE_RVA
    );

    memset(s_gatePatched, 0x90, sizeof(s_gatePatched));
    s_gatePatched[0] = 0xe9;
    writeRel32(s_gatePatched + 1, base + SITE_GATE_RVA + 5, (uintptr_t)s_cave);

    s_callPatched[0] = 0xe8;
    writeRel32(s_callPatched + 1, base + SITE_CALL_RVA + 5, base + FRESH_SEARCH_RVA);

    s_ready = true;

    return true;
}

// Called on the simulation thread only, between or during ticks, so no
// FireBallista::think is half-way through the bytes being rewritten.
void ballistaAutoFireSetEnabled(bool enabled) {
    static const uint8_t PLAYER_NOPS[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    static const uint8_t DEAD_JMP[1] = {0xeb};
    uintptr_t base = gameBase();

    if (!s_ready || enabled == s_enabled) {
        return;
    }

    if (enabled) {
        writeCode(base + SITE_GATE_RVA, s_gatePatched, sizeof(s_gatePatched));
        writeCode(base + SITE_PLAYER_RVA, PLAYER_NOPS, sizeof(PLAYER_NOPS));
        writeCode(base + SITE_DEAD_RVA, DEAD_JMP, sizeof(DEAD_JMP));
        writeCode(base + SITE_CALL_RVA, s_callPatched, sizeof(s_callPatched));
    } else {
        writeCode(base + SITE_GATE_RVA, GATE_STOCK, sizeof(GATE_STOCK));
        writeCode(base + SITE_PLAYER_RVA, PLAYER_STOCK, sizeof(PLAYER_STOCK));
        writeCode(base + SITE_DEAD_RVA, DEAD_STOCK, sizeof(DEAD_STOCK));
        writeCode(base + SITE_CALL_RVA, s_callStock, sizeof(s_callStock));
    }

    s_enabled = enabled;
}

bool ballistaAutoFireAvailable() { return s_ready; }
