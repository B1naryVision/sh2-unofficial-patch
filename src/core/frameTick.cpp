#include "frameTick.h"
#include "hook.h"
#include <cstdint>
#include <windows.h>

// Main-loop per-frame body on the sim thread. See CLAUDE.md "Game-thread
// command injection" and docs/features/stop-troops-hotkey.md for how this
// site was located and validated (no branch targets the overwritten bytes).
static const uintptr_t FRAME_BODY_RVA = 0x300c0;

// Slots for every callback the patch can register. Features that can be
// switched on from the settings panel register at install time even when they
// are off, so the count is the number of tick-using patches, not the number
// currently doing anything.
static const int MAX_TICKS = 16;

static FrameTickFn s_ticks[MAX_TICKS];
static int s_tickCount = 0;
static bool s_hookInstalled = false;

// Return address global: must NOT be static (GAS cannot resolve local statics).
uintptr_t g_frameTickReturn = 0;

extern "C" void frameTickDispatch() {
    for (int i = 0; i < s_tickCount; ++i) {
        s_ticks[i]();
    }
}

// Hook site RVA 0x300c0 (6 bytes: ff d7 8b 10 8b c8):
//   call edi ; mov edx,[eax] ; mov ecx,eax
// The dispatcher runs first (all registers and flags preserved), then the
// three original, position-independent instructions are re-emitted before
// resuming at +6.
__declspec(naked) static void frameTickHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "call _frameTickDispatch\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "call *%edi\n\t"
                     "movl (%eax), %edx\n\t"
                     "movl %eax, %ecx\n\t"
                     "jmp *_g_frameTickReturn\n\t");
}

// Register at install time. The hook is written on the first registration, and
// writing it from any other thread would rewrite six bytes of the main loop
// while the game thread is executing them.
void registerFrameTick(FrameTickFn fn) {
    if (!fn || s_tickCount >= MAX_TICKS) {
        return;
    }

    s_ticks[s_tickCount++] = fn;

    if (!s_hookInstalled) {
        s_hookInstalled = true;
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        uintptr_t site = base + FRAME_BODY_RVA;
        g_frameTickReturn = site + 6;
        installHook((void *)site, reinterpret_cast<void *>(frameTickHook), 6);
    }
}
