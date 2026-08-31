#include "zoomSpeed.h"
#include "../core/config.h"
#include "../core/frameTick.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Multiplies the mouse-wheel (and keyboard) camera zoom speed. Opt-in via
// [camera] ZoomSpeedMultiplier in sh2-unofficial-patch.ini; the stock code is
// left untouched at the default of 1.0. Patch details, offsets, and the
// reverse-engineering trail live in docs/features/zoom-speed.md.

static const int TENTHS_MIN = 1; // 0.1x
static const int TENTHS_MAX = 100; // 10.0x
static const int TENTHS_STOCK = 10; // 1.0x, the patch-inactive value

static const uint8_t kOriginal[15] = {0xd9, 0x45, 0x08, 0xd8, 0x41, 0x20, 0xd9, 0x5d,
                                      0x08, 0xd9, 0x45, 0x08, 0xd9, 0x51, 0x20};

// Operand of the patched fmul. Static storage in the (never unloaded) patch
// DLL, so the address baked into the game code stays valid for the process
// lifetime. A 4-byte aligned float, so the overlay thread's writes cannot tear
// against the game thread's reads.
static float g_zoomMultiplier = 1.0f;

static int s_tenths = TENTHS_STOCK;
static bool s_installed = false;
static bool s_failed = false;
static bool s_installPending = false;

static bool writeHook() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    void *patchSite = (void *)(base + 0x3623D8);

    // Only patch if the stock bytes are present, so a future game update that
    // shifts this code cannot be silently corrupted.
    if (memcmp(patchSite, kOriginal, sizeof(kOriginal)) != 0) {
        return false;
    }

    uint8_t patched[15] = {0xd9, 0x45, 0x08, // fld  [ebp+8]         (delta)
                           0xd8, 0x0d, 0, //    fmul [g_zoomMultiplier]
                           0,    0,    0, //         (abs32 operand filled in below)
                           0xd8, 0x41, 0x20, // fadd [ecx+0x20]      (+ distance)
                           0xd9, 0x51, 0x20}; // fst [ecx+0x20]      (store distance)

    uint32_t multiplierAddr = (uint32_t)(uintptr_t)&g_zoomMultiplier;
    memcpy(patched + 5, &multiplierAddr, sizeof(multiplierAddr));

    DWORD oldProtect;
    VirtualProtect(patchSite, sizeof(patched), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(patchSite, patched, sizeof(patched));
    VirtualProtect(patchSite, sizeof(patched), oldProtect, &oldProtect);
    return true;
}

// Runs on the game thread. A 15-byte rewrite of code the camera may be
// executing has to happen where the game is not executing it, and the frame
// tick is the one place a patch knows that. No float arithmetic here, as the
// tick site requires.
static void zoomSpeedTick() {
    if (!s_installPending) {
        return;
    }

    s_installPending = false;
    s_installed = writeHook();
    s_failed = !s_installed;
}

int zoomSpeedTenths() { return s_tenths; }

bool zoomSpeedFailed() { return s_failed; }

void zoomSpeedSetTenths(int tenths) {
    if (tenths < TENTHS_MIN || tenths > TENTHS_MAX) {
        return;
    }

    s_tenths = tenths;

    // Integer tenths to the float the game multiplies by. At 10 this is exactly
    // 1.0f, so an installed patch left at the stock value is a no-op rather
    // than an approximation of one.
    g_zoomMultiplier = (float)tenths / 10.0f;

    if (tenths == TENTHS_STOCK || s_installed) {
        return;
    }

    s_installPending = true;
}

void installZoomSpeed() {
    // Registered even when the feature is off, so the settings panel can switch
    // it on later: the tick is where the code rewrite happens, and the hook
    // behind it may only be installed at load time.
    registerFrameTick(&zoomSpeedTick);

    float multiplier = configFloat("camera", "ZoomSpeedMultiplier", 1.0f);
    int tenths = TENTHS_STOCK;

    // The negated comparison also rejects NaN. Out-of-range values fall back to
    // the stock speed, per the ini's convention.
    if (multiplier >= 0.1f && multiplier <= 10.0f) {
        tenths = (int)(multiplier * 10.0f + 0.5f);
    }

    s_tenths = tenths;
    g_zoomMultiplier = (float)tenths / 10.0f;

    if (tenths == TENTHS_STOCK) {
        return; // default: the game code is left completely untouched
    }

    // Install time runs under the loader lock with no game thread yet, so the
    // rewrite is safe to do directly rather than deferring it to a frame.
    s_installed = writeHook();
    s_failed = !s_installed;
}
