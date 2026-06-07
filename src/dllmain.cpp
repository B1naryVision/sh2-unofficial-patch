#include "core/log.h"
#include "patches/registry.h"
#include "proxy/versionProxy.h"
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        loadRealVersionDll();
        applyUnofficialPatches();
    }
#ifdef DEBUG
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        logFlush("patch_debug.txt");
    }
#endif
    return TRUE;
}
