#include "optionWidget.h"
#include <cstring>

// Palette matches keybindWidget so the two control types read as one set.
// Integer arithmetic only; the host draws from inside the EndScene detour.

static const COLORREF COL_TRACK_FILL = RGB(30, 33, 44);
static const COLORREF COL_TRACK_EDGE = RGB(90, 90, 110);
static const COLORREF COL_TRACK_DONE = RGB(64, 78, 116);
static const COLORREF COL_HANDLE_FILL = RGB(150, 165, 205);
static const COLORREF COL_HANDLE_DRAG = RGB(255, 205, 70);
static const COLORREF COL_BTN_FILL = RGB(40, 44, 58);
static const COLORREF COL_BTN_EDGE = RGB(90, 90, 110);
static const COLORREF COL_BTN_TEXT = RGB(225, 225, 230);

// Handle width, as a fraction of the track height, and the inset of the filled
// portion. Both are in panel pixels, so they follow the host's scaling.
static const int HANDLE_DIVISOR = 2;

// Only one widget may be dragged at a time; this also backs
// optionWidgetDragActive(), which the host consults before swallowing motion.
static OptionWidget *s_dragging = nullptr;

// ── helpers ─────────────────────────────────────────────────────────────────────
static int toWide(const char *s, wchar_t *out, int outLen) {
    int k = 0;

    for (const char *p = s; *p && k < outLen - 1; ++p) {
        out[k++] = (wchar_t)(unsigned char)*p;
    }

    out[k] = 0;
    return k;
}

static void fillRect(HDC dc, const RECT &r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FillRect(dc, &r, br);
    DeleteObject(br);
}

static void frameRect(HDC dc, const RECT &r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FrameRect(dc, &r, br);
    DeleteObject(br);
}

static int clampInt(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

// Snaps to the nearest step above min, then clamps. Rounding is done on
// positive quantities only, so it does not depend on the sign of the division.
static int snap(const OptionWidget &w, int value) {
    int step = w.step > 0 ? w.step : 1;
    int rel = clampInt(value, w.min, w.max) - w.min;
    int snapped = w.min + ((rel + step / 2) / step) * step;

    return clampInt(snapped, w.min, w.max);
}

static int trackWidth(const OptionWidget &w) {
    int width = (int)(w.bounds.right - w.bounds.left);

    return width > 1 ? width : 1;
}

// Handle is inset by half its own width at both ends so its centre can reach
// the extremes of the track without the box leaving it.
static int handleWidth(const OptionWidget &w) {
    int height = (int)(w.bounds.bottom - w.bounds.top);
    int width = height / HANDLE_DIVISOR;

    if (width < 4) {
        width = 4;
    }

    return width;
}

static int valueFromX(const OptionWidget &w, int x) {
    int half = handleWidth(w) / 2;
    int usable = trackWidth(w) - 2 * half;

    if (usable < 1) {
        usable = 1;
    }

    int rel = clampInt(x - ((int)w.bounds.left + half), 0, usable);
    int span = w.max - w.min;

    return snap(w, w.min + (rel * span + usable / 2) / usable);
}

static int xFromValue(const OptionWidget &w) {
    int half = handleWidth(w) / 2;
    int usable = trackWidth(w) - 2 * half;

    if (usable < 1) {
        usable = 1;
    }

    int span = w.max - w.min;

    if (span <= 0) {
        return (int)w.bounds.left + half;
    }

    return (int)w.bounds.left + half + ((w.value - w.min) * usable + span / 2) / span;
}

// ── state ───────────────────────────────────────────────────────────────────────
static void setBounds(OptionWidget &w, int left, int top, int right, int bottom) {
    w.bounds.left = left;
    w.bounds.top = top;
    w.bounds.right = right;
    w.bounds.bottom = bottom;
}

void optionWidgetInitSlider(
    OptionWidget &w, int value, int min, int max, int step, int left, int top, int right, int bottom
) {
    if (s_dragging == &w) {
        s_dragging = nullptr;
    }

    w.kind = OPTION_SLIDER;
    w.min = min;
    w.max = max > min ? max : min;
    w.step = step > 0 ? step : 1;
    w.choices = nullptr;
    w.choiceCount = 0;
    w.dragging = false;
    w.changed = false;
    w.value = snap(w, value);

    setBounds(w, left, top, right, bottom);
}

void optionWidgetInitChoice(
    OptionWidget &w, int value, const char *const *choices, int choiceCount, int left, int top,
    int right, int bottom
) {
    if (s_dragging == &w) {
        s_dragging = nullptr;
    }

    w.kind = OPTION_CHOICE;
    w.choices = choices;
    w.choiceCount = choiceCount > 0 ? choiceCount : 1;
    w.min = 0;
    w.max = w.choiceCount - 1;
    w.step = 1;
    w.dragging = false;
    w.changed = false;
    w.value = clampInt(value, 0, w.max);

    setBounds(w, left, top, right, bottom);
}

bool optionWidgetHit(const OptionWidget &w, int x, int y) {
    return x >= (int)w.bounds.left && x < (int)w.bounds.right && y >= (int)w.bounds.top &&
           y < (int)w.bounds.bottom;
}

void optionWidgetSetValue(OptionWidget &w, int value) {
    if (w.kind == OPTION_CHOICE) {
        w.value = clampInt(value, 0, w.max);
        return;
    }

    w.value = snap(w, value);
}

bool optionWidgetOnMouseDown(OptionWidget &w, int x, int y) {
    if (!optionWidgetHit(w, x, y)) {
        return false;
    }

    if (w.kind == OPTION_CHOICE) {
        w.value = (w.value + 1) % w.choiceCount;
        w.changed = true;
        return true;
    }

    int next = valueFromX(w, x);

    w.dragging = true;
    s_dragging = &w;

    if (next == w.value) {
        return false;
    }

    w.value = next;
    w.changed = true;
    return true;
}

bool optionWidgetOnMouseMove(OptionWidget &w, int x) {
    if (!w.dragging) {
        return false;
    }

    int next = valueFromX(w, x);

    if (next == w.value) {
        return false;
    }

    w.value = next;
    w.changed = true;
    return true;
}

bool optionWidgetOnMouseUp(OptionWidget &w) {
    if (!w.dragging) {
        return false;
    }

    w.dragging = false;

    if (s_dragging == &w) {
        s_dragging = nullptr;
    }

    return true;
}

bool optionWidgetDragActive() { return s_dragging != nullptr; }

bool optionWidgetTakeChanged(OptionWidget &w) {
    bool changed = w.changed;

    w.changed = false;
    return changed;
}

// ── drawing ─────────────────────────────────────────────────────────────────────
static void drawChoice(const OptionWidget &w, HDC dc) {
    const char *name = "";

    if (w.choices && w.value >= 0 && w.value < w.choiceCount && w.choices[w.value]) {
        name = w.choices[w.value];
    }

    fillRect(dc, w.bounds, COL_BTN_FILL);
    frameRect(dc, w.bounds, COL_BTN_EDGE);

    wchar_t text[64];
    int len = toWide(name, text, 64);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, text, len, &sz);

    int x = (int)w.bounds.left + ((int)(w.bounds.right - w.bounds.left) - sz.cx) / 2;
    int y = (int)w.bounds.top + ((int)(w.bounds.bottom - w.bounds.top) - sz.cy) / 2;

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COL_BTN_TEXT);
    TextOutW(dc, x, y, text, len);
}

static void drawSlider(const OptionWidget &w, HDC dc) {
    int height = (int)(w.bounds.bottom - w.bounds.top);
    int grooveH = height / 3;

    if (grooveH < 3) {
        grooveH = 3;
    }

    RECT groove;
    groove.left = w.bounds.left;
    groove.right = w.bounds.right;
    groove.top = w.bounds.top + (height - grooveH) / 2;
    groove.bottom = groove.top + grooveH;

    fillRect(dc, groove, COL_TRACK_FILL);

    int handleX = xFromValue(w);
    RECT done = groove;
    done.right = handleX;

    if (done.right > done.left) {
        fillRect(dc, done, COL_TRACK_DONE);
    }

    frameRect(dc, groove, COL_TRACK_EDGE);

    int half = handleWidth(w) / 2;
    RECT handle;
    handle.left = handleX - half;
    handle.right = handleX + half;
    handle.top = w.bounds.top;
    handle.bottom = w.bounds.bottom;

    fillRect(dc, handle, w.dragging ? COL_HANDLE_DRAG : COL_HANDLE_FILL);
    frameRect(dc, handle, COL_TRACK_EDGE);
}

void optionWidgetDraw(const OptionWidget &w, HDC dc) {
    if (w.kind == OPTION_CHOICE) {
        drawChoice(w, dc);
        return;
    }

    drawSlider(w, dc);
}
