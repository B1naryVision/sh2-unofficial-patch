#include "endgameStats.h"
#include "../core/hook.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>

// Per-player, per-unit-type recruit count [table_slot][unit_type_id].
// Incremented by the spawn hook at RVA 0x0EE3BE.
// 32 slots observed in x32dbg dump; only 8 active players max but they can be at any slot.
static uint32_t s_unitsMade[32][256] = {};

// Unit type ID → display name.  NULL = unknown / not a recruitable unit.
// Populated in installEndgameStats().  Used as a filter in the spawn hook:
// if the type isn't in this table, the write was not a barracks purchase.
static const char *s_unitNames[256] = {};

// Hook sites:
//   Win/LoseScreen::OnActivate — vtable-confirmed, first 5 bytes: 55 8b ec 6a ff
//   Unit spawn at RVA 0x0EC3BE — `cmp byte ptr [ecx+0x179], 0` (7 bytes).
//     Watchpoint on player+0xD8C (army count) + call-stack analysis confirmed:
//     EDI = unit type ID, ESI = player_base+0x674 at this site.
static const uintptr_t WIN_ONACTIVATE_RVA = 0x297fa0;
static const uintptr_t LOSE_ONACTIVATE_RVA = 0x297700;
// Scalar destructors (slot 1) — fire when the player exits the screen via any button.
// Both start with 55 8b ec 6a ff, same prologue as OnActivate.
static const uintptr_t WIN_DTOR_RVA = 0x297f10;
static const uintptr_t LOSE_DTOR_RVA = 0x297670;
static const uintptr_t SPAWN_HOOK_RVA = 0x0EE3BE;

// Player table RVA.  module_base + 0x6E8Bd8 = VA 0xAE8Bd8 at ImageBase 0x400000.
static const uintptr_t PLAYER_TABLE_RVA = 0x6E8Bd8;

// ── Player object offsets (all confirmed in x32dbg) ───────────────────────────
static const uintptr_t OFF_COLOR = 0x4;
static const uintptr_t OFF_HONOR = 0x1C;
static const uintptr_t OFF_TROOPS = 0xD8C;
static const uintptr_t OFF_SIEGE = 0xD90;
static const uintptr_t OFF_TITLE = 0xF58;
static const uintptr_t OFF_CASTLE = 0x10F8; // must be 2 for active slot
static const uintptr_t OFF_GOLD = 0x1010; // float
static const uintptr_t OFF_POPULARITY = 0x1028; // float

static const uintptr_t OFF_INC_TRADE = 0x1554; // confirmed
static const uintptr_t OFF_INC_TAX_CASTLE = 0x1558; // confirmed
static const uintptr_t OFF_INC_TAX_ESTATES = 0x1628; // consistent with 0 (no estates)

static const uintptr_t OFF_HON_BASE = 0x1714; // 8×int: Feasting,Dancing,Monastery,
                                              // Jousting,Church,Granary,Statues,Crime

// Offset from army sub-object pointer (ESI) back to player base.
// ESI = player_base + 0x674  →  player_base = ESI - 0x674
static const uintptr_t ARMY_SUBOBJ_OFF = 0x674;

// ── Names ─────────────────────────────────────────────────────────────────────

static const char *TITLE_NAMES[] = {
    "Freeman",       "Yeoman",         "Squire", "Knight", "Knight Bachelor",
    "Knight Errant", "Royal Champion", "Baron",  "Earl",   "Duke"
};
static const char *COLOR_NAMES[] = {"?",    "Red",    "Orange", "Green",    "Cyan", "Blue",
                                    "Pink", "Yellow", "Violet", "Dark Red", "Gray"};
static const COLORREF COLOR_VALS[] = {RGB(200, 200, 200), RGB(220, 50, 50),  RGB(255, 165, 0),
                                      RGB(50, 200, 50),   RGB(0, 200, 200),  RGB(80, 80, 255),
                                      RGB(255, 150, 200), RGB(220, 220, 0),  RGB(160, 0, 200),
                                      RGB(139, 0, 0),     RGB(150, 150, 150)};

static const char *INC_LABELS[] = {"Trade Income", "Tax: Castle", "Tax: Estates"};
static const char *HON_LABELS[] = {"Feasting", "Dancing", "Monastery", "Jousting",
                                   "Church",   "Granary", "Statues",   "Crime"};

// ── Snapshot ──────────────────────────────────────────────────────────────────

struct PlayerStat {
    int slot;
    int colorIdx;
    int gold;
    int honor;
    int troops;
    int siege;
    int titleIdx;
    float popularity;
    int income[3];
    int honSrc[8];
    uint32_t unitsByType[256]; // copy of s_unitsMade[slot]
};

static PlayerStat s_stats[8];
static int s_statCount = 0;
static bool s_gameWon = false;

// ── Periodic slot cache ───────────────────────────────────────────────────────
// Polled every 60 s so that eliminated/disconnected players have recent data
// available at endgame even after their table entries become stale.

struct SlotCache {
    PlayerStat stat;
    bool valid; // use this entry at endgame
    bool seen; // polled at least once
    bool everChanged; // fingerprint moved after the first poll
    uint8_t unchanged; // consecutive polls with identical fingerprint
    uint64_t lastFp; // fingerprint from previous poll
};

static SlotCache s_slotCache[32] = {};
static HANDLE s_pollTimer = NULL;

static const wchar_t OVERLAY_CLASS[] = L"SH2PatchStatsOverlay";
static HWND s_overlayHwnd = NULL;

// ── Periodic poll helpers ─────────────────────────────────────────────────────

static uint64_t makeFingerprint(uintptr_t playerPtr) {
    uint64_t gold = (uint32_t)(int)*(float *)(playerPtr + OFF_GOLD);
    uint64_t honor = (uint32_t)*(int *)(playerPtr + OFF_HONOR);
    uint64_t troops = (uint32_t)*(int *)(playerPtr + OFF_TROOPS);
    uint64_t siege = (uint32_t)*(int *)(playerPtr + OFF_SIEGE);
    return gold | (honor << 32) | (troops * 0x10001ULL) | (siege * 0x100010001ULL);
}

static void snapshotToCache(int i, uintptr_t playerPtr) {
    PlayerStat &stat = s_slotCache[i].stat;
    stat.slot = i;
    stat.colorIdx = *(int *)(playerPtr + OFF_COLOR);
    stat.gold = (int)*(float *)(playerPtr + OFF_GOLD);
    stat.honor = *(int *)(playerPtr + OFF_HONOR);
    stat.troops = *(int *)(playerPtr + OFF_TROOPS);
    stat.siege = *(int *)(playerPtr + OFF_SIEGE);
    stat.titleIdx = *(int *)(playerPtr + OFF_TITLE);
    stat.popularity = *(float *)(playerPtr + OFF_POPULARITY);
    stat.income[0] = *(int *)(playerPtr + OFF_INC_TRADE);
    stat.income[1] = *(int *)(playerPtr + OFF_INC_TAX_CASTLE);
    stat.income[2] = *(int *)(playerPtr + OFF_INC_TAX_ESTATES);

    for (int src = 0; src < 8; ++src) {
        stat.honSrc[src] = *(int *)(playerPtr + OFF_HON_BASE + (uintptr_t)src * 4);
    }

    memset(stat.unitsByType, 0, sizeof(stat.unitsByType)); // filled from s_unitsMade at endgame
}

static VOID CALLBACK pollTimerCallback(PVOID, BOOLEAN) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t *table = (uintptr_t *)(base + PLAYER_TABLE_RVA);

    for (int i = 0; i < 32; ++i) {
        uintptr_t playerPtr = table[i];

        if (!playerPtr) {
            continue;
        }

        int color = *(int *)(playerPtr + OFF_COLOR);

        if (color < 1 || color > 10) {
            continue;
        }

        uint64_t fingerprint = makeFingerprint(playerPtr);
        SlotCache &cache = s_slotCache[i];

        if (!cache.seen) {
            cache.seen = true;
            cache.lastFp = fingerprint;
            cache.unchanged = 1;
            cache.valid = true; // tentative until proven ghost
            snapshotToCache(i, playerPtr);
        } else if (fingerprint != cache.lastFp) {
            cache.lastFp = fingerprint;
            cache.everChanged = true;
            cache.unchanged = 0;
            snapshotToCache(i, playerPtr);
        } else {
            if (cache.unchanged < 255) {
                cache.unchanged++;
            }

            // Three consecutive polls with no change and no prior change seen →
            // slot is a leftover ghost entry, not a real player this session.
            if (!cache.everChanged && cache.unchanged >= 3) {
                cache.valid = false;
            }
        }
    }
}

// ── Spawn hook ────────────────────────────────────────────────────────────────
// Return-address globals: must NOT be static (GAS cannot resolve local statics).

uintptr_t g_winOnActivateReturn = 0;
uintptr_t g_loseOnActivateReturn = 0;
uintptr_t g_winDtorReturn = 0;
uintptr_t g_loseDtorReturn = 0;
uintptr_t g_spawnUnitType = 0; // EDI at hook site
uintptr_t g_spawnPlayerArmy = 0; // ESI at hook site (= player_base + 0x674)
uintptr_t g_spawnReturn = 0;

extern "C" void closeOverlayCpp() {
    if (s_overlayHwnd) {
        DestroyWindow(s_overlayHwnd);
        s_overlayHwnd = NULL;
    }
}

extern "C" void unitSpawnLogic() {
    uint32_t unitType = (uint32_t)g_spawnUnitType;

    if (unitType >= 256 || !s_unitNames[unitType]) {
        return;
    }

    uintptr_t playerBase = g_spawnPlayerArmy - ARMY_SUBOBJ_OFF;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t *table = (uintptr_t *)(base + PLAYER_TABLE_RVA);

    for (int i = 0; i < 32; ++i) {
        if (table[i] == playerBase) {
            s_unitsMade[i][unitType]++;
            // Unit recruitment is definitive proof of a real player.
            // Reinstate if the periodic poll tentatively marked this slot a ghost,
            // and snapshot now so pass 4 has data even if the timer hasn't fired yet.
            SlotCache &cache = s_slotCache[i];
            cache.valid = true;
            cache.everChanged = true;

            if (!cache.seen) {
                cache.seen = true;
                cache.lastFp = makeFingerprint(playerBase);
                snapshotToCache(i, playerBase);
            }
            break;
        }
    }
}

// Hook at SPAWN_HOOK_RVA 0x0EE3BE: `cmp byte ptr [ecx+0x179], 0`  (7 bytes: 80 B9 79 01 00 00 00)
// This CMP is NOT conditional — it executes for both regular units and siege equipment.
// Regular army count (0x0EE3B8) is conditional; siege count (0x0EE3DB) is conditional;
// but this CMP fires for all paths, so one hook covers everything.
// installHook writes a 5-byte JMP + 2 NOPs. Trampoline re-executes original then continues.
__declspec(naked) static void unitSpawnHook() {
    __asm__ volatile("pushal\n\t"
                     "movl %edi, _g_spawnUnitType\n\t"
                     "movl %esi, _g_spawnPlayerArmy\n\t"
                     "call _unitSpawnLogic\n\t"
                     "popal\n\t"
                     "cmpb $0, 0x179(%ecx)\n\t"
                     "jmp *_g_spawnReturn\n\t");
}

__declspec(naked) static void winDtorHook() {
    __asm__ volatile("pushal\n\t"
                     "call _closeOverlayCpp\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $0xffffffff\n\t"
                     "jmp *_g_winDtorReturn\n\t");
}

__declspec(naked) static void loseDtorHook() {
    __asm__ volatile("pushal\n\t"
                     "call _closeOverlayCpp\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $0xffffffff\n\t"
                     "jmp *_g_loseDtorReturn\n\t");
}

// ── Overlay window ────────────────────────────────────────────────────────────

static void drawRow(
    HDC hdc, int x, int rowY, const wchar_t *label, const int *vals, int playerCount,
    COLORREF labelClr
) {
    SetTextColor(hdc, labelClr);
    TextOutW(hdc, x, rowY, label, (int)wcslen(label));

    for (int playerIdx = 0; playerIdx < playerCount; ++playerIdx) {
        int colorIdx = 0;

        if (s_stats[playerIdx].colorIdx >= 1 && s_stats[playerIdx].colorIdx <= 10) {
            colorIdx = s_stats[playerIdx].colorIdx;
        }

        SetTextColor(hdc, COLOR_VALS[colorIdx]);
        wchar_t numBuf[16];
        _snwprintf_s(numBuf, 16, _TRUNCATE, L"%d", vals[playerIdx]);
        TextOutW(hdc, x + 200 + playerIdx * 90, rowY, numBuf, (int)wcslen(numBuf));
    }
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bgBrush = CreateSolidBrush(RGB(45, 45, 55));
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);
        // Thin border
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(100, 100, 120));
        HPEN prevPen = (HPEN)SelectObject(hdc, pen);
        HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH prevBrush = (HBRUSH)SelectObject(hdc, nullBrush);
        Rectangle(hdc, 0, 0, rc.right, rc.bottom);
        SelectObject(hdc, prevPen);
        SelectObject(hdc, prevBrush);
        DeleteObject(pen);
        SetBkMode(hdc, TRANSPARENT);

        int rowY = 8, labelX = 10, playerCount = s_statCount;

        // Title
        SetTextColor(hdc, RGB(255, 255, 255));
        wchar_t headerBuf[128];
        _snwprintf_s(
            headerBuf, 128, _TRUNCATE, L"=== End of Game Statistics — %s ===",
            s_gameWon ? L"VICTORY" : L"DEFEAT"
        );
        TextOutW(hdc, labelX, rowY, headerBuf, (int)wcslen(headerBuf));
        rowY += 22;

        // Player colour header
        for (int playerIdx = 0; playerIdx < playerCount; ++playerIdx) {
            int colorIdx = 0;

            if (s_stats[playerIdx].colorIdx >= 1 && s_stats[playerIdx].colorIdx <= 10) {
                colorIdx = s_stats[playerIdx].colorIdx;
            }

            SetTextColor(hdc, COLOR_VALS[colorIdx]);
            wchar_t colorNameBuf[16];
            _snwprintf_s(colorNameBuf, 16, _TRUNCATE, L"%-9S", COLOR_NAMES[colorIdx]);
            TextOutW(hdc, labelX + 200 + playerIdx * 90, rowY, colorNameBuf, (int)wcslen(colorNameBuf));
        }

        rowY += 20;

        // Title row
        SetTextColor(hdc, RGB(210, 210, 210));
        TextOutW(hdc, labelX, rowY, L"Title", 5);

        for (int playerIdx = 0; playerIdx < playerCount; ++playerIdx) {
            int colorIdx = 0;

            if (s_stats[playerIdx].colorIdx >= 1 && s_stats[playerIdx].colorIdx <= 10) {
                colorIdx = s_stats[playerIdx].colorIdx;
            }

            SetTextColor(hdc, COLOR_VALS[colorIdx]);
            int titleIdx = s_stats[playerIdx].titleIdx;
            const char *titleName = (titleIdx >= 0 && titleIdx <= 9) ? TITLE_NAMES[titleIdx] : "?";
            wchar_t titleNameBuf[20];
            _snwprintf_s(titleNameBuf, 20, _TRUNCATE, L"%-10S", titleName);
            TextOutW(hdc, labelX + 200 + playerIdx * 90, rowY, titleNameBuf, (int)wcslen(titleNameBuf));
        }
        rowY += 20;

        auto makeRow = [&](const wchar_t *label, auto getter) {
            int vals[8];

            for (int playerIdx = 0; playerIdx < playerCount; ++playerIdx) {
                vals[playerIdx] = getter(s_stats[playerIdx]);
            }

            drawRow(hdc, labelX, rowY, label, vals, playerCount, RGB(220, 220, 220));
            rowY += 18;
        };
        makeRow(L"Gold (treasury)", [](const PlayerStat &stat) { return stat.gold; });
        makeRow(L"Honour (total)", [](const PlayerStat &stat) { return stat.honor; });
        makeRow(L"Army (troops)", [](const PlayerStat &stat) { return stat.troops; });
        makeRow(L"Army (siege)", [](const PlayerStat &stat) { return stat.siege; });
        rowY += 6;

        // Income
        SetTextColor(hdc, RGB(255, 215, 100));
        TextOutW(hdc, labelX, rowY, L"— Gold by source —", 18);
        rowY += 18;

        for (int src = 0; src < 3; ++src) {
            int vals[8];

            for (int playerIdx = 0; playerIdx < playerCount; ++playerIdx) {
                vals[playerIdx] = s_stats[playerIdx].income[src];
            }

            wchar_t labelBuf[32];
            _snwprintf_s(labelBuf, 32, _TRUNCATE, L"  %S", INC_LABELS[src]);
            drawRow(hdc, labelX, rowY, labelBuf, vals, playerCount, RGB(230, 230, 200));
            rowY += 18;
        }
        rowY += 6;

        // Honour
        SetTextColor(hdc, RGB(140, 210, 255));
        TextOutW(hdc, labelX, rowY, L"— Honour by source —", 20);
        rowY += 18;

        for (int src = 0; src < 8; ++src) {
            int vals[8];

            for (int playerIdx = 0; playerIdx < playerCount; ++playerIdx) {
                vals[playerIdx] = s_stats[playerIdx].honSrc[src];
            }

            wchar_t labelBuf[32];
            _snwprintf_s(labelBuf, 32, _TRUNCATE, L"  %S", HON_LABELS[src]);
            drawRow(hdc, labelX, rowY, labelBuf, vals, playerCount, RGB(200, 230, 245));
            rowY += 18;
        }
        rowY += 6;

        // Units recruited — only rows where at least one player has count > 0
        SetTextColor(hdc, RGB(160, 240, 140));
        TextOutW(hdc, labelX, rowY, L"— Units recruited —", 19);
        rowY += 18;
        bool anyUnits = false;

        for (int unitType = 0; unitType < 256; ++unitType) {
            if (!s_unitNames[unitType]) {
                continue;
            }

            bool hasAny = false;

            for (int playerIdx = 0; playerIdx < playerCount; ++playerIdx) {
                if (s_stats[playerIdx].unitsByType[unitType]) {
                    hasAny = true;
                    break;
                }
            }

            if (!hasAny) {
                continue;
            }

            anyUnits = true;
            int vals[8];

            for (int playerIdx = 0; playerIdx < playerCount; ++playerIdx) {
                vals[playerIdx] = (int)s_stats[playerIdx].unitsByType[unitType];
            }

            wchar_t labelBuf[40];
            _snwprintf_s(labelBuf, 40, _TRUNCATE, L"  %S", s_unitNames[unitType]);
            drawRow(hdc, labelX, rowY, labelBuf, vals, playerCount, RGB(210, 240, 200));
            rowY += 18;
        }

        if (!anyUnits) {
            SetTextColor(hdc, RGB(160, 160, 160));
            TextOutW(hdc, labelX + 200, rowY, L"(none recorded)", 15);
            rowY += 18;
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER: {
        HWND fgWnd = GetForegroundWindow();

        if (fgWnd) {
            DWORD fgPid = 0;
            GetWindowThreadProcessId(fgWnd, &fgPid);

            if (fgPid != GetCurrentProcessId()) {
                DestroyWindow(hwnd);
            }
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        s_overlayHwnd = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ── Data collection ───────────────────────────────────────────────────────────

static void fillStat(int slot, uintptr_t playerPtr) {
    PlayerStat &stat = s_stats[s_statCount++];
    stat.slot = slot;
    stat.colorIdx = *(int *)(playerPtr + OFF_COLOR);
    stat.gold = (int)*(float *)(playerPtr + OFF_GOLD);
    stat.honor = *(int *)(playerPtr + OFF_HONOR);
    stat.troops = *(int *)(playerPtr + OFF_TROOPS);
    stat.siege = *(int *)(playerPtr + OFF_SIEGE);
    stat.titleIdx = *(int *)(playerPtr + OFF_TITLE);
    stat.popularity = *(float *)(playerPtr + OFF_POPULARITY);
    stat.income[0] = *(int *)(playerPtr + OFF_INC_TRADE);
    stat.income[1] = *(int *)(playerPtr + OFF_INC_TAX_CASTLE);
    stat.income[2] = *(int *)(playerPtr + OFF_INC_TAX_ESTATES);

    for (int src = 0; src < 8; ++src) {
        stat.honSrc[src] = *(int *)(playerPtr + OFF_HON_BASE + (uintptr_t)src * 4);
    }

    memcpy(stat.unitsByType, s_unitsMade[slot], sizeof(stat.unitsByType));
}

static void collectStats(uintptr_t base, bool won) {
    s_statCount = 0;
    s_gameWon = won;

    uintptr_t *table = (uintptr_t *)(base + PLAYER_TABLE_RVA);
    bool colorSeen[11] = {};
    bool slotAdded[32] = {};

    // The player table has 31 observed non-null entries (slots 0–30) followed by
    // null.  Slots 32+ contain unrelated .data section values that look non-null
    // and crash if dereferenced as player pointers.
    static const int TABLE_SEARCH = 32;

    // ── Pass 1: local player via the known current-player global ──────────────
    uintptr_t localPtr = *(uintptr_t *)(base + 0x6E8C60);

    if (localPtr) {
        int color = *(int *)(localPtr + OFF_COLOR);

        if (color >= 1 && color <= 10) {
            // Find this pointer's table slot for unit tracking (best effort).
            int localSlot = -1;

            for (int i = TABLE_SEARCH - 1; i >= 0; --i) {
                if (table[i] == localPtr) {
                    localSlot = i;
                    break;
                }
            }

            if (localSlot >= 0) {
                slotAdded[localSlot] = true;
                fillStat(localSlot, localPtr);
            } else {
                // Not in the first TABLE_SEARCH slots — add without unit data.
                // Use slot 0 as a sentinel; s_unitsMade[0] will be all-zero.
                fillStat(0, localPtr);
            }

            colorSeen[color] = true;
        }
    }

    // ── Pass 2: remote players — castle state 1 ──────────────────────────────
    // Confirmed via x32dbg: castle 2 = local player (always found by pass 1),
    // castle 1 = remote player (human opponent or AI), castle 0 = unused slot.
    // Stale entries from old games where the user played Orange as local player
    // have castle 2 — they are correctly excluded here.
    for (int i = TABLE_SEARCH - 1; i >= 0 && s_statCount < 8; --i) {
        if (i < 32 && slotAdded[i]) {
            continue;
        }

        uintptr_t playerPtr = table[i];

        if (!playerPtr) {
            continue;
        }

        int castle = *(int *)(playerPtr + OFF_CASTLE);

        if (castle != 1) {
            continue;
        }

        int color = *(int *)(playerPtr + OFF_COLOR);

        if (color < 1 || color > 10 || colorSeen[color]) {
            continue;
        }

        // Skip template/ghost entries that have never had any data written to them.
        // A real player always has non-zero gold or non-zero honour by game end.
        int gold = (int)*(float *)(playerPtr + OFF_GOLD);
        int honor = *(int *)(playerPtr + OFF_HONOR);

        if (gold == 0 && honor == 0) {
            continue;
        }

        colorSeen[color] = true;

        if (i < 32) {
            slotAdded[i] = true;
        }

        fillStat(i, playerPtr);
    }

    // ── Pass 3: unit-spawn tracking fallback ──────────────────────────────────
    // Any slot where our hook logged recruitment is definitively an active player.
    for (int i = TABLE_SEARCH - 1; i >= 0 && s_statCount < 8; --i) {
        if (i < 32 && slotAdded[i]) {
            continue;
        }

        uintptr_t playerPtr = table[i];

        if (!playerPtr) {
            continue;
        }

        int color = *(int *)(playerPtr + OFF_COLOR);

        if (color < 1 || color > 10 || colorSeen[color]) {
            continue;
        }

        bool hasUnits = false;

        for (int unitType = 0; unitType < 256 && !hasUnits; ++unitType) {
            if (i < 32 && s_unitsMade[i][unitType]) {
                hasUnits = true;
            }
        }

        if (!hasUnits) {
            continue;
        }

        // Skip if the live object looks zeroed (crash/cleanup cleared the fields).
        // Pass 4 will use the cache snapshot instead.
        int gold = (int)*(float *)(playerPtr + OFF_GOLD);
        int honor = *(int *)(playerPtr + OFF_HONOR);

        if (gold == 0 && honor == 0) {
            continue;
        }

        colorSeen[color] = true;

        if (i < 32) {
            slotAdded[i] = true;
        }

        fillStat(i, playerPtr);
    }

    // ── Pass 4: periodic-poll cache for eliminated/disconnected players ────────
    // Players who left or were defeated may have had their table entries cleaned
    // up before the endgame screen fired; use the last snapshot we polled.
    for (int i = 0; i < 32 && s_statCount < 8; ++i) {
        if (slotAdded[i] || !s_slotCache[i].valid) {
            continue;
        }
        int color = s_slotCache[i].stat.colorIdx;

        if (color < 1 || color > 10 || colorSeen[color]) {
            continue;
        }
        colorSeen[color] = true;
        slotAdded[i] = true;
        PlayerStat &stat = s_stats[s_statCount++];
        stat = s_slotCache[i].stat;
        memcpy(stat.unitsByType, s_unitsMade[i], sizeof(stat.unitsByType));
    }
}

// ── Show overlay ──────────────────────────────────────────────────────────────

static void showOverlay(bool won) {
    // Destroy any leftover overlay from a previous game before creating a fresh one.
    if (s_overlayHwnd) {
        DestroyWindow(s_overlayHwnd);
        s_overlayHwnd = NULL;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    collectStats(base, won);

    // Reset tracking state for the next game now that we've snapshotted into s_stats.
    memset(s_unitsMade, 0, sizeof(s_unitsMade));
    memset(s_slotCache, 0, sizeof(s_slotCache));

    HINSTANCE hInst = (HINSTANCE)GetModuleHandleA("version.dll");
    WNDCLASSW wndClass = {};
    wndClass.lpfnWndProc = OverlayWndProc;
    wndClass.hInstance = hInst;
    wndClass.lpszClassName = OVERLAY_CLASS;
    RegisterClassW(&wndClass);

    HWND gameWnd = FindWindowA(NULL, "Stronghold 2");
    RECT gameRect = {0, 0, 1024, 768};

    if (gameWnd) {
        GetWindowRect(gameWnd, &gameRect);
    }

    // Count how many unit rows will be non-zero
    int unitRows = 0;

    for (int unitType = 0; unitType < 256; ++unitType) {
        if (!s_unitNames[unitType]) {
            continue;
        }

        for (int playerIdx = 0; playerIdx < s_statCount; ++playerIdx) {
            if (s_stats[playerIdx].unitsByType[unitType]) {
                ++unitRows;
                break;
            }
        }
    }

    if (!unitRows) {
        unitRows = 1; // "none recorded" row
    }

    int winW = 200 + s_statCount * 90 + 20;

    if (winW < 500) {
        winW = 500;
    }
    // rows: 2(hdr) + 2(summary) + 4(army) + 1(gap) + 4(income) + 1(gap) + 9(honour) + 1(gap) +
    // 1+unitRows(units) + 1(footer)
    int winH = (2 + 2 + 4 + 1 + 4 + 1 + 9 + 1 + 1 + unitRows + 1) * 18 + 30;

    int winX = gameRect.left + (gameRect.right - gameRect.left - winW) / 2;
    int winY = gameRect.top + (gameRect.bottom - gameRect.top - winH) / 2;

    s_overlayHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT, OVERLAY_CLASS, L"Game Statistics",
        WS_POPUP | WS_VISIBLE, winX, winY, winW, winH, NULL, NULL, hInst, NULL
    );

    if (!s_overlayHwnd) {
        return;
    }

    SetLayeredWindowAttributes(s_overlayHwnd, 0, 230, LWA_ALPHA);
    ShowWindow(s_overlayHwnd, SW_SHOW);
    UpdateWindow(s_overlayHwnd);
    SetTimer(s_overlayHwnd, 1, 250, NULL);
}

extern "C" void showWinStats() { showOverlay(true); }
extern "C" void showLoseStats() { showOverlay(false); }

// ── Win/Lose screen trampolines ───────────────────────────────────────────────

__declspec(naked) static void winScreenHook() {
    __asm__ volatile("pushal\n\t"
                     "call _showWinStats\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $0xffffffff\n\t"
                     "jmp *_g_winOnActivateReturn\n\t");
}

__declspec(naked) static void loseScreenHook() {
    __asm__ volatile("pushal\n\t"
                     "call _showLoseStats\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $0xffffffff\n\t"
                     "jmp *_g_loseOnActivateReturn\n\t");
}

// ── Install ───────────────────────────────────────────────────────────────────

void installEndgameStats() {
    // Unit type name table (IDs confirmed in x32dbg)
    s_unitNames[0x13] = "Archer";
    s_unitNames[0x1B] = "Swordsman";
    s_unitNames[0x1C] = "Spearman";
    s_unitNames[0x1D] = "Ladderman";
    s_unitNames[0x30] = "Engineer";
    s_unitNames[0x31] = "Peasant";
    s_unitNames[0x32] = "Maceman";
    s_unitNames[0x33] = "Pikeman";
    s_unitNames[0x34] = "Crossbowman";
    s_unitNames[0x47] = "Knight";
    s_unitNames[0x48] = "Assassin";
    s_unitNames[0x49] = "Outlaw";
    s_unitNames[0x4A] = "Horse Archer";
    s_unitNames[0x4B] = "Berserker";
    s_unitNames[0x4C] = "Pictish Boat Warrior";
    s_unitNames[0x4D] = "Light Cavalry";
    s_unitNames[0x4E] = "Axe Thrower";
    s_unitNames[0x4F] = "Thief";
    s_unitNames[0x5D] = "Monk";
    s_unitNames[0x5E] = "Warrior Monk";
    // Siege equipment — same hook covers these (CMP at 0x0EE3BE executes for all unit types)
    s_unitNames[0x42] = "Trebuchet";
    s_unitNames[0x43] = "Fire Ballista";
    s_unitNames[0x44] = "Catapult";
    s_unitNames[0x58] = "Siege Tower (small)";
    s_unitNames[0x59] = "Siege Tower (large)";
    s_unitNames[0x5A] = "Battering Ram";
    s_unitNames[0x5B] = "Cat";
    s_unitNames[0x66] = "Mantlet";
    s_unitNames[0x67] = "Burning Cart";

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    // Win/Lose screen OnActivate hooks (show overlay)
    uintptr_t winSite = base + WIN_ONACTIVATE_RVA;
    uintptr_t loseSite = base + LOSE_ONACTIVATE_RVA;
    g_winOnActivateReturn = winSite + 5;
    g_loseOnActivateReturn = loseSite + 5;
    installHook((void *)winSite, reinterpret_cast<void *>(winScreenHook), 5);
    installHook((void *)loseSite, reinterpret_cast<void *>(loseScreenHook), 5);

    // Win/Lose screen scalar destructor hooks (close overlay when player exits)
    uintptr_t winDtorSite = base + WIN_DTOR_RVA;
    uintptr_t loseDtorSite = base + LOSE_DTOR_RVA;
    g_winDtorReturn = winDtorSite + 5;
    g_loseDtorReturn = loseDtorSite + 5;
    installHook((void *)winDtorSite, reinterpret_cast<void *>(winDtorHook), 5);
    installHook((void *)loseDtorSite, reinterpret_cast<void *>(loseDtorHook), 5);

    // Unit spawn hook
    uintptr_t spawnSite = base + SPAWN_HOOK_RVA;
    g_spawnReturn = spawnSite + 7;
    installHook((void *)spawnSite, reinterpret_cast<void *>(unitSpawnHook), 7);

    // Periodic player-state poll — 60 s period, first fire after 60 s.
    // Snapshots all active slots so eliminated/disconnected players have cached
    // data available when the endgame screen fires.
    CreateTimerQueueTimer(
        &s_pollTimer, NULL, pollTimerCallback, NULL, 60000, 60000, WT_EXECUTEDEFAULT
    );
}
