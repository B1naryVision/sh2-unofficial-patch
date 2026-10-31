#include "noMarket.h"
#include "autoMarket/autoMarket.h"
#include <cstdint>
#include <windows.h>

// See docs/features/no-market.md.
//
// Both blocks act on lockstep commands as they execute, so every client drops the
// same command on the same tick. Players and AI alike place buildings through
// PlaceBuildingChore and trade at the market through TradeChore (S2Governor
// builds both); the Carter's Post has its own CarterTradeChore and is untouched.
static const uintptr_t PLACE_EXECUTE_SLOT_RVA =
    0x5c5abc; // PlaceBuildingChore vtable 0x9c5ab4, slot 2
static const uintptr_t PLACE_EXECUTE_RVA = 0x1de770;
static const uintptr_t TRADE_EXECUTE_SLOT_RVA = 0x5c6198; // TradeChore vtable 0x9c6190, slot 2
static const uintptr_t TRADE_EXECUTE_RVA = 0x1e0fa0;

static const uintptr_t PLACE_TYPE_OFF = 0x10; // word: building type, as passed to placement

// The Market's building type: S2BuildingClass +0x3c, copied to each building's
// +0x1c. Read from a dump; the same id space gives SiegeCamp 0x46.
static const uint16_t MARKET_TYPE = 0x40;

typedef void(__attribute__((thiscall)) * ExecuteFn)(void *chore);

static ExecuteFn s_stockPlace = nullptr;
static ExecuteFn s_stockTrade = nullptr;
static bool s_enabled = false;

// Thiscall, no stack arguments, like the stock execute it replaces.
static void __attribute__((thiscall)) placeExecuteHook(void *chore) {
    uint16_t type = *(uint16_t *)((uintptr_t)chore + PLACE_TYPE_OFF);

    if (s_enabled && type == MARKET_TYPE) {
        return;
    }

    s_stockPlace(chore);
}

static void __attribute__((thiscall)) tradeExecuteHook(void *chore) {
    if (s_enabled) {
        return;
    }

    s_stockTrade(chore);
}

// The slot holds a relocated absolute address, so compare against the rebuilt
// one. Each execute's vtable slot is its only reference.
static bool swapSlot(uintptr_t slotRva, uintptr_t stockRva, void *hook, ExecuteFn *stockOut) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t *slot = (uintptr_t *)(base + slotRva);
    DWORD oldProtect = 0;

    if (*slot != base + stockRva) {
        return false;
    }

    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect)) {
        return false;
    }

    *stockOut = (ExecuteFn)(base + stockRva);
    *slot = (uintptr_t)hook;
    VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);

    return true;
}

bool installNoMarket() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    // Both or neither: verify the trade slot before touching the placement one.
    if (*(uintptr_t *)(base + TRADE_EXECUTE_SLOT_RVA) != base + TRADE_EXECUTE_RVA) {
        return false;
    }

    if (!swapSlot(
            PLACE_EXECUTE_SLOT_RVA, PLACE_EXECUTE_RVA, (void *)&placeExecuteHook, &s_stockPlace
        )) {
        return false;
    }

    if (!swapSlot(
            TRADE_EXECUTE_SLOT_RVA, TRADE_EXECUTE_RVA, (void *)&tradeExecuteHook, &s_stockTrade
        )) {
        return false;
    }

    return true;
}

void noMarketSetEnabled(bool enabled) {
    s_enabled = enabled;
    autoMarketSetSuppressed(enabled);
}
