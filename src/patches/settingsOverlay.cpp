#include "settingsOverlay.h"
#include "../core/d3dHook.h"
#include "../core/hotkey.h"
#include "../core/keybindWidget.h"
#include "../core/overlayPanel.h"
#include "attackHotkey.h"
#include "autoMarket/overlay.h"
#include "stopTroopsHotkey.h"
#include <d3d9.h>
#include <windows.h>

// One row per rebindable hotkey: a label, a keybind button and a clear button.
// The panel owns no game state — it moves bindings between the features, the
// widgets and the ini. See docs/features/settings-overlay.md.

static const int PANEL_X = 24;
static const int PANEL_Y = 18;
static const int PANEL_W = 446;
static const int PANEL_H = 260;

static const int TITLE_Y = 12;
static const int HINT_Y = 38;
static const int LIST_TOP = 64;
static const int ROW_H = 30;
static const int BOX_TOP = 3; // row-local
static const int BOX_BOTTOM = 27;
static const int LABEL_Y = 7; // row-local
static const int BOX_L = 246;
static const int BOX_R = 404;
static const int CLEAR_L = 412;
static const int CLEAR_R = 432;
static const int RESTART_MARK_X = 226;

static const int SEP_Y = PANEL_H - 76;
static const int FOOTER1_Y = PANEL_H - 68;
static const int FOOTER2_Y = PANEL_H - 50;
static const int STATUS_Y = PANEL_H - 28;

static const COLORREF COL_BG = RGB(26, 26, 34);
static const COLORREF COL_EDGE = RGB(90, 90, 110);
static const COLORREF COL_TITLE = RGB(255, 255, 255);
static const COLORREF COL_LABEL = RGB(225, 225, 230);
static const COLORREF COL_DIM = RGB(150, 150, 165);
static const COLORREF COL_CONFLICT = RGB(255, 140, 140);
static const COLORREF COL_OK = RGB(150, 220, 150);

// Toggle key, [hotkeys] SettingsPanel. A modifier combination by default: the
// game's own shortcuts are single keys, so this cannot collide with one.
static Hotkey s_hotkey = {'O', HK_CTRL | HK_SHIFT};
static bool s_visible = false;
static OverlayPanel s_panel;
static WNDPROC s_origWndProc = nullptr;

enum SaveStatus {
    STATUS_NONE,
    STATUS_SAVED,
    STATUS_FAILED,
};

static SaveStatus s_status = STATUS_NONE;

// ── rows ────────────────────────────────────────────────────────────────────────
typedef Hotkey (*BindingGetFn)();
typedef void (*BindingSetFn)(const Hotkey &);
typedef bool (*BindingInstalledFn)();

struct SettingRow {
    const char *label;
    const char *iniKey;
    BindingGetFn get;
    BindingSetFn set;
    BindingInstalledFn installed;
    bool allowUnbind; // false for the panel's own key — clearing it would lock
                      // the player out of the only way back in
    KeybindWidget widget;
};

static Hotkey settingsBinding() { return s_hotkey; }

static void settingsSetBinding(const Hotkey &hk) { s_hotkey = hk; }

// The panel is running, so its own hotkey never needs a restart.
static bool settingsInstalled() { return true; }

static SettingRow s_rows[] = {
    {"Stop selected troops",
     "StopTroops",
     stopTroopsBinding,
     stopTroopsSetBinding,
     stopTroopsInstalled,
     true,
     {}},
    {"Attack-move toggle",
     "AttackToggle",
     attackToggleBinding,
     attackToggleSetBinding,
     attackToggleInstalled,
     true,
     {}},
    {"Auto-market panel",
     "AutoMarketPanel",
     autoMarketOverlayBinding,
     autoMarketOverlaySetBinding,
     autoMarketOverlayInstalled,
     true,
     {}},
    {"This settings panel",
     "SettingsPanel",
     settingsBinding,
     settingsSetBinding,
     settingsInstalled,
     false,
     {}},
};

static const int ROW_COUNT = (int)(sizeof(s_rows) / sizeof(s_rows[0]));

static int rowTop(int index) { return LIST_TOP + index * ROW_H; }

// Two rows bound to the same key: the first one to see the press wins, so flag
// it rather than silently letting one shortcut shadow another.
static bool rowConflicts(int index) {
    Hotkey hk = s_rows[index].widget.binding;

    if (!hotkeyIsBound(hk)) {
        return false;
    }

    for (int i = 0; i < ROW_COUNT; ++i) {
        if (i != index && hotkeySame(s_rows[i].widget.binding, hk)) {
            return true;
        }
    }

    return false;
}

// Applies an accepted rebind to the live feature and writes it to the ini.
static void commitRow(SettingRow &row) {
    if (!keybindWidgetTakeChanged(row.widget)) {
        return;
    }

    row.set(row.widget.binding);

    if (hotkeySave("hotkeys", row.iniKey, row.widget.binding)) {
        s_status = STATUS_SAVED;
    } else {
        s_status = STATUS_FAILED;
    }
}

// ── drawing ─────────────────────────────────────────────────────────────────────
static void paintPanel(HDC dc) {
    overlayPanelFill(dc, 0, 0, PANEL_W, PANEL_H, COL_BG);
    overlayPanelFrame(dc, 0, 0, PANEL_W, PANEL_H, COL_EDGE);

    SelectObject(dc, overlayPanelFont(16, true));
    overlayPanelText(dc, 16, TITLE_Y, "Patch Settings", COL_TITLE);

    SelectObject(dc, overlayPanelFont(12, false));
    overlayPanelText(dc, 16, HINT_Y, "Click a shortcut, then press the new key.", COL_DIM);

    bool anyRestart = false;

    for (int i = 0; i < ROW_COUNT; ++i) {
        SettingRow &row = s_rows[i];
        int top = rowTop(i);
        COLORREF ink = COL_LABEL;

        if (rowConflicts(i)) {
            ink = COL_CONFLICT;
        }

        overlayPanelText(dc, 16, top + LABEL_Y, row.label, ink);

        if (!row.installed()) {
            overlayPanelText(dc, RESTART_MARK_X, top + LABEL_Y, "*", COL_DIM);
            anyRestart = true;
        }

        keybindWidgetDraw(row.widget, dc, nullptr);

        if (row.allowUnbind) {
            overlayPanelFrame(dc, CLEAR_L, top + BOX_TOP, CLEAR_R, top + BOX_BOTTOM, COL_EDGE);
            overlayPanelTextCentered(dc, CLEAR_L, CLEAR_R, top + LABEL_Y, "x", COL_DIM);
        }
    }

    overlayPanelFill(dc, 12, SEP_Y, PANEL_W - 12, SEP_Y + 1, RGB(58, 58, 74));
    overlayPanelText(dc, 16, FOOTER1_Y, "Esc closes  -  [x] clears a shortcut", COL_DIM);

    if (anyRestart) {
        overlayPanelText(
            dc, 16, FOOTER2_Y, "* saved, but applies the next time you start the game", COL_DIM
        );
    }

    if (s_status == STATUS_SAVED) {
        overlayPanelText(dc, 16, STATUS_Y, "Saved to sh2-unofficial-patch.ini", COL_OK);
    } else if (s_status == STATUS_FAILED) {
        overlayPanelText(
            dc, 16, STATUS_Y, "Could not write sh2-unofficial-patch.ini", COL_CONFLICT
        );
    }
}

// ── input ───────────────────────────────────────────────────────────────────────
static bool handleClick(int clientX, int clientY, HWND hwnd) {
    int lx = 0;
    int ly = 0;

    if (!overlayPanelMapPoint(s_panel, hwnd, clientX, clientY, lx, ly)) {
        return false; // outside the panel — let the game have the click
    }

    for (int i = 0; i < ROW_COUNT; ++i) {
        int top = rowTop(i);

        if (ly < top + BOX_TOP || ly >= top + BOX_BOTTOM) {
            continue;
        }

        if (lx >= BOX_L && lx < BOX_R) {
            keybindWidgetBeginCapture(s_rows[i].widget);
        } else if (lx >= CLEAR_L && lx < CLEAR_R && s_rows[i].allowUnbind) {
            Hotkey unbound = {0, 0};
            keybindWidgetSetBinding(s_rows[i].widget, unbound);
            commitRow(s_rows[i]);
        }

        break;
    }

    overlayPanelMarkDirty(s_panel);
    return true; // consumed (inside panel), even if between rows
}

static void setVisible(bool visible) {
    s_visible = visible;
    s_status = STATUS_NONE;

    if (!visible) {
        for (int i = 0; i < ROW_COUNT; ++i) {
            keybindWidgetCancelCapture(s_rows[i].widget);
        }
    }

    overlayPanelMarkDirty(s_panel);
}

static LRESULT CALLBACK settingsWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // A listening widget owns every message, including this panel's own toggle
    // key — that is what lets the toggle be rebound onto itself.
    for (int i = 0; i < ROW_COUNT; ++i) {
        if (keybindWidgetOnMessage(s_rows[i].widget, msg, wparam, lparam)) {
            commitRow(s_rows[i]);
            overlayPanelMarkDirty(s_panel);
            return 0;
        }
    }

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        int vk = (int)wparam;

        // Both panels subclass this window and both swallow input while up, so
        // never open on top of the auto-market editor.
        if (hotkeyMatchesKeyDown(s_hotkey, vk) && (s_visible || !autoMarketOverlayVisible())) {
            setVisible(!s_visible);
            return 0;
        }

        if (s_visible) {
            if (vk == VK_ESCAPE) {
                setVisible(false);
            }

            return 0; // swallow every key while the panel is up
        }
    } else if (s_visible) {
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK ||
            msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) {
            int x = (short)LOWORD(lparam);
            int y = (short)HIWORD(lparam);

            // Act on left-button-DOWN only; swallow the other button events
            // inside the panel so the game never sees a click meant for it.
            if (msg == WM_LBUTTONDOWN) {
                if (handleClick(x, y, hwnd)) {
                    return 0;
                }
            } else {
                int lx = 0;
                int ly = 0;

                if (overlayPanelMapPoint(s_panel, hwnd, x, y, lx, ly)) {
                    return 0;
                }
            }
        } else if (
            msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_DEADCHAR || msg == WM_KEYUP ||
            msg == WM_SYSKEYUP
        ) {
            return 0; // swallow key echoes while the panel is up
        }
    }

    return CallWindowProcW(s_origWndProc, hwnd, msg, wparam, lparam);
}

static void ensureSubclass() {
    if (s_origWndProc) {
        return;
    }

    HWND hwnd = (HWND)d3dDeviceWindow();

    if (!hwnd) {
        return;
    }

    s_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)settingsWndProc);
}

// ── render callback (render thread, inside EndScene) ────────────────────────────
static void settingsRender(IDirect3DDevice9 *device) {
    ensureSubclass();

    if (!s_visible) {
        return;
    }

    overlayPanelEnsureDevice(s_panel, device);

    if (s_panel.dirty) {
        HDC dc = overlayPanelBeginPaint(s_panel);

        if (dc) {
            paintPanel(dc);
            overlayPanelEndPaint(s_panel);
        }
    }

    overlayPanelDraw(s_panel, device);
}

void installSettingsOverlay() {
    s_hotkey = hotkeyLoad("hotkeys", "SettingsPanel", s_hotkey);

    if (!hotkeyIsBound(s_hotkey)) {
        return;
    }

    overlayPanelInit(s_panel, PANEL_X, PANEL_Y, PANEL_W, PANEL_H);

    for (int i = 0; i < ROW_COUNT; ++i) {
        int top = rowTop(i);
        keybindWidgetInit(
            s_rows[i].widget, s_rows[i].get(), BOX_L, top + BOX_TOP, BOX_R, top + BOX_BOTTOM
        );
    }

    registerD3DRender(&settingsRender);
}
