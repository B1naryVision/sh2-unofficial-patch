#include "introSkip.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

void installIntroSkip() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    DWORD oldProtect;

    void *counterInit = (void *)(base + 0x4DA9F8);
    static const uint8_t counterVal[4] = {0x02, 0x00, 0x00, 0x00};
    VirtualProtect(counterInit, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(counterInit, counterVal, 4);
    VirtualProtect(counterInit, 4, oldProtect, &oldProtect);

    void *binkOpenCall = (void *)(base + 0x27BB0D);
    static const uint8_t skipBytes[5] = {0x83, 0xC4, 0x18, 0x90, 0x90};
    VirtualProtect(binkOpenCall, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(binkOpenCall, skipBytes, 5);
    VirtualProtect(binkOpenCall, 5, oldProtect, &oldProtect);
}
