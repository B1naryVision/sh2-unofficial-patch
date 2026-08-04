#include "overlay.h"
#include "../../core/config.h"
#include "../../core/d3dHook.h"
#include "autoMarket.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <d3d9.h>
#include <windows.h>

// Panel drawn over the game from inside the EndScene hook: a GDI bitmap (text
// rendered with GDI, like the endgame-stats overlay) uploaded to a D3D texture
// and drawn as one alpha-blended quad. Goods are grouped under category headers;
// Min/Max are editable cells selectable by keyboard or mouse click. Position and
// size are compile-time constants so the render path does no runtime x87 float
// arithmetic — safe to call from the mid-function EndScene detour without fxsave.

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
static int s_hotkeyVk = 0;
static bool s_visible = false;
static bool s_dirty = true;
static int s_selRow = 0; // selected good row
static int s_selField = 0; // 0 = Min, 1 = Max
static bool s_freshEntry = true; // next digit replaces (set on any selection change)
static int s_presetSel = 0; // highlighted preset in the picklist

// Per-good panel-local Y of each row (constant layout, computed once).
static int s_goodY[64] = {};
static bool s_layoutDone = false;

// Backbuffer size, captured each frame, to map window-client click coords.
static int s_renderW = 0;
static int s_renderH = 0;

// GDI
static HDC s_memDC = nullptr;
static HBITMAP s_dib = nullptr;
static void *s_dibBits = nullptr;
static HFONT s_font = nullptr;
static HFONT s_fontBold = nullptr;
static HFONT s_fontTitle = nullptr;

// D3D
static IDirect3DTexture9 *s_tex = nullptr;
static IDirect3DDevice9 *s_texDevice = nullptr;

// Input (window subclass)
static WNDPROC s_origWndProc = nullptr;

struct PanelVtx {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

static const DWORD PANEL_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

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

// ── GDI panel bitmap ────────────────────────────────────────────────────────────
static void ensureGdi() {
    if (s_memDC) {
        return;
    }

    HDC screen = GetDC(nullptr);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = PANEL_W;
    bmi.bmiHeader.biHeight = -PANEL_H; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s_memDC = CreateCompatibleDC(screen);
    s_dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &s_dibBits, nullptr, 0);
    SelectObject(s_memDC, s_dib);

    int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    s_font = CreateFontW(
        -MulDiv(12, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );
    s_fontBold = CreateFontW(
        -MulDiv(12, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );
    s_fontTitle = CreateFontW(
        -MulDiv(16, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );

    ReleaseDC(nullptr, screen);
}

static void drawTextA(int x, int y, const char *s, COLORREF color) {
    wchar_t w[48];
    int k = 0;

    for (const char *p = s; *p && k < 47; ++p) {
        w[k++] = (wchar_t)(unsigned char)*p;
    }

    w[k] = 0;
    SetTextColor(s_memDC, color);
    TextOutW(s_memDC, x, y, w, k);
}

static void fillRect(int l, int t, int r, int b, COLORREF color) {
    RECT rc = {l, t, r, b};
    HBRUSH br = CreateSolidBrush(color);
    FillRect(s_memDC, &rc, br);
    DeleteObject(br);
}

static void frameRect(int l, int t, int r, int b, COLORREF color) {
    RECT rc = {l, t, r, b};
    HBRUSH br = CreateSolidBrush(color);
    FrameRect(s_memDC, &rc, br);
    DeleteObject(br);
}

// Draws one editable value cell; the selected cell gets a bright fill + border.
static void drawFieldCell(int x, int y, int value, bool selected) {
    int t = y - 1;
    int b = y + ROW_H - 2;

    if (selected) {
        fillRect(x, t, x + FIELD_W, b, RGB(64, 78, 116));
        frameRect(x, t, x + FIELD_W, b, RGB(255, 205, 70));
    } else {
        frameRect(x, t, x + FIELD_W, b, RGB(58, 58, 74));
    }

    wchar_t w[16];
    int k = _snwprintf(w, 16, L"%d", value);

    if (k < 0) {
        k = 0;
    }

    SetTextColor(s_memDC, selected ? RGB(255, 255, 255) : RGB(200, 200, 210));
    // Right-align inside the cell.
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(s_memDC, w, k, &sz);
    TextOutW(s_memDC, x + FIELD_W - 6 - sz.cx, y, w, k);
}

static void drawTextCentered(int l, int r, int y, const char *s, COLORREF color) {
    wchar_t w[48];
    int k = 0;

    for (const char *p = s; *p && k < 47; ++p) {
        w[k++] = (wchar_t)(unsigned char)*p;
    }

    w[k] = 0;
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(s_memDC, w, k, &sz);
    SetTextColor(s_memDC, color);
    TextOutW(s_memDC, l + (r - l - sz.cx) / 2, y, w, k);
}

// Draws the preset picklist: "Preset:  < name >  [Apply]".
static void drawPresetBar() {
    int t = PRESET_Y - 2;
    int b = PRESET_Y + 18;
    int count = autoMarketPresetCount();

    if (count > 0 && s_presetSel >= count) {
        s_presetSel = 0;
    }

    drawTextA(COL_NAME, PRESET_Y, "Preset:", RGB(180, 180, 195));

    frameRect(PRESET_PREV_L, t, PRESET_PREV_R, b, RGB(90, 90, 110));
    drawTextCentered(PRESET_PREV_L, PRESET_PREV_R, PRESET_Y, "<", RGB(220, 220, 230));
    frameRect(PRESET_NEXT_L, t, PRESET_NEXT_R, b, RGB(90, 90, 110));
    drawTextCentered(PRESET_NEXT_L, PRESET_NEXT_R, PRESET_Y, ">", RGB(220, 220, 230));

    frameRect(PRESET_NAME_L, t, PRESET_NAME_R, b, RGB(58, 58, 74));
    const char *name = count > 0 ? autoMarketPresetName(s_presetSel) : "(no presets)";
    drawTextCentered(
        PRESET_NAME_L, PRESET_NAME_R, PRESET_Y, name,
        count > 0 ? RGB(255, 235, 150) : RGB(120, 120, 130)
    );

    bool enabled = count > 0;
    fillRect(PRESET_APPLY_L, t, PRESET_APPLY_R, b, enabled ? RGB(48, 70, 48) : RGB(40, 40, 50));
    frameRect(PRESET_APPLY_L, t, PRESET_APPLY_R, b, enabled ? RGB(120, 200, 120) : RGB(70, 70, 84));
    drawTextCentered(
        PRESET_APPLY_L, PRESET_APPLY_R, PRESET_Y, "Apply",
        enabled ? RGB(220, 255, 220) : RGB(110, 110, 120)
    );
}

static void renderPanelBitmap() {
    ensureGdi();
    ensureLayout();

    RECT full = {0, 0, PANEL_W, PANEL_H};
    HBRUSH bg = CreateSolidBrush(RGB(26, 26, 34));
    FillRect(s_memDC, &full, bg);
    DeleteObject(bg);
    frameRect(0, 0, PANEL_W, PANEL_H, RGB(90, 90, 110));

    SetBkMode(s_memDC, TRANSPARENT);

    SelectObject(s_memDC, s_fontTitle);
    drawTextA(COL_NAME, TITLE_Y, "Auto-Market", RGB(255, 255, 255));

    SelectObject(s_memDC, s_font);
    drawPresetBar();
    drawTextA(MIN_X + 4, COLHDR_Y, "Min", RGB(150, 150, 165));
    drawTextA(MAX_X + 4, COLHDR_Y, "Max", RGB(150, 150, 165));

    int count = autoMarketGoodCount();
    const char *lastCat = nullptr;

    for (int i = 0; i < count; ++i) {
        int y = s_goodY[i];
        int id = autoMarketGoodId(i);
        const char *cat = autoMarketGoodCategory(i);

        // Category header above the first good of each category.
        if (!lastCat || strcmp(cat, lastCat) != 0) {
            int cy = y - CAT_H;
            fillRect(4, cy + 2, PANEL_W - 4, cy + CAT_H - 1, RGB(40, 44, 58));
            SelectObject(s_memDC, s_fontBold);
            drawTextA(COL_NAME, cy + 3, cat, RGB(150, 200, 255));
            SelectObject(s_memDC, s_font);
            lastCat = cat;
        }

        if (i == s_selRow) {
            fillRect(4, y - 1, PANEL_W - 4, y + ROW_H - 2, RGB(44, 50, 68));
        }

        drawTextA(COL_NAME, y, autoMarketGoodName(i), RGB(225, 225, 230));
        drawFieldCell(MIN_X, y, autoMarketGetMin(id), i == s_selRow && s_selField == 0);
        drawFieldCell(MAX_X, y, autoMarketGetMax(id), i == s_selRow && s_selField == 1);
    }

    drawTextA(
        COL_NAME, PANEL_H - 32, "Click/arrows select - type set - Del clear", RGB(140, 140, 150)
    );
    drawTextA(
        COL_NAME, PANEL_H - 18, "PgUp/PgDn preset - Enter apply - Esc close", RGB(140, 140, 150)
    );

    // GDI leaves the alpha byte at 0; make the whole panel opaque.
    uint32_t *px = (uint32_t *)s_dibBits;

    for (int i = 0; i < PANEL_W * PANEL_H; ++i) {
        px[i] |= 0xFF000000u;
    }

    s_dirty = false;
}

// ── D3D texture + draw ──────────────────────────────────────────────────────────
static void ensureTexture(IDirect3DDevice9 *device) {
    if (s_tex && s_texDevice == device) {
        return;
    }

    if (s_tex) {
        s_tex->Release();
        s_tex = nullptr;
    }

    if (SUCCEEDED(device->CreateTexture(
            PANEL_W, PANEL_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_tex, nullptr
        ))) {
        s_texDevice = device;
        s_dirty = true;
    }
}

static void uploadTexture() {
    if (!s_tex) {
        return;
    }

    D3DLOCKED_RECT lr;

    if (FAILED(s_tex->LockRect(0, &lr, nullptr, 0))) {
        return;
    }

    for (int row = 0; row < PANEL_H; ++row) {
        memcpy(
            (BYTE *)lr.pBits + row * lr.Pitch, (BYTE *)s_dibBits + row * PANEL_W * 4, PANEL_W * 4
        );
    }

    s_tex->UnlockRect(0);
}

static void drawPanel(IDirect3DDevice9 *device) {
    static const float X0 = (float)PANEL_X - 0.5f;
    static const float Y0 = (float)PANEL_Y - 0.5f;
    static const float X1 = (float)(PANEL_X + PANEL_W) - 0.5f;
    static const float Y1 = (float)(PANEL_Y + PANEL_H) - 0.5f;

    PanelVtx quad[4] = {
        {X0, Y0, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f},
        {X1, Y0, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f},
        {X0, Y1, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f},
        {X1, Y1, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f},
    };

    IDirect3DStateBlock9 *saved = nullptr;

    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved))) {
        return;
    }

    device->SetPixelShader(nullptr);
    device->SetVertexShader(nullptr);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetTexture(0, s_tex);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetFVF(PANEL_FVF);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(PanelVtx));

    saved->Apply();
    saved->Release();
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

    s_dirty = true;
}

// Moves selection; a fresh selection means the next digit types from scratch.
static void selectCell(int row, int field) {
    int count = autoMarketGoodCount();
    s_selRow = (row % count + count) % count;
    s_selField = field & 1;
    s_freshEntry = true;
    s_dirty = true;
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
    s_dirty = true;
}

static void applyCurrentPreset() {
    if (autoMarketPresetCount() <= 0) {
        return;
    }

    autoMarketApplyPreset(s_presetSel);
    s_dirty = true;
}

static bool handleEditKey(int vk) {
    int id = autoMarketGoodId(s_selRow);

    if (vk == VK_ESCAPE) {
        s_visible = false;
        s_dirty = true;
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

// Maps a window-client point to panel-local coords. Returns true if inside the
// panel (side-effect-free, so it can gate swallowing without acting).
static bool mapToPanel(int clientX, int clientY, HWND hwnd, int &lx, int &ly) {
    if (s_renderW <= 0 || s_renderH <= 0) {
        return false;
    }

    RECT cr;

    if (!GetClientRect(hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) {
        return false;
    }

    // Client -> backbuffer coords (handles windowed scaling), then panel-local.
    lx = clientX * s_renderW / cr.right - PANEL_X;
    ly = clientY * s_renderH / cr.bottom - PANEL_Y;
    return lx >= 0 && ly >= 0 && lx < PANEL_W && ly < PANEL_H;
}

// Performs the click action (button-DOWN only). Returns true if inside the panel.
static bool handleClick(int clientX, int clientY, HWND hwnd) {
    int lx = 0;
    int ly = 0;

    if (!mapToPanel(clientX, clientY, hwnd, lx, ly)) {
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

        if (vk == s_hotkeyVk) {
            s_visible = !s_visible;
            s_dirty = true;
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

                if (mapToPanel(x, y, hwnd, lx, ly)) {
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

    D3DVIEWPORT9 vp;

    if (SUCCEEDED(device->GetViewport(&vp))) {
        s_renderW = (int)vp.Width;
        s_renderH = (int)vp.Height;
    }

    ensureTexture(device);

    if (s_dirty) {
        renderPanelBitmap();
        uploadTexture();
    }

    drawPanel(device);
}

void autoMarketOverlayReset() {
    s_visible = false;
    s_selRow = 0;
    s_selField = 0;
    s_freshEntry = true;
    s_dirty = true;
}

bool installAutoMarketOverlay() {
    // Default toggle: the ` / ~ key (VK_OEM_3), which Stronghold 2 does not use
    // (F1–F12 are taunts). Configurable via [hotkeys] AutoMarketPanel.
    s_hotkeyVk = configHotkey("AutoMarketPanel", VK_OEM_3);

    if (s_hotkeyVk == 0) {
        return false;
    }

    registerD3DRender(&overlayRender);
    return true;
}
