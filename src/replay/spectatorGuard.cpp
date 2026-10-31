#include "spectatorGuard.h"

#include "../core/d3dHook.h"
#include "../core/exeBase.h"
#include "../core/hotkey.h"
#include "../patches/endgameStats/killTracker.h"
#include "replaySession.h"
#include "replayViewer.h"

#include <windows.h>

static volatile LONG s_active = 0;
static WNDPROC s_origWndProc = nullptr;

typedef SHORT(WINAPI *GetAsyncKeyStateFn)(int);
static GetAsyncKeyStateFn s_realGetAsyncKeyState = nullptr;

// Import slots currently redirected to guardGetAsyncKeyState, for the restore.
static const int MAX_POLL_SLOTS = 16;
static void **s_pollSlots[MAX_POLL_SLOTS];
static int s_pollSlotCount = 0;

bool spectatorGuardActive() { return s_active != 0; }

// Camera and inspection only. Everything a pane needs to turn into a Chore
// goes through a mouse button or a key, so those are the two families that are
// blocked; movement, wheel and the scroll/arrow keys are left alone so the
// viewer can still look around a replaying match.
//
// W/A/S/D, Q/E, Space and Tab are let through too. The game reads
// WASD pan and Q/E rotate as held bits of Dragonfly::EventMgr::ms_keyStateSet
// in the camera sampler FUN_005fe010 (word +8 bits 87/65/83/68 and 81/69).
// None of the eight is a case of the in-game key-event handler FUN_005f2510,
// whose cases (digits = group recall -> SelectTroopsChore in MP, B C G J-P R
// T V X Z, Esc, F-key range -> InsultChore in MP) stay blocked. Space and Tab
// are dispatched as key events whose Dragonfly code was not pinned statically;
// the admission hook refusing UI-origin commands remains the backstop.
static bool isCameraKey(WPARAM vk) {
    switch (vk) {
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT:
    case 'W':
    case 'A':
    case 'S':
    case 'D':
    case 'Q':
    case 'E':
    case VK_SPACE:
    case VK_TAB:
    // ESC opens the stock in-game menu (FUN_005f2510 case 0x10b), the way out
    // of a replay. The viewer pauses while it is up and lets its clicks through.
    case VK_ESCAPE:
    // The middle button turns and tilts the free-view camera (the camera
    // panel's eye button; camera sampler FUN_005fe010, button bit 2 of
    // ms_keyStateSet word +0x20). No pane acts on it.
    case VK_MBUTTON:
        return true;
    default:
        return false;
    }
}

static bool blocksMessage(UINT msg, WPARAM wparam) {
    switch (msg) {
    // Mouse buttons reach the game only while an in-match popup (the ESC menu)
    // is up, so its buttons work. The middle button is camera input (see
    // isCameraKey) and always passes.
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        return !replayViewerMenuOpen();
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        return !isCameraKey(wparam);
    // A character code is not a virtual-key code ('%' is VK_LEFT, 'W' is a
    // shifted w), and the camera keys act as key events, so no character is
    // camera input.
    case WM_CHAR:
    case WM_SYSCHAR:
        return true;
    default:
        return false;
    }
}

static LRESULT CALLBACK spectatorWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // The last sidecar write of a recording closed mid-match. Before the stock
    // handler, which may exit the process from inside WM_CLOSE. The session
    // writes it once, whichever of these arrives first.
    if (msg == WM_CLOSE || msg == WM_DESTROY || (msg == WM_ENDSESSION && wparam)) {
        replaySessionOnWindowClosing();
    }

    // The replay viewer sees input first: its toggle key and clicks on its
    // own panel are consumed there and never reach the game.
    if (s_active && replayViewerHandleMessage(hwnd, msg, wparam, lparam)) {
        return 0;
    }

    if (s_active && blocksMessage(msg, wparam)) {
        return 0;
    }

    return CallWindowProcW(s_origWndProc, hwnd, msg, wparam, lparam);
}

// The window queue is not the only way this game reads input. Four functions
// poll GetAsyncKeyState directly -- the hotkey dispatcher FUN_006af0d0
// (0x2af0d0..0x2af607), the mouse/camera sampler FUN_005fe010, the engineer
// action helper FUN_00631320, and an environment-FX debug key in FUN_005fb0f0
// -- and the first of those reaches the posters for MoveTroopsChore,
// PlaceBuildingChore, GameGroundFeatureChore and PlaceTowerEquipmentChore. A
// window subclass cannot touch a polled key state, so the import is redirected
// instead. GetAsyncKeyState is the whole direct-device surface: the executable
// imports no GetKeyState, GetKeyboardState, raw input or DirectInput.
//
// The real import is still called before the result is dropped. GetAsyncKeyState
// reports "pressed since the last call" in bit 0, so skipping it would let that
// bit accumulate and fire a stale press the moment playback stops.
static SHORT WINAPI guardGetAsyncKeyState(int vk) {
    SHORT state = s_realGetAsyncKeyState(vk);

    if (s_active && !isCameraKey((WPARAM)vk)) {
        return 0;
    }

    return state;
}

static void writeSlot(void **slot, void *value) {
    DWORD old = 0;

    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        return;
    }

    *slot = value;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

// Redirects every GetAsyncKeyState slot in `module`'s import table. Matched by
// the value already in the slot rather than by name, so a bound or
// delay-loaded import cannot be patched by mistake and a slot another hook
// already owns is left alone.
static void redirectImports(HMODULE module) {
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }

    IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (!dir.VirtualAddress || !dir.Size) {
        return;
    }

    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
         imp->Name; ++imp) {
        if (!imp->FirstThunk) {
            continue;
        }

        for (void **thunk = (void **)(base + imp->FirstThunk); *thunk; ++thunk) {
            if (*thunk != (void *)s_realGetAsyncKeyState || s_pollSlotCount == MAX_POLL_SLOTS) {
                continue;
            }

            writeSlot(thunk, (void *)&guardGetAsyncKeyState);
            s_pollSlots[s_pollSlotCount++] = thunk;
        }
    }
}

// Only the two modules that read input for the game, the executable and
// dxrenderer.dll (which owns the window and the message pump), and only while
// a replay plays: anything else in the process -- the Steam overlay, ReShade,
// a D3D wrapper -- keeps its own hotkeys. Both are loaded long before a
// playback starts; dxrenderer.dll is pinned for the walk regardless.
static void redirectPolls() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    s_realGetAsyncKeyState =
        user32 ? (GetAsyncKeyStateFn)(void *)GetProcAddress(user32, "GetAsyncKeyState") : nullptr;

    if (!s_realGetAsyncKeyState) {
        return;
    }

    redirectImports((HMODULE)exeBase());
    HMODULE renderer = nullptr;

    if (GetModuleHandleExW(0, L"dxrenderer.dll", &renderer)) {
        redirectImports(renderer);
        FreeLibrary(renderer);
    }
}

static void restorePolls() {
    for (int i = 0; i < s_pollSlotCount; ++i) {
        if (*s_pollSlots[i] == (void *)&guardGetAsyncKeyState) {
            writeSlot(s_pollSlots[i], (void *)s_realGetAsyncKeyState);
        }
    }

    s_pollSlotCount = 0;
}

void spectatorGuardSetActive(uint32_t active) {
    const bool on = active != 0;

    if (on == (s_active != 0)) {
        return;
    }

    if (on) {
        redirectPolls();
        InterlockedExchange(&s_active, 1);
    } else {
        InterlockedExchange(&s_active, 0);
        restorePolls();
    }

    killTrackerSetActive(on);
}

// Subclassing needs the device window, which does not exist at install time.
// The render callback is the same trigger the overlays use for this.
static void ensureSubclass(IDirect3DDevice9 *) {
    if (s_origWndProc) {
        return;
    }

    HWND hwnd = (HWND)d3dDeviceWindow();

    if (!hwnd) {
        return;
    }

    s_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)spectatorWndProc);
}

void installSpectatorGuard() {
    hotkeySetSuppressor(spectatorGuardActive);
    registerD3DRender(ensureSubclass);
}
