#include "introSkip.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

#include <mmsystem.h>

static const uintptr_t COUNTER_INIT_RVA = 0x4DA9F8;
static const uintptr_t VIDEO_OPEN_CALL_RVA = 0x27BB0D;
static const uintptr_t VIDEO_OPEN_RVA = 0x3FEA30;

void installIntroSkip() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint8_t *counterInit = (uint8_t *)(base + COUNTER_INIT_RVA);
    uint8_t *videoOpenCall = (uint8_t *)(base + VIDEO_OPEN_CALL_RVA);

    static const uint8_t counterStock[4] = {0x00, 0x00, 0x00, 0x00};
    static const uint8_t counterVal[4] = {0x02, 0x00, 0x00, 0x00};

    uint8_t callStock[5] = {0xE8};
    int32_t rel = (int32_t)(VIDEO_OPEN_RVA - (VIDEO_OPEN_CALL_RVA + 5));
    memcpy(callStock + 1, &rel, sizeof(rel));
    static const uint8_t skipBytes[5] = {0x83, 0xC4, 0x18, 0x90, 0x90};

    if (memcmp(counterInit, counterStock, sizeof(counterStock)) != 0 ||
        memcmp(videoOpenCall, callStock, sizeof(callStock)) != 0) {
        return;
    }

    DWORD oldProtect;

    VirtualProtect(counterInit, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(counterInit, counterVal, 4);
    VirtualProtect(counterInit, 4, oldProtect, &oldProtect);

    VirtualProtect(videoOpenCall, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(videoOpenCall, skipBytes, 5);
    VirtualProtect(videoOpenCall, 5, oldProtect, &oldProtect);

    // Bink's audio thread leaves a 1 ms timer request behind for the whole
    // session when the intro plays; the tick pacer's Sleep() depends on it.
    timeBeginPeriod(1);
}
