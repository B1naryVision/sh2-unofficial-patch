#include "keybindWidget.h"
#include <cstring>

// Drawing matches the auto-market editor's palette so a settings panel using
// both looks like one control set. Text goes through TextOutW like the other
// overlays; no float arithmetic anywhere, since the host draws from inside the
// EndScene detour (see docs/features/auto-market.md).

static const COLORREF COL_IDLE_FILL = RGB(40, 44, 58);
static const COLORREF COL_IDLE_EDGE = RGB(90, 90, 110);
static const COLORREF COL_IDLE_TEXT = RGB(225, 225, 230);
static const COLORREF COL_LISTEN_FILL = RGB(64, 78, 116);
static const COLORREF COL_LISTEN_EDGE = RGB(255, 205, 70);
static const COLORREF COL_LISTEN_TEXT = RGB(255, 235, 150);
static const COLORREF COL_UNBOUND_TEXT = RGB(130, 130, 140);
static const COLORREF COL_CAPTION = RGB(180, 180, 195);

static const int CAPTION_GAP = 8;

static const char *PROMPT = "Press any key...";

// Only one widget may listen at a time; this also backs keybindCaptureActive(),
// which the polling hotkeys consult.
static KeybindWidget *s_capturing = nullptr;

// ── helpers ─────────────────────────────────────────────────────────────────────
static int toWide(const char *s, wchar_t *out, int outLen) {
    int k = 0;

    for (const char *p = s; *p && k < outLen - 1; ++p) {
        out[k++] = (wchar_t)(unsigned char)*p;
    }

    out[k] = 0;
    return k;
}

static void fillRect(HDC dc, const RECT &r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FillRect(dc, &r, br);
    DeleteObject(br);
}

static void frameRect(HDC dc, const RECT &r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FrameRect(dc, &r, br);
    DeleteObject(br);
}

// ── state ───────────────────────────────────────────────────────────────────────
void keybindWidgetInit(
    KeybindWidget &w, const Hotkey &binding, int left, int top, int right, int bottom
) {
    w.binding = binding;
    w.bounds.left = left;
    w.bounds.top = top;
    w.bounds.right = right;
    w.bounds.bottom = bottom;
    w.capturing = false;
    w.changed = false;
}

bool keybindWidgetHit(const KeybindWidget &w, int x, int y) {
    return x >= w.bounds.left && x < w.bounds.right && y >= w.bounds.top && y < w.bounds.bottom;
}

void keybindWidgetSetBinding(KeybindWidget &w, const Hotkey &hk) {
    keybindWidgetCancelCapture(w);

    if (!hotkeySame(w.binding, hk)) {
        w.binding = hk;
        w.changed = true;
    }
}

void keybindWidgetBeginCapture(KeybindWidget &w) {
    if (s_capturing && s_capturing != &w) {
        s_capturing->capturing = false;
    }

    s_capturing = &w;
    w.capturing = true;
}

void keybindWidgetCancelCapture(KeybindWidget &w) {
    w.capturing = false;

    if (s_capturing == &w) {
        s_capturing = nullptr;
    }
}

bool keybindWidgetTakeChanged(KeybindWidget &w) {
    bool changed = w.changed;
    w.changed = false;
    return changed;
}

bool keybindCaptureActive() { return s_capturing != nullptr; }

// Commits a captured key with whatever modifiers are held right now. Codes
// outside the writable virtual-key range (the driver reports 0xFF for keys it
// cannot identify) are ignored, so a capture can never produce a binding that
// hotkeyToString would write and hotkeyParse would then reject.
static void acceptBinding(KeybindWidget &w, int vk) {
    if (vk <= 0 || vk >= 255) {
        return;
    }

    Hotkey hk = {vk, 0};

    if (GetKeyState(VK_CONTROL) & 0x8000) {
        hk.mods |= HK_CTRL;
    }

    if (GetKeyState(VK_SHIFT) & 0x8000) {
        hk.mods |= HK_SHIFT;
    }

    if (GetKeyState(VK_MENU) & 0x8000) {
        hk.mods |= HK_ALT;
    }

    keybindWidgetCancelCapture(w);

    if (!hotkeySame(w.binding, hk)) {
        w.binding = hk;
        w.changed = true;
    }
}

// ── input ───────────────────────────────────────────────────────────────────────
bool keybindWidgetOnMessage(KeybindWidget &w, UINT msg, WPARAM wparam, LPARAM lparam) {
    (void)lparam;

    if (!w.capturing) {
        return false;
    }

    // Losing focus mid-capture (alt-tab) would strand the prompt and keep the
    // polling hotkeys muted, so drop back to idle — but let the game see the
    // message, it has its own focus handling.
    if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
        keybindWidgetCancelCapture(w);
        return false;
    }

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        int vk = (int)wparam;

        if (vk == VK_ESCAPE) {
            keybindWidgetCancelCapture(w); // cancel, binding unchanged
        } else if (!hotkeyIsModifierVk(vk)) {
            acceptBinding(w, vk);
        }

        // A bare Ctrl/Shift/Alt press is swallowed but keeps listening: it is
        // part of a combination, not a binding of its own.
        return true;
    }

    if (msg == WM_MBUTTONDOWN) {
        acceptBinding(w, VK_MBUTTON);
        return true;
    }

    if (msg == WM_XBUTTONDOWN) {
        int vk = VK_XBUTTON2;

        if (HIWORD(wparam) == XBUTTON1) {
            vk = VK_XBUTTON1;
        }

        acceptBinding(w, vk);
        return true;
    }

    // Clicking away with the normal buttons cancels; those two stay usable for
    // the rest of the panel, so they are never bindable here.
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        keybindWidgetCancelCapture(w);
        return true;
    }

    // Everything else the keyboard and mouse produce is swallowed while
    // listening, so the game sees no input at all behind the prompt: key-ups
    // and character echoes (including WM_SYSCHAR, which would otherwise beep on
    // an Alt combination), button-ups, double-clicks, wheel and motion.
    switch (msg) {
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
    case WM_UNICHAR:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
        return true;
    }

    return false;
}

// ── drawing ─────────────────────────────────────────────────────────────────────
void keybindWidgetDraw(const KeybindWidget &w, HDC dc, const char *caption) {
    wchar_t text[HOTKEY_NAME_MAX];
    char name[HOTKEY_NAME_MAX];
    COLORREF fill = COL_IDLE_FILL;
    COLORREF edge = COL_IDLE_EDGE;
    COLORREF ink = COL_IDLE_TEXT;

    if (w.capturing) {
        fill = COL_LISTEN_FILL;
        edge = COL_LISTEN_EDGE;
        ink = COL_LISTEN_TEXT;
        strcpy(name, PROMPT);
    } else {
        hotkeyDisplayName(w.binding, name, sizeof(name));

        if (!hotkeyIsBound(w.binding)) {
            ink = COL_UNBOUND_TEXT;
        }
    }

    fillRect(dc, w.bounds, fill);
    frameRect(dc, w.bounds, edge);

    int len = toWide(name, text, HOTKEY_NAME_MAX);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, text, len, &sz);

    // Centred in the box; a long layout-supplied name simply starts at the left
    // edge rather than overflowing symmetrically.
    int x = w.bounds.left + (w.bounds.right - w.bounds.left - sz.cx) / 2;
    int y = w.bounds.top + (w.bounds.bottom - w.bounds.top - sz.cy) / 2;

    if (x < w.bounds.left + 4) {
        x = w.bounds.left + 4;
    }

    SetTextColor(dc, ink);
    TextOutW(dc, x, y, text, len);

    if (caption) {
        wchar_t cap[HOTKEY_NAME_MAX];
        int capLen = toWide(caption, cap, HOTKEY_NAME_MAX);
        SIZE capSz = {0, 0};
        GetTextExtentPoint32W(dc, cap, capLen, &capSz);

        SetTextColor(dc, COL_CAPTION);
        TextOutW(
            dc, w.bounds.left - CAPTION_GAP - capSz.cx,
            w.bounds.top + (w.bounds.bottom - w.bounds.top - capSz.cy) / 2, cap, capLen
        );
    }
}
