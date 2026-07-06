#include "stopTroopsHotkey.h"
#include "../core/hook.h"
#include <cstdint>
#include <windows.h>

// See docs/features/stop-troops-hotkey.md.
static const uintptr_t STOP_FUNCTION_RVA = 0xf3140; // S2ActorHandler::StopSelectedTroops(int)
static const uintptr_t ACTOR_HANDLER_RVA = 0xdb8cb8; // S2ActorHandler static global (the `this`)
static const uintptr_t LOCAL_SLOT_RVA = 0x6e8c5c; // int: local player table slot
static const uintptr_t FRAME_BODY_RVA =
    0x300c0; // main-loop body, runs once per frame on the sim thread

static const int HOTKEY_VK = 'H';

typedef void(__attribute__((thiscall)) * StopSelectedTroopsFn)(void *handler, int playerSlot);

// base+FRAME_BODY_RVA+6 — resume point after the three re-emitted instructions.
uintptr_t g_frameHookReturn = 0;

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

// Runs once per frame on the game/sim thread (from the main-loop body hook), so
// the stop command is submitted on the same thread that services the command
// queue — exactly as a real Stop button click would be.
extern "C" void stopHotkeyTick() {
    static bool prevDown = false;
    bool down = (GetAsyncKeyState(HOTKEY_VK) & 0x8000) != 0;

    if (down && !prevDown && gameWindowFocused()) {
        triggerStopSelectedTroops();
    }

    prevDown = down;
}

// Hook site RVA 0x300c0 (6 bytes: ff d7 8b 10 8b c8):
//   call edi ; mov edx,[eax] ; mov ecx,eax
// The tick runs first (all registers preserved), then the three original,
// position-independent instructions are re-emitted before resuming at +6.
__declspec(naked) static void frameHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "call _stopHotkeyTick\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "call *%edi\n\t"
                     "movl (%eax), %edx\n\t"
                     "movl %eax, %ecx\n\t"
                     "jmp *_g_frameHookReturn\n\t");
}

void installStopTroopsHotkey() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t site = base + FRAME_BODY_RVA;
    g_frameHookReturn = site + 6;
    installHook((void *)site, reinterpret_cast<void *>(frameHook), 6);
}
