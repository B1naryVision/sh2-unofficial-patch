#include "mpConnectCompleteCrash.h"
#include <cstring>
#include <windows.h>

void installMpConnectCompleteCrashFix() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    DWORD oldProtect;

    // Redirect null-peer je in handleConnectCompleteMessage to function return.
    // RVA 0x3d85c6: je 0x7d869a (6 bytes)
    // When findPeer() returns null, the error-log path at 0x7d869a calls sprintf
    // then does `add esi,0x18` (esi=0 → esi=0x18), then reads [esi+0x14] from
    // address 0x2C → access violation. Skip the error log; return cleanly instead.
    // Before: 0f 84 ce 00 00 00  (je → error log path)
    // After:  0f 84 32 01 00 00  (je → function epilogue at RVA 0x3d86fe)
    static const unsigned char patch[] = {0x0f, 0x84, 0x32, 0x01, 0x00, 0x00};
    void *site = (void *)(base + 0x3d85c6);
    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(site, patch, 6);
    VirtualProtect(site, 6, oldProtect, &oldProtect);
}
