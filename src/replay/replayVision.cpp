#include "replayVision.h"

#include "replayCommon.h"
#include "replayViewer.h"
#include "spectatorGuard.h"
#include "../core/config.h"
#include "../core/d3dHook.h"
#include "../core/frameTick.h"
#include "../core/keybindWidget.h"
#include "../core/observerContext.h"
#include "../core/readableMemory.h"

#include <cstdint>
#include <cstring>
#include <windows.h>

// ── Ally vision ─────────────────────────────────────────────────────────────
// allied(a, b) is a thiscall on the diplomacy object (ECX = 0x11781d0) with two
// stack arguments, `ret 8`, answer in AL: a == b, or b's stance in a's
// diplomacy table is below 3. Every site below is the same five-byte
// `call rel32` preceded by `push owner; push local` (or the reverse), and every
// one compares against the LOCAL player id (0xae8c5c). That is what makes them
// safe to change: code that depends on which machine runs it cannot feed the
// lockstep simulation, or a stock multiplayer match would desync. The
// allied() calls the simulation does make (gate links, territory, the hidden
// units' own update FUN_004f7b40) are left alone.
//
//   Assassin / Outlaw  vt+0x58 0x4fb5b0, vt+0xf8 0x50efb0, vt+0xf4 0x50f060:
//       queue the unit for drawing. Hidden (vt+0x164) and not allied: not
//       drawn. Allied: drawn translucent (+0x2ab).
//   Thief  vt+0x58 0x5117e0, vt+0xf8 0x511840, vt+0xf4 0x5118c0: translucency;
//       vt+0x5c 0x511960: hidden and not allied draws the disguise instead.
//   Hovered workshops  FUN_005f4860 (game screen update, every 3rd frame):
//       the building under the cursor shows its interior (0x468480 -> +0xac,
//       40 ticks) if it is one of the workshop types and allied with the local
//       player. (Interiors opened by a close camera are the enemy() sites.)
//   ManTrap vt+0x5c 0x4c6f60, KillingPit vt+0x5c 0x4c7b30: an unsprung trap is
//       drawn only for allies.
static const uintptr_t ALLIED_RVA = 0x219e0;
static const uintptr_t ALLY_SITES[] = {
    0xfb5f3,  0x10f018, 0x10f0de,           // Assassin / Outlaw
    0x111806, 0x111869, 0x1118e9, 0x11199e, // Thief
    0x1f6287,                               // building interiors
    0xc6f9f,  0xc7b76,                      // ManTrap, KillingPit
};
static const int ALLY_SITE_COUNT = (int)(sizeof(ALLY_SITES) / sizeof(ALLY_SITES[0]));

// enemy(a, b) (RVA 0x21a30, same convention): a != b and b's stance in a's
// table is 3. Building interiors and stock piles hide behind this test, not
// allied(), so those sites get the opposite answer, "not an enemy":
//   the building draw loop FUN_0040df30 passes each building's draw (vt+0x5c)
//       the camera-close flag 0x1246514 (written by the camera, FUN_005f97c0),
//       or 0 when enemy(local, owner): the interior stays shut for enemies;
//   StockPile vt+0x84 0x4b1970 (goods shown): the same flag and test.
static const uintptr_t ENEMY_RVA = 0x21a30;
static const uintptr_t ENEMY_SITES[] = {
    0xdfb5,  // building draw loop
    0xb19d4, // stock pile goods
};
static const int ENEMY_SITE_COUNT = (int)(sizeof(ENEMY_SITES) / sizeof(ENEMY_SITES[0]));

// ── Free view ───────────────────────────────────────────────────────────────
// The camera panel (CamerasSubPanel, handler 0x637c20) button 0x15e0 calls
// FUN_005f7a50 on the game screen's world pane (GameScreen vt+0x80, screen
// +0x356c): mode +0x17c = 1, HUD hidden (GameScreen vt+0x58(1)), free camera
// limits. Leaving it is FUN_005f7c40 (mode 0, HUD back), the same call the
// panel makes when the button is toggled off or the panel closes.
static const uintptr_t GAME_SCREEN_PTR_RVA = 0xec0d6c; // GameScreen * (FUN_005ec430)
static const uintptr_t GAME_SCREEN_VT_RVA = 0x5c76ec;
static const uintptr_t SCREEN_WORLD_PANE_RVA = 0x1ecea0; // GameScreen vt+0x80
static const uintptr_t FREE_VIEW_ON_RVA = 0x1f7a50;
static const uintptr_t FREE_VIEW_OFF_RVA = 0x1f7c40;
static const uint8_t FREE_VIEW_ON_STOCK[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08, 0xd9, 0x05};
static const uint8_t FREE_VIEW_OFF_STOCK[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08, 0x56, 0x8b, 0xf1};
static const uint32_t CAMERA_MODE_OFFSET = 0x17c;
static const int CAMERA_MODE_FREE = 1;

typedef uintptr_t(__attribute__((thiscall)) * WorldPaneFn)(uintptr_t screen);
typedef void(__attribute__((thiscall)) * CameraModeFn)(uintptr_t pane);

// Read by the thunk on every patched call. Not static: GAS names them.
extern "C" {
volatile uint32_t g_allyVisionOn = 0;
uintptr_t g_allyVisionReal = 0;
uintptr_t g_enemyVisionReal = 0;
}

static bool s_allyEnabled = true;
static bool s_allyPatched = false;
static bool s_freeViewReady = false;
static Hotkey s_freeViewKey = {'F', 0};
static volatile LONG s_freeViewRequest = 0;
static WNDPROC s_origWndProc = nullptr;

__declspec(naked) static void allyVisionThunk() {
    __asm__ volatile("cmpl $0, _g_allyVisionOn\n\t"
                     "jne 1f\n\t"
                     "jmp *_g_allyVisionReal\n\t"
                     "1:\n\t"
                     "movl $1, %eax\n\t"
                     "ret $8\n\t");
}

__declspec(naked) static void enemyVisionThunk() {
    __asm__ volatile("cmpl $0, _g_allyVisionOn\n\t"
                     "jne 1f\n\t"
                     "jmp *_g_enemyVisionReal\n\t"
                     "1:\n\t"
                     "xorl %eax, %eax\n\t"
                     "ret $8\n\t");
}

static bool callsTarget(uintptr_t base, uintptr_t rva, uintptr_t target) {
    const uint8_t *site = (const uint8_t *)(base + rva);
    int32_t rel = 0;
    memcpy(&rel, site + 1, 4);
    return site[0] == 0xe8 && (uintptr_t)site + 5 + rel == target;
}

static void redirectCall(uintptr_t base, uintptr_t rva, void *thunk) {
    uint8_t *site = (uint8_t *)(base + rva);
    DWORD old = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        return; // this site keeps calling the stock function
    }
    const int32_t rel = (int32_t)((uintptr_t)thunk - (uintptr_t)site - 5);
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
}

// All twelve sites or none: each must still call its stock function.
static void patchAllySites() {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const uintptr_t allied = base + ALLIED_RVA;
    const uintptr_t enemy = base + ENEMY_RVA;
    for (int i = 0; i < ALLY_SITE_COUNT; ++i) {
        if (!callsTarget(base, ALLY_SITES[i], allied)) {
            return; // unknown build: ally vision stays off
        }
    }
    for (int i = 0; i < ENEMY_SITE_COUNT; ++i) {
        if (!callsTarget(base, ENEMY_SITES[i], enemy)) {
            return;
        }
    }
    g_allyVisionReal = allied;
    g_enemyVisionReal = enemy;
    for (int i = 0; i < ALLY_SITE_COUNT; ++i) {
        redirectCall(base, ALLY_SITES[i], (void *)&allyVisionThunk);
    }
    for (int i = 0; i < ENEMY_SITE_COUNT; ++i) {
        redirectCall(base, ENEMY_SITES[i], (void *)&enemyVisionThunk);
    }
    s_allyPatched = true;
}

static bool freeViewStockBytes() {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    return !memcmp((const void *)(base + FREE_VIEW_ON_RVA), FREE_VIEW_ON_STOCK,
                   sizeof(FREE_VIEW_ON_STOCK)) &&
           !memcmp((const void *)(base + FREE_VIEW_OFF_RVA), FREE_VIEW_OFF_STOCK,
                   sizeof(FREE_VIEW_OFF_STOCK));
}

// The game screen's world pane, or 0 when there is no in-match game screen.
static uintptr_t worldPane(uintptr_t base) {
    const uintptr_t screen = *(const volatile uintptr_t *)(base + GAME_SCREEN_PTR_RVA);
    if (!screen || !observerReadable((const void *)screen, 4)) {
        return 0;
    }
    const uintptr_t vt = *(const uintptr_t *)screen;
    if (vt != base + GAME_SCREEN_VT_RVA || *(const uintptr_t *)(vt + 0x80) != base + SCREEN_WORLD_PANE_RVA) {
        return 0;
    }
    const uintptr_t pane = ((WorldPaneFn)(base + SCREEN_WORLD_PANE_RVA))(screen);
    return observerReadable((const void *)(pane + CAMERA_MODE_OFFSET), 4) ? pane : 0;
}

static bool freeViewOn(uintptr_t pane) {
    return *(const volatile int32_t *)(pane + CAMERA_MODE_OFFSET) == CAMERA_MODE_FREE;
}

static void setFreeView(uintptr_t base, uintptr_t pane, bool on) {
    ObserverFpScope fp; // both camera calls use x87; the tick site is mid-function
    ((CameraModeFn)(base + (on ? FREE_VIEW_ON_RVA : FREE_VIEW_OFF_RVA)))(pane);
}

static void visionTick() {
    const bool playing = spectatorGuardActive();
    g_allyVisionOn = playing && s_allyEnabled ? 1 : 0;
    const bool request = InterlockedExchange(&s_freeViewRequest, 0) != 0;
    if (!playing || !s_freeViewReady || !replayInGame()) {
        return;
    }
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    // The ESC menu is the way out of a replay: give the HUD back first so the
    // match is never left in free view.
    if (replayViewerMenuOpen()) {
        const uintptr_t pane = worldPane(base);
        if (pane && freeViewOn(pane)) {
            setFreeView(base, pane, false);
        }
        return;
    }
    if (request) {
        const uintptr_t pane = worldPane(base);
        if (pane) {
            setFreeView(base, pane, !freeViewOn(pane));
        }
    }
}

// Outside the spectator guard's subclass (installed after it), so the key is
// seen before the guard swallows it. Only records the request; the frame tick
// acts on the game thread.
static LRESULT CALLBACK visionWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && spectatorGuardActive() &&
        !keybindCaptureActive() && hotkeyMatchesKeyDown(s_freeViewKey, (int)wparam)) {
        if (!(lparam & (1 << 30))) { // ignore auto-repeat
            InterlockedExchange(&s_freeViewRequest, 1);
        }
        return 0;
    }
    return CallWindowProcW(s_origWndProc, hwnd, msg, wparam, lparam);
}

static void ensureSubclass(IDirect3DDevice9 *) {
    if (s_origWndProc) {
        return;
    }
    HWND hwnd = (HWND)d3dDeviceWindow();
    if (!hwnd) {
        return;
    }
    s_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)visionWndProc);
}

Hotkey replayFreeViewBinding() {
    return s_freeViewKey;
}

void replayFreeViewSetBinding(const Hotkey &hk) {
    s_freeViewKey = hk;
}

bool replayFreeViewInstalled() {
    return s_freeViewReady;
}

void installReplayVision() {
    s_allyEnabled = configInt("replay", "AllyVision", 1) != 0;
    s_freeViewKey = hotkeyLoad("hotkeys", "ReplayFreeView", s_freeViewKey);
    if (s_allyEnabled) {
        patchAllySites();
    }
    s_freeViewReady = freeViewStockBytes();
    if (!s_allyPatched && !s_freeViewReady) {
        return;
    }
    registerFrameTick(visionTick);
    if (s_freeViewReady) {
        registerD3DRender(ensureSubclass);
    }
}
