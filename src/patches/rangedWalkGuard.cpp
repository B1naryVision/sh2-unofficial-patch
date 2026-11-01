#include "rangedWalkGuard.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Archers, crossbowmen and axe throwers look for a new target every 40 ticks
// unless they are moving, and "moving" only means "has speed". A unit holding a
// walk order but standing still (waiting for its path, queued in a column)
// counted as idle, and starting the attack erased the walk. This makes the check
// also count a unit whose current action is a walk. A fix that changes the
// simulation, so it is switched on per match by patches/matchRules.cpp only when
// every player has it. Offsets and the reverse-engineering trail live in
// docs/bugs/ranged-move-orders.md.

// S2Actor's speed test (thiscall, no arguments, bool). 38 callers; only the
// three below are retargeted.
static const uintptr_t IS_MOVING_RVA = 0x85360;

// The auto-pick guard in each ranged think():
//   mov ecx,esi / call isMoving / test al,al / jne <skip the auto-pick>
static const int SITE_COUNT = 3;
static const uintptr_t CALL_RVAS[SITE_COUNT] = {
    0x113eb0, // Archer::think
    0xfe4e7, // Crossbowman::think
    0x10a367, // AxeThrower::think
};

static const uint8_t BEFORE_STOCK[2] = {0x8b, 0xce}; // mov ecx,esi
static const uint8_t AFTER_STOCK[4] = {0x84, 0xc0, 0x0f, 0x85}; // test al,al / jne rel32

// The actor's action stack (std::vector<GlowWorm::Action *>) and the class of
// the action an ordered move pushes on top of it.
static const uintptr_t ACTOR_STACK_BEGIN = 0x158;
static const uintptr_t ACTOR_STACK_END = 0x15c;
static const uintptr_t GOTO_LOCATION_VT_RVA = 0x5f4738;

typedef bool(__attribute__((thiscall)) * IsMovingFn)(void *);

static uintptr_t s_base = 0;
static IsMovingFn s_isMoving = nullptr;
static int32_t s_stockRel[SITE_COUNT];
static int32_t s_patchedRel[SITE_COUNT];
static bool s_ready = false;
static bool s_enabled = false;

static bool walkingOnTop(const uint8_t *actor) {
    uint32_t begin = 0;
    uint32_t end = 0;
    uint32_t top = 0;

    memcpy(&begin, actor + ACTOR_STACK_BEGIN, sizeof(begin));
    memcpy(&end, actor + ACTOR_STACK_END, sizeof(end));

    if (end <= begin) {
        return false;
    }

    top = *(const uint32_t *)(uintptr_t)(end - 4);

    if (!top) {
        return false;
    }

    return *(const uint32_t *)(uintptr_t)top == s_base + GOTO_LOCATION_VT_RVA;
}

// Reads only simulation state the actor already holds, so every client decides
// the same way on the same tick.
static bool __attribute__((thiscall)) movingOrWalking(void *actor) {
    if (s_isMoving(actor)) {
        return true;
    }

    return walkingOnTop((const uint8_t *)actor);
}

static bool siteIsStock(uintptr_t site) {
    int32_t rel = 0;

    if (memcmp((const void *)(site - sizeof(BEFORE_STOCK)), BEFORE_STOCK, sizeof(BEFORE_STOCK)) !=
        0) {
        return false;
    }

    if (*(const uint8_t *)site != 0xe8) {
        return false;
    }

    if (memcmp((const void *)(site + 5), AFTER_STOCK, sizeof(AFTER_STOCK)) != 0) {
        return false;
    }

    memcpy(&rel, (const void *)(site + 1), sizeof(rel));

    return site + 5 + rel == s_base + IS_MOVING_RVA;
}

bool installRangedWalkGuard() {
    s_base = (uintptr_t)GetModuleHandleA(NULL);

    // All-or-nothing: the three classes must behave alike.
    for (int i = 0; i < SITE_COUNT; ++i) {
        if (!siteIsStock(s_base + CALL_RVAS[i])) {
            return false;
        }
    }

    for (int i = 0; i < SITE_COUNT; ++i) {
        uintptr_t site = s_base + CALL_RVAS[i];
        memcpy(&s_stockRel[i], (const void *)(site + 1), sizeof(s_stockRel[i]));
        s_patchedRel[i] = (int32_t)((uintptr_t)(void *)movingOrWalking - (site + 5));
    }

    s_isMoving = (IsMovingFn)(s_base + IS_MOVING_RVA);
    s_ready = true;

    return true;
}

// Called on the simulation thread only, so no think() is half-way through a
// call operand being rewritten.
void rangedWalkGuardSetEnabled(bool enabled) {
    if (!s_ready || enabled == s_enabled) {
        return;
    }

    for (int i = 0; i < SITE_COUNT; ++i) {
        uint8_t *operand = (uint8_t *)(s_base + CALL_RVAS[i] + 1);
        const int32_t *rel = &s_stockRel[i];
        DWORD oldProtect = 0;

        if (enabled) {
            rel = &s_patchedRel[i];
        }

        if (!VirtualProtect(operand, sizeof(*rel), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            continue;
        }

        memcpy(operand, rel, sizeof(*rel));
        VirtualProtect(operand, sizeof(*rel), oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), operand - 1, 5);
    }

    s_enabled = enabled;
}

bool rangedWalkGuardAvailable() { return s_ready; }
