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

// Design-space constants: what the panel measures at 1080p. Nothing here is a
// panel pixel until it has been through S() — see docs/features/ui-scale.md.
// Widths that have to hold text (the name column, the value cells, the preset
// bar) are measured from the font rather than written down, so a wider scale or
// a longer good name grows the box instead of spilling out of it.
static const int D_PANEL_X = 24;
static const int D_PANEL_Y = 18;
static const int D_MARGIN = 16; // text inset from the panel edge
static const int D_TITLE_Y = 10;
static const int D_CELL_PAD = 6; // value cell inset
static const int D_COL_GAP = 16; // name column -> first value cell
static const int D_FIELD_GAP = 8; // between the two value cells
static const int D_ARROW_W = 20; // preset < and > buttons
static const int D_NAME_MIN = 120; // preset name box
static const int D_ROW_H = 19; // authored good-row pitch
static const int D_ROW_GAP = 2; // between one good row and the next
static const int D_CAT_H = 23; // authored category-header pitch
static const int D_CAT_GAP = 4;

static const int TITLE_POINTS = 16;
static const int BODY_POINTS = 12;

static const int MAX_GOODS = 64;
static const int VALUE_DIGITS = 5; // the widest value a cell can hold: 99999

// Resolved layout, in panel pixels. Rebuilt whenever the scale changes.
struct Layout {
    int scale; // scale the layout was drawn at, after any shrink-to-fit
    int builtAt; // resolved scale it started from, and the render target it was
    int fitW, fitH; // fitted to — a change in either rebuilds
    int panelX, panelY, panelW, panelH;
    int margin;
    int rowH, cellH, cellOffset, catH, catTextY;
    int titleY, presetY, presetT, presetB, colHdrY, listTop;
    int fieldW, minX, maxX;
    int presetPrevL, presetPrevR;
    int presetNameL, presetNameR;
    int presetNextL, presetNextR;
    int presetApplyL, presetApplyR;
    int footer1Y, footer2Y;
    int goodY[MAX_GOODS];
};

static Layout s_l = {};
static bool s_layoutDone = false;

static const char *PRESET_NONE = "(no presets)";
static const char *FOOTER1 = "Click/arrows select - type set - Del clear";
static const char *FOOTER2 = "PgUp/PgDn preset - Enter apply - Esc close";

// ── state ──────────────────────────────────────────────────────────────────────
static Hotkey s_hotkey = {0, 0};
static bool s_installed = false;
static bool s_visible = false;
static int s_selRow = 0; // selected good row
static int s_selField = 0; // 0 = Min, 1 = Max
static bool s_freshEntry = true; // next digit replaces (set on any selection change)
static int s_presetSel = 0; // highlighted preset in the picklist

static OverlayPanel s_panel;

// Input (window subclass)
static WNDPROC s_origWndProc = nullptr;

// ── layout ──────────────────────────────────────────────────────────────────────
static int maxInt(int a, int b) { return a > b ? a : b; }

// Builds the whole layout at `scale`. Pure measurement and integer arithmetic —
// it touches no D3D object, so the caller decides when it is safe to run.
static void buildLayout(Layout &l, int scale) {
    HFONT body = overlayPanelFont(BODY_POINTS, false, scale);
    HFONT bodyBold = overlayPanelFont(BODY_POINTS, true, scale);
    HFONT title = overlayPanelFont(TITLE_POINTS, true, scale);

    int line = overlayPanelLineHeight(body);
    int boldLine = overlayPanelLineHeight(bodyBold);
    int titleLine = overlayPanelLineHeight(title);

    l.scale = scale;
    l.margin = overlayScaleBy(D_MARGIN, scale);
    l.panelX = overlayScaleBy(D_PANEL_X, scale);
    l.panelY = overlayScaleBy(D_PANEL_Y, scale);
    // A row is never shorter than the line box of the text inside it, and never
    // tighter than the pitch the panel was authored at. The first term keeps
    // the density; the second is what stops a cell from clipping its digits
    // when the scale (or a substituted font) makes the text taller than the
    // design assumed.
    int rowGap = overlayScaleBy(D_ROW_GAP, scale);
    l.rowH = maxInt(overlayScaleBy(D_ROW_H, scale), line + rowGap);
    l.cellH = l.rowH - rowGap;
    l.cellOffset = (l.cellH - line) / 2; // centres the text's line box in the cell
    l.catH = maxInt(overlayScaleBy(D_CAT_H, scale), boldLine + overlayScaleBy(D_CAT_GAP, scale));
    l.catTextY = (l.catH - boldLine) / 2;

    // ── columns ──
    char widest[VALUE_DIGITS + 1];

    for (int i = 0; i < VALUE_DIGITS; ++i) {
        widest[i] = '9';
    }

    widest[VALUE_DIGITS] = 0;

    int cellPad = overlayScaleBy(D_CELL_PAD, scale);
    l.fieldW = overlayPanelTextWidth(body, widest) + 2 * cellPad;

    int nameW = 0;
    int count = autoMarketGoodCount();

    for (int i = 0; i < count && i < MAX_GOODS; ++i) {
        nameW = maxInt(nameW, overlayPanelTextWidth(body, autoMarketGoodName(i)));
        nameW = maxInt(nameW, overlayPanelTextWidth(bodyBold, autoMarketGoodCategory(i)));
    }

    l.minX = l.margin + nameW + overlayScaleBy(D_COL_GAP, scale);
    l.maxX = l.minX + l.fieldW + overlayScaleBy(D_FIELD_GAP, scale);

    // ── preset bar ──
    int arrowW = overlayScaleBy(D_ARROW_W, scale);
    int gapS = overlayScaleBy(4, scale);
    int applyW = overlayPanelTextWidth(body, "Apply") + 4 * cellPad;
    int presetLabelW = overlayPanelTextWidth(body, "Preset:");

    // The name box holds whichever preset name is longest, so a name authored
    // in the ini can't spill over the arrows, the label or Apply.
    int nameBoxW = maxInt(
        overlayScaleBy(D_NAME_MIN, scale),
        overlayPanelTextWidth(body, PRESET_NONE) + 2 * cellPad
    );
    int presetCount = autoMarketPresetCount();

    for (int i = 0; i < presetCount; ++i) {
        nameBoxW = maxInt(nameBoxW, overlayPanelTextWidth(body, autoMarketPresetName(i)) + 2 * cellPad);
    }

    int barMin = l.margin + presetLabelW + gapS * 2 + arrowW + gapS + nameBoxW + gapS + arrowW +
                 gapS + applyW + l.margin;

    // The panel is as wide as its widest line needs it to be.
    l.panelW = l.maxX + l.fieldW + l.margin;
    l.panelW = maxInt(l.panelW, barMin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(title, "Auto-Market") + 2 * l.margin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(body, FOOTER1) + 2 * l.margin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(body, FOOTER2) + 2 * l.margin);

    l.presetPrevL = l.margin + presetLabelW + gapS * 2;
    l.presetPrevR = l.presetPrevL + arrowW;
    l.presetApplyR = l.panelW - l.margin;
    l.presetApplyL = l.presetApplyR - applyW;
    l.presetNextR = l.presetApplyL - gapS;
    l.presetNextL = l.presetNextR - arrowW;
    l.presetNameL = l.presetPrevR + gapS;
    l.presetNameR = l.presetNextL - gapS;

    // ── rows ──
    l.titleY = overlayScaleBy(D_TITLE_Y, scale);
    l.presetY = l.titleY + titleLine + overlayScaleBy(6, scale);
    l.presetT = l.presetY - overlayScaleBy(2, scale);
    l.presetB = l.presetY + line + overlayScaleBy(4, scale);
    l.colHdrY = l.presetB + overlayScaleBy(6, scale);
    l.listTop = l.colHdrY + line + overlayScaleBy(4, scale);

    int y = l.listTop;
    const char *lastCat = nullptr;

    for (int i = 0; i < count && i < MAX_GOODS; ++i) {
        const char *cat = autoMarketGoodCategory(i);

        if (!lastCat || strcmp(cat, lastCat) != 0) {
            y += l.catH;
            lastCat = cat;
        }

        l.goodY[i] = y;
        y += l.rowH;
    }

    l.footer1Y = y + overlayScaleBy(10, scale);
    l.footer2Y = l.footer1Y + line + overlayScaleBy(2, scale);
    l.panelH = l.footer2Y + line + overlayScaleBy(8, scale);
}

// Rebuilds the layout when the scale or the render target has changed. A panel
// taller than the screen shrinks itself the rest of the way: the tall goods
// list is the one panel that will not fit at 800x600 even at the minimum
// automatic scale. Bounded to two correction passes so the font cache sees a
// handful of sizes, not a sweep.
static void ensureLayout() {
    int scale = overlayScalePercent();
    int renderW = 0;
    int renderH = 0;
    bool haveSize = d3dBackbufferSize(renderW, renderH);

    if (s_layoutDone && scale == s_l.builtAt && renderW == s_l.fitW && renderH == s_l.fitH) {
        return;
    }

    s_l.builtAt = scale;
    s_l.fitW = renderW;
    s_l.fitH = renderH;
    buildLayout(s_l, scale);

    if (haveSize) {
        for (int pass = 0; pass < 2; ++pass) {
            int available = renderH - 2 * s_l.panelY;

            if (s_l.panelH <= available && s_l.panelW <= renderW - 2 * s_l.panelX) {
                break;
            }

            int byHeight = scale * available / maxInt(s_l.panelH, 1);
            int byWidth = scale * (renderW - 2 * s_l.panelX) / maxInt(s_l.panelW, 1);
            int fit = byHeight < byWidth ? byHeight : byWidth;

            if (fit < 40) {
                fit = 40;
            }

            if (fit >= scale) {
                break;
            }

            scale = fit;
            buildLayout(s_l, scale);
        }
    }

    s_layoutDone = true;
}

// Moves the panel onto the freshly built layout. **Does float work** (the panel
// quad), so it must not run on the render thread — the toggle path is a
// WndProc, which the game reaches through DispatchMessage with an empty x87
// stack, so it is safe there.
static void applyLayout() {
    ensureLayout();
    overlayPanelSetBounds(s_panel, s_l.panelX, s_l.panelY, s_l.panelW, s_l.panelH);
}

// ── panel bitmap ────────────────────────────────────────────────────────────────
// Draws one editable value cell; the selected cell gets a bright fill + border.
static void drawFieldCell(HDC dc, int x, int y, int value, bool selected) {
    int t = y - s_l.cellOffset;
    int b = t + s_l.cellH;
    COLORREF ink = RGB(200, 200, 210);

    if (selected) {
        overlayPanelFill(dc, x, t, x + s_l.fieldW, b, RGB(64, 78, 116));
        overlayPanelFrame(dc, x, t, x + s_l.fieldW, b, RGB(255, 205, 70));
        ink = RGB(255, 255, 255);
    } else {
        overlayPanelFrame(dc, x, t, x + s_l.fieldW, b, RGB(58, 58, 74));
    }

    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    overlayPanelTextRight(dc, x + s_l.fieldW - overlayScaleBy(D_CELL_PAD, s_l.scale), y, text, ink);
}

// Draws the preset picklist: "Preset:  < name >  [Apply]".
static void drawPresetBar(HDC dc) {
    int t = s_l.presetT;
    int b = s_l.presetB;
    int count = autoMarketPresetCount();

    if (count > 0 && s_presetSel >= count) {
        s_presetSel = 0;
    }

    overlayPanelText(dc, s_l.margin, s_l.presetY, "Preset:", RGB(180, 180, 195));

    overlayPanelFrame(dc, s_l.presetPrevL, t, s_l.presetPrevR, b, RGB(90, 90, 110));
    overlayPanelTextCentered(
        dc, s_l.presetPrevL, s_l.presetPrevR, s_l.presetY, "<", RGB(220, 220, 230)
    );
    overlayPanelFrame(dc, s_l.presetNextL, t, s_l.presetNextR, b, RGB(90, 90, 110));
    overlayPanelTextCentered(
        dc, s_l.presetNextL, s_l.presetNextR, s_l.presetY, ">", RGB(220, 220, 230)
    );

    overlayPanelFrame(dc, s_l.presetNameL, t, s_l.presetNameR, b, RGB(58, 58, 74));

    const char *name = PRESET_NONE;
    COLORREF nameInk = RGB(120, 120, 130);

    if (count > 0) {
        name = autoMarketPresetName(s_presetSel);
        nameInk = RGB(255, 235, 150);
    }

    overlayPanelTextCentered(dc, s_l.presetNameL, s_l.presetNameR, s_l.presetY, name, nameInk);

    COLORREF applyFill = RGB(40, 40, 50);
    COLORREF applyEdge = RGB(70, 70, 84);
    COLORREF applyInk = RGB(110, 110, 120);

    if (count > 0) {
        applyFill = RGB(48, 70, 48);
        applyEdge = RGB(120, 200, 120);
        applyInk = RGB(220, 255, 220);
    }

    overlayPanelFill(dc, s_l.presetApplyL, t, s_l.presetApplyR, b, applyFill);
    overlayPanelFrame(dc, s_l.presetApplyL, t, s_l.presetApplyR, b, applyEdge);
    overlayPanelTextCentered(
        dc, s_l.presetApplyL, s_l.presetApplyR, s_l.presetY, "Apply", applyInk
    );
}

// The layout is never rebuilt from here: it belongs to the toggle path, so the
// bitmap and the panel bounds can never disagree. A render target that changes
// while the panel is open keeps the old scale until it is closed and reopened.
static void paintPanel(HDC dc) {
    overlayPanelFill(dc, 0, 0, s_l.panelW, s_l.panelH, RGB(26, 26, 34));
    overlayPanelFrame(dc, 0, 0, s_l.panelW, s_l.panelH, RGB(90, 90, 110));

    HFONT font = overlayPanelFont(BODY_POINTS, false, s_l.scale);
    HFONT fontBold = overlayPanelFont(BODY_POINTS, true, s_l.scale);

    SelectObject(dc, overlayPanelFont(TITLE_POINTS, true, s_l.scale));
    overlayPanelText(dc, s_l.margin, s_l.titleY, "Auto-Market", RGB(255, 255, 255));

    SelectObject(dc, font);
    drawPresetBar(dc);

    int hdrPad = overlayScaleBy(4, s_l.scale);
    overlayPanelText(dc, s_l.minX + hdrPad, s_l.colHdrY, "Min", RGB(150, 150, 165));
    overlayPanelText(dc, s_l.maxX + hdrPad, s_l.colHdrY, "Max", RGB(150, 150, 165));

    int inset = overlayScaleBy(4, s_l.scale);
    int count = autoMarketGoodCount();
    const char *lastCat = nullptr;

    for (int i = 0; i < count && i < MAX_GOODS; ++i) {
        int y = s_l.goodY[i];
        int id = autoMarketGoodId(i);
        const char *cat = autoMarketGoodCategory(i);

        // Category header above the first good of each category.
        if (!lastCat || strcmp(cat, lastCat) != 0) {
            int cy = y - s_l.catH;
            overlayPanelFill(
                dc, inset, cy + overlayScaleBy(2, s_l.scale), s_l.panelW - inset,
                cy + s_l.catH - overlayScaleBy(1, s_l.scale), RGB(40, 44, 58)
            );
            SelectObject(dc, fontBold);
            overlayPanelText(dc, s_l.margin, cy + s_l.catTextY, cat, RGB(150, 200, 255));
            SelectObject(dc, font);
            lastCat = cat;
        }

        if (i == s_selRow) {
            int t = y - s_l.cellOffset;
            overlayPanelFill(dc, inset, t, s_l.panelW - inset, t + s_l.cellH, RGB(44, 50, 68));
        }

        overlayPanelText(dc, s_l.margin, y, autoMarketGoodName(i), RGB(225, 225, 230));
        drawFieldCell(dc, s_l.minX, y, autoMarketGetMin(id), i == s_selRow && s_selField == 0);
        drawFieldCell(dc, s_l.maxX, y, autoMarketGetMax(id), i == s_selRow && s_selField == 1);
    }

    overlayPanelText(dc, s_l.margin, s_l.footer1Y, FOOTER1, RGB(140, 140, 150));
    overlayPanelText(dc, s_l.margin, s_l.footer2Y, FOOTER2, RGB(140, 140, 150));
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
    if (ly >= s_l.presetT && ly < s_l.presetB) {
        if (lx >= s_l.presetPrevL && lx < s_l.presetPrevR) {
            cyclePreset(-1);
        } else if (lx >= s_l.presetNextL && lx < s_l.presetNextR) {
            cyclePreset(1);
        } else if (lx >= s_l.presetNameL && lx < s_l.presetApplyR) {
            applyCurrentPreset(); // clicking the name or the Apply button applies
        }

        return true;
    }

    int count = autoMarketGoodCount();

    for (int i = 0; i < count && i < MAX_GOODS; ++i) {
        if (ly >= s_l.goodY[i] && ly < s_l.goodY[i] + s_l.rowH) {
            if (lx >= s_l.minX && lx < s_l.minX + s_l.fieldW) {
                selectCell(i, 0);
            } else if (lx >= s_l.maxX && lx < s_l.maxX + s_l.fieldW) {
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
            // Lay out on the way up, not at install: the panel is sized against
            // the backbuffer, which does not exist yet when patches install.
            // This is a WndProc, so the float work in applyLayout is safe here.
            if (!s_visible) {
                applyLayout();
            }

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
    // Real bounds are set by applyLayout on the first show, once there is a
    // backbuffer to size against.
    overlayPanelInit(s_panel, 0, 0, 1, 1);
    registerD3DRender(&overlayRender);
    s_installed = true;
    return true;
}

bool autoMarketOverlayVisible() { return s_visible; }

Hotkey autoMarketOverlayBinding() { return s_hotkey; }

void autoMarketOverlaySetBinding(const Hotkey &hk) { s_hotkey = hk; }

bool autoMarketOverlayInstalled() { return s_installed; }
