#include "skyBackdrop.h"
#include "../core/config.h"
#include "../core/hook.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Stretches the sky backdrop down to the bottom of the screen so the area
// beyond the map's edge shows sky instead of black at a raised
// ZoomOutMultiplier. Opt-in via [camera] ExtendSky in
// sh2-unofficial-patch.ini. Patch details, offsets, and the
// reverse-engineering trail live in docs/features/sky-backdrop.md.

// Where the sky quad's height is written into the draw call's argument block,
// inside the effect-pass loop of drawSky (VA 0x74b540).
static const uintptr_t HEIGHT_ARG_RVA = 0x34b540;

static const uint8_t HEIGHT_ARG_STOCK[7] = {0xd9, 0x45, 0xa8, // fld  [ebp-0x58]
                                            0xd9, 0x5c, 0x24, 0x0c}; // fstp [esp+0xc]

// Renderer viewport height in pixels, as an int.
static const uintptr_t VIEWPORT_HEIGHT_RVA = 0x6c781c;

// Read by the hook below, so neither may be static (GAS cannot resolve local
// statics). The viewport height is reached through a pointer because the exe
// is base-relocated: its address is only known at install time.
uint32_t *g_skyViewportHeight = 0;
uintptr_t g_skyBackdropReturn = 0;

// Replaces `arg4 = quadHeight` with `arg4 = viewportHeight - quadTop`, so the
// quad always ends exactly at the bottom of the screen, and rewrites arg8 (the
// bottom V coordinate, stock 1.0) by the same ratio so the texture keeps its
// original scale — the sky above the horizon is drawn exactly as before, and
// only the part that used to fall short is new.
//
// eax and edx are dead here (their last use is the arg-block pointer store at
// 0x74b525); ecx holds the renderer for the call at 0x74b564 and is untouched.
// esp still points at the argument block, and the x87 stack is left balanced.
__declspec(naked) static void skyHeightHook() {
    __asm__ volatile("movl _g_skyViewportHeight, %eax\n\t"
                     "fildl (%eax)\n\t"
                     "fsubs -0x48(%ebp)\n\t" // - quadTop
                     "fsts 0xc(%esp)\n\t" // arg4 = height
                     "fdivs -0x58(%ebp)\n\t" // / stock height
                     "fstps 0x1c(%esp)\n\t" // arg8 = bottom V
                     "jmp *_g_skyBackdropReturn\n\t");
}

void installSkyBackdrop() {
    if (configInt("camera", "ExtendSky", 0) == 0) {
        return;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint8_t *site = (uint8_t *)(base + HEIGHT_ARG_RVA);

    // Only patch if the stock bytes are present, so a future game update that
    // shifts this code cannot be silently corrupted.
    if (memcmp(site, HEIGHT_ARG_STOCK, sizeof(HEIGHT_ARG_STOCK)) != 0) {
        return;
    }

    g_skyViewportHeight = (uint32_t *)(base + VIEWPORT_HEIGHT_RVA);
    g_skyBackdropReturn = (uintptr_t)site + sizeof(HEIGHT_ARG_STOCK);

    installHook(site, reinterpret_cast<void *>(skyHeightHook), sizeof(HEIGHT_ARG_STOCK));
}
