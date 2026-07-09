#include "stopTroopsHotkey.h"
#include "../core/frameTick.h"
#include <cstdint>
#include <windows.h>

// See docs/features/stop-troops-hotkey.md.
static const uintptr_t STOP_FUNCTION_RVA = 0xf3140; // S2ActorHandler::StopSelectedTroops(int)
static const uintptr_t ACTOR_HANDLER_RVA = 0xdb8cb8; // S2ActorHandler static global (the `this`)
static const uintptr_t LOCAL_SLOT_RVA = 0x6e8c5c; // int: local player table slot

static const int HOTKEY_VK = 'H';

typedef void(__attribute__((thiscall)) * StopSelectedTroopsFn)(void *handler, int playerSlot);

static bool gameWindowFocused() {
    HWND foreground = GetForegroundWindow();

    if (!foreground) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);

    return pid == GetCurrentProcessId();
}

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
    bool down = (GetAsyncKeyState(HOTKEY_VK) & 0x8000) != 0;

    if (down && !prevDown && gameWindowFocused()) {
        triggerStopSelectedTroops();
    }

    prevDown = down;
}

void installStopTroopsHotkey() { registerFrameTick(stopHotkeyTick); }
