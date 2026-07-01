#include <windows.h>

static HMODULE s_realD3d9Instance = nullptr;
FARPROC g_d3d9Functions[5] = {nullptr};

void loadRealD3d9Dll() {
    char systemDir[MAX_PATH];
    GetSystemDirectoryA(systemDir, MAX_PATH);
    strcat_s(systemDir, MAX_PATH, "\\d3d9.dll");
    s_realD3d9Instance = LoadLibraryA(systemDir);

    if (!s_realD3d9Instance) {
        return;
    }

    g_d3d9Functions[0] = GetProcAddress(s_realD3d9Instance, "Direct3DCreate9");
    g_d3d9Functions[1] = GetProcAddress(s_realD3d9Instance, "Direct3DCreate9Ex");
    g_d3d9Functions[2] = GetProcAddress(s_realD3d9Instance, "Direct3DShaderValidatorCreate9");
    g_d3d9Functions[3] = GetProcAddress(s_realD3d9Instance, "PSGPError");
    g_d3d9Functions[4] = GetProcAddress(s_realD3d9Instance, "PSGPSampleTexture");
}

extern "C" {
__declspec(naked) void WINAPI Direct3DCreate9(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[0]));
}
__declspec(naked) void WINAPI Direct3DCreate9Ex(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[1]));
}
__declspec(naked) void WINAPI Direct3DShaderValidatorCreate9(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[2]));
}
__declspec(naked) void WINAPI PSGPError(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[3]));
}
__declspec(naked) void WINAPI PSGPSampleTexture(void) {
    __asm__("jmp *%0" : : "m"(g_d3d9Functions[4]));
}
}
