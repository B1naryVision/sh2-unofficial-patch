#include "overlay.h"
#include "../../core/d3dHook.h"
#include "../../core/overlayPanel.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>

// Drawn as an OverlayPanel over the game (see src/core/overlayPanel.cpp), not
// as a layered window: the panel is part of the game's own frame, so it needs
// no z-order ownership, no topmost flag for exclusive fullscreen and no
// foreground polling to survive an alt-tab.
//
// Sizing happens on the show path, which is a function-prologue hook
// (Win/LoseScreen::OnActivate) and therefore the one place float work — the
// panel quad — is safe. See docs/features/endgame-stats.md.

// ── Display names ─────────────────────────────────────────────────────────────

static const wchar_t *TITLE_NAMES[] = {
    L"Freeman",       L"Yeoman",         L"Squire", L"Knight", L"Knight Bachelor",
    L"Knight Errant", L"Royal Champion", L"Baron",  L"Earl",   L"Duke"
};
static const int TITLE_NAME_COUNT = (int)(sizeof(TITLE_NAMES) / sizeof(TITLE_NAMES[0]));

static const wchar_t *COLOR_NAMES[] = {L"?",    L"Red",    L"Orange", L"Green",    L"Cyan", L"Blue",
                                       L"Pink", L"Yellow", L"Violet", L"Dark Red", L"Gray"};

// ── Theme ─────────────────────────────────────────────────────────────────────

static const COLORREF CLR_BACKGROUND = RGB(45, 45, 55);
static const COLORREF CLR_BORDER = RGB(100, 100, 120);
// Player name/title/stat text is rendered in a fixed colour rather than the
// player's in-game colour — several in-game colours (e.g. Red, Dark Red) are
// hard to read against the overlay background.
static const COLORREF CLR_PLAYER_TEXT = RGB(255, 255, 255);
static const COLORREF CLR_TITLE = RGB(255, 255, 255);
static const COLORREF CLR_LABEL = RGB(220, 220, 220);
static const COLORREF CLR_GOLD_HEAD = RGB(255, 215, 100);
static const COLORREF CLR_GOLD_ROW = RGB(230, 230, 200);
static const COLORREF CLR_HONOUR_HEAD = RGB(140, 210, 255);
static const COLORREF CLR_HONOUR_ROW = RGB(200, 230, 245);
static const COLORREF CLR_UNITS_HEAD = RGB(160, 240, 140);
static const COLORREF CLR_UNITS_ROW = RGB(210, 240, 200);
static const COLORREF CLR_NOTE = RGB(160, 160, 160);

static const BYTE OVERLAY_ALPHA = 230;
static const int FONT_POINTS = 10;
static const int MARGIN = 10;
static const int CELL_PAD = 12;
static const int MIN_COL_WIDTH = 84;
// Max displayed name length — clan-tagged names like "4H|TheSettler" must fit.
static const int NAME_DISPLAY_CHARS = 16;

// ── Row model ─────────────────────────────────────────────────────────────────
// The overlay is a flat list of rows built once per show. Both the window
// sizing and the paint handler walk this same list, so layout exists in
// exactly one place.

enum class RowKind { Title, ColumnHeader, Section, Values, Note, Gap };

struct Row {
    RowKind kind = RowKind::Gap;
    std::wstring label;
    COLORREF labelColor = CLR_LABEL;
    std::array<std::wstring, kMaxPlayers> cells;
    COLORREF cellColor = CLR_PLAYER_TEXT;
    bool rightAlignCells = false;
    int height = 0; // assigned by layout from font metrics
};

static std::vector<Row> s_rows;
static int s_playerCount = 0;
static int s_labelColW = 0;
static int s_valueColW = 0;

static OverlayPanel s_panel;
static bool s_visible = false;

// ── Row construction ──────────────────────────────────────────────────────────

static std::wstring widen(const char *s) {
    std::wstring out;

    while (*s) {
        out.push_back((wchar_t)(unsigned char)*s++);
    }

    return out;
}

static std::wstring formatInt(int value) {
    wchar_t buf[16];
    _snwprintf_s(buf, 16, _TRUNCATE, L"%d", value);
    return buf;
}

static Row makeGap() {
    Row row;
    row.kind = RowKind::Gap;
    return row;
}

static Row makeSection(const wchar_t *label, COLORREF color) {
    Row row;
    row.kind = RowKind::Section;
    row.label = label;
    row.labelColor = color;
    return row;
}

template <typename Getter>
static Row
makeValueRow(const EndgameSnapshot &snap, std::wstring label, COLORREF labelColor, Getter getter) {
    Row row;
    row.kind = RowKind::Values;
    row.label = std::move(label);
    row.labelColor = labelColor;
    row.rightAlignCells = true;

    for (int p = 0; p < snap.count; ++p) {
        row.cells[p] = formatInt(getter(snap.players[p]));
    }

    return row;
}

static std::wstring playerHeaderName(const PlayerRow &player) {
    if (player.name[0] != 0) {
        std::wstring name = player.name;

        if ((int)name.size() > NAME_DISPLAY_CHARS) {
            name.resize(NAME_DISPLAY_CHARS);
        }

        return name;
    }

    // No name available (e.g. vs AI, or the player left and the game freed
    // the string) — fall back to the colour label.
    int colorIdx = 0;

    if (player.colorIdx >= 1 && player.colorIdx <= 10) {
        colorIdx = player.colorIdx;
    }

    return COLOR_NAMES[colorIdx];
}

static std::vector<Row> buildRows(const EndgameSnapshot &snap) {
    std::vector<Row> rows;

    Row title;
    title.kind = RowKind::Title;
    title.label = std::wstring(L"=== End of Game Statistics — ") +
                  (snap.won ? L"VICTORY" : L"DEFEAT") + L" ===";
    title.labelColor = CLR_TITLE;
    rows.push_back(title);

    Row names;
    names.kind = RowKind::ColumnHeader;

    for (int p = 0; p < snap.count; ++p) {
        names.cells[p] = playerHeaderName(snap.players[p]);
    }

    rows.push_back(names);

    Row titles;
    titles.kind = RowKind::Values;
    titles.label = L"Title";
    titles.labelColor = RGB(210, 210, 210);

    for (int p = 0; p < snap.count; ++p) {
        int idx = snap.players[p].titleIdx;

        if (idx >= 0 && idx < TITLE_NAME_COUNT) {
            titles.cells[p] = TITLE_NAMES[idx];
        } else {
            titles.cells[p] = L"?";
        }
    }

    rows.push_back(titles);

    rows.push_back(makeValueRow(snap, L"Gold (treasury)", CLR_LABEL, [](const PlayerRow &p) {
        return p.gold;
    }));
    rows.push_back(makeValueRow(snap, L"Honour (total)", CLR_LABEL, [](const PlayerRow &p) {
        return p.honor;
    }));
    rows.push_back(makeValueRow(snap, L"Popularity", CLR_LABEL, [](const PlayerRow &p) {
        return p.popularity;
    }));
    rows.push_back(makeValueRow(snap, L"Army (troops)", CLR_LABEL, [](const PlayerRow &p) {
        return p.troops;
    }));
    rows.push_back(makeValueRow(snap, L"Army (siege)", CLR_LABEL, [](const PlayerRow &p) {
        return p.siege;
    }));
    rows.push_back(makeGap());

    static const wchar_t *INC_LABELS[] = {L"  Trade Income", L"  Tax: Castle", L"  Tax: Estates"};
    rows.push_back(makeSection(L"— Gold by source —", CLR_GOLD_HEAD));

    for (int src = 0; src < 3; ++src) {
        rows.push_back(makeValueRow(snap, INC_LABELS[src], CLR_GOLD_ROW, [src](const PlayerRow &p) {
            return p.income[src];
        }));
    }

    rows.push_back(makeGap());

    static const wchar_t *HON_LABELS[] = {L"  Feasting", L"  Dancing", L"  Monastery",
                                          L"  Jousting", L"  Church",  L"  Granary",
                                          L"  Statues",  L"  Crime"};
    rows.push_back(makeSection(L"— Honour by source —", CLR_HONOUR_HEAD));

    for (int src = 0; src < 8; ++src) {
        rows.push_back(
            makeValueRow(snap, HON_LABELS[src], CLR_HONOUR_ROW, [src](const PlayerRow &p) {
                return p.honSrc[src];
            })
        );
    }

    rows.push_back(makeGap());
    rows.push_back(makeSection(L"— Units recruited —", CLR_UNITS_HEAD));

    bool anyUnits = false;

    for (int u = 0; u < kUnitCount; ++u) {
        bool hasAny = false;

        for (int p = 0; p < snap.count; ++p) {
            if (snap.players[p].units[u]) {
                hasAny = true;
                break;
            }
        }

        if (!hasAny) {
            continue;
        }

        anyUnits = true;
        rows.push_back(makeValueRow(
            snap, L"  " + widen(kUnits[u].name), CLR_UNITS_ROW,
            [u](const PlayerRow &p) { return (int)p.units[u]; }
        ));
    }

    if (!anyUnits) {
        Row note;
        note.kind = RowKind::Note;
        note.label = L"  (none recorded)";
        note.labelColor = CLR_NOTE;
        rows.push_back(note);
    }

    return rows;
}

// ── Layout ────────────────────────────────────────────────────────────────────

static int textWidth(HDC hdc, const std::wstring &s) {
    SIZE size = {0, 0};
    GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &size);
    return size.cx;
}

// Assigns row heights from font metrics and measures the label / value column
// widths. Returns the total content height.
static int layoutRows(HDC hdc) {
    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    int line = tm.tmHeight + tm.tmExternalLeading;

    s_labelColW = 0;
    s_valueColW = MIN_COL_WIDTH;
    int totalH = 0;

    for (Row &row : s_rows) {
        switch (row.kind) {
        case RowKind::Title:
            row.height = line + 8;
            break;
        case RowKind::ColumnHeader:
            row.height = line + 5;
            break;
        case RowKind::Section:
            row.height = line + 4;
            break;
        case RowKind::Values:
        case RowKind::Note:
            row.height = line + 2;
            break;
        case RowKind::Gap:
            row.height = 6;
            break;
        }

        totalH += row.height;

        if (row.kind != RowKind::Title && !row.label.empty()) {
            int w = textWidth(hdc, row.label);

            if (w > s_labelColW) {
                s_labelColW = w;
            }
        }

        for (int p = 0; p < s_playerCount; ++p) {
            if (row.cells[p].empty()) {
                continue;
            }

            int w = textWidth(hdc, row.cells[p]) + CELL_PAD;

            if (w > s_valueColW) {
                s_valueColW = w;
            }
        }
    }

    s_labelColW += 16;
    return totalH;
}

// ── Painting ──────────────────────────────────────────────────────────────────

static void paintOverlay(HDC hdc) {
    overlayPanelFill(hdc, 0, 0, s_panel.w, s_panel.h, CLR_BACKGROUND);
    overlayPanelFrame(hdc, 0, 0, s_panel.w, s_panel.h, CLR_BORDER);

    SelectObject(hdc, overlayPanelFont(FONT_POINTS, false));

    int y = MARGIN;

    for (const Row &row : s_rows) {
        if (!row.label.empty()) {
            SetTextColor(hdc, row.labelColor);
            TextOutW(hdc, MARGIN, y, row.label.c_str(), (int)row.label.size());
        }

        SetTextColor(hdc, row.cellColor);

        for (int p = 0; p < s_playerCount; ++p) {
            if (row.cells[p].empty()) {
                continue;
            }

            RECT cell;
            cell.left = MARGIN + s_labelColW + p * s_valueColW;
            cell.right = cell.left + s_valueColW - CELL_PAD;
            cell.top = y;
            cell.bottom = y + row.height;

            UINT align = DT_LEFT;

            if (row.rightAlignCells) {
                align = DT_RIGHT;
            }

            DrawTextW(
                hdc, row.cells[p].c_str(), (int)row.cells[p].size(), &cell,
                align | DT_TOP | DT_SINGLELINE | DT_NOPREFIX
            );
        }

        y += row.height;
    }
}

// Render callback (render thread, inside EndScene). Registered at install so
// the dispatcher slot exists before the device is created.
static void statsRender(IDirect3DDevice9 *device) {
    if (!s_visible) {
        return;
    }

    overlayPanelEnsureDevice(s_panel, device);

    if (s_panel.dirty) {
        HDC hdc = overlayPanelBeginPaint(s_panel);

        if (hdc) {
            paintOverlay(hdc);
            overlayPanelEndPaint(s_panel);
        }
    }

    overlayPanelDraw(s_panel, device);
}

void installStatsOverlay() {
    overlayPanelInit(s_panel, 0, 0, 1, 1); // real bounds are set on each show
    overlayPanelSetAlpha(s_panel, OVERLAY_ALPHA);
    registerD3DRender(&statsRender);
}

void closeStatsOverlay() {
    s_visible = false;
    s_rows.clear();
}

void showStatsOverlay(const EndgameSnapshot &snap) {
    // Hide before rebuilding: the render callback walks s_rows, and a show on
    // top of a still-visible overlay (a second endgame screen without a return
    // to the menu in between) would otherwise let it read a half-built list.
    // Visibility is raised again only once the rows and bounds are final.
    s_visible = false;

    s_rows = buildRows(snap);
    s_playerCount = snap.count;

    // Measure against a screen DC: the panel's own bitmap belongs to the render
    // thread and may not exist yet, and the font metrics are identical either
    // way (same font, same device caps).
    HDC screen = GetDC(NULL);
    HFONT prevFont = (HFONT)SelectObject(screen, overlayPanelFont(FONT_POINTS, false));
    int contentH = layoutRows(screen);
    SelectObject(screen, prevFont);
    ReleaseDC(NULL, screen);

    // Centre in the backbuffer rather than the game window: these are the
    // coordinates the panel quad is drawn in, so this is correct windowed and
    // fullscreen alike.
    int renderW = 1024;
    int renderH = 768;
    d3dBackbufferSize(renderW, renderH);

    int panelW = MARGIN + s_labelColW + s_playerCount * s_valueColW + MARGIN;

    // Never wider than the screen: shrink the value columns to fit.
    if (panelW > renderW && s_playerCount > 0) {
        int maxColW = (renderW - 2 * MARGIN - s_labelColW) / s_playerCount;

        if (maxColW < 60) {
            maxColW = 60;
        }

        if (maxColW < s_valueColW) {
            s_valueColW = maxColW;
            panelW = MARGIN + s_labelColW + s_playerCount * s_valueColW + MARGIN;
        }
    }

    if (panelW < 500) {
        panelW = 500;
    }

    int panelH = contentH + 2 * MARGIN;

    // A panel larger than the backbuffer would be clipped by the quad anyway;
    // clamping keeps the texture allocation sane on a small render target.
    if (panelW > renderW) {
        panelW = renderW;
    }

    if (panelH > renderH) {
        panelH = renderH;
    }

    // Float work lives here: the show path is a function-prologue hook, so the
    // x87 stack is empty (see endgameStats.cpp).
    overlayPanelSetBounds(s_panel, (renderW - panelW) / 2, (renderH - panelH) / 2, panelW, panelH);
    s_visible = true;
}
