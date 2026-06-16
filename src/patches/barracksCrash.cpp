#include "barracksCrash.h"
#include <cstring>
#include <windows.h>

void installBarracksCrashFix() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    DWORD oldProtect;

    // Fix null-ESI dereference in barracks UI message handler when Lord dies.
    // RVA 0x2207c6: the recruitment event handler calls 0x411360 to get the
    // barracks object; when the Lord has died that call returns NULL and ESI is
    // set to 0 before jumping here. The existing flag-check (cmp ds:0x11b8454,0)
    // was intended to guard this but has a timing gap — the flag is set after
    // the event fires — so it reads 0 and falls through into mov edi,[esi+0xd90].
    // Fix: replace the flag check with a direct ESI null test. If ESI is null,
    // jump to the function's clean exit at RVA 0x220815 (the same target the
    // original jne used); otherwise proceed normally.
    // Before: 83 3d 54 84 1b 01 00 75 46  (cmp ds:0x11b8454,0 / jne 0x620815)
    // After:  85 f6 0f 84 47 00 00 00 90  (test esi,esi / je near 0x620815 / nop)
    static const unsigned char patch[] = {0x85, 0xf6, 0x0f, 0x84, 0x47, 0x00, 0x00, 0x00, 0x90};
    void *site = (void *)(base + 0x2207c6);
    VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(site, patch, sizeof(patch));
    VirtualProtect(site, sizeof(patch), oldProtect, &oldProtect);
}
