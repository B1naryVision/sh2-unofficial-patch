#include "settingsOverlay.h"
#include "../core/config.h"
#include "../core/d3dHook.h"
#include "../core/hotkey.h"
#include "../core/keybindWidget.h"
#include "../core/optionWidget.h"
#include "../core/overlayPanel.h"
#include "attackHotkey.h"
#include "autoMarket/overlay.h"
#include "lobbyInProgressFilter.h"
#include "panSpeed.h"
#include "shiftRecruit.h"
#include "siegeCampHotkey.h"
#include "stopTroopsHotkey.h"
#include "zoomLimit.h"
#include "zoomSpeed.h"
#include <cstdio>
#include <d3d9.h>
#include <windows.h>

// Two lists in one panel: a row per rebindable hotkey, and a row per settable
// ini option. The panel owns no game state — it moves values between the
// features, the widgets and the ini. Every option applies to the running game
// as it is changed, so a player can find the setting they want by feel rather
// than by editing a file between launches. See docs/features/settings-overlay.md.

// Design-space constants: what the panel measures at 1080p. Everything is
// scaled through overlayScaleBy, and the label and control columns are measured
// from the font, so a long label or a wide scale moves the box instead of
// running into it. See docs/features/ui-scale.md.
static const int D_PANEL_X = 24;
static const int D_PANEL_Y = 18;
static const int D_MARGIN = 16;
static const int D_TITLE_Y = 12;
static const int D_CLEAR_W = 20;
static const int D_BOX_MIN_W = 140;
static const int D_SLIDER_W = 170;

static const int TITLE_POINTS = 16;
static const int BODY_POINTS = 12;

static const char *HINT = "Click a shortcut then press a key; drag a slider or click a switch.";
static const char *FOOTER1 =
    "Esc closes  -  [x] clears a shortcut  -  changes apply as you make them";
static const char *FOOTER2 = "* saved, but applies the next time you start the game";
static const char *FOOTER3 = "! saved, but this copy of the game could not be patched";
static const char *STATUS_OK = "Saved to sh2-unofficial-patch.ini";
static const char *STATUS_FAIL = "Could not write sh2-unofficial-patch.ini";
static const char *PROMPT = "Press any key...";
static const char *HEAD_KEYS = "Shortcuts";
static const char *HEAD_OPTS = "Options";

// Resolved layout, in panel pixels. Rebuilt on each show.
struct Layout {
    int scale;
    int panelX, panelY, panelW, panelH;
    int margin;
    int titleY, hintY, rowH;
    int keysHeadY, keysTop;
    int optsHeadY, optsTop;
    int boxTop, boxBottom, labelY; // row-local
    int ctrlL, ctrlR, trailL, clearR, markX;
    int sepY, footer1Y, footer2Y, footer3Y, statusY;
};

static Layout s_l = {};

static const COLORREF COL_BG = RGB(26, 26, 34);
static const COLORREF COL_EDGE = RGB(90, 90, 110);
static const COLORREF COL_TITLE = RGB(255, 255, 255);
static const COLORREF COL_LABEL = RGB(225, 225, 230);
static const COLORREF COL_HEAD = RGB(160, 175, 215);
static const COLORREF COL_DIM = RGB(150, 150, 165);
static const COLORREF COL_VALUE = RGB(225, 225, 230);
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

// ── hotkey rows ─────────────────────────────────────────────────────────────────
typedef Hotkey (*BindingGetFn)();
typedef void (*BindingSetFn)(const Hotkey &);
typedef bool (*BindingInstalledFn)();

struct HotkeyRow {
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

static HotkeyRow s_keyRows[] = {
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

static const int KEY_ROW_COUNT = (int)(sizeof(s_keyRows) / sizeof(s_keyRows[0]));

// ── option rows ─────────────────────────────────────────────────────────────────
// Each row owns the whole round trip for one ini key: read the live value from
// the feature, hand an edit back to it, and render both the control and the
// text that goes in the file. Values are integers end to end (a multiplier is
// carried in tenths), so nothing here does float arithmetic.
typedef int (*OptionGetFn)();
typedef void (*OptionSetFn)(int);
typedef bool (*OptionFailedFn)();
typedef void (*OptionFormatFn)(int value, char *buf, int bufLen);

struct OptionRow {
    const char *label;
    const char *section;
    const char *iniKey;
    OptionKind kind;
    int min, max, step; // slider range
    const char *const *choices; // choice: button captions
    const char *const *iniValues; // choice: what each option writes to the ini
    int choiceCount;
    OptionGetFn get;
    OptionSetFn set;
    OptionFailedFn failed;
    OptionFormatFn display; // slider: the value text beside the track
    OptionFormatFn toIni; // slider: the value written to the ini
    bool relayout; // the panel's own size changed; rebuild it once settled
    OptionWidget widget;
};

// A multiplier carried in tenths, as "1.5" for the ini and "1.5x" on screen.
static void formatTenths(int tenths, char *buf, int bufLen, const char *suffix) {
    snprintf(buf, bufLen, "%d.%d%s", tenths / 10, tenths % 10, suffix);
}

static void displayTenths(int tenths, char *buf, int bufLen) {
    formatTenths(tenths, buf, bufLen, "x");
}

static void iniTenths(int tenths, char *buf, int bufLen) { formatTenths(tenths, buf, bufLen, ""); }

// The panel-size slider carries Auto as its lowest position, below the 50%
// minimum, so one control covers "pick it for me" and a hand-set percentage.
static const int SCALE_AUTO = 45;

static void displayScale(int value, char *buf, int bufLen) {
    if (value < 50) {
        snprintf(buf, bufLen, "Auto");
        return;
    }

    snprintf(buf, bufLen, "%d%%", value);
}

static void iniScale(int value, char *buf, int bufLen) {
    if (value < 50) {
        snprintf(buf, bufLen, "Auto");
        return;
    }

    snprintf(buf, bufLen, "%d", value);
}

static void displayRecruit(int value, char *buf, int bufLen) {
    if (value <= 1) {
        snprintf(buf, bufLen, "Off");
        return;
    }

    snprintf(buf, bufLen, "%d", value);
}

static void iniPlain(int value, char *buf, int bufLen) { snprintf(buf, bufLen, "%d", value); }

// ── option accessors ──
static int zoomSpeedGet() { return zoomSpeedTenths(); }

static void zoomSpeedSet(int v) { zoomSpeedSetTenths(v); }

static int panSpeedGet() { return panSpeedTenths(); }

static void panSpeedSet(int v) { panSpeedSetTenths(v); }

static int zoomLimitGet() { return zoomLimitEnabled() ? 1 : 0; }

static void zoomLimitSet(int v) { zoomLimitSetEnabled(v != 0); }

static int siegeCampGet() { return siegeCampTwoStep() ? 1 : 0; }

static void siegeCampSet(int v) { siegeCampSetTwoStep(v != 0); }

static int lobbyFilterGet() { return lobbyFilterEnabled() ? 1 : 0; }

static void lobbyFilterSet(int v) { lobbyFilterSetEnabled(v != 0); }

static int recruitGet() { return shiftRecruitMultiplier(); }

static void recruitSet(int v) { shiftRecruitSetMultiplier(v); }

// The scale is the one option the panel itself owns rather than a feature.
// overlayScalePercent() is called first so a value that has never been read out
// of the ini is resolved before it is reported.
static int scaleGet() {
    overlayScalePercent();

    int fixed = overlayScaleFixed();

    if (fixed < 50) {
        return SCALE_AUTO;
    }

    return fixed;
}

static void scaleSet(int v) {
    if (v < 50) {
        overlayScaleSetFixed(0);
        return;
    }

    overlayScaleSetFixed(v);
}

static bool neverFails() { return false; }

static const char *const CHOICE_LIMIT[] = {"Vanilla", "Auto"};
static const char *const INI_LIMIT[] = {"Vanilla", "Auto"};
static const char *const CHOICE_SIEGE[] = {"Jump at once", "Open first"};
static const char *const CHOICE_LOBBY[] = {"Show all", "Hide started"};
static const char *const INI_BOOL[] = {"0", "1"};

static OptionRow s_optRows[] = {
    {"Camera zoom speed",
     "camera",
     "ZoomSpeedMultiplier",
     OPTION_SLIDER,
     1,
     100,
     1,
     nullptr,
     nullptr,
     0,
     zoomSpeedGet,
     zoomSpeedSet,
     zoomSpeedFailed,
     displayTenths,
     iniTenths,
     false,
     {}},
    {"Camera pan speed",
     "camera",
     "PanSpeedMultiplier",
     OPTION_SLIDER,
     1,
     100,
     1,
     nullptr,
     nullptr,
     0,
     panSpeedGet,
     panSpeedSet,
     panSpeedFailed,
     displayTenths,
     iniTenths,
     false,
     {}},
    {"Zoom out limit",
     "camera",
     "ZoomOutLimit",
     OPTION_CHOICE,
     0,
     1,
     1,
     CHOICE_LIMIT,
     INI_LIMIT,
     2,
     zoomLimitGet,
     zoomLimitSet,
     zoomLimitFailed,
     nullptr,
     nullptr,
     false,
     {}},
    {"Siege camp shortcut",
     "interface",
     "SiegeCampJumpOnSecondPress",
     OPTION_CHOICE,
     0,
     1,
     1,
     CHOICE_SIEGE,
     INI_BOOL,
     2,
     siegeCampGet,
     siegeCampSet,
     siegeCampFailed,
     nullptr,
     nullptr,
     false,
     {}},
    {"Shift-click recruits",
     "recruitment",
     "RecruitmentShiftMultiplier",
     OPTION_SLIDER,
     1,
     100,
     1,
     nullptr,
     nullptr,
     0,
     recruitGet,
     recruitSet,
     shiftRecruitFailed,
     displayRecruit,
     iniPlain,
     false,
     {}},
    {"Started games in the list",
     "multiplayer",
     "HideInProgressLobbies",
     OPTION_CHOICE,
     0,
     1,
     1,
     CHOICE_LOBBY,
     INI_BOOL,
     2,
     lobbyFilterGet,
     lobbyFilterSet,
     lobbyFilterFailed,
     nullptr,
     nullptr,
     false,
     {}},
    {"Panel size",
     "ui",
     "Scale",
     OPTION_SLIDER,
     SCALE_AUTO,
     300,
     5,
     nullptr,
     nullptr,
     0,
     scaleGet,
     scaleSet,
     neverFails,
     displayScale,
     iniScale,
     true,
     {}},
};

static const int OPT_ROW_COUNT = (int)(sizeof(s_optRows) / sizeof(s_optRows[0]));

// A row's value as it is drawn on screen; also what the column is measured for.
static void optionDisplayText(const OptionRow &row, int value, char *buf, int bufLen) {
    buf[0] = 0;

    if (row.kind == OPTION_CHOICE || !row.display) {
        return; // the choice button draws its own caption
    }

    row.display(value, buf, bufLen);
}

static int keyRowTop(int index) { return s_l.keysTop + index * s_l.rowH; }

static int optRowTop(int index) { return s_l.optsTop + index * s_l.rowH; }

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
    // One control column and one trailing column across both lists, so the
    // keybind boxes, the switches and the sliders all line up.
    int labelW = 0;
    int ctrlW =
        maxInt(overlayPanelTextWidth(body, PROMPT) + pad, overlayScaleBy(D_BOX_MIN_W, scale));
    int trailW = overlayScaleBy(D_CLEAR_W, scale);

    ctrlW = maxInt(ctrlW, overlayScaleBy(D_SLIDER_W, scale));

    for (int i = 0; i < KEY_ROW_COUNT; ++i) {
        char name[HOTKEY_NAME_MAX];
        labelW = maxInt(labelW, overlayPanelTextWidth(body, s_keyRows[i].label));
        hotkeyDisplayName(s_keyRows[i].get(), name, sizeof(name));
        ctrlW = maxInt(ctrlW, overlayPanelTextWidth(body, name) + pad);
    }

    for (int i = 0; i < OPT_ROW_COUNT; ++i) {
        const OptionRow &row = s_optRows[i];
        labelW = maxInt(labelW, overlayPanelTextWidth(body, row.label));

        for (int c = 0; c < row.choiceCount; ++c) {
            ctrlW = maxInt(ctrlW, overlayPanelTextWidth(body, row.choices[c]) + pad);
        }

        // Measured at both ends of the range, so the column never resizes as
        // the value changes under the player's mouse.
        char text[32];
        optionDisplayText(row, row.min, text, sizeof(text));
        trailW = maxInt(trailW, overlayPanelTextWidth(body, text));
        optionDisplayText(row, row.max, text, sizeof(text));
        trailW = maxInt(trailW, overlayPanelTextWidth(body, text));
    }

    l.markX = l.margin + labelW + gap;
    l.ctrlL = l.markX + overlayPanelTextWidth(body, "*") + gap * 2;
    l.ctrlR = l.ctrlL + ctrlW;
    l.trailL = l.ctrlR + gap;
    l.clearR = l.trailL + overlayScaleBy(D_CLEAR_W, scale);

    l.panelW = l.trailL + trailW + l.margin;
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(title, "Patch Settings") + 2 * l.margin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(body, HINT) + 2 * l.margin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(body, FOOTER1) + 2 * l.margin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(body, FOOTER2) + 2 * l.margin);
    l.panelW = maxInt(l.panelW, overlayPanelTextWidth(body, FOOTER3) + 2 * l.margin);
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

    l.keysHeadY = l.hintY + line + overlayScaleBy(12, scale);
    l.keysTop = l.keysHeadY + line + overlayScaleBy(4, scale);

    l.optsHeadY = l.keysTop + KEY_ROW_COUNT * l.rowH + overlayScaleBy(10, scale);
    l.optsTop = l.optsHeadY + line + overlayScaleBy(4, scale);

    l.sepY = l.optsTop + OPT_ROW_COUNT * l.rowH + overlayScaleBy(10, scale);
    l.footer1Y = l.sepY + overlayScaleBy(8, scale);
    l.footer2Y = l.footer1Y + line + overlayScaleBy(2, scale);
    l.footer3Y = l.footer2Y + line + overlayScaleBy(2, scale);
    l.statusY = l.footer3Y + line + overlayScaleBy(2, scale);
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

    for (int i = 0; i < KEY_ROW_COUNT; ++i) {
        int top = keyRowTop(i);
        keybindWidgetInit(
            s_keyRows[i].widget, s_keyRows[i].get(), s_l.ctrlL, top + s_l.boxTop, s_l.ctrlR,
            top + s_l.boxBottom
        );
    }

    for (int i = 0; i < OPT_ROW_COUNT; ++i) {
        OptionRow &row = s_optRows[i];
        int top = optRowTop(i);

        if (row.kind == OPTION_CHOICE) {
            optionWidgetInitChoice(
                row.widget, row.get(), row.choices, row.choiceCount, s_l.ctrlL, top + s_l.boxTop,
                s_l.ctrlR, top + s_l.boxBottom
            );
        } else {
            optionWidgetInitSlider(
                row.widget, row.get(), row.min, row.max, row.step, s_l.ctrlL, top + s_l.boxTop,
                s_l.ctrlR, top + s_l.boxBottom
            );
        }
    }

    overlayPanelSetBounds(s_panel, s_l.panelX, s_l.panelY, s_l.panelW, s_l.panelH);
}

// Two rows bound to the same key: the first one to see the press wins, so flag
// it rather than silently letting one shortcut shadow another.
static bool rowConflicts(int index) {
    Hotkey hk = s_keyRows[index].widget.binding;

    if (!hotkeyIsBound(hk)) {
        return false;
    }

    for (int i = 0; i < KEY_ROW_COUNT; ++i) {
        if (i != index && hotkeySame(s_keyRows[i].widget.binding, hk)) {
            return true;
        }
    }

    return false;
}

static void noteSave(bool ok) {
    if (ok) {
        s_status = STATUS_SAVED;
        return;
    }

    s_status = STATUS_FAILED;
}

// Applies an accepted rebind to the live feature and writes it to the ini.
static void commitKeyRow(HotkeyRow &row) {
    if (!keybindWidgetTakeChanged(row.widget)) {
        return;
    }

    row.set(row.widget.binding);
    noteSave(hotkeySave("hotkeys", row.iniKey, row.widget.binding));
}

// Hands the value to the feature straight away, so the player sees the change
// in the running game. Persisting is separate: a slider is written once the
// drag ends, not on every pixel of it.
static void applyOptionRow(OptionRow &row) { row.set(row.widget.value); }

// Writes the row to the ini and, for a row that changes the panel's own size,
// rebuilds the layout. That is deferred to here rather than done on every step
// of a drag because a rebuild re-creates the widgets, which would drop the drag
// the player is still holding.
static void persistOptionRow(OptionRow &row) {
    if (!optionWidgetTakeChanged(row.widget)) {
        return;
    }

    char text[32];

    if (row.kind == OPTION_CHOICE) {
        int index = row.widget.value;

        if (index >= 0 && index < row.choiceCount) {
            noteSave(configSetString(row.section, row.iniKey, row.iniValues[index]));
        }
    } else {
        row.toIni(row.widget.value, text, sizeof(text));
        noteSave(configSetString(row.section, row.iniKey, text));
    }

    if (row.relayout) {
        applyLayout();
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
    bool anyFailed = false;

    overlayPanelText(dc, s_l.margin, s_l.keysHeadY, HEAD_KEYS, COL_HEAD);

    for (int i = 0; i < KEY_ROW_COUNT; ++i) {
        HotkeyRow &row = s_keyRows[i];
        int top = keyRowTop(i);
        COLORREF ink = COL_LABEL;

        if (rowConflicts(i)) {
            ink = COL_CONFLICT;
        }

        overlayPanelText(dc, s_l.margin, top + s_l.labelY, row.label, ink);

        if (!row.installed()) {
            overlayPanelText(dc, s_l.markX, top + s_l.labelY, "*", COL_DIM);
            anyRestart = true;
        }

        keybindWidgetDraw(row.widget, dc, nullptr);

        if (row.allowUnbind) {
            overlayPanelFrame(
                dc, s_l.trailL, top + s_l.boxTop, s_l.clearR, top + s_l.boxBottom, COL_EDGE
            );
            overlayPanelTextCentered(dc, s_l.trailL, s_l.clearR, top + s_l.labelY, "x", COL_DIM);
        }
    }

    overlayPanelText(dc, s_l.margin, s_l.optsHeadY, HEAD_OPTS, COL_HEAD);

    for (int i = 0; i < OPT_ROW_COUNT; ++i) {
        OptionRow &row = s_optRows[i];
        int top = optRowTop(i);

        overlayPanelText(dc, s_l.margin, top + s_l.labelY, row.label, COL_LABEL);

        if (row.failed()) {
            overlayPanelText(dc, s_l.markX, top + s_l.labelY, "!", COL_CONFLICT);
            anyFailed = true;
        }

        optionWidgetDraw(row.widget, dc);

        char text[32];
        optionDisplayText(row, row.widget.value, text, sizeof(text));

        if (text[0]) {
            overlayPanelText(dc, s_l.trailL, top + s_l.labelY, text, COL_VALUE);
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

    if (anyFailed) {
        overlayPanelText(dc, s_l.margin, s_l.footer3Y, FOOTER3, COL_DIM);
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

    for (int i = 0; i < KEY_ROW_COUNT; ++i) {
        int top = keyRowTop(i);

        if (ly < top + s_l.boxTop || ly >= top + s_l.boxBottom) {
            continue;
        }

        if (lx >= s_l.ctrlL && lx < s_l.ctrlR) {
            keybindWidgetBeginCapture(s_keyRows[i].widget);
        } else if (lx >= s_l.trailL && lx < s_l.clearR && s_keyRows[i].allowUnbind) {
            Hotkey unbound = {0, 0};
            keybindWidgetSetBinding(s_keyRows[i].widget, unbound);
            commitKeyRow(s_keyRows[i]);
        }

        overlayPanelMarkDirty(s_panel);
        return true;
    }

    for (int i = 0; i < OPT_ROW_COUNT; ++i) {
        OptionRow &row = s_optRows[i];

        if (!optionWidgetHit(row.widget, lx, ly)) {
            continue;
        }

        if (optionWidgetOnMouseDown(row.widget, lx, ly)) {
            applyOptionRow(row);
        }

        // A switch is done in one click; a slider persists when the drag ends.
        if (row.kind == OPTION_CHOICE) {
            persistOptionRow(row);
        }

        break;
    }

    overlayPanelMarkDirty(s_panel);
    return true; // consumed (inside panel), even if between rows
}

static bool handleDragEnd();

// Returns true when a drag consumed the movement, so the game never sees a
// mouse it would act on while a slider is being dragged.
static bool handleDragMove(int clientX, int clientY, HWND hwnd) {
    if (!optionWidgetDragActive()) {
        return false;
    }

    // The button can come up where this window never hears about it — released
    // outside the game, or swallowed by something else — which would otherwise
    // leave the slider stuck to the cursor. The key state is the authority.
    if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
        handleDragEnd();
        return true;
    }

    int lx = 0;
    int ly = 0;

    // The raw mapping, because a drag that has left the panel still steers the
    // slider it started in — that is what a player expects from a slider.
    if (!overlayPanelMapPointRaw(s_panel, hwnd, clientX, clientY, lx, ly)) {
        return true;
    }

    for (int i = 0; i < OPT_ROW_COUNT; ++i) {
        if (optionWidgetOnMouseMove(s_optRows[i].widget, lx)) {
            applyOptionRow(s_optRows[i]);
            overlayPanelMarkDirty(s_panel);
        }
    }

    return true;
}

static bool handleDragEnd() {
    if (!optionWidgetDragActive()) {
        return false;
    }

    for (int i = 0; i < OPT_ROW_COUNT; ++i) {
        if (optionWidgetOnMouseUp(s_optRows[i].widget)) {
            persistOptionRow(s_optRows[i]);
        }
    }

    overlayPanelMarkDirty(s_panel);
    return true;
}

static void setVisible(bool visible) {
    // Lay out on the way up, not at install: the panel is sized against the
    // backbuffer, which does not exist yet when patches install. This also
    // re-reads every live value, so the panel always opens showing the truth.
    if (visible) {
        applyLayout();
    }

    s_visible = visible;
    s_status = STATUS_NONE;

    if (!visible) {
        for (int i = 0; i < KEY_ROW_COUNT; ++i) {
            keybindWidgetCancelCapture(s_keyRows[i].widget);
        }

        for (int i = 0; i < OPT_ROW_COUNT; ++i) {
            optionWidgetOnMouseUp(s_optRows[i].widget);
        }
    }

    overlayPanelMarkDirty(s_panel);
}

static LRESULT CALLBACK settingsWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // A listening widget owns every message, including this panel's own toggle
    // key — that is what lets the toggle be rebound onto itself.
    for (int i = 0; i < KEY_ROW_COUNT; ++i) {
        if (keybindWidgetOnMessage(s_keyRows[i].widget, msg, wparam, lparam)) {
            commitKeyRow(s_keyRows[i]);
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
        if (msg == WM_MOUSEMOVE) {
            if (handleDragMove((short)LOWORD(lparam), (short)HIWORD(lparam), hwnd)) {
                return 0;
            }
        } else if (
            msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK ||
            msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP
        ) {
            int x = (short)LOWORD(lparam);
            int y = (short)HIWORD(lparam);

            // Act on left-button-DOWN only; swallow the other button events
            // inside the panel so the game never sees a click meant for it.
            if (msg == WM_LBUTTONDOWN) {
                if (handleClick(x, y, hwnd)) {
                    return 0;
                }
            } else {
                if (msg == WM_LBUTTONUP && handleDragEnd()) {
                    return 0;
                }

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

    for (int i = 0; i < KEY_ROW_COUNT; ++i) {
        keybindWidgetInit(s_keyRows[i].widget, s_keyRows[i].get(), 0, 0, 0, 0);
    }

    for (int i = 0; i < OPT_ROW_COUNT; ++i) {
        OptionRow &row = s_optRows[i];

        if (row.kind == OPTION_CHOICE) {
            optionWidgetInitChoice(row.widget, row.get(), row.choices, row.choiceCount, 0, 0, 0, 0);
        } else {
            optionWidgetInitSlider(row.widget, row.get(), row.min, row.max, row.step, 0, 0, 0, 0);
        }
    }

    registerD3DRender(&settingsRender);
}
