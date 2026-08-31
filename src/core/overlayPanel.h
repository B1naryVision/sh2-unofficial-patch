#pragma once
#include <windows.h>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

// The plumbing behind a panel drawn over the game: a GDI bitmap the caller
// paints with ordinary GDI calls, uploaded to a D3D texture and drawn as one
// alpha-blended pre-transformed quad from inside the EndScene hook.
//
// The render path must stay float-free (the EndScene detour is a mid-function
// hook that may have live x87 state), so the quad's vertices are built **once**
// by overlayPanelInit and only copied afterwards. Call overlayPanelInit from an
// install function, never from the render callback.
//
// See docs/features/keybinding.md and docs/features/auto-market.md.

struct OverlayPanelVtx {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

struct OverlayPanel {
    int x, y, w, h;
    OverlayPanelVtx quad[4]; // prebuilt; the draw path never computes a float
    BYTE alpha; // whole-panel opacity, 255 = opaque

    HDC dc; // GDI memory DC, created on the first paint
    HBITMAP dib;
    void *bits;
    int dcW, dcH; // size the DIB was created at

    IDirect3DTexture9 *tex;
    IDirect3DDevice9 *texDevice;
    int texW, texH; // size the texture was created at

    bool dirty; // repaint needed
};

void overlayPanelInit(OverlayPanel &p, int x, int y, int w, int h);

// Moves and/or resizes the panel, rebuilding the quad. **Does float
// arithmetic**, so call it from an install function or a function-prologue hook
// — never from a frame-tick or render callback, where the interrupted code may
// hold live x87 state. The GDI bitmap and texture are recreated lazily on the
// render thread when the size changes, so no D3D object is touched here.
void overlayPanelSetBounds(OverlayPanel &p, int x, int y, int w, int h);

// Whole-panel opacity (255 = opaque, the default). Applied when the bitmap is
// uploaded, so it takes effect on the next repaint.
void overlayPanelSetAlpha(OverlayPanel &p, BYTE alpha);

void overlayPanelMarkDirty(OverlayPanel &p);

// Creates or recreates the texture for `device`. Marks the panel dirty when the
// texture is new, so the caller repaints into it.
void overlayPanelEnsureDevice(OverlayPanel &p, IDirect3DDevice9 *device);

// Returns the memory DC to paint into (GDI objects are created on first use),
// or null if it could not be created. Bracket painting with EndPaint.
HDC overlayPanelBeginPaint(OverlayPanel &p);

// Forces the bitmap opaque (GDI leaves the alpha byte at 0), uploads it to the
// texture and clears the dirty flag.
void overlayPanelEndPaint(OverlayPanel &p);

// Draws the quad. Saves and restores all device state.
void overlayPanelDraw(OverlayPanel &p, IDirect3DDevice9 *device);

// Maps a window-client point to panel-local coordinates, through the backbuffer
// size so it is correct windowed. Returns false when the point is outside the
// panel. Side-effect-free, so it can gate swallowing without acting.
bool overlayPanelMapPoint(
    const OverlayPanel &p, HWND hwnd, int clientX, int clientY, int &lx, int &ly
);

// The same mapping without the bounds test: true whenever the coordinates could
// be mapped at all, inside the panel or not. For tracking a drag that has left
// the control it started in.
bool overlayPanelMapPointRaw(
    const OverlayPanel &p, HWND hwnd, int clientX, int clientY, int &lx, int &ly
);

// ── UI scale ────────────────────────────────────────────────────────────────────
// Panel layouts are authored in "design pixels": what the panel measures at
// 1080p. Every dimension — box, padding, gap and font alike — is routed through
// overlayScale so a different render target scales the panel as a whole. Sizing
// a font on its own (the desktop's DPI) while the boxes around it stay put is
// what makes text clip. Integer arithmetic only, so this is safe on the render
// path. See docs/features/ui-scale.md.

// Resolved scale in percent: [ui] Scale from the ini when it is 50..300,
// otherwise derived from the backbuffer height. Re-resolves when the render
// target changes; 100 until the backbuffer is known.
int overlayScalePercent();

// Overrides the resolved scale live, for the settings overlay: 0 means Auto
// (derive it from the backbuffer), 50..300 fixes it. Values outside that range
// are ignored. Panels pick the new scale up the next time they lay out, so the
// caller re-lays out whatever is on screen.
void overlayScaleSetFixed(int percent);

// The current fixed scale, or 0 when it is on Auto.
int overlayScaleFixed();

// Design pixels -> panel pixels at `percent`. Panels that shrink to fit a small
// render target pass their own percent rather than the resolved one.
int overlayScaleBy(int designPx, int percent);

// Shared font cache (Segoe UI). `designPoints` is the point size the panel was
// authored at on a 96-DPI display; the real height is derived from that and
// `percent`, never from the desktop DPI. Valid for the process lifetime.
HFONT overlayPanelFont(int designPoints, bool bold, int percent);

// Height of one line of `font`, for layouts that size rows from their text.
int overlayPanelLineHeight(HFONT font);

// Width of `text` (ASCII, measured as UTF-16) in `font`.
int overlayPanelTextWidth(HFONT font, const char *text);

// GDI conveniences shared by the panels; `text` is ASCII, drawn as UTF-16.
void overlayPanelFill(HDC dc, int l, int t, int r, int b, COLORREF color);
void overlayPanelFrame(HDC dc, int l, int t, int r, int b, COLORREF color);
void overlayPanelText(HDC dc, int x, int y, const char *text, COLORREF color);
void overlayPanelTextCentered(HDC dc, int l, int r, int y, const char *text, COLORREF color);
void overlayPanelTextRight(HDC dc, int right, int y, const char *text, COLORREF color);
