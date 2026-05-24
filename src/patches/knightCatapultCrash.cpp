/**
 * [BUG-001] Knight/Catapult Mount Crash
 *
 * Hook 1 — base+0x39031F: CALL EDX + MOV [ESI+10],EAX (5 bytes)
 * Hook 2 — base+0x1048BB: MOV BYTE PTR [ECX+300],1   (7 bytes)
 *
 * Symptom: Crash when a knight is struck by a catapult while mounting a horse.
 * Cause:   Sub-object at [ESI+0x440] (ECX) is zeroed during partial destruction;
 *          the game's own validity check at CD5520 fires too early to catch this window.
 * Fix:     Hook 1 logs EDX for diagnostics. Hook 2 null-guards ECX before the write.
 */
#include "knightCatapultCrash.h"
#include "../core/hook.h"
#include "../core/log.h"
#include <windows.h>

uintptr_t g_targetRegisterContext = 0;

uintptr_t s_returnAddress = 0;
uintptr_t s_crashSiteReturn = 0;
uintptr_t s_crashSiteSkip = 0;

extern "C" void logEngineState() {
    logPushContext(g_targetRegisterContext);
}

__declspec(naked) static void gameLoopHook() {
    __asm__ volatile(
        "pushal\n\t"
        "movl %edx, _g_targetRegisterContext\n\t"
        "call _logEngineState\n\t"
        "popal\n\t"
        "call *%edx\n\t"
        "movl %eax, 0x10(%esi)\n\t"
        "jmp *_s_returnAddress\n\t");
}

__declspec(naked) static void nullGuardHook() {
    __asm__ volatile(
        "testl %ecx, %ecx\n\t"
        "je .LNullGuardSkip\n\t"
        "movb $1, 0x300(%ecx)\n\t"
        "jmp *_s_crashSiteReturn\n\t"
        ".LNullGuardSkip:\n\t"
        "jmp *_s_crashSiteSkip\n\t");
}

void installKnightCatapultCrashFix() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    uintptr_t hook1Site = base + 0x39031F;
    s_returnAddress = hook1Site + 5;
    installHook((void *)hook1Site, reinterpret_cast<void *>(gameLoopHook), 5);

    uintptr_t hook2Site = base + 0x1048BB;
    s_crashSiteReturn = hook2Site + 7;
    s_crashSiteSkip = base + 0x104C58;
    installHook((void *)hook2Site, reinterpret_cast<void *>(nullGuardHook), 7);
}
