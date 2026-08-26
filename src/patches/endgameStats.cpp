#include "endgameStats.h"
#include "../core/frameTick.h"
#include "../core/hook.h"
#include "autoMarket/autoMarket.h"
#include "endgameStats/collect.h"
#include "endgameStats/debugDump.h"
#include "endgameStats/overlay.h"
#include "endgameStats/session.h"
#include "endgameStats/unitTracker.h"
#include <cstdint>
#include <windows.h>

// Wiring only: hooks the Win/LoseScreen show/close points and connects the
// endgameStats modules (unitTracker → session → collect → overlay). See
// docs/features/endgame-stats.md.

// Win/LoseScreen::OnActivate — vtable slot 2, found via RTTI. Both start with
// the prologue 55 8b ec 6a ff (push ebp; mov ebp,esp; push -1), re-emitted by
// the trampolines below.
static const uintptr_t WIN_ONACTIVATE_RVA = 0x297fa0;
static const uintptr_t LOSE_ONACTIVATE_RVA = 0x297700;
// MainMenuScreen::OnActivate (vtable slot 2, same prologue) — fires whenever
// the main menu becomes the active screen. It resets the session on any return
// to the menu, including quitting a game mid-way, where no endgame screen ever
// appears. It is not the only dismiss point: the campaign goes straight on to
// the next mission, so the overlay is really dismissed by the screen-detach
// tick below. The Win/LoseScreen destructors never fire on normal screen exit
// (confirmed in a main-menu minidump where both endgame screens were still
// alive).
static const uintptr_t MAINMENU_ONACTIVATE_RVA = 0x27dd30;
// Scalar destructors (vtable slot 1) — kept as a safety net for teardown or
// any path that really does destroy the screens. Same prologue bytes.
static const uintptr_t WIN_DTOR_RVA = 0x297f10;
static const uintptr_t LOSE_DTOR_RVA = 0x297670;

// Pane::parent. Exactly one screen is attached to the root pane at a time —
// that is the screen being displayed — so a screen's parent going back to null
// is the "the player left this screen" event the destructors never gave us.
// Same offset for every Pane; Pane::isVisible (RVA 0xcfa0) walks it.
static const uintptr_t PANE_PARENT_OFF = 0x80;

// The endgame screen whose OnActivate raised the overlay, and whether it has
// been seen attached since. Cleared by endgameStatsReset.
static void *s_endgameScreen = nullptr;
static bool s_screenWasAttached = false;

// Return-address globals: must NOT be static (GAS cannot resolve local statics).
uintptr_t g_winOnActivateReturn = 0;
uintptr_t g_loseOnActivateReturn = 0;
uintptr_t g_mainMenuOnActivateReturn = 0;
uintptr_t g_winDtorReturn = 0;
uintptr_t g_loseDtorReturn = 0;

static bool paneAttached(void *pane) {
    return *(uintptr_t *)((uintptr_t)pane + PANE_PARENT_OFF) != 0;
}

// OnActivate is a function prologue, so unlike the frame-tick/spawn hook
// paths this runs with an empty x87 stack — float conversion is safe here.
// `screen` is the hook site's ECX (the Win/LoseScreen `this`), kept so the
// tick below can tell when the player leaves that screen.
static void showEndgameStats(bool won, void *screen) {
    const EndgameSnapshot &snap = collectEndgameStats(won);
    dumpEndgameDebug(snap);
    showStatsOverlay(snap);
    s_endgameScreen = screen;
    s_screenWasAttached = paneAttached(screen);
}

extern "C" void showWinStats(void *screen) { showEndgameStats(true, screen); }
extern "C" void showLoseStats(void *screen) { showEndgameStats(false, screen); }

// Dismiss the overlay and reset all tracking state, returning the session to
// Idle. Fired when the player leaves the endgame screen (the primary path, and
// the only one the campaign takes), on every main-menu activation (covers
// quitting a game mid-way) and on endgame screen destruction (safety net).
// Idempotent, and a no-op at app start when the menu first appears. Tracking
// starts again when the next game's first unit recruit proves a game is
// running — so nothing is polled (and no identity is frozen off a stale player
// table) while in the menus.
extern "C" void endgameStatsReset() {
    s_endgameScreen = nullptr;
    s_screenWasAttached = false;
    closeStatsOverlay();
    sessionReset();
    resetUnitCounts();
    // Auto-market thresholds are per-game; clear them at every game boundary
    // (in the campaign that boundary is leaving the endgame screen, not the menu).
    autoMarketResetThresholds();
}

// Runs once per frame on the game thread. Float-free (see core/frameTick.h):
// one pointer read. Dismisses the overlay when the endgame screen stops being
// the attached screen, which is the only teardown signal that covers the
// singleplayer campaign — clicking on to the next mission never returns to the
// main menu, so the main-menu hook below never fires and the overlay used to
// stay up over the new mission.
static void endgameScreenTick() {
    if (!s_endgameScreen) {
        return;
    }

    // Arm only once the screen has actually been seen attached, so an
    // OnActivate that runs before the screen is put on screen cannot make the
    // overlay dismiss itself on the very next frame.
    if (!s_screenWasAttached) {
        s_screenWasAttached = paneAttached(s_endgameScreen);
        return;
    }

    if (paneAttached(s_endgameScreen)) {
        return;
    }

    endgameStatsReset();
}

__declspec(naked) static void winScreenHook() {
    __asm__ volatile("pushal\n\t"
                     "pushl %ecx\n\t"
                     "call _showWinStats\n\t"
                     "addl $4, %esp\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $0xffffffff\n\t"
                     "jmp *_g_winOnActivateReturn\n\t");
}

__declspec(naked) static void loseScreenHook() {
    __asm__ volatile("pushal\n\t"
                     "pushl %ecx\n\t"
                     "call _showLoseStats\n\t"
                     "addl $4, %esp\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $0xffffffff\n\t"
                     "jmp *_g_loseOnActivateReturn\n\t");
}

__declspec(naked) static void mainMenuHook() {
    __asm__ volatile("pushal\n\t"
                     "call _endgameStatsReset\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $0xffffffff\n\t"
                     "jmp *_g_mainMenuOnActivateReturn\n\t");
}

__declspec(naked) static void winDtorHook() {
    __asm__ volatile("pushal\n\t"
                     "call _endgameStatsReset\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $0xffffffff\n\t"
                     "jmp *_g_winDtorReturn\n\t");
}

__declspec(naked) static void loseDtorHook() {
    __asm__ volatile("pushal\n\t"
                     "call _endgameStatsReset\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $0xffffffff\n\t"
                     "jmp *_g_loseDtorReturn\n\t");
}

void installEndgameStats() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    // Win/Lose screen OnActivate hooks (collect stats + show overlay)
    uintptr_t winSite = base + WIN_ONACTIVATE_RVA;
    uintptr_t loseSite = base + LOSE_ONACTIVATE_RVA;
    g_winOnActivateReturn = winSite + 5;
    g_loseOnActivateReturn = loseSite + 5;
    installHook((void *)winSite, reinterpret_cast<void *>(winScreenHook), 5);
    installHook((void *)loseSite, reinterpret_cast<void *>(loseScreenHook), 5);

    // Main-menu activation hook (dismiss overlay, reset session + unit counts)
    uintptr_t menuSite = base + MAINMENU_ONACTIVATE_RVA;
    g_mainMenuOnActivateReturn = menuSite + 5;
    installHook((void *)menuSite, reinterpret_cast<void *>(mainMenuHook), 5);

    // Win/Lose screen scalar destructor hooks (teardown safety net)
    uintptr_t winDtorSite = base + WIN_DTOR_RVA;
    uintptr_t loseDtorSite = base + LOSE_DTOR_RVA;
    g_winDtorReturn = winDtorSite + 5;
    g_loseDtorReturn = loseDtorSite + 5;
    installHook((void *)winDtorSite, reinterpret_cast<void *>(winDtorHook), 5);
    installHook((void *)loseDtorSite, reinterpret_cast<void *>(loseDtorHook), 5);

    // Per-frame check for the endgame screen being left (see endgameScreenTick).
    registerFrameTick(endgameScreenTick);

    installStatsOverlay();
    installUnitTracker();
    installSession();
}
