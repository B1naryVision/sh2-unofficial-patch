#include "panSpeed.h"
#include "../core/config.h"
#include "../core/frameTick.h"
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

static const int TENTHS_MIN = 1; // 0.1x
static const int TENTHS_MAX = 100; // 10.0x
static const int TENTHS_STOCK = 10; // 1.0x, the patch-inactive value

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
// lifetime. A 4-byte aligned float, so the overlay thread's writes cannot tear
// against the game thread's reads.
static float g_panSpeed = STOCK_SPEED;

static int s_tenths = TENTHS_STOCK;
static bool s_installed = false;
static bool s_failed = false;
static bool s_installPending = false;

static bool writeHook() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint8_t *site = (uint8_t *)(base + SITE_RVA);

    if (memcmp(site, FMUL_QWORD_ABS32, sizeof(FMUL_QWORD_ABS32)) != 0) {
        return false;
    }

    // The stock operand is an absolute address, which the loader relocates, so
    // it is rebuilt from the running base instead of compared against the bytes
    // on disk (see CLAUDE.md).
    uint32_t operand = 0;
    memcpy(&operand, site + 2, sizeof(operand));

    if (operand != (uint32_t)(base + SPEED_CONSTANT_RVA)) {
        return false;
    }

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
    return true;
}

// Runs on the game thread, for the same reason as the zoom-speed tick: the
// controller may be executing these bytes when the overlay changes the value.
static void panSpeedTick() {
    if (!s_installPending) {
        return;
    }

    s_installPending = false;
    s_installed = writeHook();
    s_failed = !s_installed;
}

int panSpeedTenths() { return s_tenths; }

bool panSpeedFailed() { return s_failed; }

void panSpeedSetTenths(int tenths) {
    if (tenths < TENTHS_MIN || tenths > TENTHS_MAX) {
        return;
    }

    s_tenths = tenths;

    // At 10 tenths this is exactly the stock 76800.0f, so an installed patch
    // left at the stock value multiplies by the same constant the game did.
    g_panSpeed = STOCK_SPEED * (float)tenths / 10.0f;

    if (tenths == TENTHS_STOCK || s_installed) {
        return;
    }

    s_installPending = true;
}

void installPanSpeed() {
    // Registered even when the feature is off, so the settings panel can switch
    // it on later: the tick is where the code rewrite happens, and the hook
    // behind it may only be installed at load time.
    registerFrameTick(&panSpeedTick);

    float multiplier = configFloat("camera", "PanSpeedMultiplier", 1.0f);
    int tenths = TENTHS_STOCK;

    // The negated comparison also rejects NaN. Out-of-range values fall back to
    // the stock speed, per the ini's convention.
    if (multiplier >= 0.1f && multiplier <= 10.0f) {
        tenths = (int)(multiplier * 10.0f + 0.5f);
    }

    s_tenths = tenths;
    g_panSpeed = STOCK_SPEED * (float)tenths / 10.0f;

    if (tenths == TENTHS_STOCK) {
        return; // default: the game code is left completely untouched
    }

    // Install time runs under the loader lock with no game thread yet, so the
    // rewrite is safe to do directly rather than deferring it to a frame.
    s_installed = writeHook();
    s_failed = !s_installed;
}
