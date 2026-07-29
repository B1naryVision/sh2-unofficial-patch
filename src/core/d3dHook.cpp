#include "d3dHook.h"
#include <cstdint>
#include <cstring>
#include <d3d9.h>
#include <windows.h>

// Stronghold 2 renders through dxrenderer.dll, which copies the D3D9 device
// vtable to private memory and inline-hooks d3d9's Present/Reset — so patching
// the device vtable does nothing (the game never calls through it). d3d9's
// EndScene is left un-hooked, so we install an inline trampoline on the real
// EndScene *function* instead: that catches the scene render regardless of the
// vtable copy. The device is resolved once via a temporary CreateDevice detour
// (only to read EndScene's address out of vtable[42]).

// ── device resolution (temporary CreateDevice detour) ──────────────────────────
typedef HRESULT(WINAPI *CreateDeviceFn)(
    IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **
);

static CreateDeviceFn s_origCreateDevice = nullptr;
static bool s_endSceneHooked = false;
static HWND s_deviceWindow = nullptr;

void *d3dDeviceWindow() { return s_deviceWindow; }

static const int VT_CREATE_DEVICE = 16; // IDirect3D9::CreateDevice
static const int VT_ENDSCENE = 42; // IDirect3DDevice9::EndScene

static void *patchVtableSlot(void *comObject, int index, void *hook) {
    void **vtable = *(void ***)comObject;
    DWORD oldProtect;
    VirtualProtect(&vtable[index], sizeof(void *), PAGE_READWRITE, &oldProtect);
    void *original = vtable[index];
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void *), oldProtect, &oldProtect);
    return original;
}

// ── render callback registry ───────────────────────────────────────────────────
static D3DRenderFn s_renderCbs[4] = {};
static int s_renderCbCount = 0;

void registerD3DRender(D3DRenderFn fn) {
    if (fn && s_renderCbCount < 4) {
        s_renderCbs[s_renderCbCount++] = fn;
    }
}

// Called from the naked EndScene trampoline with the device (`this`), while the
// scene is still open. Dispatches to registered render callbacks. Callbacks must
// be float-free (no x87 arithmetic) — the interrupted EndScene may hold FP state.
extern "C" void renderOverlayC(IDirect3DDevice9 *device) {
    for (int i = 0; i < s_renderCbCount; ++i) {
        s_renderCbs[i](device);
    }
}

// ── inline trampoline hook on the real EndScene ────────────────────────────────
// Non-static so the naked asm can reference it (GAS cannot resolve local statics).
unsigned char *g_endSceneTramp = nullptr;

// Naked detour installed at EndScene's entry (reached via the JMP we write over
// the prologue). Saves state, calls the overlay with `this`, then jumps to the
// trampoline (original 7 prologue bytes + JMP to EndScene+7).
__declspec(naked) static void endSceneDetour() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "movl 40(%esp), %eax\n\t" // this (device): 32 (pushal) + 4 (pushfl) + 4 (ret)
                     "pushl %eax\n\t"
                     "call _renderOverlayC\n\t"
                     "addl $4, %esp\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "jmp *_g_endSceneTramp\n\t");
}

static void installEndSceneHook(void *endScene) {
    unsigned char *fn = (unsigned char *)endScene;

    // Expect the standard SEH prologue: push 0x14 (6a 14) + mov eax, imm32 (b8..).
    // 7 position-independent bytes; refuse anything else rather than corrupt it.
    if (!(fn[0] == 0x6a && fn[2] == 0xb8)) {
        return;
    }

    const int copyLen = 7;

    g_endSceneTramp = (unsigned char *)VirtualAlloc(
        nullptr, copyLen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
    );

    if (!g_endSceneTramp) {
        return;
    }

    memcpy(g_endSceneTramp, fn, copyLen);
    g_endSceneTramp[copyLen] = 0xE9;
    *(int32_t *)(g_endSceneTramp + copyLen + 1) =
        (int32_t)((uintptr_t)(fn + copyLen) - (uintptr_t)(g_endSceneTramp + copyLen) - 5);

    DWORD oldProtect;
    VirtualProtect(fn, copyLen, PAGE_EXECUTE_READWRITE, &oldProtect);
    fn[0] = 0xE9;
    *(int32_t *)(fn + 1) = (int32_t)((uintptr_t)&endSceneDetour - (uintptr_t)fn - 5);

    for (int i = 5; i < copyLen; ++i) {
        fn[i] = 0x90;
    }

    VirtualProtect(fn, copyLen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), fn, copyLen);
}

static HRESULT WINAPI hookCreateDevice(
    IDirect3D9 *self, UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS *params, IDirect3DDevice9 **returnedDevice
) {
    HRESULT hr =
        s_origCreateDevice(self, adapter, type, focusWindow, behaviorFlags, params, returnedDevice);

    if (SUCCEEDED(hr) && returnedDevice && *returnedDevice && !s_endSceneHooked) {
        s_endSceneHooked = true;

        if (focusWindow) {
            s_deviceWindow = focusWindow;
        } else if (params && params->hDeviceWindow) {
            s_deviceWindow = params->hDeviceWindow;
        }

        void **vtable = *(void ***)*returnedDevice;
        installEndSceneHook(vtable[VT_ENDSCENE]);
    }

    return hr;
}

void installD3DHook(IDirect3D9 *d3d) {
    static bool installed = false;

    if (installed || !d3d) {
        return;
    }

    installed = true;
    s_origCreateDevice =
        (CreateDeviceFn)patchVtableSlot(d3d, VT_CREATE_DEVICE, (void *)hookCreateDevice);
}
