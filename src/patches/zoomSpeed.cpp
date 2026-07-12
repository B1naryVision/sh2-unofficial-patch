#include "zoomSpeed.h"
#include "../core/config.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Multiplies the mouse-wheel (and keyboard) camera zoom speed. Opt-in via
// [camera] ZoomSpeedMultiplier in sh2-unofficial-patch.ini; the stock code is
// left untouched at the default of 1.0. Patch details, offsets, and the
// reverse-engineering trail live in docs/features/zoom-speed.md.

static const float MULTIPLIER_MIN = 0.1f;
static const float MULTIPLIER_MAX = 10.0f;

static const uint8_t kOriginal[15] = {0xd9, 0x45, 0x08, 0xd8, 0x41, 0x20, 0xd9, 0x5d,
                                      0x08, 0xd9, 0x45, 0x08, 0xd9, 0x51, 0x20};

// Operand of the patched fmul. Static storage in the (never unloaded) patch
// DLL, so the address baked into the game code stays valid for the process
// lifetime.
static float g_zoomMultiplier = 1.0f;

void installZoomSpeed() {
    float multiplier = configFloat("camera", "ZoomSpeedMultiplier", 1.0f);

    // The negated comparison also rejects NaN.
    if (!(multiplier >= MULTIPLIER_MIN && multiplier <= MULTIPLIER_MAX) || multiplier == 1.0f) {
        return;
    }

    g_zoomMultiplier = multiplier;

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    void *patchSite = (void *)(base + 0x3623D8);

    // Only patch if the stock bytes are present, so a future game update that
    // shifts this code cannot be silently corrupted.
    if (memcmp(patchSite, kOriginal, sizeof(kOriginal)) != 0) {
        return;
    }

    uint8_t patched[15] = {0xd9, 0x45, 0x08, // fld  [ebp+8]         (delta)
                           0xd8, 0x0d, 0,    // fmul [g_zoomMultiplier]
                           0,    0,    0, //      (abs32 operand filled in below)
                           0xd8, 0x41, 0x20, // fadd [ecx+0x20]      (+ distance)
                           0xd9, 0x51, 0x20}; // fst [ecx+0x20]      (store distance)

    uint32_t multiplierAddr = (uint32_t)(uintptr_t)&g_zoomMultiplier;
    memcpy(patched + 5, &multiplierAddr, sizeof(multiplierAddr));

    DWORD oldProtect;
    VirtualProtect(patchSite, sizeof(patched), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(patchSite, patched, sizeof(patched));
    VirtualProtect(patchSite, sizeof(patched), oldProtect, &oldProtect);
}
