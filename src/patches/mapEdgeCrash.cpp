#include "mapEdgeCrash.h"
#include "../core/hook.h"
#include <cstdint>
#include <windows.h>

// Crash fix for out-of-bounds troop formations. See
// docs/bugs/map-edge-formation-crash.md for the full investigation.
//
// GlowWorm::KnotFormation (vtable slot 6, RVA 0x37db80) walks the sample points of a
// formation/path spline and, for each, reads a per-tile flag from the 256x256 map-flag
// array. It computes a flat tile index (tileY*256 + tileX) from sign-extended tile
// coordinates and reads it with no bounds check. When a sample point lands off the map
// edge the coordinate goes negative (or overshoots), the index balloons far past the
// 64 KB array, and the read faults (0xC0000005 at RVA 0x37dd6b).
//
// The game's other map-flag accessors guard this by comparing each coordinate against
// 0x100 (the grid is a hard 256x256). We do the equivalent on the already-combined
// index: valid cells are [0, 0x10000); anything at or above that is off-map and is
// diverted to the routine's own "off map" return path.

static const uintptr_t HOOK_SITE_RVA = 0x37dd6b; // the unguarded `test [ecx+mapflags],0x3f`
static const uintptr_t ARRAY_DISP_OFF = 2; // disp32 within that instruction
static const uintptr_t CONTINUE_RVA = 0x37dd74; // in-range: resume the sample loop
static const uintptr_t OFFMAP_RVA = 0x37dd8d; // out-of-range: the "off map" (false) return

// Live re-emit state for the trampoline. g_mapFlagBase is the array base captured from the
// instruction's disp32 — it is ASLR-relocated in memory, so it must be read live, not baked.
uint32_t g_mapFlagBase = 0;
uintptr_t g_mapEdgeContinue = 0;
uintptr_t g_mapEdgeOffmap = 0;

// Original 9 bytes at the hook site: `test byte ptr [ecx+0x1d6124c],0x3f` (f6 81 <disp32> 3f)
// followed by `je 0x77dd8d` (74 19). disp32 (indices 2..5) is relocated at load time, so it
// is excluded from the stock-bytes check below.
static const uint8_t kOriginal[9] = {0xf6, 0x81, 0x4c, 0x12, 0xd6, 0x01, 0x3f, 0x74, 0x19};

// ecx already holds the flat tile index (tileY*256 + tileX). EAX is dead here and the two
// resume targets read neither EAX nor the incoming flags, so we are free to clobber both.
__declspec(naked) static void mapEdgeGuard() {
    __asm__ volatile("cmpl $0x10000, %ecx\n\t" // index outside the 256x256 grid?
                     "jae 1f\n\t" //   -> off-map
                     "movl _g_mapFlagBase, %eax\n\t" // relocated array base
                     "testb $0x3f, (%eax,%ecx,1)\n\t" // original: test the tile flag bits
                     "jz 1f\n\t" // original je: flag clear -> off-map/false
                     "jmp *_g_mapEdgeContinue\n\t" // flag set -> resume the loop at +9
                     "1:\n\t"
                     "jmp *_g_mapEdgeOffmap\n\t");
}

void installMapEdgeCrashFix() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    void *site = (void *)(base + HOOK_SITE_RVA);
    uint8_t *p = (uint8_t *)site;

    // Bail if a future game build shifted this code (compare everything but the relocated
    // disp32) so we never corrupt the wrong bytes.
    if (p[0] != kOriginal[0] || p[1] != kOriginal[1] || p[6] != kOriginal[6] ||
        p[7] != kOriginal[7] || p[8] != kOriginal[8]) {
        return;
    }

    g_mapFlagBase = *(uint32_t *)(base + HOOK_SITE_RVA + ARRAY_DISP_OFF);
    g_mapEdgeContinue = base + CONTINUE_RVA;
    g_mapEdgeOffmap = base + OFFMAP_RVA;

    installHook(site, reinterpret_cast<void *>(mapEdgeGuard), sizeof(kOriginal));
}
