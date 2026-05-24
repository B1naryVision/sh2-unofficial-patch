#include <windows.h>

static HMODULE hRealVersionInstance = nullptr;
FARPROC realFunctions[6] = {nullptr};

void LoadRealVersionDll()
{
    char systemDir[MAX_PATH];
    GetSystemDirectoryA(systemDir, MAX_PATH);
    strcat_s(systemDir, MAX_PATH, "\\version.dll");
    hRealVersionInstance = LoadLibraryA(systemDir);
    if (!hRealVersionInstance) return;

    realFunctions[0] = GetProcAddress(hRealVersionInstance, "GetFileVersionInfoA");
    realFunctions[1] = GetProcAddress(hRealVersionInstance, "GetFileVersionInfoW");
    realFunctions[2] = GetProcAddress(hRealVersionInstance, "GetFileVersionInfoSizeA");
    realFunctions[3] = GetProcAddress(hRealVersionInstance, "GetFileVersionInfoSizeW");
    realFunctions[4] = GetProcAddress(hRealVersionInstance, "VerQueryValueA");
    realFunctions[5] = GetProcAddress(hRealVersionInstance, "VerQueryValueW");
}

extern "C"
{
    __declspec(naked) WINBOOL WINAPI GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
    {
        __asm__("jmp *%0" : : "m"(realFunctions[0]));
    }
    __declspec(naked) WINBOOL WINAPI GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
    {
        __asm__("jmp *%0" : : "m"(realFunctions[1]));
    }
    __declspec(naked) DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle)
    {
        __asm__("jmp *%0" : : "m"(realFunctions[2]));
    }
    __declspec(naked) DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle)
    {
        __asm__("jmp *%0" : : "m"(realFunctions[3]));
    }
    __declspec(naked) WINBOOL WINAPI VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID *lplpBuffer, PUINT puLen)
    {
        __asm__("jmp *%0" : : "m"(realFunctions[4]));
    }
    __declspec(naked) WINBOOL WINAPI VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID *lplpBuffer, PUINT puLen)
    {
        __asm__("jmp *%0" : : "m"(realFunctions[5]));
    }
}
