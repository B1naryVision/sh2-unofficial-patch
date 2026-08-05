#include "stopTroopsHotkey.h"
#include "../core/frameTick.h"
#include "../core/hotkey.h"
#include "../core/keybindWidget.h"
#include <cstdint>
#include <windows.h>

// See docs/features/stop-troops-hotkey.md.
static const uintptr_t STOP_FUNCTION_RVA = 0xf3140; // S2ActorHandler::StopSelectedTroops(int)
static const uintptr_t ACTOR_HANDLER_RVA = 0xdb8cb8; // S2ActorHandler static global (the `this`)
static const uintptr_t LOCAL_SLOT_RVA = 0x6e8c5c; // int: local player table slot

// Configurable via [hotkeys] StopTroops in sh2-unofficial-patch.ini; None = disabled.
// Rebindable at runtime from the settings overlay (docs/features/settings-overlay.md).
static Hotkey s_hotkey = {'H', 0};
static bool s_installed = false;

typedef void(__attribute__((thiscall)) * StopSelectedTroopsFn)(void *handler, int playerSlot);

static void triggerStopSelectedTroops() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    int playerSlot = *(int *)(base + LOCAL_SLOT_RVA);

    if (playerSlot < 0) {
        return;
    }

    void *handler = (void *)(base + ACTOR_HANDLER_RVA);
    StopSelectedTroopsFn stop = (StopSelectedTroopsFn)(base + STOP_FUNCTION_RVA);
    stop(handler, playerSlot);
}

// Runs once per frame on the game/sim thread (via the shared frame-tick
// dispatcher), so the stop command is submitted on the same thread that
// services the command queue — exactly as a real Stop button click would be.
static void stopHotkeyTick() {
    static bool prevDown = false;
    bool down = hotkeyHeld(s_hotkey);

    // Polling bypasses the window procedure, so a rebind prompt cannot swallow
    // this key the way it swallows the game's own input — check it explicitly.
    if (down && !prevDown && !keybindCaptureActive() && gameWindowFocused()) {
        triggerStopSelectedTroops();
    }

    prevDown = down;
}

void installStopTroopsHotkey() {
    s_hotkey = hotkeyLoad("hotkeys", "StopTroops", s_hotkey);

    if (!hotkeyIsBound(s_hotkey)) {
        return;
    }

    registerFrameTick(stopHotkeyTick);
    s_installed = true;
}

Hotkey stopTroopsBinding() { return s_hotkey; }

void stopTroopsSetBinding(const Hotkey &hk) { s_hotkey = hk; }

bool stopTroopsInstalled() { return s_installed; }
