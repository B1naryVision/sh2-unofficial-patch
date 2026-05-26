#include "mpAiEnable.h"
#include <windows.h>
#include <cstring>

void installMpAiEnable() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    void *patchSite = (void *)(base + 0x2A0F69);

    DWORD oldProtect;
    VirtualProtect(patchSite, 14, PAGE_EXECUTE_READWRITE, &oldProtect);
    memset(patchSite, 0x90, 14);
    VirtualProtect(patchSite, 14, oldProtect, &oldProtect);
}
