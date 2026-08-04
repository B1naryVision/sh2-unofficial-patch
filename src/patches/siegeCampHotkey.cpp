#include "siegeCampHotkey.h"
#include "../core/config.h"
#include "../core/hook.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// See docs/features/siege-camp-hotkey.md.
static const uintptr_t LATCH_SITE_RVA =
    0x1f2c65; // `mov ebx,2` at the head of the siege camp key case
static const uintptr_t GATE_SITE_RVA = 0x1f2c99; // first instruction of the case's camera block
static const uintptr_t GATE_RESUME_RVA = 0x1f2cdf; // first instruction after the camera block
static const uintptr_t CAMERA_REFRESH_RVA = 0x6e5adc; // byte: the camera moved, refresh the world
static const uintptr_t IS_VISIBLE_RVA = 0xcfa0; // Pane::isVisible() — thiscall, no args
static const uintptr_t SIEGE_PANEL_OFF = 0xdacec; // SubPanelSiegeCamp embedded in GameScreen
static const uintptr_t SIEGE_VTABLE_RVA = 0x5ccba4; // SubPanelSiegeCamp vtable (for validation)

static const uint8_t LATCH_STOCK[5] = {0xbb, 0x02, 0x00, 0x00, 0x00};
static const uint8_t GATE_STOCK[6] = {0x8d, 0x8d, 0x78, 0xff, 0xff, 0xff};

typedef uint8_t(__attribute__((thiscall)) * IsVisibleFn)(void *pane);

// GameScreen `this` (edi at the hook site), captured by the latch trampoline.
void *g_siegeGameScreen = nullptr;
uintptr_t g_siegeLatchReturn = 0;

// Consumed by the gate trampoline: nonzero means skip the camera block for this
// keypress. Latched once, before the case body runs — the body runs twice per
// press (the stock loop re-enters after resetting its cycle index), so reading
// the panel state inside the block would see the panel the first pass opened.
int g_siegeSuppressCamera = 0;

// Resume addresses and the refresh-flag address for the gate trampoline, all
// resolved at install time (the flag's address is base-relocated).
uintptr_t g_siegeGateStock = 0;
uintptr_t g_siegeGateSkip = 0;
uintptr_t g_siegeRefreshFlag = 0;

extern "C" void siegeCampLatch() {
    uint8_t *screen = (uint8_t *)g_siegeGameScreen;

    g_siegeSuppressCamera = 0;

    if (!screen) {
        return;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    void *panel = screen + SIEGE_PANEL_OFF;

    // Anything other than the expected panel means the offset no longer holds;
    // fall back to the stock "always jump" behaviour rather than guessing.
    if (*(uintptr_t *)panel != base + SIEGE_VTABLE_RVA) {
        return;
    }

    IsVisibleFn isVisible = (IsVisibleFn)(base + IS_VISIBLE_RVA);

    if (!isVisible(panel)) {
        g_siegeSuppressCamera = 1;
    }
}

// Hook site RVA 0x1f2c65 (5 bytes: bb 02 00 00 00 = `mov ebx,0x2`), reached only through the
// key handler's jump table entry for the siege camp key. Position-independent, so the
// instruction is re-emitted verbatim after the callback.
__declspec(naked) static void latchHook() {
    __asm__ volatile("movl %edi, _g_siegeGameScreen\n\t"
                     "pushal\n\t"
                     "pushfl\n\t"
                     "call _siegeCampLatch\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "movl $2, %ebx\n\t"
                     "jmp *_g_siegeLatchReturn\n\t");
}

// Hook site RVA 0x1f2c99 (6 bytes: 8d 8d 78 ff ff ff = `lea ecx,[ebp-0x88]`), the head of the
// case's camera block. eax, ecx, edx and the flags are all dead here; esi, edi, ebx and ebp are
// not touched. Either the stock block runs unchanged, or it is skipped in favour of just the
// three instructions the block interleaves into it for the panel call, plus the world-refresh
// flag the stock code always sets. Skipping the whole block keeps the two fld/fstp pairs
// balanced, so the x87 stack is undisturbed either way.
__declspec(naked) static void cameraGateHook() {
    __asm__ volatile("movl _g_siegeSuppressCamera, %eax\n\t"
                     "testl %eax, %eax\n\t"
                     "jne .LsiegeSkipCamera\n\t"
                     "leal -0x88(%ebp), %ecx\n\t"
                     "jmp *_g_siegeGateStock\n\t"
                     ".LsiegeSkipCamera:\n\t"
                     "movl -0x1c(%ebp), %ecx\n\t"
                     "subl $8, %esp\n\t"
                     "movl %esp, %eax\n\t"
                     "movl _g_siegeRefreshFlag, %edx\n\t"
                     "movb $1, (%edx)\n\t"
                     "jmp *_g_siegeGateSkip\n\t");
}

void installSiegeCampHotkey() {
    if (configInt("interface", "SiegeCampJumpOnSecondPress", 1) == 0) {
        return;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint8_t *latchSite = (uint8_t *)(base + LATCH_SITE_RVA);
    uint8_t *gateSite = (uint8_t *)(base + GATE_SITE_RVA);

    // All or nothing: a build whose key handler moved must not be half-patched.
    if (memcmp(latchSite, LATCH_STOCK, sizeof(LATCH_STOCK)) != 0 ||
        memcmp(gateSite, GATE_STOCK, sizeof(GATE_STOCK)) != 0) {
        return;
    }

    g_siegeRefreshFlag = base + CAMERA_REFRESH_RVA;
    g_siegeGateStock = (uintptr_t)gateSite + sizeof(GATE_STOCK);
    g_siegeGateSkip = base + GATE_RESUME_RVA;

    // The latch is installed first: on its own it only sets a flag nobody reads.
    g_siegeLatchReturn = (uintptr_t)latchSite + sizeof(LATCH_STOCK);
    installHook(latchSite, reinterpret_cast<void *>(&latchHook), sizeof(LATCH_STOCK));
    installHook(gateSite, reinterpret_cast<void *>(&cameraGateHook), sizeof(GATE_STOCK));
}
