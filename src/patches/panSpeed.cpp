#include "panSpeed.h"
#include "../core/config.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Multiplies how fast the camera pans across the map, for both the keyboard
// pan keys and pushing the mouse against a screen edge. Opt-in via
// [camera] PanSpeedMultiplier in sh2-unofficial-patch.ini; the stock code is
// left untouched at the default of 1.0. Patch details, offsets, and the
// reverse-engineering trail live in docs/features/pan-speed.md.
//
// Note the engine's own names run the other way: Camera::scroll is the mover
// patched here, and Camera::pan is an unrelated bounds clamp. Engine symbols
// below keep their real names.

static const float MULTIPLIER_MIN = 0.1f;
static const float MULTIPLIER_MAX = 10.0f;

// fmul qword [pan speed], inside the camera controller's per-frame update.
// It turns the frame time into the target velocity Camera::scroll ramps up to,
// and it is the only reference to that constant in the exe.
static const uintptr_t SITE_RVA = 0x1f9d86;
static const uintptr_t SPEED_CONSTANT_RVA = 0x5c81e0;

static const uint8_t FMUL_QWORD_ABS32[2] = {0xdc, 0x0d};
static const uint8_t FMUL_DWORD_ABS32[2] = {0xd8, 0x0d};

// The stock constant's value: world units per second, before Camera::scroll
// scales it by the camera's zoom distance.
static const float STOCK_SPEED = 76800.0f;

// Operand of the patched fmul. Static storage in the (never unloaded) patch
// DLL, so the address baked into the game code stays valid for the process
// lifetime.
static float g_panSpeed = STOCK_SPEED;

void installPanSpeed() {
    float multiplier = configFloat("camera", "PanSpeedMultiplier", 1.0f);

    // The negated comparison also rejects NaN.
    if (!(multiplier >= MULTIPLIER_MIN && multiplier <= MULTIPLIER_MAX) || multiplier == 1.0f) {
        return;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint8_t *site = (uint8_t *)(base + SITE_RVA);

    if (memcmp(site, FMUL_QWORD_ABS32, sizeof(FMUL_QWORD_ABS32)) != 0) {
        return;
    }

    // The stock operand is an absolute address, which the loader relocates, so
    // it is rebuilt from the running base instead of compared against the bytes
    // on disk (see CLAUDE.md).
    uint32_t operand = 0;
    memcpy(&operand, site + 2, sizeof(operand));

    if (operand != (uint32_t)(base + SPEED_CONSTANT_RVA)) {
        return;
    }

    g_panSpeed = STOCK_SPEED * multiplier;

    // Same length: the qword form reads the game's double, the dword form reads
    // our float.
    uint8_t patched[6];
    uint32_t speedAddr = (uint32_t)(uintptr_t)&g_panSpeed;

    memcpy(patched, FMUL_DWORD_ABS32, sizeof(FMUL_DWORD_ABS32));
    memcpy(patched + 2, &speedAddr, sizeof(speedAddr));

    DWORD oldProtect;
    VirtualProtect(site, sizeof(patched), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(site, patched, sizeof(patched));
    VirtualProtect(site, sizeof(patched), oldProtect, &oldProtect);
}
