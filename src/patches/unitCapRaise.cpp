#include "unitCapRaise.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Fixes the per-player unit cap at 550 instead of 300 + 500 / players. A
// gameplay change, so it is switched on per match by patches/matchRules.cpp only
// when the host chooses it and every player has it. Offsets and the
// reverse-engineering trail live in docs/features/unit-cap-raise.md.

// The three cap checks each compute `500 / players` and then `add eax, 300`.
// Overwriting the add with `mov eax, 550` discards the quotient.
static const uintptr_t CAP_RVAS[3] = {0x16827, 0x18768, 0x189fb};
static const uint8_t CAP_STOCK[5] = {0x05, 0x2c, 0x01, 0x00, 0x00}; // add eax, 0x12c
static const uint8_t CAP_PATCHED[5] = {0xb8, 0x26, 0x02, 0x00, 0x00}; // mov eax, 0x226

// The barracks and mercenary post panels warn that the army is nearly full 20
// units below the cap, with their own copy of the formula (`add eax, 280`).
// Advisor speech only, so it moves with the cap without needing a rule bit.
static const uintptr_t WARN_RVAS[2] = {0x2207fd, 0x2255f2};
static const uint8_t WARN_STOCK[5] = {0x05, 0x18, 0x01, 0x00, 0x00}; // add eax, 0x118
static const uint8_t WARN_PATCHED[5] = {0xb8, 0x12, 0x02, 0x00, 0x00}; // mov eax, 0x212

static bool s_ready = false;
static bool s_warnReady = false;
static bool s_enabled = false;

bool installUnitCapRaise() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    // All-or-nothing: a cap raised in one check and not the others would let a
    // unit through one recruitment path and refuse it on another.
    for (uintptr_t rva : CAP_RVAS) {
        if (memcmp((const void *)(base + rva), CAP_STOCK, sizeof(CAP_STOCK)) != 0) {
            return false;
        }
    }

    s_ready = true;
    s_warnReady = true;

    for (uintptr_t rva : WARN_RVAS) {
        if (memcmp((const void *)(base + rva), WARN_STOCK, sizeof(WARN_STOCK)) != 0) {
            s_warnReady = false;
        }
    }

    return true;
}

static void writeSites(uintptr_t base, const uintptr_t *rvas, size_t count, const uint8_t *bytes) {
    for (size_t i = 0; i < count; ++i) {
        void *site = (void *)(base + rvas[i]);
        DWORD oldProtect = 0;

        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            continue;
        }

        memcpy(site, bytes, 5);
        VirtualProtect(site, 5, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
    }
}

// Called on the simulation thread only, so no cap check is half-way through the
// five bytes being rewritten.
void unitCapRaiseSetEnabled(bool enabled) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const uint8_t *capBytes = CAP_STOCK;
    const uint8_t *warnBytes = WARN_STOCK;

    if (!s_ready || enabled == s_enabled) {
        return;
    }

    if (enabled) {
        capBytes = CAP_PATCHED;
        warnBytes = WARN_PATCHED;
    }

    writeSites(base, CAP_RVAS, 3, capBytes);

    if (s_warnReady) {
        writeSites(base, WARN_RVAS, 2, warnBytes);
    }

    s_enabled = enabled;
}
