#include <windows.h>
#include "proxy/version_proxy.h"
#include "patches/registry.h"
#include "core/log.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        LoadRealVersionDll();
        ApplyUnofficialPatches();
    }
#ifdef DEBUG
    else if (ul_reason_for_call == DLL_PROCESS_DETACH)
    {
        Log_Flush("patch_debug.txt");
    }
#endif
    return TRUE;
}
