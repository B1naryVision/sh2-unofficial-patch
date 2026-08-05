#include "attackHotkey.h"
#include "../core/hook.h"
#include "../core/hotkey.h"
#include "../core/keybindWidget.h"
#include <cstdint>
#include <windows.h>

// See docs/features/attack-command-hotkey.md.
static const uintptr_t FRAME_SITE_RVA = 0x300e1; // per-frame main-loop instruction (sim thread)
static const uintptr_t DISPATCHER_RVA = 0x22e7d0; // SubPanelTroops::handleCommand (vtable slot 0)
static const uintptr_t PANEL_VTABLE_RVA = 0x5cd4cc; // SubPanelTroops vtable (for validation)

static const uint16_t SET_STATE_OPCODE = 0x68; // message opcode: set toggle-button state
static const uint16_t ATTACK_STANCE_SUBID = 0x1dbd; // the "Attack" stance toggle button
static const uintptr_t ATTACK_FLAG_OFF = 0x8be0; // panel byte: attack stance on/off

// Configurable via [hotkeys] AttackToggle in sh2-unofficial-patch.ini; None = disabled.
// Rebindable at runtime from the settings overlay (docs/features/settings-overlay.md).
static Hotkey s_hotkey = {VK_XBUTTON1, 0}; // Mouse4
static bool s_installed = false;

typedef void(__attribute__((thiscall)) * DispatchFn)(void *panel, void *msg);

// Cached pointer to the live troop command panel (SubPanelTroops), captured from the
// dispatcher hook. Validated against the panel vtable before use.
void *g_troopPanel = nullptr;
uintptr_t g_dispReturn = 0;

// Re-emit state for the per-frame trampoline (see frameHook). The overwritten 5 bytes are
// `mov ecx, imm32` whose immediate is an ASLR-relocated module address, captured live.
uint32_t g_attackHookEcx = 0;
uintptr_t g_attackHookReturn = 0;

// Toggle the "Attack" stance on the currently-selected troops by replaying the exact
// message the stock Attack toggle button sends to the panel. Setting panel+0x8be0 makes the
// game turn subsequent move orders into attack-moves (the flag's only reader swaps the move
// command parameter from 0x3 to 0x63). Client-local UI state; the resulting move order is a
// normal networked player command.
static void triggerAttackToggle() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    void *panel = g_troopPanel;

    if (!panel || *(uintptr_t *)panel != base + PANEL_VTABLE_RVA) {
        return;
    }

    uint8_t cur = *((uint8_t *)panel + ATTACK_FLAG_OFF);
    uint8_t next = cur ? 0 : 1;

    uint8_t msg[0x18] = {0};
    *(uint16_t *)(msg + 0x00) = SET_STATE_OPCODE;
    *(uint16_t *)(msg + 0x02) = ATTACK_STANCE_SUBID;
    msg[0x0c] = next;

    DispatchFn dispatch = (DispatchFn)(base + DISPATCHER_RVA);
    dispatch(panel, msg);
}

// Runs once per frame on the game/sim thread (from the main-loop body hook), so the toggle
// is applied on the same thread that owns the panel — exactly as a real click would be.
extern "C" void attackHotkeyTick() {
    static bool prevDown = false;
    bool down = hotkeyHeld(s_hotkey);

    // Polling bypasses the window procedure, so a rebind prompt cannot swallow
    // this key the way it swallows the game's own input — check it explicitly.
    if (down && !prevDown && !keybindCaptureActive() && gameWindowFocused()) {
        triggerAttackToggle();
    }

    prevDown = down;
}

// Caches the live troop panel `this` (ecx on entry, thiscall). Hook site RVA 0x22e7d0,
// 6 bytes: 55 8b ec 83 ec 08 = `push ebp ; mov ebp,esp ; sub esp,8` (position-independent,
// re-emitted verbatim). Only a mov to a global — no register/flag clobber, so no save/restore.
__declspec(naked) static void dispatcherHook() {
    __asm__ volatile("movl %ecx, _g_troopPanel\n\t"
                     "push %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "subl $8, %esp\n\t"
                     "jmp *_g_dispReturn\n\t");
}

// Hook site RVA 0x300e1 (5 bytes: b9 00 d9 ac 02) = `mov ecx, 0x2acd900`, a few instructions
// past the Stop hotkey's hook in the same per-frame main-loop body. The tick runs first (all
// registers preserved), then the original `mov ecx, imm32` is re-emitted from g_attackHookEcx
// (its immediate is ASLR-relocated) before resuming at +5.
__declspec(naked) static void frameHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "call _attackHotkeyTick\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "movl _g_attackHookEcx, %ecx\n\t"
                     "jmp *_g_attackHookReturn\n\t");
}

void installAttackHotkey() {
    s_hotkey = hotkeyLoad("hotkeys", "AttackToggle", s_hotkey);

    if (!hotkeyIsBound(s_hotkey)) {
        return;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    uintptr_t site = base + FRAME_SITE_RVA;
    g_attackHookEcx = *(uint32_t *)(site + 1); // relocated `mov ecx, imm32` operand
    g_attackHookReturn = site + 5;
    installHook((void *)site, reinterpret_cast<void *>(frameHook), 5);

    uintptr_t disp = base + DISPATCHER_RVA;
    g_dispReturn = disp + 6;
    installHook((void *)disp, reinterpret_cast<void *>(dispatcherHook), 6);
    s_installed = true;
}

Hotkey attackToggleBinding() { return s_hotkey; }

void attackToggleSetBinding(const Hotkey &hk) { s_hotkey = hk; }

bool attackToggleInstalled() { return s_installed; }
