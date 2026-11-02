#include "startupQuitCrash.h"
#include "../core/exeBase.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// See docs/bugs/startup-quit-crash.md.

static const uintptr_t TEARDOWN_CALL_RVA = 0x30331;
static const uintptr_t SCENE_TEARDOWN_RVA = 0x1f8240;
static const uintptr_t LANDSCAPE_RVA = 0xf5bdb0;
static const uintptr_t LANDSCAPE_TEXTURE_ARRAY_OFF = 0x2130b8;

static uintptr_t s_sceneTeardown = 0;
static const uint32_t *s_landscapeTextureArray = nullptr;

// The array is allocated only by the scene setup, so a null pointer means the
// setup never ran and there is nothing to tear down.
static void sceneTeardownIfBuilt() {
    if (*s_landscapeTextureArray == 0) {
        return;
    }

    reinterpret_cast<void (*)()>(s_sceneTeardown)();
}

void installStartupQuitCrashFix() {
    uintptr_t base = exeBase();
    uint8_t *site = (uint8_t *)(base + TEARDOWN_CALL_RVA);

    uint8_t expected[5] = {0xe8, 0, 0, 0, 0};
    int32_t stockRel = (int32_t)(SCENE_TEARDOWN_RVA - (TEARDOWN_CALL_RVA + 5));
    memcpy(expected + 1, &stockRel, sizeof(stockRel));

    if (memcmp(site, expected, sizeof(expected)) != 0) {
        return;
    }

    s_sceneTeardown = base + SCENE_TEARDOWN_RVA;
    s_landscapeTextureArray =
        (const uint32_t *)(base + LANDSCAPE_RVA + LANDSCAPE_TEXTURE_ARRAY_OFF);

    uint8_t patched[5] = {0xe8, 0, 0, 0, 0};
    int32_t stubRel = (int32_t)((uintptr_t)sceneTeardownIfBuilt - ((uintptr_t)site + 5));
    memcpy(patched + 1, &stubRel, sizeof(stubRel));

    DWORD oldProtect;
    VirtualProtect(site, sizeof(patched), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(site, patched, sizeof(patched));
    VirtualProtect(site, sizeof(patched), oldProtect, &oldProtect);
}
