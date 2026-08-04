#include "disbandRefundGuard.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Makes the troop disband handler idempotent: a unit can only ever hand one
// peasant back, no matter how many disband commands land on it before the
// simulation removes it. Offsets and the reverse-engineering trail live in
// docs/bugs/disband-peasant-duplication.md.

// Soldier::disband — vtable slot 0x15c, shared by all 20 troop classes. Sets
// the actor's lifecycle state to 3 ("marked for removal") and then returns a
// peasant to the campfire. Siege engines and civilians have their own slot
// 0x15c overrides and neither refunds anything, so both are left alone.
static const uintptr_t DISBAND_BODY_RVA = 0xf6416; // the state test, past the prologue
static const uintptr_t DISBAND_EPILOGUE_RVA = 0xf6468; // mov esp,ebp / pop ebp / ret

// The state test as the stock game emits it: skip the state write when the
// actor is already fully removed (4), but fall through to the refund anyway.
static const uint8_t BODY_STOCK[13] = {
    0x83, 0x79, 0x20, 0x04, // cmp dword [ecx+0x20], 4
    0x74, 0x07, // je +7 (over the state write)
    0xc7, 0x41, 0x20, 0x03, 0x00, 0x00, 0x00 // mov dword [ecx+0x20], 3
};

static const uint8_t EPILOGUE_STOCK[4] = {0x8b, 0xe5, 0x5d, 0xc3};

// Rewritten in place as `cmp dword [ecx+0x20], 3` / `jae <epilogue>`: state 3
// and state 4 both mean the unit is already on its way out, so the refund is
// skipped entirely instead of running a second time.
static const uintptr_t GUARD_RVA = 0xf6419; // the cmp's imm8, then the jcc
static const uint8_t GUARD_IMM = 0x03;
static const uint8_t GUARD_JAE = 0x73;

void installDisbandRefundGuard() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint8_t *body = (uint8_t *)(base + DISBAND_BODY_RVA);
    uint8_t *epilogue = (uint8_t *)(base + DISBAND_EPILOGUE_RVA);

    // All-or-nothing: the rewritten branch hard-codes the distance to the
    // function's own epilogue, so both ends have to be the stock bytes.
    if (memcmp(body, BODY_STOCK, sizeof(BODY_STOCK)) != 0) {
        return;
    }

    if (memcmp(epilogue, EPILOGUE_STOCK, sizeof(EPILOGUE_STOCK)) != 0) {
        return;
    }

    uint8_t *site = (uint8_t *)(base + GUARD_RVA);
    intptr_t rel = (intptr_t)DISBAND_EPILOGUE_RVA - (intptr_t)(GUARD_RVA + 3);

    if (rel < -128 || rel > 127) {
        return;
    }

    uint8_t patched[3] = {GUARD_IMM, GUARD_JAE, (uint8_t)rel};

    DWORD oldProtect;

    if (!VirtualProtect(site, sizeof(patched), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }

    memcpy(site, patched, sizeof(patched));
    VirtualProtect(site, sizeof(patched), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patched));
}
