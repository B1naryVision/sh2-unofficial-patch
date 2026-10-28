#include "lobbyPanel.h"
#include "../core/d3dHook.h"
#include "../core/frameTick.h"
#include "../core/keybindWidget.h"
#include "../core/overlayPanel.h"
#include "../core/steamLobby.h"
#include "handshake.h"
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
static const uintptr_t LOBBY_SCREEN_PTR_RVA = 0xec5f28; // registerWindow("scenario lobby screen")
static const uintptr_t LOBBY_DRAW_SLOT_RVA = 0x5e2c64; // Pane vtable 0x9e2c54, slot 4 (draw)
static const uintptr_t LOBBY_DRAW_RVA = 0x29f650; // ScenarioLobbyScreen::draw
static const uintptr_t PANE_PARENT_OFF = 0x80; // non-null only for the screen on display

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

static const char *TITLE = "Unofficial Patch";
static const char *STATUS_WAIT = "Waiting for the Steam lobby...";
static const char *STATUS_ALL = "Everyone has the patch";
static const char *STATUS_SOME = "Vanilla rules only - not everyone has it";
static const char *HOST_TAG = "host";

enum Status { STATUS_WAITING, STATUS_EVERYONE, STATUS_NOT_EVERYONE };

// What the panel shows. Rebuilt only when this differs from the last build.
struct Content {
    int count;
    LobbyPlayer players[LOBBY_MAX_MEMBERS];
    Status status;
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

static Hotkey s_hotkey = {'L', HK_CTRL | HK_SHIFT};
static bool s_installed = false;

typedef void(__attribute__((thiscall)) * DrawFn)(void *self, uint32_t arg);
static DrawFn s_stockDraw = nullptr;

static uintptr_t gameBase() { return (uintptr_t)GetModuleHandleA(NULL); }

static bool lobbyScreenUp() {
    uintptr_t screen = *(uintptr_t *)(gameBase() + LOBBY_SCREEN_PTR_RVA);

    if (!screen) {
        return false;
    }

    return *(uintptr_t *)(screen + PANE_PARENT_OFF) != 0;
}

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

    panelW = s_layout.hostX + hostW + margin;
    panelW = maxInt(panelW, margin + textW + margin);
    panelW = maxInt(panelW, margin + titleW + margin);
    panelW = maxInt(panelW, overlayScaleBy(D_MIN_W, scale));
    panelH = s_layout.statusY + s_layout.lineH + margin;

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

static void lobbyPanelTick() {
    static bool prevDown = false;
    bool down = hotkeyHeld(s_hotkey);

    if (down && !prevDown && !keybindCaptureActive() && gameWindowFocused() && lobbyScreenUp()) {
        s_hidden = !s_hidden;
    }

    prevDown = down;
}

static void lobbyPanelRender(IDirect3DDevice9 *device) {
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
