#include "unitCapRaise.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

void installUnitCapRaise() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    DWORD oldProtect;
    static const uint8_t movEax550[5] = {0xB8, 0x26, 0x02, 0x00, 0x00};

    static const uintptr_t patchRvas[3] = {0x16827, 0x18768, 0x189FB};

    for (int i = 0; i < 3; i++) {
        void *addr = (void *)(base + patchRvas[i]);
        VirtualProtect(addr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(addr, movEax550, 5);
        VirtualProtect(addr, 5, oldProtect, &oldProtect);
    }
}
