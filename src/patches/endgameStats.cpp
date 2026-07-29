#include "endgameStats.h"
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
// the main menu becomes the active screen. This is the real dismiss/reset
// point: screen objects persist for the whole process lifetime, so the
// Win/LoseScreen destructors below never fire on normal screen exit (confirmed
// in a main-menu minidump where both endgame screens were still alive).
static const uintptr_t MAINMENU_ONACTIVATE_RVA = 0x27dd30;
// Scalar destructors (vtable slot 1) — kept as a safety net for teardown or
// any path that really does destroy the screens. Same prologue bytes.
static const uintptr_t WIN_DTOR_RVA = 0x297f10;
static const uintptr_t LOSE_DTOR_RVA = 0x297670;

// Return-address globals: must NOT be static (GAS cannot resolve local statics).
uintptr_t g_winOnActivateReturn = 0;
uintptr_t g_loseOnActivateReturn = 0;
uintptr_t g_mainMenuOnActivateReturn = 0;
uintptr_t g_winDtorReturn = 0;
uintptr_t g_loseDtorReturn = 0;

// OnActivate is a function prologue, so unlike the frame-tick/spawn hook
// paths this runs with an empty x87 stack — float conversion is safe here.
static void showEndgameStats(bool won) {
    const EndgameSnapshot &snap = collectEndgameStats(won);
    dumpEndgameDebug(snap);
    showStatsOverlay(snap);
}

extern "C" void showWinStats() { showEndgameStats(true); }
extern "C" void showLoseStats() { showEndgameStats(false); }

// Dismiss the overlay and reset all tracking state, returning the session to
// Idle. Fired on every main-menu activation (the primary path — covers both
// leaving the endgame screen and quitting a game mid-way) and on endgame
// screen destruction (safety net). Idempotent, and a no-op at app start when
// the menu first appears. Tracking starts again when the next game's first
// unit recruit proves a game is running — so nothing is polled (and no
// identity is frozen off a stale player table) while in the menus.
extern "C" void endgameStatsReset() {
    closeStatsOverlay();
    sessionReset();
    resetUnitCounts();
    // Auto-market thresholds are per-game; clear them on every return to menu.
    autoMarketResetThresholds();
}

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

    installUnitTracker();
    installSession();
}
