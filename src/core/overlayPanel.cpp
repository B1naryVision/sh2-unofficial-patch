#include "overlayPanel.h"
#include "d3dHook.h"
#include <cstdint>
#include <cstring>
#include <d3d9.h>

static const DWORD PANEL_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// ── font cache ──────────────────────────────────────────────────────────────────
struct FontEntry {
    int pointSize;
    bool bold;
    HFONT font;
};

static FontEntry s_fonts[8] = {};
static int s_fontCount = 0;

HFONT overlayPanelFont(int pointSize, bool bold) {
    for (int i = 0; i < s_fontCount; ++i) {
        if (s_fonts[i].pointSize == pointSize && s_fonts[i].bold == bold) {
            return s_fonts[i].font;
        }
    }

    if (s_fontCount >= (int)(sizeof(s_fonts) / sizeof(s_fonts[0]))) {
        return nullptr;
    }

    HDC screen = GetDC(nullptr);
    int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    int weight = FW_NORMAL;

    if (bold) {
        weight = FW_BOLD;
    }

    HFONT font = CreateFontW(
        -MulDiv(pointSize, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );

    ReleaseDC(nullptr, screen);

    if (!font) {
        return nullptr;
    }

    s_fonts[s_fontCount].pointSize = pointSize;
    s_fonts[s_fontCount].bold = bold;
    s_fonts[s_fontCount].font = font;
    ++s_fontCount;
    return font;
}

// ── GDI helpers ─────────────────────────────────────────────────────────────────
static int toWide(const char *s, wchar_t *out, int outLen) {
    int k = 0;

    for (const char *p = s; *p && k < outLen - 1; ++p) {
        out[k++] = (wchar_t)(unsigned char)*p;
    }

    out[k] = 0;
    return k;
}

void overlayPanelFill(HDC dc, int l, int t, int r, int b, COLORREF color) {
    RECT rc = {l, t, r, b};
    HBRUSH br = CreateSolidBrush(color);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

void overlayPanelFrame(HDC dc, int l, int t, int r, int b, COLORREF color) {
    RECT rc = {l, t, r, b};
    HBRUSH br = CreateSolidBrush(color);
    FrameRect(dc, &rc, br);
    DeleteObject(br);
}

void overlayPanelText(HDC dc, int x, int y, const char *text, COLORREF color) {
    wchar_t w[128];
    int len = toWide(text, w, 128);
    SetTextColor(dc, color);
    TextOutW(dc, x, y, w, len);
}

void overlayPanelTextCentered(HDC dc, int l, int r, int y, const char *text, COLORREF color) {
    wchar_t w[128];
    int len = toWide(text, w, 128);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, w, len, &sz);
    SetTextColor(dc, color);
    TextOutW(dc, l + (r - l - sz.cx) / 2, y, w, len);
}

void overlayPanelTextRight(HDC dc, int right, int y, const char *text, COLORREF color) {
    wchar_t w[128];
    int len = toWide(text, w, 128);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, w, len, &sz);
    SetTextColor(dc, color);
    TextOutW(dc, right - sz.cx, y, w, len);
}

// ── panel ───────────────────────────────────────────────────────────────────────
void overlayPanelInit(OverlayPanel &p, int x, int y, int w, int h) {
    memset(&p, 0, sizeof(p));
    p.alpha = 255;
    overlayPanelSetBounds(p, x, y, w, h);
}

void overlayPanelSetAlpha(OverlayPanel &p, BYTE alpha) {
    if (p.alpha != alpha) {
        p.alpha = alpha;
        p.dirty = true;
    }
}

void overlayPanelSetBounds(OverlayPanel &p, int x, int y, int w, int h) {
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    p.dirty = true;

    // The only float arithmetic in this file, deliberately kept off the render
    // thread. The -0.5f is the D3D9 texel/pixel alignment offset.
    float x0 = (float)x - 0.5f;
    float y0 = (float)y - 0.5f;
    float x1 = (float)(x + w) - 0.5f;
    float y1 = (float)(y + h) - 0.5f;

    const float u[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    const float v[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const float px[4] = {x0, x1, x0, x1};
    const float py[4] = {y0, y0, y1, y1};

    for (int i = 0; i < 4; ++i) {
        p.quad[i].x = px[i];
        p.quad[i].y = py[i];
        p.quad[i].z = 0.0f;
        p.quad[i].rhw = 1.0f;
        p.quad[i].color = 0xFFFFFFFF;
        p.quad[i].u = u[i];
        p.quad[i].v = v[i];
    }
}

void overlayPanelMarkDirty(OverlayPanel &p) { p.dirty = true; }

void overlayPanelEnsureDevice(OverlayPanel &p, IDirect3DDevice9 *device) {
    if (p.tex && p.texDevice == device && p.texW == p.w && p.texH == p.h) {
        return;
    }

    if (p.tex) {
        p.tex->Release();
        p.tex = nullptr;
    }

    // D3DPOOL_MANAGED survives a device reset, so alt-tab needs no handling.
    if (SUCCEEDED(
            device->CreateTexture(p.w, p.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &p.tex, nullptr)
        )) {
        p.texDevice = device;
        p.texW = p.w;
        p.texH = p.h;
        p.dirty = true;
    }
}

HDC overlayPanelBeginPaint(OverlayPanel &p) {
    if (p.dc && p.dcW == p.w && p.dcH == p.h) {
        return p.dc;
    }

    // A resize invalidates the bitmap; drop it and build one at the new size.
    if (p.dc) {
        DeleteDC(p.dc);
        p.dc = nullptr;
    }

    if (p.dib) {
        DeleteObject(p.dib);
        p.dib = nullptr;
        p.bits = nullptr;
    }

    HDC screen = GetDC(nullptr);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = p.w;
    bmi.bmiHeader.biHeight = -p.h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    p.dc = CreateCompatibleDC(screen);
    p.dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &p.bits, nullptr, 0);
    ReleaseDC(nullptr, screen);

    if (!p.dc || !p.dib) {
        return nullptr;
    }

    SelectObject(p.dc, p.dib);
    SetBkMode(p.dc, TRANSPARENT);
    p.dcW = p.w;
    p.dcH = p.h;
    return p.dc;
}

void overlayPanelEndPaint(OverlayPanel &p) {
    if (!p.bits) {
        return;
    }

    // GDI leaves the alpha byte at 0; stamp the panel's opacity into it.
    uint32_t *px = (uint32_t *)p.bits;
    uint32_t alpha = (uint32_t)p.alpha << 24;

    for (int i = 0; i < p.w * p.h; ++i) {
        px[i] = (px[i] & 0x00FFFFFFu) | alpha;
    }

    // A resize between paint and upload would copy the wrong number of rows.
    // Leave the panel dirty so it repaints once the texture has caught up.
    if (!p.tex || p.texW != p.w || p.texH != p.h) {
        return;
    }

    D3DLOCKED_RECT lr;

    if (FAILED(p.tex->LockRect(0, &lr, nullptr, 0))) {
        return;
    }

    for (int row = 0; row < p.h; ++row) {
        memcpy((BYTE *)lr.pBits + row * lr.Pitch, (BYTE *)p.bits + row * p.w * 4, p.w * 4);
    }

    p.tex->UnlockRect(0);
    p.dirty = false;
}

void overlayPanelDraw(OverlayPanel &p, IDirect3DDevice9 *device) {
    if (!p.tex) {
        return;
    }

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
    device->SetTexture(0, p.tex);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetFVF(PANEL_FVF);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, p.quad, sizeof(OverlayPanelVtx));

    saved->Apply();
    saved->Release();
}

bool overlayPanelMapPoint(
    const OverlayPanel &p, HWND hwnd, int clientX, int clientY, int &lx, int &ly
) {
    int renderW = 0;
    int renderH = 0;

    if (!d3dBackbufferSize(renderW, renderH)) {
        return false;
    }

    RECT cr;

    if (!GetClientRect(hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) {
        return false;
    }

    // Client -> backbuffer coords (handles windowed scaling), then panel-local.
    lx = clientX * renderW / cr.right - p.x;
    ly = clientY * renderH / cr.bottom - p.y;
    return lx >= 0 && ly >= 0 && lx < p.w && ly < p.h;
}
