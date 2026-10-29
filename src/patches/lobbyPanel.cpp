#include "lobbyPanel.h"
#include "../core/d3dHook.h"
#include "../core/frameTick.h"
#include "../core/keybindWidget.h"
#include "../core/overlayPanel.h"
#include "../core/screens.h"
#include "../core/steamLobby.h"
#include "handshake.h"
#include "matchRules.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>

// See docs/features/lobby-panel.md.
//
// Layout is rebuilt from ScenarioLobbyScreen::draw, reached through a vtable
// slot swap: it runs every frame the lobby is on screen and only then, and it is
// a function boundary, so the x87 stack is empty and the panel quad's float work
// is legal there. Painting and the texture stay on the render callback.
static const uintptr_t LOBBY_DRAW_SLOT_RVA = 0x5e2c64; // Pane vtable 0x9e2c54, slot 4 (draw)
static const uintptr_t LOBBY_DRAW_RVA = 0x29f650; // ScenarioLobbyScreen::draw

static const COLORREF COL_BG = RGB(26, 26, 34);
static const COLORREF COL_EDGE = RGB(90, 90, 110);
static const COLORREF COL_TITLE = RGB(255, 255, 255);
static const COLORREF COL_NAME = RGB(225, 225, 230);
static const COLORREF COL_DIM = RGB(150, 150, 165);
static const COLORREF COL_OK = RGB(150, 220, 150);
static const COLORREF COL_BAD = RGB(255, 140, 140);

static const BYTE PANEL_ALPHA = 230;
static const int TITLE_POINTS = 11;
static const int BODY_POINTS = 10;

// Design pixels at 1080p; scaled through overlayScaleBy. See docs/features/ui-scale.md.
static const int D_EDGE = 16; // gap to the right and top of the screen
static const int D_MARGIN = 10;
static const int D_COL_GAP = 16;
static const int D_ROW_PAD = 3;
static const int D_MIN_W = 220;
static const int D_BUTTON_PAD = 6; // around a rule's state, where the host clicks

static const char *TITLE = "Unofficial Patch";
static const char *STATUS_WAIT = "Waiting for the Steam lobby...";
static const char *STATUS_ALL = "Everyone has the patch";
static const char *STATUS_SOME = "Vanilla rules only - not everyone has it";
static const char *HOST_TAG = "host";
static const char *RULES_HEAD = "Match rules";
static const char *RULE_ON = "on";
static const char *RULE_FIX_ON = "on (fix)";
static const char *RULE_OFF = "off";
static const char *RULE_UNAVAILABLE = "unavailable";
static const char *HOST_HINT = "Click a rule to switch it on or off";

static const int MAX_RULES = 8;

enum Status { STATUS_WAITING, STATUS_EVERYONE, STATUS_NOT_EVERYONE };

// What the panel shows. Rebuilt only when this differs from the last build.
struct Content {
    int count;
    LobbyPlayer players[LOBBY_MAX_MEMBERS];
    Status status;
    int ruleCount;
    MatchRuleView rules[MAX_RULES];
    bool localIsHost;
    int scale;
    int renderW;
    int renderH;
};

struct Layout {
    int lineH;
    int rowH;
    int titleY;
    int rowsTop;
    int statusY;
    int rulesHeadY;
    int rulesTop;
    int hintY;
    int ruleStateX;
    int ruleStateW;
    int buttonPad;
    int nameX;
    int versionX;
    int hostX;
};

static Content s_content = {};
static Layout s_layout = {};
static OverlayPanel s_panel;
static bool s_visible = false; // lowered while the content is rebuilt
static bool s_hidden = false; // the player pressed the toggle
static bool s_built = false;

static WNDPROC s_origWndProc = nullptr;

static Hotkey s_hotkey = {'L', HK_CTRL | HK_SHIFT};
static bool s_installed = false;

typedef void(__attribute__((thiscall)) * DrawFn)(void *self, uint32_t arg);
static DrawFn s_stockDraw = nullptr;

static uintptr_t gameBase() { return (uintptr_t)GetModuleHandleA(NULL); }

static bool playerCompatible(const LobbyPlayer &p) {
    return p.state == PATCH_FOUND && p.protocol == HANDSHAKE_PROTOCOL;
}

static void versionText(const LobbyPlayer &p, char *buf, int cap) {
    if (p.state == PATCH_FOUND && p.protocol == HANDSHAKE_PROTOCOL) {
        snprintf(buf, cap, "%s", p.version);
    } else if (p.state == PATCH_FOUND) {
        snprintf(buf, cap, "%s (incompatible)", p.version);
    } else if (p.state == PATCH_UNREADABLE) {
        snprintf(buf, cap, "unknown");
    } else {
        snprintf(buf, cap, "0.7 or older");
    }
}

static const char *statusText(Status status) {
    if (status == STATUS_EVERYONE) {
        return STATUS_ALL;
    }

    if (status == STATUS_NOT_EVERYONE) {
        return STATUS_SOME;
    }

    return STATUS_WAIT;
}

static const char *ruleStateText(const MatchRuleView &rule) {
    if (!rule.everyoneSupports) {
        return RULE_UNAVAILABLE;
    }

    if (rule.fix) {
        return RULE_FIX_ON;
    }

    if (rule.proposed) {
        return RULE_ON;
    }

    return RULE_OFF;
}

static void readContent(Content &c) {
    memset(&c, 0, sizeof(c));
    c.count = handshakePlayers(c.players, LOBBY_MAX_MEMBERS);
    c.status = STATUS_WAITING;

    if (c.count > 0) {
        c.status = STATUS_EVERYONE;

        for (int i = 0; i < c.count; ++i) {
            if (!playerCompatible(c.players[i])) {
                c.status = STATUS_NOT_EVERYONE;
                break;
            }
        }
    }

    for (int i = 0; i < c.count; ++i) {
        if (c.players[i].isLocal && c.players[i].isOwner) {
            c.localIsHost = true;
        }
    }

    c.ruleCount = matchRulesView(c.rules, MAX_RULES);
    c.scale = overlayScalePercent();
    c.renderW = 1920;
    c.renderH = 1080;
    d3dBackbufferSize(c.renderW, c.renderH);
}

static int wideWidth(HDC dc, const wchar_t *text) {
    SIZE size = {0, 0};
    int n = (int)wcslen(text);

    GetTextExtentPoint32W(dc, text, n, &size);

    return size.cx;
}

static int asciiWidth(HDC dc, const char *text) {
    SIZE size = {0, 0};

    GetTextExtentPoint32A(dc, text, (int)strlen(text), &size);

    return size.cx;
}

static int maxInt(int a, int b) { return a > b ? a : b; }

// Measures against a screen DC: the panel's own bitmap belongs to the render
// thread and may not exist yet, and the metrics are identical (same font).
// **Does float work** (overlayPanelSetBounds), so it is only called from the
// draw hook.
static void buildLayout() {
    int scale = s_content.scale;
    HFONT title = overlayPanelFont(TITLE_POINTS, true, scale);
    HFONT body = overlayPanelFont(BODY_POINTS, false, scale);
    HDC dc = GetDC(NULL);
    HFONT prev = (HFONT)SelectObject(dc, body);
    int margin = overlayScaleBy(D_MARGIN, scale);
    int gap = overlayScaleBy(D_COL_GAP, scale);
    int nameW = 0;
    int versionW = 0;
    int hostW = asciiWidth(dc, HOST_TAG);
    int textW = asciiWidth(dc, statusText(s_content.status));
    int titleW = 0;
    int ruleLabelW = asciiWidth(dc, RULES_HEAD);
    int ruleStateW = 0;
    int hintW = asciiWidth(dc, HOST_HINT);
    int panelW = 0;
    int panelH = 0;
    char version[40];

    for (int i = 0; i < s_content.count; ++i) {
        const LobbyPlayer &p = s_content.players[i];

        if (p.name[0]) {
            nameW = maxInt(nameW, wideWidth(dc, p.name));
        } else {
            nameW = maxInt(nameW, asciiWidth(dc, "?"));
        }

        versionText(p, version, sizeof(version));
        versionW = maxInt(versionW, asciiWidth(dc, version));
    }

    for (int i = 0; i < s_content.ruleCount; ++i) {
        ruleLabelW = maxInt(ruleLabelW, asciiWidth(dc, s_content.rules[i].label));
        ruleStateW = maxInt(ruleStateW, asciiWidth(dc, ruleStateText(s_content.rules[i])));
    }

    s_layout.lineH = overlayPanelLineHeight(body);
    SelectObject(dc, title);
    titleW = asciiWidth(dc, TITLE);
    SelectObject(dc, prev);
    ReleaseDC(NULL, dc);

    s_layout.rowH = s_layout.lineH + overlayScaleBy(D_ROW_PAD, scale);
    s_layout.titleY = margin;
    s_layout.rowsTop = s_layout.titleY + overlayPanelLineHeight(title) + overlayScaleBy(6, scale);
    s_layout.statusY =
        s_layout.rowsTop + s_content.count * s_layout.rowH + overlayScaleBy(6, scale);
    s_layout.nameX = margin;
    s_layout.versionX = s_layout.nameX + nameW + gap;
    s_layout.hostX = s_layout.versionX + versionW + gap;
    s_layout.rulesHeadY = s_layout.statusY + s_layout.lineH + overlayScaleBy(10, scale);
    s_layout.rulesTop = s_layout.rulesHeadY + s_layout.rowH;
    s_layout.buttonPad = overlayScaleBy(D_BUTTON_PAD, scale);
    s_layout.ruleStateX = s_layout.nameX + ruleLabelW + gap + s_layout.buttonPad;
    s_layout.ruleStateW = ruleStateW;
    s_layout.hintY = s_layout.rulesTop + s_content.ruleCount * s_layout.rowH;

    panelW = s_layout.hostX + hostW + margin;
    panelW = maxInt(panelW, s_layout.ruleStateX + ruleStateW + s_layout.buttonPad + margin);
    panelW = maxInt(panelW, margin + textW + margin);
    panelW = maxInt(panelW, margin + titleW + margin);
    panelW = maxInt(panelW, overlayScaleBy(D_MIN_W, scale));
    panelH = s_layout.statusY + s_layout.lineH + margin;

    if (s_content.ruleCount > 0) {
        panelH = s_layout.hintY + margin;
    }

    if (s_content.ruleCount > 0 && s_content.localIsHost) {
        panelW = maxInt(panelW, margin + hintW + margin);
        panelH = s_layout.hintY + s_layout.lineH + margin;
    }

    if (panelW > s_content.renderW) {
        panelW = s_content.renderW;
    }

    if (panelH > s_content.renderH) {
        panelH = s_content.renderH;
    }

    int edge = overlayScaleBy(D_EDGE, scale);
    overlayPanelSetBounds(s_panel, s_content.renderW - panelW - edge, edge, panelW, panelH);
}

static void paintPanel(HDC dc) {
    char version[40];

    overlayPanelFill(dc, 0, 0, s_panel.w, s_panel.h, COL_BG);
    overlayPanelFrame(dc, 0, 0, s_panel.w, s_panel.h, COL_EDGE);

    SelectObject(dc, overlayPanelFont(TITLE_POINTS, true, s_content.scale));
    overlayPanelText(dc, s_layout.nameX, s_layout.titleY, TITLE, COL_TITLE);

    SelectObject(dc, overlayPanelFont(BODY_POINTS, false, s_content.scale));

    for (int i = 0; i < s_content.count; ++i) {
        const LobbyPlayer &p = s_content.players[i];
        int y = s_layout.rowsTop + i * s_layout.rowH;
        COLORREF versionColor = COL_BAD;

        SetTextColor(dc, COL_NAME);

        if (p.name[0]) {
            TextOutW(dc, s_layout.nameX, y, p.name, (int)wcslen(p.name));
        } else {
            TextOutW(dc, s_layout.nameX, y, L"?", 1);
        }

        if (playerCompatible(p)) {
            versionColor = COL_OK;
        }

        versionText(p, version, sizeof(version));
        overlayPanelText(dc, s_layout.versionX, y, version, versionColor);

        if (p.isOwner) {
            overlayPanelText(dc, s_layout.hostX, y, HOST_TAG, COL_DIM);
        }
    }

    COLORREF statusColor = COL_DIM;

    if (s_content.status == STATUS_EVERYONE) {
        statusColor = COL_OK;
    } else if (s_content.status == STATUS_NOT_EVERYONE) {
        statusColor = COL_BAD;
    }

    overlayPanelText(
        dc, s_layout.nameX, s_layout.statusY, statusText(s_content.status), statusColor
    );

    if (s_content.ruleCount == 0) {
        return;
    }

    overlayPanelText(dc, s_layout.nameX, s_layout.rulesHeadY, RULES_HEAD, COL_TITLE);

    for (int i = 0; i < s_content.ruleCount; ++i) {
        const MatchRuleView &rule = s_content.rules[i];
        int y = s_layout.rulesTop + i * s_layout.rowH;
        COLORREF stateColor = COL_DIM;

        if (!rule.everyoneSupports) {
            stateColor = COL_BAD;
        } else if (rule.proposed) {
            stateColor = COL_OK;
        }

        overlayPanelText(dc, s_layout.nameX, y, rule.label, COL_NAME);
        overlayPanelText(dc, s_layout.ruleStateX, y, ruleStateText(rule), stateColor);

        // Only the host can switch a rule, and only one everyone can run, so
        // only those get a button shape.
        if (s_content.localIsHost && rule.everyoneSupports && !rule.fix) {
            overlayPanelFrame(
                dc, s_layout.ruleStateX - s_layout.buttonPad, y - 1,
                s_layout.ruleStateX + s_layout.ruleStateW + s_layout.buttonPad,
                y + s_layout.rowH - 1, COL_EDGE
            );
        }
    }

    if (s_content.localIsHost) {
        overlayPanelText(dc, s_layout.nameX, s_layout.hintY, HOST_HINT, COL_DIM);
    }
}

// Runs in place of ScenarioLobbyScreen::draw (thiscall, one stack argument).
static void __attribute__((thiscall)) lobbyDrawHook(void *self, uint32_t arg) {
    Content now;

    s_stockDraw(self, arg);

    readContent(now);

    if (!s_built || memcmp(&now, &s_content, sizeof(now)) != 0) {
        // Lower visibility before rebuilding, so the render callback never paints
        // a half-built panel.
        s_visible = false;
        s_content = now;
        buildLayout();
        s_built = true;
        overlayPanelMarkDirty(s_panel);
    }

    s_visible = true;
}

static bool panelOnScreen() { return s_visible && !s_hidden && lobbyScreenUp(); }

// The rule whose row contains panel point (x, y), or -1.
static int ruleAt(int x, int y) {
    int right = s_layout.ruleStateX + s_layout.ruleStateW + s_layout.buttonPad;

    if (x < s_layout.nameX || x >= right) {
        return -1;
    }

    for (int i = 0; i < s_content.ruleCount; ++i) {
        int top = s_layout.rulesTop + i * s_layout.rowH;

        if (y >= top && y < top + s_layout.rowH) {
            return i;
        }
    }

    return -1;
}

// Clicks on the panel never reach the lobby beneath it. A host's click on a rule
// switches it; matchRuleToggle refuses everyone else, and rules someone cannot run.
static LRESULT CALLBACK lobbyPanelWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    bool click = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK ||
                 msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP;

    if (click && panelOnScreen()) {
        int lx = 0;
        int ly = 0;

        if (overlayPanelMapPoint(
                s_panel, hwnd, (short)LOWORD(lparam), (short)HIWORD(lparam), lx, ly
            )) {
            if (msg == WM_LBUTTONDOWN) {
                int rule = ruleAt(lx, ly);

                if (rule >= 0) {
                    matchRuleToggle(rule);
                }
            }

            return 0;
        }
    }

    return CallWindowProcW(s_origWndProc, hwnd, msg, wparam, lparam);
}

// From the first render callback, as every panel does: the device window exists
// by then, and registration order then decides subclass order. The settings
// panel is installed later, so it sits outside this one and sees input first.
static void ensureSubclass() {
    if (s_origWndProc) {
        return;
    }

    HWND hwnd = (HWND)d3dDeviceWindow();

    if (!hwnd) {
        return;
    }

    s_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)lobbyPanelWndProc);
}

static void lobbyPanelTick() {
    static bool prevDown = false;
    bool down = hotkeyHeld(s_hotkey);

    if (down && !prevDown && !keybindCaptureActive() && gameWindowFocused() && lobbyScreenUp()) {
        s_hidden = !s_hidden;
    }

    prevDown = down;
}

static void lobbyPanelRender(IDirect3DDevice9 *device) {
    ensureSubclass();

    if (!s_visible || s_hidden || !lobbyScreenUp()) {
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

// The slot holds a relocated absolute address, so compare against the rebuilt
// one rather than the bytes on disk.
static bool swapDrawSlot() {
    uintptr_t base = gameBase();
    uintptr_t *slot = (uintptr_t *)(base + LOBBY_DRAW_SLOT_RVA);
    uintptr_t hook = (uintptr_t)&lobbyDrawHook;
    DWORD oldProtect = 0;

    if (*slot != base + LOBBY_DRAW_RVA) {
        return false;
    }

    s_stockDraw = (DrawFn)(base + LOBBY_DRAW_RVA);

    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect)) {
        return false;
    }

    *slot = hook;
    VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);

    return true;
}

void installLobbyPanel() {
    s_hotkey = hotkeyLoad("hotkeys", "LobbyPanel", s_hotkey);

    // Real bounds are set by the draw hook, once there is a backbuffer to size
    // against.
    overlayPanelInit(s_panel, 0, 0, 1, 1);
    overlayPanelSetAlpha(s_panel, PANEL_ALPHA);

    if (!swapDrawSlot()) {
        return;
    }

    s_installed = true;
    registerD3DRender(lobbyPanelRender);
    registerFrameTick(lobbyPanelTick);
}

Hotkey lobbyPanelBinding() { return s_hotkey; }

void lobbyPanelSetBinding(const Hotkey &hk) { s_hotkey = hk; }

bool lobbyPanelInstalled() { return s_installed; }
