#include <windows.h>

static HMODULE s_realVersionInstance = nullptr;
FARPROC g_realFunctions[6] = {nullptr};

void loadRealVersionDll() {
    char systemDir[MAX_PATH];
    GetSystemDirectoryA(systemDir, MAX_PATH);
    strcat_s(systemDir, MAX_PATH, "\\version.dll");
    s_realVersionInstance = LoadLibraryA(systemDir);

    if (!s_realVersionInstance) {
        return;
    }

    g_realFunctions[0] = GetProcAddress(s_realVersionInstance, "GetFileVersionInfoA");
    g_realFunctions[1] = GetProcAddress(s_realVersionInstance, "GetFileVersionInfoW");
    g_realFunctions[2] = GetProcAddress(s_realVersionInstance, "GetFileVersionInfoSizeA");
    g_realFunctions[3] = GetProcAddress(s_realVersionInstance, "GetFileVersionInfoSizeW");
    g_realFunctions[4] = GetProcAddress(s_realVersionInstance, "VerQueryValueA");
    g_realFunctions[5] = GetProcAddress(s_realVersionInstance, "VerQueryValueW");
}

extern "C" {
__declspec(naked) WINBOOL WINAPI GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle,
                                                     DWORD dwLen, LPVOID lpData) {
    __asm__("jmp *%0" : : "m"(g_realFunctions[0]));
}
__declspec(naked) WINBOOL WINAPI GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle,
                                                     DWORD dwLen, LPVOID lpData) {
    __asm__("jmp *%0" : : "m"(g_realFunctions[1]));
}
__declspec(naked) DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle) {
    __asm__("jmp *%0" : : "m"(g_realFunctions[2]));
}
__declspec(naked) DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle) {
    __asm__("jmp *%0" : : "m"(g_realFunctions[3]));
}
__declspec(naked) WINBOOL WINAPI VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock,
                                                LPVOID *lplpBuffer, PUINT puLen) {
    __asm__("jmp *%0" : : "m"(g_realFunctions[4]));
}
__declspec(naked) WINBOOL WINAPI VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock,
                                                LPVOID *lplpBuffer, PUINT puLen) {
    __asm__("jmp *%0" : : "m"(g_realFunctions[5]));
}
}
