#include "overlay.h"
#include "../../core/d3dHook.h"
#include "../../core/hotkey.h"
#include "../../core/overlayPanel.h"
#include "autoMarket.h"
#include <cstdio>
#include <cstring>
#include <d3d9.h>
#include <windows.h>

// Panel drawn over the game from inside the EndScene hook, on the shared
// OverlayPanel plumbing (GDI bitmap -> managed texture -> one alpha-blended
// quad; see src/core/overlayPanel.cpp). Goods are grouped under category
// headers; Min/Max are editable cells selectable by keyboard or mouse click.
// Everything here is integer arithmetic — the render path must stay float-free
// for the mid-function EndScene detour.

static const int PANEL_X = 24;
static const int PANEL_Y = 18;
static const int PANEL_W = 354;
static const int PANEL_H = 782; // sized for 28 goods across 5 categories + preset bar

static const int TITLE_Y = 10;
static const int PRESET_Y = 40;
static const int COLHDR_Y = 68;
static const int LIST_TOP = 88;
static const int ROW_H = 19;
static const int CAT_H = 23;

static const int COL_NAME = 16;
static const int FIELD_W = 66;
static const int MIN_X = 196;
static const int MAX_X = 270;

// Preset bar hit regions (panel-local x); the bar spans PRESET_Y-2 .. PRESET_Y+18.
static const int PRESET_PREV_L = 76;
static const int PRESET_PREV_R = 96;
static const int PRESET_NAME_L = 100;
static const int PRESET_NAME_R = 234;
static const int PRESET_NEXT_L = 236;
static const int PRESET_NEXT_R = 256;
static const int PRESET_APPLY_L = 262;
static const int PRESET_APPLY_R = 346;

// ── state ──────────────────────────────────────────────────────────────────────
static Hotkey s_hotkey = {0, 0};
static bool s_installed = false;
static bool s_visible = false;
static int s_selRow = 0; // selected good row
static int s_selField = 0; // 0 = Min, 1 = Max
static bool s_freshEntry = true; // next digit replaces (set on any selection change)
static int s_presetSel = 0; // highlighted preset in the picklist

// Per-good panel-local Y of each row (constant layout, computed once).
static int s_goodY[64] = {};
static bool s_layoutDone = false;

static OverlayPanel s_panel;

// Input (window subclass)
static WNDPROC s_origWndProc = nullptr;

// ── layout ──────────────────────────────────────────────────────────────────────
static void ensureLayout() {
    if (s_layoutDone) {
        return;
    }

    int y = LIST_TOP;
    const char *lastCat = nullptr;
    int n = autoMarketGoodCount();

    for (int i = 0; i < n && i < 64; ++i) {
        const char *cat = autoMarketGoodCategory(i);

        if (!lastCat || strcmp(cat, lastCat) != 0) {
            y += CAT_H;
            lastCat = cat;
        }

        s_goodY[i] = y;
        y += ROW_H;
    }

    s_layoutDone = true;
}

// ── panel bitmap ────────────────────────────────────────────────────────────────
// Draws one editable value cell; the selected cell gets a bright fill + border.
static void drawFieldCell(HDC dc, int x, int y, int value, bool selected) {
    int t = y - 1;
    int b = y + ROW_H - 2;
    COLORREF ink = RGB(200, 200, 210);

    if (selected) {
        overlayPanelFill(dc, x, t, x + FIELD_W, b, RGB(64, 78, 116));
        overlayPanelFrame(dc, x, t, x + FIELD_W, b, RGB(255, 205, 70));
        ink = RGB(255, 255, 255);
    } else {
        overlayPanelFrame(dc, x, t, x + FIELD_W, b, RGB(58, 58, 74));
    }

    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    overlayPanelTextRight(dc, x + FIELD_W - 6, y, text, ink);
}

// Draws the preset picklist: "Preset:  < name >  [Apply]".
static void drawPresetBar(HDC dc) {
    int t = PRESET_Y - 2;
    int b = PRESET_Y + 18;
    int count = autoMarketPresetCount();

    if (count > 0 && s_presetSel >= count) {
        s_presetSel = 0;
    }

    overlayPanelText(dc, COL_NAME, PRESET_Y, "Preset:", RGB(180, 180, 195));

    overlayPanelFrame(dc, PRESET_PREV_L, t, PRESET_PREV_R, b, RGB(90, 90, 110));
    overlayPanelTextCentered(dc, PRESET_PREV_L, PRESET_PREV_R, PRESET_Y, "<", RGB(220, 220, 230));
    overlayPanelFrame(dc, PRESET_NEXT_L, t, PRESET_NEXT_R, b, RGB(90, 90, 110));
    overlayPanelTextCentered(dc, PRESET_NEXT_L, PRESET_NEXT_R, PRESET_Y, ">", RGB(220, 220, 230));

    overlayPanelFrame(dc, PRESET_NAME_L, t, PRESET_NAME_R, b, RGB(58, 58, 74));

    const char *name = "(no presets)";
    COLORREF nameInk = RGB(120, 120, 130);

    if (count > 0) {
        name = autoMarketPresetName(s_presetSel);
        nameInk = RGB(255, 235, 150);
    }

    overlayPanelTextCentered(dc, PRESET_NAME_L, PRESET_NAME_R, PRESET_Y, name, nameInk);

    COLORREF applyFill = RGB(40, 40, 50);
    COLORREF applyEdge = RGB(70, 70, 84);
    COLORREF applyInk = RGB(110, 110, 120);

    if (count > 0) {
        applyFill = RGB(48, 70, 48);
        applyEdge = RGB(120, 200, 120);
        applyInk = RGB(220, 255, 220);
    }

    overlayPanelFill(dc, PRESET_APPLY_L, t, PRESET_APPLY_R, b, applyFill);
    overlayPanelFrame(dc, PRESET_APPLY_L, t, PRESET_APPLY_R, b, applyEdge);
    overlayPanelTextCentered(dc, PRESET_APPLY_L, PRESET_APPLY_R, PRESET_Y, "Apply", applyInk);
}

static void paintPanel(HDC dc) {
    ensureLayout();

    overlayPanelFill(dc, 0, 0, PANEL_W, PANEL_H, RGB(26, 26, 34));
    overlayPanelFrame(dc, 0, 0, PANEL_W, PANEL_H, RGB(90, 90, 110));

    HFONT font = overlayPanelFont(12, false);
    HFONT fontBold = overlayPanelFont(12, true);

    SelectObject(dc, overlayPanelFont(16, true));
    overlayPanelText(dc, COL_NAME, TITLE_Y, "Auto-Market", RGB(255, 255, 255));

    SelectObject(dc, font);
    drawPresetBar(dc);
    overlayPanelText(dc, MIN_X + 4, COLHDR_Y, "Min", RGB(150, 150, 165));
    overlayPanelText(dc, MAX_X + 4, COLHDR_Y, "Max", RGB(150, 150, 165));

    int count = autoMarketGoodCount();
    const char *lastCat = nullptr;

    for (int i = 0; i < count; ++i) {
        int y = s_goodY[i];
        int id = autoMarketGoodId(i);
        const char *cat = autoMarketGoodCategory(i);

        // Category header above the first good of each category.
        if (!lastCat || strcmp(cat, lastCat) != 0) {
            int cy = y - CAT_H;
            overlayPanelFill(dc, 4, cy + 2, PANEL_W - 4, cy + CAT_H - 1, RGB(40, 44, 58));
            SelectObject(dc, fontBold);
            overlayPanelText(dc, COL_NAME, cy + 3, cat, RGB(150, 200, 255));
            SelectObject(dc, font);
            lastCat = cat;
        }

        if (i == s_selRow) {
            overlayPanelFill(dc, 4, y - 1, PANEL_W - 4, y + ROW_H - 2, RGB(44, 50, 68));
        }

        overlayPanelText(dc, COL_NAME, y, autoMarketGoodName(i), RGB(225, 225, 230));
        drawFieldCell(dc, MIN_X, y, autoMarketGetMin(id), i == s_selRow && s_selField == 0);
        drawFieldCell(dc, MAX_X, y, autoMarketGetMax(id), i == s_selRow && s_selField == 1);
    }

    overlayPanelText(
        dc, COL_NAME, PANEL_H - 32, "Click/arrows select - type set - Del clear", RGB(140, 140, 150)
    );
    overlayPanelText(
        dc, COL_NAME, PANEL_H - 18, "PgUp/PgDn preset - Enter apply - Esc close", RGB(140, 140, 150)
    );
}

// ── editing ─────────────────────────────────────────────────────────────────────
static const int VALUE_MAX = 99999;

static int currentField(int goodId) {
    return s_selField == 0 ? autoMarketGetMin(goodId) : autoMarketGetMax(goodId);
}

static void setCurrentField(int goodId, int value) {
    if (value < 0) {
        value = 0;
    }

    if (value > VALUE_MAX) {
        value = VALUE_MAX;
    }

    if (s_selField == 0) {
        autoMarketSetMin(goodId, value);
    } else {
        autoMarketSetMax(goodId, value);
    }

    overlayPanelMarkDirty(s_panel);
}

// Moves selection; a fresh selection means the next digit types from scratch.
static void selectCell(int row, int field) {
    int count = autoMarketGoodCount();
    s_selRow = (row % count + count) % count;
    s_selField = field & 1;
    s_freshEntry = true;
    overlayPanelMarkDirty(s_panel);
}

static void typeDigit(int digit) {
    int id = autoMarketGoodId(s_selRow);
    int base = s_freshEntry ? 0 : currentField(id);
    s_freshEntry = false;
    setCurrentField(id, base * 10 + digit);
}

static void cyclePreset(int dir) {
    int count = autoMarketPresetCount();

    if (count <= 0) {
        return;
    }

    s_presetSel = ((s_presetSel + dir) % count + count) % count;
    overlayPanelMarkDirty(s_panel);
}

static void applyCurrentPreset() {
    if (autoMarketPresetCount() <= 0) {
        return;
    }

    autoMarketApplyPreset(s_presetSel);
    overlayPanelMarkDirty(s_panel);
}

static bool handleEditKey(int vk) {
    int id = autoMarketGoodId(s_selRow);

    if (vk == VK_ESCAPE) {
        s_visible = false;
        overlayPanelMarkDirty(s_panel);
    } else if (vk == VK_PRIOR) {
        cyclePreset(-1);
    } else if (vk == VK_NEXT) {
        cyclePreset(1);
    } else if (vk == VK_RETURN) {
        applyCurrentPreset();
    } else if (vk == VK_UP) {
        selectCell(s_selRow - 1, s_selField);
    } else if (vk == VK_DOWN) {
        selectCell(s_selRow + 1, s_selField);
    } else if (vk == VK_LEFT) {
        selectCell(s_selRow, 0);
    } else if (vk == VK_RIGHT) {
        selectCell(s_selRow, 1);
    } else if (vk == VK_TAB) {
        selectCell(s_selRow, s_selField ^ 1);
    } else if (vk >= '0' && vk <= '9') {
        typeDigit(vk - '0');
    } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        typeDigit(vk - VK_NUMPAD0);
    } else if (vk == VK_BACK) {
        s_freshEntry = false;
        setCurrentField(id, currentField(id) / 10);
    } else if (vk == VK_DELETE) {
        setCurrentField(id, 0);
        s_freshEntry = true;
    }

    return true; // all keys swallowed while the editor is open
}

// Performs the click action (button-DOWN only). Returns true if inside the panel.
static bool handleClick(int clientX, int clientY, HWND hwnd) {
    int lx = 0;
    int ly = 0;

    if (!overlayPanelMapPoint(s_panel, hwnd, clientX, clientY, lx, ly)) {
        return false; // outside the panel — let the game have the click
    }

    // Preset bar: prev / next / name / apply.
    if (ly >= PRESET_Y - 2 && ly < PRESET_Y + 18) {
        if (lx >= PRESET_PREV_L && lx < PRESET_PREV_R) {
            cyclePreset(-1);
        } else if (lx >= PRESET_NEXT_L && lx < PRESET_NEXT_R) {
            cyclePreset(1);
        } else if (lx >= PRESET_NAME_L && lx < PRESET_APPLY_R) {
            applyCurrentPreset(); // clicking the name or the Apply button applies
        }

        return true;
    }

    ensureLayout();
    int count = autoMarketGoodCount();

    for (int i = 0; i < count; ++i) {
        if (ly >= s_goodY[i] && ly < s_goodY[i] + ROW_H) {
            if (lx >= MIN_X && lx < MIN_X + FIELD_W) {
                selectCell(i, 0);
            } else if (lx >= MAX_X && lx < MAX_X + FIELD_W) {
                selectCell(i, 1);
            } else {
                selectCell(i, s_selField);
            }

            break;
        }
    }

    return true; // consumed (inside panel), even if between rows
}

// ── window subclass ─────────────────────────────────────────────────────────────
static LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        int vk = (int)wparam;

        if (hotkeyMatchesKeyDown(s_hotkey, vk)) {
            s_visible = !s_visible;
            overlayPanelMarkDirty(s_panel);
            return 0;
        }

        if (s_visible) {
            handleEditKey(vk);
            return 0;
        }
    } else if (s_visible) {
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK ||
            msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) {
            int x = (short)LOWORD(lparam);
            int y = (short)HIWORD(lparam);

            // Act only on left-button-DOWN; swallow the other button events when
            // they fall inside the panel (no action) so the game never sees a
            // click meant for the editor — and so a button-up never re-triggers.
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
            return 0; // swallow key echoes while the editor is open
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

    s_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)overlayWndProc);
}

// ── render callback (render thread, inside EndScene) ────────────────────────────
static void overlayRender(IDirect3DDevice9 *device) {
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

void autoMarketOverlayReset() {
    s_visible = false;
    s_selRow = 0;
    s_selField = 0;
    s_freshEntry = true;
    overlayPanelMarkDirty(s_panel);
}

bool installAutoMarketOverlay() {
    // Default toggle: the ` / ~ key (VK_OEM_3), which Stronghold 2 does not use
    // (F1–F12 are taunts). Configurable via [hotkeys] AutoMarketPanel.
    Hotkey def = {VK_OEM_3, 0};
    s_hotkey = hotkeyLoad("hotkeys", "AutoMarketPanel", def);

    if (!hotkeyIsBound(s_hotkey)) {
        return false;
    }

    // Install time, not render time: this is where the quad's floats are built.
    overlayPanelInit(s_panel, PANEL_X, PANEL_Y, PANEL_W, PANEL_H);
    registerD3DRender(&overlayRender);
    s_installed = true;
    return true;
}

bool autoMarketOverlayVisible() { return s_visible; }

Hotkey autoMarketOverlayBinding() { return s_hotkey; }

void autoMarketOverlaySetBinding(const Hotkey &hk) { s_hotkey = hk; }

bool autoMarketOverlayInstalled() { return s_installed; }
