#include "../core/d3dHook.h"
#include "../core/hook.h"
#include <cstring>
#include <windows.h>

// Index order must match the naked stubs below and d3d9.def. The set is
// limited to exports present in every d3d9.dll the game can run against
// (Windows 7+, WineD3D, DXVK); Win10-only exports such as
// Direct3DCreate9On12 are deliberately not proxied so we never export a
// name the underlying DLL might lack.
static const char *const kExportNames[] = {
    "Direct3DCreate9",   "Direct3DCreate9Ex",  "Direct3DShaderValidatorCreate9",
    "PSGPError",         "PSGPSampleTexture",  "D3DPERF_BeginEvent",
    "D3DPERF_EndEvent",  "D3DPERF_GetStatus",  "D3DPERF_QueryRepeatFrame",
    "D3DPERF_SetMarker", "D3DPERF_SetOptions", "D3DPERF_SetRegion",
    "DebugSetMute",
};

static const size_t kExportCount = sizeof(kExportNames) / sizeof(kExportNames[0]);

static HMODULE s_chainedD3d9Instance = nullptr;
static HMODULE s_systemD3d9Instance = nullptr;
FARPROC g_d3d9Functions[kExportCount] = {nullptr};

static const char kChainFilename[] = "d3d9_chain.dll";

static void reportLoadFailure(const char *message) {
    MessageBoxA(NULL, message, "SH2 Unofficial Patch", MB_OK | MB_ICONERROR);
}

// A D3D9 wrapper (DXVK, ReShade, dgVoodoo) renamed to d3d9_chain.dll next to
// us takes the place of the system library. See docs/features/dll-chaining.md.
static HMODULE loadChainedD3d9() {
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(g_patchModule, path, MAX_PATH);

    if (len == 0 || len >= MAX_PATH) {
        return nullptr;
    }

    char *lastSlash = strrchr(path, '\\');

    if (!lastSlash || (size_t)(lastSlash - path) + sizeof(kChainFilename) + 1 > MAX_PATH) {
        return nullptr;
    }

    strcpy(lastSlash + 1, kChainFilename);

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        return nullptr;
    }

    HMODULE module = LoadLibraryA(path);

    if (!module) {
        reportLoadFailure(
            "SH2 Unofficial Patch: d3d9_chain.dll is in the game folder but could not\n"
            "be loaded. Falling back to the system d3d9.dll."
        );
    }

    return module;
}

static HMODULE loadSystemD3d9() {
    char path[MAX_PATH];
    UINT len = GetSystemDirectoryA(path, MAX_PATH);

    if (len == 0 || len >= MAX_PATH - sizeof("\\d3d9.dll")) {
        return nullptr;
    }

    strcat_s(path, MAX_PATH, "\\d3d9.dll");
    return LoadLibraryA(path);
}

void loadRealD3d9Dll() {
    s_chainedD3d9Instance = loadChainedD3d9();

    // Every export but Direct3DCreate9 is a bare jmp through its slot, so a
    // null slot is a jump to address 0. A wrapper only implements what games
    // call; the system library fills in whatever it lacks.
    bool missing = true;

    if (s_chainedD3d9Instance) {
        missing = false;

        for (size_t i = 0; i < kExportCount; ++i) {
            g_d3d9Functions[i] = GetProcAddress(s_chainedD3d9Instance, kExportNames[i]);

            if (!g_d3d9Functions[i]) {
                missing = true;
            }
        }
    }

    if (!missing) {
        return;
    }

    s_systemD3d9Instance = loadSystemD3d9();

    if (!s_systemD3d9Instance) {
        reportLoadFailure(
            "SH2 Unofficial Patch: failed to load the system d3d9.dll.\n"
            "The patch is disabled and the game will likely fail to start."
        );
        return;
    }

    for (size_t i = 0; i < kExportCount; ++i) {
        if (!g_d3d9Functions[i]) {
            g_d3d9Functions[i] = GetProcAddress(s_systemD3d9Instance, kExportNames[i]);
        }
    }
}

extern "C" {
// Direct3DCreate9 is the one export we do more than forward: we call the real
// implementation, then hand the IDirect3D9 to the device hook so it can detour
// EndScene/Reset for the overlay. Everything else is a plain jmp forwarder.
IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion) {
    typedef IDirect3D9 *(WINAPI * Direct3DCreate9Fn)(UINT);

    // memcpy rather than a cast: FARPROC and the real signature are incompatible
    // function-pointer types, and a direct cast trips -Wcast-function-type.
    Direct3DCreate9Fn real;
    memcpy(&real, &g_d3d9Functions[0], sizeof(real));

    if (!real) {
        return nullptr;
    }

    IDirect3D9 *d3d = real(SDKVersion);

    if (d3d) {
        installD3DHook(d3d);
    }

    return d3d;
}
__declspec(naked) void WINAPI Direct3DCreate9Ex(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[1]));
}
__declspec(naked) void WINAPI Direct3DShaderValidatorCreate9(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[2]));
}
__declspec(naked) void WINAPI PSGPError(void) { __asm__("jmp *%0" : : "m"(g_d3d9Functions[3])); }
__declspec(naked) void WINAPI PSGPSampleTexture(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[4]));
}
__declspec(naked) void WINAPI D3DPERF_BeginEvent(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[5]));
}
__declspec(naked) void WINAPI D3DPERF_EndEvent(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[6]));
}
__declspec(naked) void WINAPI D3DPERF_GetStatus(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[7]));
}
__declspec(naked) void WINAPI D3DPERF_QueryRepeatFrame(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[8]));
}
__declspec(naked) void WINAPI D3DPERF_SetMarker(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[9]));
}
__declspec(naked) void WINAPI D3DPERF_SetOptions(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[10]));
}
__declspec(naked) void WINAPI D3DPERF_SetRegion(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[11]));
}
__declspec(naked) void WINAPI DebugSetMute(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[12]));
}
}
