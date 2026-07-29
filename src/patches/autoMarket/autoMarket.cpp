#include "autoMarket.h"
#include "../../core/frameTick.h"
#include "overlay.h"
#include <cstdint>
#include <windows.h>

// Auto-Market QoL: keeps each good's stock inside a [min, max] band by posting
// the Market's own TradeChore buy/sell commands through the game's networked
// command layer — byte-for-byte equivalent to a player clicking Buy/Sell, so it
// is safe for MP version mismatch. Thresholds are set at runtime via the in-game
// editor (src/patches/autoMarket/overlay.cpp), not the ini, and reset each game.
// All state lives in this DLL; the only game mutation is the command post itself.
//
// Reverse-engineering trail, offsets and MP analysis: docs/features/auto-market.md.

// ── Engine offsets (RVAs; VA = RVA + 0x400000) ────────────────────────────────
static const uintptr_t WORLD_RVA = 0xd781d0; // World/game singleton
static const uintptr_t GET_PLAYER_RVA = 0x11360; // World::getLocalPlayer() -> player or NULL
// TradeChore command: allocation, ctor, refcounted-handle discipline, dispatch.
static const uintptr_t ALLOC_FN_IAT_RVA = 0x500980; // IAT slot -> refcounting allocator
static const uintptr_t ALLOC_CTX_IAT_RVA = 0x50097c; // IAT slot -> allocator context/pool
static const uintptr_t TRADECHORE_CTOR_RVA = 0x1e1160;
static const uintptr_t MAKE_HANDLE_RVA = 0x29e2c0; // Handle::assign(event) -> refcount++
static const uintptr_t COPY_HANDLE_RVA = 0x210d60; // Handle copy -> refcount++ (queue ref)
static const uintptr_t RELEASE_HANDLE_RVA = 0x19e9f0; // Handle release -> refcount--
static const uintptr_t WORLD_DISPATCH_VTBL_OFF = 0x20; // World::vtbl[0x20](Event*): enqueue

// TradeChore event layout (0x1c bytes; refcount header precedes the pointer).
static const uintptr_t EVT_GOOD = 0x08; // dword: good id
static const uintptr_t EVT_AMOUNT = 0x0c; // dword: units to trade
static const uintptr_t EVT_PRICE = 0x10; // dword: unit price (sim recomputes; advisory)
static const uintptr_t EVT_BUY = 0x14; // byte: buy flag
static const uintptr_t EVT_SELL = 0x15; // byte: sell flag
static const uintptr_t EVT_PLAYER = 0x18; // dword: player id
static const size_t EVT_SIZE = 0x1c;

// Player object layout.
static const uintptr_t PLAYER_ID_OFF = 0x08; // dword: player id
static const uintptr_t GOODS_STOCK_BASE_OFF = 0xf5c; // stock array base; stock = [base + id*4]

// Market good-definition / price table (static .data). Entries are NOT in
// good-id order, so it is searched by entry[0] == good id. Each entry carries
// the good's current buy/sell unit price — the same values the trade window
// shows — so each posted trade settles gold exactly like a manual Buy/Sell.
// See auto-market.md.
static const uintptr_t PRICE_TABLE_RVA = 0x6bcfd8;
static const int PRICE_TABLE_COUNT = 30;
static const uintptr_t PRICE_ENTRY_STRIDE = 0x28;
static const uintptr_t PRICE_ENTRY_SELL = 0x20; // dword: sell unit price
static const uintptr_t PRICE_ENTRY_BUY = 0x24; // dword: buy unit price

// ── Function pointer types ────────────────────────────────────────────────────
typedef void *(__cdecl *AllocFn)(size_t size, void *ctx);
typedef void(__attribute__((thiscall)) * TradeChoreCtorFn)(void *event);
typedef void *(__attribute__((thiscall)) * MakeHandleFn)(void *self, void *event, int zero);
typedef void *(__attribute__((thiscall)) * CopyHandleFn)(void *self, void *srcHandle);
typedef void(__attribute__((thiscall)) * ReleaseHandleFn)(void *self);
typedef void(__attribute__((thiscall)) * DispatchFn)(void *world, void *event);
typedef void *(__attribute__((thiscall)) * GetPlayerFn)(void *world);

// ── Config / state ────────────────────────────────────────────────────────────
static const int MAX_GOOD_ID = 47; // highest good id addressed
// Frames between threshold evaluations. Fixed (not user-configurable): too small
// a value would spam the command queue with drops when a target is unaffordable;
// ~1s is responsive for gradual stock changes and cheap.
static const int TICK_INTERVAL = 60;
static const int MAX_BATCH = 1000; // clamp on units posted per command

struct Threshold {
    int min; // buy up to this when stock is below it; 0 = no auto-buy
    int max; // sell down to this when stock is above it; 0 = no auto-sell
};

static Threshold s_thresh[MAX_GOOD_ID + 1] = {};
static bool s_active[MAX_GOOD_ID + 1] = {};
static uintptr_t s_priceEntryRva[MAX_GOOD_ID + 1] = {}; // 0 = good not tradeable

// Good id → display name + category, shown as the editor's rows (grouped by
// category, in this order). Ids confirmed against a live session; a few slots
// are unnamed and simply not shown.
struct GoodName {
    const char *name;
    int id;
    const char *category;
};

static const GoodName GOOD_NAMES[] = {
    {"Wood", 1, "Resources"},
    {"Stone", 2, "Resources"},
    {"Iron", 3, "Resources"},
    {"Pitch", 9, "Resources"},

    {"Wheat", 4, "Food"},
    {"Flour", 5, "Food"},
    {"Bread", 23, "Food"},
    {"Cheese", 24, "Food"},
    {"Meat", 25, "Food"},
    {"Apples", 22, "Food"},

    {"Vegetables", 18, "Kitchen food"},
    {"Eels", 14, "Kitchen food"},
    {"Geese", 15, "Kitchen food"},
    {"Pigs", 17, "Kitchen food"},
    {"Grapes", 19, "Kitchen food"},

    {"Candles", 10, "Goods"},
    {"Wool", 11, "Goods"},
    {"Cloth", 12, "Goods"},
    {"Hops", 6, "Goods"},
    {"Ale", 7, "Goods"},

    {"Bows", 30, "Weapons"},
    {"Crossbows", 31, "Weapons"},
    {"Swords", 32, "Weapons"},
    {"Maces", 33, "Weapons"},
    {"Pikes", 34, "Weapons"},
    {"Spears", 35, "Weapons"},
    {"Armour", 36, "Weapons"},
    {"LeatherArmour", 37, "Weapons"},
};

// Reads a good's stock; float-free (raw int) as required on the frame-tick path.
static int readStock(void *player, int goodId) {
    uintptr_t addr = (uintptr_t)player + GOODS_STOCK_BASE_OFF + (uintptr_t)goodId * 4;
    return *(int *)addr;
}

// Builds one TradeChore and dispatches it exactly as SubPanelTrading::process
// does for a Buy/Sell click, substituting our good/amount for the panel reads.
// The refcount discipline is delegated to the game's own handle functions.
static void postTrade(uintptr_t base, void *world, void *player, int goodId, int amount, bool buy) {
    // Post the good's real market price. A missing entry or a non-positive
    // price means the good is not market-tradeable — skip it.
    if (goodId < 0 || goodId > MAX_GOOD_ID || s_priceEntryRva[goodId] == 0) {
        return;
    }

    uintptr_t entry = base + s_priceEntryRva[goodId];
    int price = buy ? *(int *)(entry + PRICE_ENTRY_BUY) : *(int *)(entry + PRICE_ENTRY_SELL);

    if (price <= 0) {
        return;
    }

    AllocFn alloc = *(AllocFn *)(base + ALLOC_FN_IAT_RVA);
    void *allocCtx = *(void **)(base + ALLOC_CTX_IAT_RVA);

    void *event = alloc(EVT_SIZE, allocCtx);

    if (!event) {
        return;
    }

    TradeChoreCtorFn ctor = (TradeChoreCtorFn)(base + TRADECHORE_CTOR_RVA);
    MakeHandleFn makeHandle = (MakeHandleFn)(base + MAKE_HANDLE_RVA);
    CopyHandleFn copyHandle = (CopyHandleFn)(base + COPY_HANDLE_RVA);
    ReleaseHandleFn releaseHandle = (ReleaseHandleFn)(base + RELEASE_HANDLE_RVA);

    ctor(event);

    // Local owning handle (refcount 0 -> 1).
    void *handle = nullptr;
    makeHandle(&handle, event, 0);

    uintptr_t e = (uintptr_t)event;
    *(uint32_t *)(e + EVT_GOOD) = (uint32_t)goodId;
    *(uint32_t *)(e + EVT_AMOUNT) = (uint32_t)amount;
    *(uint32_t *)(e + EVT_PRICE) = (uint32_t)price;
    *(uint8_t *)(e + EVT_BUY) = buy ? 1 : 0;
    *(uint8_t *)(e + EVT_SELL) = buy ? 0 : 1;
    *(uint32_t *)(e + EVT_PLAYER) = *(uint32_t *)((uintptr_t)player + PLAYER_ID_OFF);

    // Second handle copy (refcount 1 -> 2): the reference the command queue
    // retains, exactly as the stock handler leaves it — do NOT release it.
    void *queueRef = nullptr;
    copyHandle(&queueRef, &handle);

    DispatchFn dispatch =
        (DispatchFn)(*(uintptr_t *)((*(uintptr_t *)world) + WORLD_DISPATCH_VTBL_OFF));
    dispatch(world, event);

    // Release only the local handle (refcount 2 -> 1); the queue keeps its ref.
    releaseHandle(&handle);
}

// Runs on the sim thread via the shared frame-tick dispatcher. Float-free.
static void autoMarketTick() {
    static int counter = 0;

    if (++counter < TICK_INTERVAL) {
        return;
    }

    counter = 0;

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    void *world = (void *)(base + WORLD_RVA);
    GetPlayerFn getPlayer = (GetPlayerFn)(base + GET_PLAYER_RVA);
    void *player = getPlayer(world);

    if (!player) {
        return;
    }

    // At most one command per good per interval; the simulation re-validates
    // gold/stock and drops what cannot execute, so any overshoot simply retries
    // next interval (the shift-recruit graceful-degradation model).
    for (int gid = 0; gid <= MAX_GOOD_ID; ++gid) {
        if (!s_active[gid]) {
            continue;
        }

        int stock = readStock(player, gid);
        const Threshold &t = s_thresh[gid];

        if (t.min > 0 && stock < t.min) {
            int amount = t.min - stock;

            if (amount > MAX_BATCH) {
                amount = MAX_BATCH;
            }

            postTrade(base, world, player, gid, amount, true);
        } else if (t.max > 0 && stock > t.max) {
            int amount = stock - t.max;

            if (amount > MAX_BATCH) {
                amount = MAX_BATCH;
            }

            postTrade(base, world, player, gid, amount, false);
        }
    }
}

// Scans the good-definition table once and records each tradeable good's entry
// offset, so per-trade price reads are a direct index rather than a search.
static void buildPriceMap(uintptr_t base) {
    for (int n = 0; n < PRICE_TABLE_COUNT; ++n) {
        uintptr_t entryRva = PRICE_TABLE_RVA + (uintptr_t)n * PRICE_ENTRY_STRIDE;
        int id = *(int *)(base + entryRva);

        if (id >= 0 && id <= MAX_GOOD_ID) {
            s_priceEntryRva[id] = entryRva;
        }
    }
}

// ── In-game editor accessors ───────────────────────────────────────────────────
static int goodNameCount() { return (int)(sizeof(GOOD_NAMES) / sizeof(GOOD_NAMES[0])); }

int autoMarketGoodCount() { return goodNameCount(); }

const char *autoMarketGoodName(int row) {
    if (row < 0 || row >= goodNameCount()) {
        return "";
    }

    return GOOD_NAMES[row].name;
}

int autoMarketGoodId(int row) {
    if (row < 0 || row >= goodNameCount()) {
        return -1;
    }

    return GOOD_NAMES[row].id;
}

const char *autoMarketGoodCategory(int row) {
    if (row < 0 || row >= goodNameCount()) {
        return "";
    }

    return GOOD_NAMES[row].category;
}

static int priceAt(int goodId, uintptr_t fieldOff) {
    if (goodId < 0 || goodId > MAX_GOOD_ID || s_priceEntryRva[goodId] == 0) {
        return 0;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    return *(int *)(base + s_priceEntryRva[goodId] + fieldOff);
}

int autoMarketBuyPrice(int goodId) { return priceAt(goodId, PRICE_ENTRY_BUY); }
int autoMarketSellPrice(int goodId) { return priceAt(goodId, PRICE_ENTRY_SELL); }

int autoMarketGetMin(int goodId) {
    if (goodId < 0 || goodId > MAX_GOOD_ID) {
        return 0;
    }

    return s_thresh[goodId].min;
}

int autoMarketGetMax(int goodId) {
    if (goodId < 0 || goodId > MAX_GOOD_ID) {
        return 0;
    }

    return s_thresh[goodId].max;
}

static void refreshActive(int goodId) {
    s_active[goodId] = (s_thresh[goodId].min > 0 || s_thresh[goodId].max > 0);
}

void autoMarketSetMin(int goodId, int value) {
    if (goodId < 0 || goodId > MAX_GOOD_ID) {
        return;
    }

    s_thresh[goodId].min = value < 0 ? 0 : value;
    refreshActive(goodId);
}

void autoMarketSetMax(int goodId, int value) {
    if (goodId < 0 || goodId > MAX_GOOD_ID) {
        return;
    }

    s_thresh[goodId].max = value < 0 ? 0 : value;
    refreshActive(goodId);
}

// Clears every threshold and hides the editor. Called when the main menu
// activates (return to menu / quit a game), so thresholds never carry across
// games — trading needs change through a match, so each game starts clean.
void autoMarketResetThresholds() {
    for (int gid = 0; gid <= MAX_GOOD_ID; ++gid) {
        s_thresh[gid].min = 0;
        s_thresh[gid].max = 0;
        s_active[gid] = false;
    }

    autoMarketOverlayReset();
}

void installAutoMarket() {
    // Thresholds are set at runtime through the in-game editor (the ini no longer
    // configures per-good values — they reset every game). If the editor is
    // disabled (its hotkey set to None), the whole feature is off — zero
    // footprint, no hooks.
    if (!installAutoMarketOverlay()) {
        return;
    }

    buildPriceMap((uintptr_t)GetModuleHandleA(NULL));

    registerFrameTick(autoMarketTick);
}
