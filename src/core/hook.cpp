#include "hook.h"

void installHook(void *targetAddress, void *detourFunction, size_t instructionLength) {
    DWORD oldProtect;
    VirtualProtect(targetAddress, instructionLength, PAGE_EXECUTE_READWRITE, &oldProtect);

    uintptr_t relativeAddress = ((uintptr_t)detourFunction - (uintptr_t)targetAddress) - 5;
    *(unsigned char *)targetAddress = 0xE9;
    *(uintptr_t *)((uintptr_t)targetAddress + 1) = relativeAddress;

    for (size_t i = 5; i < instructionLength; ++i) {
        *(unsigned char *)((uintptr_t)targetAddress + i) = 0x90;
    }

    VirtualProtect(targetAddress, instructionLength, oldProtect, &oldProtect);
}
