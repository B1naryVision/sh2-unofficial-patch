#include "zoomSpeed.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Triples the mouse-wheel (and keyboard) camera zoom speed.
// Patch details, offsets, and the reverse-engineering trail live in
// docs/features/zoom-speed.md.

static const uint8_t kOriginal[15] = {0xd9, 0x45, 0x08, 0xd8, 0x41, 0x20, 0xd9, 0x5d,
                                      0x08, 0xd9, 0x45, 0x08, 0xd9, 0x51, 0x20};

static const uint8_t kPatched[15] = {0xd9, 0x45, 0x08, // fld  [ebp+8]      (delta)
                                     0xd8, 0xc0, // fadd st, st(0)   (2*delta)
                                     0xd8, 0x45, 0x08, // fadd [ebp+8]     (3*delta)
                                     0xd8, 0x41, 0x20, // fadd [ecx+0x20]  (+ distance)
                                     0xd9, 0x51, 0x20, // fst  [ecx+0x20]  (store distance)
                                     0x90};

void installZoomSpeed() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    void *patchSite = (void *)(base + 0x3623D8);

    // Only patch if the stock bytes are present, so a future game update that
    // shifts this code cannot be silently corrupted.
    if (memcmp(patchSite, kOriginal, sizeof(kOriginal)) != 0) {
        return;
    }

    DWORD oldProtect;
    VirtualProtect(patchSite, sizeof(kPatched), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(patchSite, kPatched, sizeof(kPatched));
    VirtualProtect(patchSite, sizeof(kPatched), oldProtect, &oldProtect);
}
