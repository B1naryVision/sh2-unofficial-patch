#include "farPlane.h"
#include "../core/config.h"
#include "../core/hook.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <windows.h>

// Multiplies the far clip plane the projection matrix is built with, so the
// ground keeps drawing when the camera is pulled further back than the stock
// view distance allows. Opt-in via [camera] FarPlaneMultiplier in
// sh2-unofficial-patch.ini. Patch details, offsets, and the
// reverse-engineering trail live in docs/features/far-plane.md.

static const float MULTIPLIER_MIN = 1.0f;
static const float MULTIPLIER_MAX = 10.0f;

// The sites that load the renderer's far distance (renderer +0x184). The
// first two pass it to setPerspective; the third derives the distance fog
// range from it, which has to scale with the far plane or the fog saturates
// to its own colour long before the clip plane is reached. Each is a 6-byte
// `fld`.
static const uintptr_t CAMERA_SITE_RVA = 0x1fa5e7;
static const uintptr_t RENDERER_SITE_RVA = 0x335dee;
static const uintptr_t FOG_SITE_RVA = 0x1ff578;

// fld [<far distance>] — reached with ecx already holding the renderer. Only
// the opcode is a literal: the operand is an absolute address the loader
// base-relocates, so it is rebuilt from the runtime base instead of compared
// against the stale on-disk bytes (see CLAUDE.md).
static const uint8_t FLD_ABS32[2] = {0xd9, 0x05};

// Full length of `fld [abs32]`: the two-byte opcode plus the address.
static const size_t FLD_ABS32_LENGTH = 6;

// fld [esi+0x184]
static const uint8_t RENDERER_STOCK[6] = {0xd9, 0x86, 0x84, 0x01, 0x00, 0x00};

// Renderer far-distance field: the operand both absolute sites load, and the
// address the fog stub dereferences.
static const uintptr_t FAR_DISTANCE_RVA = 0x6c782c;

// The renderer object, whose address is the immediate of the `mov ecx` before
// the camera site.
static const uintptr_t RENDERER_RVA = 0x6c76a8;

// Opcode of the `mov ecx, <renderer>` five bytes before the camera site, which
// is what lets the camera stub reach the renderer without an absolute address.
static const uint8_t MOV_ECX_IMM32 = 0xb9;

// True when `site` is `fld [<base + rva>]`, rebuilding the relocated operand
// rather than trusting the on-disk immediate.
static bool isAbsoluteLoadOf(const uint8_t *site, uintptr_t base, uintptr_t rva) {
    uint32_t operand = 0;

    if (memcmp(site, FLD_ABS32, sizeof(FLD_ABS32)) != 0) {
        return false;
    }

    memcpy(&operand, site + sizeof(FLD_ABS32), sizeof(operand));

    return operand == (uint32_t)(base + rva);
}

// Same idea for the `mov ecx, <renderer>` guard.
static bool isMovEcxImm32Of(const uint8_t *site, uintptr_t base, uintptr_t rva) {
    uint32_t operand = 0;

    if (site[0] != MOV_ECX_IMM32) {
        return false;
    }

    memcpy(&operand, site + 1, sizeof(operand));

    return operand == (uint32_t)(base + rva);
}

// Read by the hooks below, so neither may be static (GAS cannot resolve local
// statics).
float g_farPlaneMultiplier = 1.0f;
float *g_farPlaneDistance = 0;
uintptr_t g_farPlaneCameraReturn = 0;
uintptr_t g_farPlaneRendererReturn = 0;
uintptr_t g_farPlaneFogReturn = 0;

// Both stubs reload the far distance through the register the stock code
// already has pointing at the renderer, so neither needs the relocated
// absolute address. The x87 stack is left exactly as the replaced `fld` left
// it: one value pushed, which the instruction after the site stores.
__declspec(naked) static void cameraFarHook() {
    __asm__ volatile("flds 0x184(%ecx)\n\t"
                     "fmuls _g_farPlaneMultiplier\n\t"
                     "jmp *_g_farPlaneCameraReturn\n\t");
}

__declspec(naked) static void rendererFarHook() {
    __asm__ volatile("flds 0x184(%esi)\n\t"
                     "fmuls _g_farPlaneMultiplier\n\t"
                     "jmp *_g_farPlaneRendererReturn\n\t");
}

// eax is dead across this site: the preceding call's return value is unused
// and the stock code overwrites eax with `mov eax, esp` before reading it.
__declspec(naked) static void fogFarHook() {
    __asm__ volatile("movl _g_farPlaneDistance, %eax\n\t"
                     "flds (%eax)\n\t"
                     "fmuls _g_farPlaneMultiplier\n\t"
                     "jmp *_g_farPlaneFogReturn\n\t");
}

void installFarPlane() {
    float multiplier = configFloat("camera", "FarPlaneMultiplier", 1.0f);

    // The negated comparison also rejects NaN.
    if (!(multiplier >= MULTIPLIER_MIN && multiplier <= MULTIPLIER_MAX) || multiplier == 1.0f) {
        return;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint8_t *cameraSite = (uint8_t *)(base + CAMERA_SITE_RVA);
    uint8_t *rendererSite = (uint8_t *)(base + RENDERER_SITE_RVA);
    uint8_t *fogSite = (uint8_t *)(base + FOG_SITE_RVA);

    // All or nothing: a build where either projection path has moved must not
    // end up with one of the two scaled and the other not.
    if (!isAbsoluteLoadOf(cameraSite, base, FAR_DISTANCE_RVA) ||
        memcmp(rendererSite, RENDERER_STOCK, sizeof(RENDERER_STOCK)) != 0 ||
        !isAbsoluteLoadOf(fogSite, base, FAR_DISTANCE_RVA) ||
        !isMovEcxImm32Of(cameraSite - 5, base, RENDERER_RVA)) {
        return;
    }

    g_farPlaneMultiplier = multiplier;
    g_farPlaneDistance = (float *)(base + FAR_DISTANCE_RVA);
    g_farPlaneCameraReturn = (uintptr_t)cameraSite + FLD_ABS32_LENGTH;
    g_farPlaneRendererReturn = (uintptr_t)rendererSite + sizeof(RENDERER_STOCK);
    g_farPlaneFogReturn = (uintptr_t)fogSite + FLD_ABS32_LENGTH;

    installHook(cameraSite, reinterpret_cast<void *>(cameraFarHook), FLD_ABS32_LENGTH);
    installHook(rendererSite, reinterpret_cast<void *>(rendererFarHook), sizeof(RENDERER_STOCK));
    installHook(fogSite, reinterpret_cast<void *>(fogFarHook), FLD_ABS32_LENGTH);
}
