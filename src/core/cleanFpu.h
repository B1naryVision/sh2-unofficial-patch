#pragma once
#include <cstdint>

// Runs `fn` with an empty x87 stack, restoring the interrupted FPU/SSE state after.
// For mid-function hooks (the frame tick) that must call code which uses floats -
// engine functions, steam_api. See CLAUDE.md "Mid-function hooks that must call
// float-using engine code".
template <typename Fn> inline void cleanFpu(Fn fn) {
    // fxsave faults unless its destination is 16-byte aligned, and nothing guarantees
    // the alignment of a stack entered from the game's main loop.
    uint8_t buffer[512 + 16];
    uint8_t *state = (uint8_t *)(((uintptr_t)buffer + 15) & ~(uintptr_t)15);

    __asm__ volatile("fxsave (%0)" : : "r"(state) : "memory");
    __asm__ volatile("fninit");

    fn();

    __asm__ volatile("fxrstor (%0)" : : "r"(state) : "memory");
}
