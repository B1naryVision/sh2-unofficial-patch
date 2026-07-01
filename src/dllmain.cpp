#include "core/log.h"
#include "patches/registry.h"
#include "proxy/d3d9Proxy.h"
#include <windows.h>

static DWORD WINAPI patchThread(LPVOID) {
    applyUnofficialPatches();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        loadRealD3d9Dll();
        HANDLE hThread = CreateThread(nullptr, 0, patchThread, nullptr, 0, nullptr);

        if (hThread) {
            CloseHandle(hThread);
        }
    }
#ifdef DEBUG
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        logFlush("patch_debug.txt");
    }
#endif
    return TRUE;
}
