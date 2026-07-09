#include "hook.h"
#include <cstdint>

void installHook(void *targetAddress, void *detourFunction, size_t instructionLength) {
    DWORD oldProtect;

    if (!VirtualProtect(targetAddress, instructionLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }

    int32_t displacement = (int32_t)((uintptr_t)detourFunction - (uintptr_t)targetAddress - 5);
    *(unsigned char *)targetAddress = 0xE9;
    *(int32_t *)((uintptr_t)targetAddress + 1) = displacement;

    for (size_t i = 5; i < instructionLength; ++i) {
        *(unsigned char *)((uintptr_t)targetAddress + i) = 0x90;
    }

    VirtualProtect(targetAddress, instructionLength, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), targetAddress, instructionLength);
}
