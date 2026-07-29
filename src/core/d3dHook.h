#pragma once

// Direct3D9 device hook. The patch DLL is itself the d3d9 proxy, so the
// Direct3DCreate9 wrapper hands the freshly created IDirect3D9 here; we detour
// its CreateDevice vtable slot, and on the first device creation detour the
// device's EndScene and Reset slots. This gives an every-frame render hook and
// a device-reset notification without wrapping any COM interface.
//
// See docs/features/auto-market.md (overlay section).

struct IDirect3D9;
struct IDirect3DDevice9;

// Called from the Direct3DCreate9 proxy wrapper with the real IDirect3D9.
void installD3DHook(IDirect3D9 *d3d);

// Registers a callback invoked every frame from inside the game's EndScene
// (scene still open, so it may draw). Runs on the render thread. Register
// before the device is created (i.e. from an install function in DllMain).
typedef void (*D3DRenderFn)(IDirect3DDevice9 *device);
void registerD3DRender(D3DRenderFn fn);

// The device's focus/window handle (HWND as void*), captured at CreateDevice.
// NULL until the device is created. Used by the overlay to subclass the window
// for keyboard input.
void *d3dDeviceWindow();
