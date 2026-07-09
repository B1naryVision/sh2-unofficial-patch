#include "core/hook.h"
#include "core/log.h"
#include "patches/registry.h"
#include "proxy/d3d9Proxy.h"
#include <windows.h>

HMODULE g_patchModule = nullptr;

// Patches are applied synchronously here rather than on a worker thread.
// d3d9.dll is a static import of Stronghold2.exe, so this runs before the
// game's entry point — no game code can execute a patch site mid-write.
// Every install function uses only loader-lock-safe calls (GetModuleHandleA,
// VirtualProtect, plain memory writes).
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_patchModule = hModule;
        DisableThreadLibraryCalls(hModule);
        loadRealD3d9Dll();
        applyUnofficialPatches();
    }
#ifdef DEBUG
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        logFlush("patch_debug.txt");
    }
#endif
    return TRUE;
}
