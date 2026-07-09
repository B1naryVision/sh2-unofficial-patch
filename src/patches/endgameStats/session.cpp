#include "session.h"
#include "../../core/frameTick.h"
#include "gameOffsets.h"
#include "unitTracker.h"
#include <cstring>
#include <windows.h>

// Poll cadence while a game is active. Everything here runs on the game
// thread, so a short interval is safe — the only cost is a 32-slot scan.
static const DWORD POLL_INTERVAL_MS = 10000;

struct SlotCache {
    RawPlayerStat stat;
    uintptr_t objPtr; // player object this state was captured from
    bool seen;
    int stableColor; // colour frozen at first sighting, never overwritten
                     // (the live colour field is rewritten to the conqueror's
                     // colour when a player is eliminated). 0 = not captured.
    wchar_t stableName[kMaxNameChars]; // name resolved while the colour was
                                       // still true; bound to the slot so two
                                       // slots can never collapse onto one name
};

static SlotCache s_slots[PLAYER_TABLE_SLOTS] = {};
static bool s_inGame = false;
static DWORD s_lastPoll = 0;
static EndgameCounters s_counters = {};

// Reads one name-array record (index = colour - 1). Each record is an MSVC
// std::wstring: a 16-byte union at +0x10, then _Mysize at +0x20 and _Myres
// (capacity) at +0x24. _Myres is the small-string-optimisation discriminator:
// capacity < 8 means the characters live INLINE in the union; only capacity
// >= 8 means the union holds a heap pointer. Dereferencing the union
// unconditionally (the old behaviour) turned a short name's first two UTF-16
// characters into an "address" — usually mapped, so the garbage read there
// rendered as CJK ideographs ("Chinese names"). Confirmed live: "[AoG]
// Halli" (11 chars, heap) resolved fine; "Halli" (5 chars, inline) broke.
//
// The heap path guards the pointer with a range check plus VirtualQuery, and
// never reads past the committed region. Both paths trust _Mysize only after
// validating it against _Myres, which also rejects most stale records whose
// memory was recycled (a coherent size/capacity pair rarely survives reuse).
static bool loadNameAtIndex(uintptr_t base, int idx, wchar_t *out) {
    out[0] = 0;

    if (idx < 0 || idx >= NAME_ARRAY_COUNT) {
        return false;
    }

    uintptr_t recordBase = base + NAME_ARRAY_RVA + (uintptr_t)idx * NAME_ARRAY_STRIDE;
    uintptr_t unionAddr = recordBase + NAME_ARRAY_UNION_OFF;
    uint32_t size = *(uint32_t *)(recordBase + NAME_ARRAY_SIZE_OFF);
    uint32_t capacity = *(uint32_t *)(recordBase + NAME_ARRAY_RES_OFF);

    if (size == 0 || size > capacity || capacity > 4096) {
        return false;
    }

    const wchar_t *src;
    int maxChars = kMaxNameChars - 1;

    if (capacity < NAME_SSO_CAPACITY) {
        // Short string: characters are inline in the union (exe .data —
        // always readable). At most 7 chars fit.
        src = (const wchar_t *)unionAddr;
    } else {
        uintptr_t ptr = *(uintptr_t *)unionAddr;

        if (ptr < 0x10000 || ptr >= 0x80000000) {
            return false;
        }

        MEMORY_BASIC_INFORMATION mbi;

        if (!VirtualQuery((LPCVOID)ptr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) {
            return false;
        }

        uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        int regionChars = (int)((regionEnd - ptr) / sizeof(wchar_t));

        if (regionChars < maxChars) {
            maxChars = regionChars;
        }

        src = (const wchar_t *)ptr;
    }

    if ((int)size < maxChars) {
        maxChars = (int)size;
    }

    int len = 0;

    while (len < maxChars && src[len] != 0) {
        out[len] = src[len];
        ++len;
    }

    out[len] = 0;
    return len > 0;
}

// Freezes a slot's identity (colour + resolved name) the first time it is
// seen with a valid colour. This must happen early — the live colour field is
// rewritten to the conqueror's colour when a player is eliminated, so the
// earliest reading is the reliable one.
static void freezeIdentity(int slot, uintptr_t base, int color) {
    SlotCache &cache = s_slots[slot];

    if (cache.stableColor != 0 || !isValidColor(color)) {
        return;
    }

    cache.stableColor = color;
    loadNameAtIndex(base, color - 1, cache.stableName);
    s_counters.identityFreezes++;
}

// If the object at this slot is not the one we captured state from, the slot
// was recycled (new game without an endgame screen, or engine reuse) — the
// old identity and unit counts belong to somebody else, so drop them.
static void handleSlotRealloc(int slot, uintptr_t playerPtr) {
    SlotCache &cache = s_slots[slot];

    if (cache.objPtr == 0 || cache.objPtr == playerPtr) {
        return;
    }

    memset(&cache, 0, sizeof(cache));
    resetUnitCountsForSlot(slot);
    s_counters.slotReallocs++;
}

static void snapshotSlot(int slot, uintptr_t playerPtr) {
    SlotCache &cache = s_slots[slot];
    cache.stat = readRawPlayerStat(slot, playerPtr);
    cache.objPtr = playerPtr;
    cache.seen = true;
    s_counters.snapshots++;
}

// Runs once per frame on the game thread via the frame-tick dispatcher.
// Float-free (see core/frameTick.h): snapshots store raw float bits only.
static void pollTick() {
    if (!s_inGame) {
        s_counters.pollsSkippedIdle++;
        return;
    }

    // DWORD subtraction is wraparound-safe across GetTickCount's 49.7-day cycle.
    DWORD now = GetTickCount();

    if (now - s_lastPoll < POLL_INTERVAL_MS) {
        return;
    }

    s_lastPoll = now;
    s_counters.pollTicks++;

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t *table = playerTable(base);

    for (int i = 0; i < PLAYER_TABLE_SLOTS; ++i) {
        uintptr_t playerPtr = table[i];

        if (!playerPtr) {
            continue;
        }

        handleSlotRealloc(i, playerPtr);

        int color = PlayerView{playerPtr}.color();

        if (!isValidColor(color)) {
            continue;
        }

        freezeIdentity(i, base, color);
        snapshotSlot(i, playerPtr);
    }
}

void sessionOnRecruit(int slot, uintptr_t playerBase) {
    s_counters.recruitEvents++;

    if (!s_inGame) {
        s_inGame = true;
        s_lastPoll = GetTickCount() - POLL_INTERVAL_MS; // poll on the next frame
        s_counters.sessionStarts++;
    }

    handleSlotRealloc(slot, playerBase);

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    freezeIdentity(slot, base, PlayerView{playerBase}.color());

    if (!s_slots[slot].seen) {
        snapshotSlot(slot, playerBase);
    }
}

void sessionReset() {
    memset(s_slots, 0, sizeof(s_slots));
    s_inGame = false;
    s_counters.sessionResets++;
}

bool sessionInGame() { return s_inGame; }

SlotView sessionSlot(int slot) {
    SlotView view = {};

    if (slot < 0 || slot >= PLAYER_TABLE_SLOTS) {
        view.stableName = L"";
        return view;
    }

    const SlotCache &cache = s_slots[slot];
    view.seen = cache.seen;
    view.objPtr = cache.objPtr;
    view.stableColor = cache.stableColor;
    view.stableName = cache.stableName;
    view.stat = &cache.stat;
    return view;
}

bool sessionLoadName(int color, wchar_t *out) {
    if (!isValidColor(color)) {
        out[0] = 0;
        return false;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    return loadNameAtIndex(base, color - 1, out);
}

const EndgameCounters &endgameCounters() { return s_counters; }

void installSession() { registerFrameTick(pollTick); }
