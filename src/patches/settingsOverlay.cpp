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

// Design-space constants: what the panel measures at 1080p. Everything is
// scaled through overlayScaleBy, and the label and keybind columns are measured
// from the font, so a long label or a wide scale moves the box instead of
// running into it. See docs/features/ui-scale.md.
static const int D_PANEL_X = 24;
static const int D_PANEL_Y = 18;
static const int D_MARGIN = 16;
static const int D_TITLE_Y = 12;
static const int D_CLEAR_W = 20;
static const int D_BOX_MIN_W = 140;

static const int TITLE_POINTS = 16;
static const int BODY_POINTS = 12;

static const char *HINT = "Click a shortcut, then press the new key.";
static const char *FOOTER1 = "Esc closes  -  [x] clears a shortcut";
static const char *FOOTER2 = "* saved, but applies the next time you start the game";
static const char *STATUS_OK = "Saved to sh2-unofficial-patch.ini";
static const char *STATUS_FAIL = "Could not write sh2-unofficial-patch.ini";
static const char *PROMPT = "Press any key...";

// Resolved layout, in panel pixels. Rebuilt on each show.
struct Layout {
    int scale;
    int panelX, panelY, panelW, panelH;
    int margin;
    int titleY, hintY, listTop, rowH;
    int boxTop, boxBottom, labelY; // row-local
    int boxL, boxR, clearL, clearR, restartMarkX;
    int sepY, footer1Y, footer2Y, statusY;
};

static Layout s_l = {};

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

static int rowTop(int index) { return s_l.listTop + index * s_l.rowH; }

// ── layout ──────────────────────────────────────────────────────────────────────
static int maxInt(int a, int b) { return a > b ? a : b; }

// Measures the panel at `scale`. Integer arithmetic and GDI measurement only —
// no float, no D3D object touched.
static void buildLayout(Layout &l, int scale) {
    HFONT body = overlayPanelFont(BODY_POINTS, false, scale);
    HFONT title = overlayPanelFont(TITLE_POINTS, true, scale);
    int line = overlayPanelLineHeight(body);
    int titleLine = overlayPanelLineHeight(title);

    l.scale = scale;
    l.margin = overlayScaleBy(D_MARGIN, scale);
    l.panelX = overlayScaleBy(D_PANEL_X, scale);
    l.panelY = overlayScaleBy(D_PANEL_Y, scale);

    int gap = overlayScaleBy(8, scale);
    int pad = overlayScaleBy(10, scale);

    // ── columns ──
    int labelW = 0;
    int boxW = maxInt(overlayPanelTextWidth(body, PROMPT) + pad, overlayScaleBy(D_BOX_MIN_W, scale));

    for (int i = 0; i < ROW_COUNT; ++i) {
        char name[HOTKEY_NAME_MAX];
        labelW = maxInt(labelW, overlayPanelTextWidth(body, s_rows[i].label));
        hotkeyDisplayName(s_rows[i].get(), name, sizeof(name));
        boxW = maxInt(boxW, overlayPanelTextWidth(body, name) + pad);
    }

    l.restartMarkX = l.margin + labelW + gap;
    l.boxL = l.restartMarkX + overlayPanelTextWidth(body, "*") + gap * 2;
    l.boxR = l.boxL + boxW;
    l.clearL = l.boxR + gap;
    l.clearR = l.clearL + overlayScaleBy(D_CLEAR_W, scale);

    l.panelW = l.clearR + l.margin;
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(title, "Patch Settings") + 2 * l.margin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(body, HINT) + 2 * l.margin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(body, FOOTER1) + 2 * l.margin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(body, FOOTER2) + 2 * l.margin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(body, STATUS_OK) + 2 * l.margin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(body, STATUS_FAIL) + 2 * l.margin);

    // ── rows ──
    int boxH = line + pad;
    l.boxTop = overlayScaleBy(3, scale);
    l.boxBottom = l.boxTop + boxH;
    l.labelY = l.boxTop + (boxH - line) / 2;
    l.rowH = boxH + overlayScaleBy(6, scale);

    l.titleY = overlayScaleBy(D_TITLE_Y, scale);
    l.hintY = l.titleY + titleLine + overlayScaleBy(4, scale);
    l.listTop = l.hintY + line + overlayScaleBy(12, scale);

    l.sepY = l.listTop + ROW_COUNT * l.rowH + overlayScaleBy(10, scale);
    l.footer1Y = l.sepY + overlayScaleBy(8, scale);
    l.footer2Y = l.footer1Y + line + overlayScaleBy(2, scale);
    l.statusY = l.footer2Y + line + overlayScaleBy(2, scale);
    l.panelH = l.statusY + line + overlayScaleBy(10, scale);
}

// Rebuilds the layout and moves the panel and its widgets onto it. **Does float
// work** (the panel quad), so it runs from the WndProc toggle — reached through
// DispatchMessage with an empty x87 stack — and never from the render thread.
static void applyLayout() {
    int scale = overlayScalePercent();
    buildLayout(s_l, scale);

    // Shrink to fit a small render target, the same way the auto-market panel
    // does. One correction pass is enough for a panel this size.
    int renderW = 0;
    int renderH = 0;

    if (d3dBackbufferSize(renderW, renderH)) {
        int availW = renderW - 2 * s_l.panelX;
        int availH = renderH - 2 * s_l.panelY;

        if (s_l.panelW > availW || s_l.panelH > availH) {
            int byW = scale * availW / maxInt(s_l.panelW, 1);
            int byH = scale * availH / maxInt(s_l.panelH, 1);
            int fit = byW < byH ? byW : byH;

            if (fit < 40) {
                fit = 40;
            }

            if (fit < scale) {
                buildLayout(s_l, fit);
            }
        }
    }

    for (int i = 0; i < ROW_COUNT; ++i) {
        int top = rowTop(i);
        keybindWidgetInit(
            s_rows[i].widget, s_rows[i].get(), s_l.boxL, top + s_l.boxTop, s_l.boxR,
            top + s_l.boxBottom
        );
    }

    overlayPanelSetBounds(s_panel, s_l.panelX, s_l.panelY, s_l.panelW, s_l.panelH);
}

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
// Draws the layout as applyLayout built it; the render thread never rebuilds it,
// so the bitmap and the panel bounds can never disagree.
static void paintPanel(HDC dc) {
    overlayPanelFill(dc, 0, 0, s_l.panelW, s_l.panelH, COL_BG);
    overlayPanelFrame(dc, 0, 0, s_l.panelW, s_l.panelH, COL_EDGE);

    SelectObject(dc, overlayPanelFont(TITLE_POINTS, true, s_l.scale));
    overlayPanelText(dc, s_l.margin, s_l.titleY, "Patch Settings", COL_TITLE);

    SelectObject(dc, overlayPanelFont(BODY_POINTS, false, s_l.scale));
    overlayPanelText(dc, s_l.margin, s_l.hintY, HINT, COL_DIM);

    bool anyRestart = false;

    for (int i = 0; i < ROW_COUNT; ++i) {
        SettingRow &row = s_rows[i];
        int top = rowTop(i);
        COLORREF ink = COL_LABEL;

        if (rowConflicts(i)) {
            ink = COL_CONFLICT;
        }

        overlayPanelText(dc, s_l.margin, top + s_l.labelY, row.label, ink);

        if (!row.installed()) {
            overlayPanelText(dc, s_l.restartMarkX, top + s_l.labelY, "*", COL_DIM);
            anyRestart = true;
        }

        keybindWidgetDraw(row.widget, dc, nullptr);

        if (row.allowUnbind) {
            overlayPanelFrame(
                dc, s_l.clearL, top + s_l.boxTop, s_l.clearR, top + s_l.boxBottom, COL_EDGE
            );
            overlayPanelTextCentered(dc, s_l.clearL, s_l.clearR, top + s_l.labelY, "x", COL_DIM);
        }
    }

    int sepInset = overlayScaleBy(12, s_l.scale);
    overlayPanelFill(
        dc, sepInset, s_l.sepY, s_l.panelW - sepInset, s_l.sepY + overlayScaleBy(1, s_l.scale),
        RGB(58, 58, 74)
    );
    overlayPanelText(dc, s_l.margin, s_l.footer1Y, FOOTER1, COL_DIM);

    if (anyRestart) {
        overlayPanelText(dc, s_l.margin, s_l.footer2Y, FOOTER2, COL_DIM);
    }

    if (s_status == STATUS_SAVED) {
        overlayPanelText(dc, s_l.margin, s_l.statusY, STATUS_OK, COL_OK);
    } else if (s_status == STATUS_FAILED) {
        overlayPanelText(dc, s_l.margin, s_l.statusY, STATUS_FAIL, COL_CONFLICT);
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

        if (ly < top + s_l.boxTop || ly >= top + s_l.boxBottom) {
            continue;
        }

        if (lx >= s_l.boxL && lx < s_l.boxR) {
            keybindWidgetBeginCapture(s_rows[i].widget);
        } else if (lx >= s_l.clearL && lx < s_l.clearR && s_rows[i].allowUnbind) {
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
    // Lay out on the way up, not at install: the panel is sized against the
    // backbuffer, which does not exist yet when patches install.
    if (visible) {
        applyLayout();
    }

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

    // Real bounds and widget rectangles are set by applyLayout on the first
    // show, once there is a backbuffer to size against.
    overlayPanelInit(s_panel, 0, 0, 1, 1);

    for (int i = 0; i < ROW_COUNT; ++i) {
        keybindWidgetInit(s_rows[i].widget, s_rows[i].get(), 0, 0, 0, 0);
    }

    registerD3DRender(&settingsRender);
}
