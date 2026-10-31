#include "spectatorGuard.h"

#include "replayViewer.h"
#include "../core/d3dHook.h"

#include <windows.h>

static volatile LONG s_active = 0;
static volatile LONG s_localCommands = 0;
static volatile LONG s_blockedMessages = 0;
static volatile LONG s_blockedPolls = 0;
static WNDPROC s_origWndProc = nullptr;

typedef SHORT(WINAPI *GetAsyncKeyStateFn)(int);
static GetAsyncKeyStateFn s_realGetAsyncKeyState = nullptr;
static volatile LONG s_pollThunks = 0;

bool spectatorGuardActive() {
    return s_active != 0;
}

void spectatorGuardSetActive(uint32_t active) {
    InterlockedExchange(&s_active, active ? 1 : 0);
}

void spectatorGuardNoteLocalCommand() {
    InterlockedIncrement(&s_localCommands);
}

uint32_t spectatorGuardLocalCommands() {
    return (uint32_t)s_localCommands;
}

uint32_t spectatorGuardBlockedMessages() {
    return (uint32_t)s_blockedMessages;
}

uint32_t spectatorGuardBlockedPolls() {
    return (uint32_t)s_blockedPolls;
}

bool spectatorGuardSuppressOwnPoll() {
    if (!s_active) {
        return false;
    }
    InterlockedIncrement(&s_blockedPolls);
    return true;
}

uint32_t spectatorGuardPollThunks() {
    return (uint32_t)s_pollThunks;
}

bool spectatorGuardHooked() {
    return s_origWndProc != nullptr;
}

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
// the admission-side proof counter remains the backstop.
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
    // is up, so its buttons work. The proof counter still covers this path.
    // The middle button is camera input (see isCameraKey) and always passes.
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
    case WM_CHAR:
    case WM_SYSCHAR:
        return !isCameraKey(wparam);
    default:
        return false;
    }
}

static LRESULT CALLBACK spectatorWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam) {
    // The replay viewer sees input first: its toggle key and clicks on its
    // own panel are consumed there and never reach the game.
    if (s_active && replayViewerHandleMessage(hwnd, msg, wparam, lparam)) {
        return 0;
    }
    if (s_active && blocksMessage(msg, wparam)) {
        InterlockedIncrement(&s_blockedMessages);
        return 0;
    }
    return CallWindowProcW(s_origWndProc, hwnd, msg, wparam, lparam);
}

// The window queue is not the only way this game reads input. Four functions
// poll GetAsyncKeyState directly -- the hotkey dispatcher FUN_006af0d0
// (0x2af0d0..0x2af607), the mouse/camera sampler FUN_005fe010, the engineer
// action helper FUN_00631320, and an environment-FX debug key in FUN_005fb0f0
// -- and the first of those reaches the posters for MoveTroopsChore,
// PlaceBuildingChore, GameGroundFeatureChore and PlaceTowerEquipmentChore.
// A window subclass cannot touch a polled key state, so the executable's import
// thunk is redirected instead. GetAsyncKeyState is the whole direct-device
// surface: the executable imports no GetKeyState, GetKeyboardState, raw input
// or DirectInput. GetCursorPos is imported but yields position only, never a
// button, so it is deliberately left alone and the camera still follows the
// mouse.
//
// The real import is still called before the result is dropped. GetAsyncKeyState
// reports "pressed since the last call" in bit 0, so skipping it would let that
// bit accumulate and fire a stale press the moment playback stops.
static SHORT WINAPI guardGetAsyncKeyState(int vk) {
    SHORT state = s_realGetAsyncKeyState(vk);
    if (s_active && !isCameraKey((WPARAM)vk)) {
        if (state) {
            InterlockedIncrement(&s_blockedPolls);
        }
        return 0;
    }
    return state;
}

// Redirect the GetAsyncKeyState import thunk of every loaded module.
//
// The executable is not the only importer: dxrenderer.dll, which owns the
// window and the message pump, polls it too. Modules are walked through the
// PEB loader list rather than EnumProcessModules so the import gate stays at
// its current 15 DLLs. This module is skipped deliberately -- our own hotkey
// polls are suppressed at their own chokepoint in hotkeyHeld(), where the key
// is already known to be held and the count stays meaningful.
//
// Idempotent, and re-run every frame: a module loaded later is picked up on
// the next pass, and a slot already holding our thunk is left alone.
static void patchModuleImports(BYTE *base, GetAsyncKeyStateFn real) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    IMAGE_DATA_DIRECTORY &dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) {
        return;
    }

    IMAGE_IMPORT_DESCRIPTOR *imp =
        (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        if (!imp->FirstThunk) {
            continue;
        }
        void **thunk = (void **)(base + imp->FirstThunk);
        for (; *thunk; ++thunk) {
            // Matched by the value already in the slot rather than by name, so
            // a bound or delay-loaded import cannot be patched by mistake and
            // the slot is left alone unless it holds exactly the USER32 export.
            if (*thunk != (void *)real) {
                continue;
            }
            DWORD old = 0;
            if (!VirtualProtect(thunk, sizeof(void *), PAGE_READWRITE, &old)) {
                continue;
            }
            *thunk = (void *)&guardGetAsyncKeyState;
            VirtualProtect(thunk, sizeof(void *), old, &old);
            FlushInstructionCache(GetCurrentProcess(), thunk, sizeof(void *));
            InterlockedIncrement(&s_pollThunks);
        }
    }
}

// 32-bit PEB walk. PEB at fs:[0x30]; PEB+0x0c is PEB_LDR_DATA; LDR+0x14 is the
// InMemoryOrderModuleList head, whose links sit 8 bytes into each
// LDR_DATA_TABLE_ENTRY, and DllBase is at +0x18 of that entry.
static void ensurePollHook() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    GetAsyncKeyStateFn real =
        (GetAsyncKeyStateFn)(void *)GetProcAddress(user32, "GetAsyncKeyState");
    if (!real) {
        return;
    }
    s_realGetAsyncKeyState = real;

    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&guardGetAsyncKeyState, &self)) {
        return;
    }

    BYTE *peb = nullptr;
    __asm__ __volatile__("movl %%fs:0x30, %0" : "=r"(peb));
    if (!peb) {
        return;
    }
    BYTE *ldr = *(BYTE **)(peb + 0x0c);
    if (!ldr) {
        return;
    }
    LIST_ENTRY *head = (LIST_ENTRY *)(ldr + 0x14);
    for (LIST_ENTRY *cur = head->Flink; cur && cur != head; cur = cur->Flink) {
        BYTE *entry = (BYTE *)cur - 8;
        BYTE *dllBase = *(BYTE **)(entry + 0x18);
        if (!dllBase || dllBase == (BYTE *)self) {
            continue;
        }
        patchModuleImports(dllBase, real);
    }
}

// Subclassing needs the device window, which does not exist at install time.
// The render callback is the same trigger the overlays use for this.
static void ensureSubclass(IDirect3DDevice9 *) {
    ensurePollHook();
    if (s_origWndProc) {
        return;
    }
    HWND hwnd = (HWND)d3dDeviceWindow();
    if (!hwnd) {
        return;
    }
    s_origWndProc =
        (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)spectatorWndProc);
}

void installSpectatorGuard() {
    registerD3DRender(ensureSubclass);
}
