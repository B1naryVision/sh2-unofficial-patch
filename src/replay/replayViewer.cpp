#include "replayViewer.h"

#include "replayCommon.h"
#include "replayPlayer.h"
#include "spectatorGuard.h"
#include "../core/d3dHook.h"
#include "../core/frameTick.h"
#include "../core/hotkey.h"
#include "../core/observerContext.h"
#include "../core/overlayPanel.h"
#include "../core/readableMemory.h"
#include "../patches/endgameStats/gameOffsets.h"
#include "../patches/endgameStats/killTracker.h"
#include "../patches/endgameStats/session.h"
#include "../patches/endgameStats/unitTracker.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Threading. The frame tick (game thread) polls the player table, builds the
// row model, measures it and sizes the panel; the EndScene callback paints and
// draws it; the spectator guard's window procedure records the toggle and
// slider requests, which the frame tick applies. All three share one lock so
// the painter never walks a half-built row list, whichever threads they turn
// out to run on.
//
// Floats. Gold and popularity are read as raw bits and converted with integer
// arithmetic. The one float operation, the panel quad in
// overlayPanelSetBounds, runs inside ObserverFpScope (fxsave / fninit /
// fxrstor), because the frame-tick site is mid-function with live x87 state.

static const uintptr_t TICK_RATE_RVA = 0xe46518; // Game Speed setting (u32)
static const int SPEED_MIN = 10;
static const int SPEED_MAX = 500;
static const int SPEED_STEP = 5;
static const DWORD POLL_MS = 250;
// Show / hide key, [hotkeys] ReplayPanel; rebindable from the settings panel.
static Hotkey s_toggle = {VK_OEM_6, 0}; // ']'
static const int PAUSE_VK = 'P';
// Pause. The stock pause flag (+0xe46305, simPauseDisable.h) cannot be used:
// it freezes the world while FUN_00767950 keeps advancing the job counter
// (NetGameMgr+6), so scheduled commands land on the wrong world state
// (a siege test: world diverged from rel 118 after a flag pause).
// The viewer instead skips the whole update loop of the main-loop body
// FUN_00421e00: at RVA 0x21ea2 `cmp [esi+0xce30c],edi; jbe 0x421ed0` (edi = 0)
// is detoured to jump straight to 0x421ed0 while paused, so neither the job
// counter nor the world advances -- a zero-speed frame. The pacer has already
// consumed its time debt for the frame, so resuming does not burst. The stock
// flag is held at 0 during playback (stock `p` or an SP ESC menu would set it).
static const uintptr_t PAUSE_FLAG_RVA = 0xe46305;
static const uintptr_t LOOP_SITE_RVA = 0x21ea2;
static const uint8_t LOOP_SITE_STOCK[6] = {0x39, 0xbe, 0x0c, 0xe3, 0x0c, 0x00};
static const uintptr_t LOOP_SKIP_RVA = 0x21ed0;
// Frames out of a match before the viewer ends the playback (quit to menu).
static const int LEFT_MATCH_FRAMES = 30;
static const size_t PLAYER_SPAN = OFF_HON_BASE + 8 * 4;

static const wchar_t *TITLE_NAMES[] = {
    L"Freeman",       L"Yeoman",         L"Squire", L"Knight", L"Knight Bachelor",
    L"Knight Errant", L"Royal Champion", L"Baron",  L"Earl",   L"Duke"};
static const wchar_t *COLOR_NAMES[] = {L"?",    L"Red",    L"Orange", L"Green",    L"Cyan", L"Blue",
                                       L"Pink", L"Yellow", L"Violet", L"Dark Red", L"Gray"};

// ── Theme (the stat screen's palette) ───────────────────────────────────────
static const COLORREF CLR_BACKGROUND = RGB(45, 45, 55);
static const COLORREF CLR_BORDER = RGB(100, 100, 120);
static const COLORREF CLR_TEXT = RGB(255, 255, 255);
static const COLORREF CLR_LABEL = RGB(220, 220, 220);
static const COLORREF CLR_KEY = RGB(255, 235, 150);
static const COLORREF CLR_GOLD_HEAD = RGB(255, 215, 100);
static const COLORREF CLR_GOLD_ROW = RGB(230, 230, 200);
static const COLORREF CLR_HONOUR_HEAD = RGB(140, 210, 255);
static const COLORREF CLR_HONOUR_ROW = RGB(200, 230, 245);
static const COLORREF CLR_UNITS_HEAD = RGB(160, 240, 140);
static const COLORREF CLR_UNITS_ROW = RGB(210, 240, 200);
static const COLORREF CLR_KILLS_HEAD = RGB(255, 150, 140);
static const COLORREF CLR_KILLS_ROW = RGB(245, 215, 210);
static const COLORREF CLR_NOTE = RGB(160, 160, 160);
static const COLORREF CLR_TRACK = RGB(70, 70, 88);
static const COLORREF CLR_TRACK_FILL = RGB(120, 150, 210);
static const COLORREF CLR_THUMB = RGB(235, 235, 245);
static const BYTE PANEL_ALPHA = 225;
static const int FONT_POINTS = 10;

// Design pixels (1080p), scaled through S().
static const int D_MARGIN = 10;
static const int D_CELL_PAD = 12;
static const int D_MIN_COL = 70;
static const int D_LABEL_PAD = 16;
static const int D_SLIDER_W = 260;
static const int D_SLIDER_H = 8;
static const int D_THUMB_W = 10;
static const int D_SCREEN_GAP = 12;
static const int NAME_CHARS = 16;
// The stock HUD is drawn at fixed pixel size: a ~30 px resource bar along the
// top and the ~165 px command panel / book along the bottom (1024x768 capture).
// The panel stays between them.
static const int HUD_TOP_PX = 32;
static const int HUD_BOTTOM_PX = 170;

// ── Player model ────────────────────────────────────────────────────────────
// Internal linkage for every type here. The endgameStats overlay
// defines its own global `struct Row` / `RowKind`; two global definitions of
// one name break the one-definition rule, the linker keeps ONE copy of each
// inline std::vector<Row> member, and the viewer then built rows with the
// overlay's layout (GetTextExtentPoint32W
// handed a string pointer of 1 -> access violation in gdi32full).
namespace {

struct ViewerPlayer {
    int slot;
    bool live; // object currently in the table and passing the filter
    RawPlayerStat stat;
    uint32_t units[kUnitCount];
    uint32_t kills[kKillRowCount];
    wchar_t name[kMaxNameChars];
};

static ViewerPlayer s_players[kMaxPlayers];
static int s_playerCount = 0;

// ── Row model ───────────────────────────────────────────────────────────────
enum class RowKind { Title, Slider, Header, Section, Values, Note, Gap };

struct Row {
    RowKind kind = RowKind::Gap;
    std::wstring label;
    COLORREF labelColor = CLR_LABEL;
    std::array<std::wstring, kMaxPlayers> cells;
    COLORREF cellColor = CLR_TEXT;
    int height = 0;
};

} // namespace

static CRITICAL_SECTION s_lock;
static std::vector<Row> s_rows;
static OverlayPanel s_panel;
static int s_scale = 100;
static int s_labelColW = 0;
static int s_valueColW = 0;
static int s_columns = 0;
static bool s_layoutReady = false;
static int s_sliderL = 0, s_sliderR = 0, s_sliderT = 0, s_sliderB = 0; // panel-local

static volatile LONG s_visible = 0;
static volatile LONG s_toggleRequest = 0;
static volatile LONG s_speedRequest = -1;
static volatile LONG s_speed = 0;
static bool s_dragging = false;
static uint32_t s_polls = 0;
static uint32_t s_speedWrites = 0;
static DWORD s_lastPoll = 0;
static uint32_t s_lastEpoch = 0;
static uint32_t s_jobNow = 0;
static volatile LONG s_pauseToggle = 0;
static volatile LONG s_menuOpen = 0;
static bool s_userPaused = false;
static bool s_loopHooked = false;
static bool s_paused = false;
static int s_outOfMatchFrames = 0;
// Set by the replay session when playback reaches the end of the recording:
// the simulation is held paused (past this point it would run on without the
// recorded input, which is no longer the match) and the panel is shown.
static volatile LONG s_ended = 0;
static bool s_endedShown = false;
static wchar_t s_replayName[64] = L"";

static int S(int designPx) { return overlayScaleBy(designPx, s_scale); }

static inline uint32_t load32(uintptr_t address) {
    return *reinterpret_cast<const volatile uint32_t *>(address);
}

// IEEE-754 single -> int, truncating, integer-only (frame-tick safe).
static int floatBitsToInt(uint32_t bits) {
    const int exponent = (int)((bits >> 23) & 0xff) - 127;
    if (exponent < 0) {
        return 0;
    }
    if (exponent > 30) {
        return (bits >> 31) ? INT32_MIN : INT32_MAX;
    }
    const uint32_t mantissa = (bits & 0x7fffffu) | 0x800000u;
    const uint32_t magnitude =
        exponent >= 23 ? mantissa << (exponent - 23) : mantissa >> (23 - exponent);
    return (bits >> 31) ? -(int)magnitude : (int)magnitude;
}

static int clampSpeed(int speed) {
    speed = (speed + SPEED_STEP / 2) / SPEED_STEP * SPEED_STEP;
    return speed < SPEED_MIN ? SPEED_MIN : speed > SPEED_MAX ? SPEED_MAX : speed;
}

// ── Polling (game thread) ───────────────────────────────────────────────────

// A slot is a player when its object is readable, has a valid colour, has a
// castle (2 local, 1 other) and is not an all-zero template — the same
// selection the endgame stats collector uses for live players.
static bool readLivePlayer(uintptr_t base, int slot, RawPlayerStat &out) {
    const uintptr_t ptr = playerTable(base)[slot];
    if (!ptr || !observerReadable((const void *)ptr, PLAYER_SPAN)) {
        return false;
    }
    PlayerView pv = {ptr};
    const int castle = pv.castle();
    if (!isValidColor(pv.color()) || (castle != 1 && castle != 2) ||
        (pv.goldBits() == 0 && pv.honor() == 0)) {
        return false;
    }
    out = readRawPlayerStat(slot, ptr);
    return true;
}

static void pollPlayers(uintptr_t base) {
    for (int i = 0; i < s_playerCount; ++i) {
        s_players[i].live = false;
    }
    for (int slot = 0; slot < PLAYER_TABLE_SLOTS; ++slot) {
        RawPlayerStat stat;
        if (!readLivePlayer(base, slot, stat)) {
            continue;
        }
        ViewerPlayer *player = nullptr;
        for (int i = 0; i < s_playerCount; ++i) {
            if (s_players[i].slot == slot) {
                player = &s_players[i];
            }
        }
        if (!player) {
            if (s_playerCount >= kMaxPlayers) {
                continue;
            }
            player = &s_players[s_playerCount++];
            memset(player, 0, sizeof(*player));
            player->slot = slot;
            // Colour is frozen at first sighting: an eliminated player's colour
            // field is rewritten to its conqueror's.
            sessionLoadName(stat.colorIdx, player->name);
        } else {
            stat.colorIdx = player->stat.colorIdx;
        }
        player->stat = stat;
        player->live = true;
    }
    // Tallies are kept for every seen slot, live or not.
    for (int i = 0; i < s_playerCount; ++i) {
        memcpy(s_players[i].units, unitCountsForSlot(s_players[i].slot), sizeof(s_players[i].units));
        memcpy(s_players[i].kills, killCountsForSlot(s_players[i].slot), sizeof(s_players[i].kills));
    }
}

// ── Row construction ────────────────────────────────────────────────────────

static std::wstring formatInt(int value) {
    wchar_t buf[16];
    _snwprintf(buf, 16, L"%d", value);
    buf[15] = 0;
    return buf;
}

static std::wstring widen(const char *s) {
    std::wstring out;
    while (*s) {
        out.push_back((wchar_t)(unsigned char)*s++);
    }
    return out;
}

static Row makeRow(RowKind kind, std::wstring label, COLORREF color) {
    Row row;
    row.kind = kind;
    row.label = std::move(label);
    row.labelColor = color;
    return row;
}

template <typename Getter>
static Row valueRow(std::wstring label, COLORREF labelColor, COLORREF cellColor, Getter get) {
    Row row = makeRow(RowKind::Values, std::move(label), labelColor);
    row.cellColor = cellColor;
    for (int p = 0; p < s_playerCount; ++p) {
        row.cells[p] = formatInt(get(s_players[p]));
    }
    return row;
}

// Columns are identified by colour. The session name array is not reliable
// in an offline replay (every slot read "Lord Grey" in a siege
// test), so names get their own row only when they tell players apart.
static std::wstring headerName(const ViewerPlayer &player) {
    const int color = isValidColor(player.stat.colorIdx) ? player.stat.colorIdx : 0;
    std::wstring name = COLOR_NAMES[color];
    return player.live ? name : name + L" (out)";
}

static bool namesDistinct() {
    for (int a = 0; a < s_playerCount; ++a) {
        if (!s_players[a].name[0]) {
            return false;
        }
        for (int b = a + 1; b < s_playerCount; ++b) {
            if (!wcscmp(s_players[a].name, s_players[b].name)) {
                return false;
            }
        }
    }
    return s_playerCount > 1;
}

// `rowCount` rows of `counts`, labelled by `rowName`; all-zero rows are hidden.
template <typename Counts, typename RowName>
static void unitSection(std::vector<Row> &rows, const wchar_t *title, COLORREF head,
                        COLORREF body, int rowCount, RowName rowName, Counts counts) {
    rows.push_back(makeRow(RowKind::Section, title, head));
    bool any = false;
    for (int u = 0; u < rowCount; ++u) {
        bool used = false;
        for (int p = 0; p < s_playerCount && !used; ++p) {
            used = counts(s_players[p])[u] != 0;
        }
        if (!used) {
            continue;
        }
        any = true;
        rows.push_back(valueRow(L"  " + widen(rowName(u)), body, CLR_TEXT,
                                [&](const ViewerPlayer &p) { return (int)counts(p)[u]; }));
    }
    if (!any) {
        rows.push_back(makeRow(RowKind::Note, L"  (none yet)", CLR_NOTE));
    }
}

static std::vector<Row> buildRows() {
    std::vector<Row> rows;
    char keyName[HOTKEY_NAME_MAX] = "";
    if (hotkeyIsBound(s_toggle)) {
        hotkeyDisplayName(s_toggle, keyName, sizeof(keyName));
    }
    wchar_t title[192];
    _snwprintf(title, 192, L"%ls%lsjob %u%s   ( %hs%hsP pause )",
               s_replayName[0] ? s_replayName : L"Replay viewer", L" \x2014 ", s_jobNow,
               s_ended ? L"  ENDED" : s_paused ? L"  PAUSED" : L"", keyName,
               keyName[0] ? " close, " : "");
    title[191] = 0;
    rows.push_back(makeRow(RowKind::Title, title, CLR_TEXT));
    if (s_ended) {
        rows.push_back(makeRow(RowKind::Note, L"End of the recording. Esc \x2192 Quit \x2192 Exit to leave.",
                               CLR_KEY));
    }

    wchar_t speed[48];
    _snwprintf(speed, 48, L"Game speed  %ld", (long)s_speed);
    speed[47] = 0;
    rows.push_back(makeRow(RowKind::Slider, speed, CLR_KEY));
    rows.push_back(makeRow(RowKind::Gap, L"", CLR_LABEL));

    if (!s_playerCount) {
        rows.push_back(makeRow(RowKind::Note, L"(no players found yet)", CLR_NOTE));
        return rows;
    }

    Row names = makeRow(RowKind::Header, L"", CLR_LABEL);
    for (int p = 0; p < s_playerCount; ++p) {
        names.cells[p] = headerName(s_players[p]);
    }
    rows.push_back(names);
    if (namesDistinct()) {
        Row players = makeRow(RowKind::Values, L"Player", CLR_LABEL);
        for (int p = 0; p < s_playerCount; ++p) {
            std::wstring name = s_players[p].name;
            if ((int)name.size() > NAME_CHARS) {
                name.resize(NAME_CHARS);
            }
            players.cells[p] = name;
        }
        rows.push_back(players);
    }

    Row titles = makeRow(RowKind::Values, L"Title", CLR_LABEL);
    for (int p = 0; p < s_playerCount; ++p) {
        const int idx = s_players[p].stat.titleIdx;
        titles.cells[p] = idx >= 0 && idx < (int)(sizeof(TITLE_NAMES) / sizeof(TITLE_NAMES[0]))
                              ? TITLE_NAMES[idx] : L"?";
    }
    rows.push_back(titles);

    // Gold, honour and troops lead: they are what a viewer checks first.
    rows.push_back(valueRow(L"Gold", CLR_KEY, CLR_KEY, [](const ViewerPlayer &p) {
        return floatBitsToInt(p.stat.goldBits);
    }));
    rows.push_back(valueRow(L"Honour", CLR_KEY, CLR_KEY,
                            [](const ViewerPlayer &p) { return p.stat.honor; }));
    rows.push_back(valueRow(L"Troops", CLR_KEY, CLR_KEY,
                            [](const ViewerPlayer &p) { return p.stat.troops; }));
    rows.push_back(valueRow(L"Siege", CLR_LABEL, CLR_TEXT,
                            [](const ViewerPlayer &p) { return p.stat.siege; }));
    rows.push_back(valueRow(L"Popularity", CLR_LABEL, CLR_TEXT, [](const ViewerPlayer &p) {
        return floatBitsToInt(p.stat.popBits);
    }));
    rows.push_back(makeRow(RowKind::Gap, L"", CLR_LABEL));

    static const wchar_t *INCOME[] = {L"  Trade Income", L"  Tax: Castle", L"  Tax: Estates"};
    rows.push_back(makeRow(RowKind::Section, L"\x2014 Gold by source \x2014", CLR_GOLD_HEAD));
    for (int src = 0; src < 3; ++src) {
        rows.push_back(valueRow(INCOME[src], CLR_GOLD_ROW, CLR_TEXT,
                                [src](const ViewerPlayer &p) { return p.stat.income[src]; }));
    }
    rows.push_back(makeRow(RowKind::Gap, L"", CLR_LABEL));

    static const wchar_t *HONOUR[] = {L"  Feasting", L"  Dancing", L"  Monastery", L"  Jousting",
                                      L"  Church",   L"  Granary", L"  Statues",   L"  Crime"};
    rows.push_back(makeRow(RowKind::Section, L"\x2014 Honour by source \x2014", CLR_HONOUR_HEAD));
    for (int src = 0; src < 8; ++src) {
        rows.push_back(valueRow(HONOUR[src], CLR_HONOUR_ROW, CLR_TEXT,
                                [src](const ViewerPlayer &p) { return p.stat.honSrc[src]; }));
    }
    rows.push_back(makeRow(RowKind::Gap, L"", CLR_LABEL));

    unitSection(rows, L"\x2014 Units recruited \x2014", CLR_UNITS_HEAD, CLR_UNITS_ROW, kUnitCount,
                [](int u) { return kUnits[u].name; },
                [](const ViewerPlayer &p) { return p.units; });
    rows.push_back(makeRow(RowKind::Gap, L"", CLR_LABEL));
    unitSection(rows, L"\x2014 Units killed \x2014", CLR_KILLS_HEAD, CLR_KILLS_ROW, kKillRowCount,
                killRowName, [](const ViewerPlayer &p) { return p.kills; });
    return rows;
}

// ── Layout (game thread, integer only) ──────────────────────────────────────

static int textWidth(HDC dc, const std::wstring &s) {
    SIZE size = {0, 0};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &size);
    return size.cx;
}

// Measures the rows at `scale`; returns the content height and leaves the
// column widths set.
static int measure(std::vector<Row> &rows, int scale) {
    s_scale = scale;
    HDC dc = GetDC(NULL);
    HFONT old = (HFONT)SelectObject(dc, overlayPanelFont(FONT_POINTS, false, scale));
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    const int line = tm.tmHeight + tm.tmExternalLeading;
    int labelW = 0;
    int valueW = S(D_MIN_COL);
    int titleW = 0;
    int total = 0;
    for (Row &row : rows) {
        switch (row.kind) {
        case RowKind::Title: row.height = line + S(6); break;
        case RowKind::Slider: row.height = line + S(8); break;
        case RowKind::Header: row.height = line + S(5); break;
        case RowKind::Section: row.height = line + S(4); break;
        case RowKind::Values:
        case RowKind::Note: row.height = line + S(2); break;
        case RowKind::Gap: row.height = S(6); break;
        }
        total += row.height;
        if (row.kind == RowKind::Title || row.kind == RowKind::Note ||
            row.kind == RowKind::Section) {
            const int w = textWidth(dc, row.label);
            titleW = w > titleW ? w : titleW;
        } else if (row.kind == RowKind::Slider) {
            const int w = textWidth(dc, L"Game speed  500") + S(D_LABEL_PAD) + S(D_SLIDER_W);
            titleW = w > titleW ? w : titleW;
        } else if (!row.label.empty()) {
            const int w = textWidth(dc, row.label);
            labelW = w > labelW ? w : labelW;
        }
        for (int p = 0; p < s_columns; ++p) {
            if (!row.cells[p].empty()) {
                const int w = textWidth(dc, row.cells[p]) + S(D_CELL_PAD);
                valueW = w > valueW ? w : valueW;
            }
        }
    }
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    s_labelColW = labelW + S(D_LABEL_PAD);
    s_valueColW = valueW;
    // Widen the value columns if a title or the slider needs more room.
    const int body = s_labelColW + s_columns * s_valueColW;
    if (titleW > body && s_columns > 0) {
        s_valueColW += (titleW - body + s_columns - 1) / s_columns;
    } else if (titleW > body) {
        s_labelColW = titleW;
    }
    return total;
}

// Rebuilds the rows and, when the size changed, the panel quad. Called with the
// lock held.
static void relayout() {
    std::vector<Row> rows = buildRows();
    s_columns = s_playerCount;
    int renderW = 1024, renderH = 768;
    d3dBackbufferSize(renderW, renderH);
    int scale = overlayScalePercent();
    int panelW = 0, panelH = 0;
    for (int pass = 0; pass < 3; ++pass) {
        const int contentH = measure(rows, scale);
        panelW = 2 * S(D_MARGIN) + s_labelColW + s_columns * s_valueColW;
        panelH = 2 * S(D_MARGIN) + contentH;
        const int availW = renderW - 2 * S(D_SCREEN_GAP);
        const int availH = renderH - HUD_TOP_PX - HUD_BOTTOM_PX - 2 * S(D_SCREEN_GAP);
        if (panelW <= availW && panelH <= availH) {
            break;
        }
        const int byH = scale * availH / (panelH > 0 ? panelH : 1);
        const int byW = scale * availW / (panelW > 0 ? panelW : 1);
        int fit = byH < byW ? byH : byW;
        fit = fit < 40 ? 40 : fit;
        if (fit >= scale) {
            break;
        }
        scale = fit;
    }
    s_rows.swap(rows);
    // Top-right under the resource bar, so the battlefield in the middle and
    // the stock HUD both stay visible.
    const int x = renderW - panelW - S(D_SCREEN_GAP);
    const int y = HUD_TOP_PX + S(D_SCREEN_GAP);
    if (!s_layoutReady || x != s_panel.x || y != s_panel.y || panelW != s_panel.w ||
        panelH != s_panel.h) {
        ObserverFpScope fp; // the quad is float; see the file comment
        overlayPanelSetBounds(s_panel, x < 0 ? 0 : x, y, panelW, panelH);
    }
    s_layoutReady = true;
    overlayPanelMarkDirty(s_panel);
}

// ── Frame tick (game thread) ────────────────────────────────────────────────

static void resetTallies() {
    resetUnitCounts();
    resetKillCounts();
    s_playerCount = 0;
}

// Read by the loop-site detour on every frame. Not static: GAS names it.
extern "C" {
volatile uint32_t g_viewerSimPaused = 0;
uintptr_t g_viewerLoopResume = 0;
uintptr_t g_viewerLoopSkip = 0;
}

__declspec(naked) static void loopSiteHook() {
    __asm__ volatile("cmpl $0, _g_viewerSimPaused\n\t"
                     "jne 1f\n\t"
                     "cmpl %edi, 0xce30c(%esi)\n\t" // stock; jbe at resume
                     "jmp *_g_viewerLoopResume\n\t"
                     "1:\n\t"
                     "jmp *_g_viewerLoopSkip\n\t");
}

// Pauses by skipping whole simulation updates; see LOOP_SITE_RVA. Also holds
// the stock pause flag clear while playback is armed.
static void applyPause(uintptr_t base, bool want) {
    volatile uint8_t *flag = reinterpret_cast<volatile uint8_t *>(base + PAUSE_FLAG_RVA);
    if (*flag == 1) {
        *flag = 0;
    }
    want = want && s_loopHooked;
    g_viewerSimPaused = want ? 1 : 0;
    s_paused = want;
}

static void releasePause() {
    g_viewerSimPaused = 0;
    s_paused = false;
}

static void viewerTick() {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (!spectatorGuardActive()) {
        if (s_visible) {
            InterlockedExchange(&s_visible, 0);
        }
        InterlockedExchange(&s_toggleRequest, 0);
        InterlockedExchange(&s_pauseToggle, 0);
        InterlockedExchange(&s_menuOpen, 0);
        s_dragging = false;
        s_userPaused = false;
        s_outOfMatchFrames = 0;
        InterlockedExchange(&s_ended, 0);
        s_endedShown = false;
        releasePause(); // never leave a viewer pause behind
        return;
    }

    // Quit to the main menu from the in-game menu: end the playback so the
    // spectator guard releases the menus, and drop any viewer pause first.
    if (!replayInGame()) {
        releasePause();
        if (++s_outOfMatchFrames >= LEFT_MATCH_FRAMES) {
            replayPlayerControl(0);
            s_outOfMatchFrames = 0;
        }
        return;
    }
    s_outOfMatchFrames = 0;

    // The ESC menu is a Dragonfly popup. While it is up the replay pauses and
    // the guard lets mouse clicks through so its buttons work.
    const bool menuOpen = replayScreenPopupOn();
    InterlockedExchange(&s_menuOpen, menuOpen ? 1 : 0);
    bool pauseChanged = false;
    if (InterlockedExchange(&s_pauseToggle, 0)) {
        s_userPaused = !s_userPaused;
        pauseChanged = true;
    }
    const bool wasPaused = s_paused;
    applyPause(base, s_userPaused || menuOpen || s_ended);
    pauseChanged = pauseChanged || wasPaused != s_paused;

    ReplayPlayerStatus status = {};
    if (replayPlayerStatus(&status, sizeof(status)) && status.playbackEpoch) {
        // A new playback in this process starts its tallies from zero; spawns
        // replayed by the checkpoint load are not recruits.
        if (status.playbackEpoch != s_lastEpoch) {
            s_lastEpoch = status.playbackEpoch;
            EnterCriticalSection(&s_lock);
            resetTallies();
            LeaveCriticalSection(&s_lock);
        }
        s_jobNow = status.currentTick - status.playbackEpoch;
    }

    bool force = pauseChanged;
    if (s_ended && !s_endedShown) {
        s_endedShown = true;
        InterlockedExchange(&s_visible, 1);
        force = true;
    }
    if (InterlockedExchange(&s_toggleRequest, 0)) {
        InterlockedExchange(&s_visible, s_visible ? 0 : 1);
        force = true;
    }
    const LONG request = InterlockedExchange(&s_speedRequest, -1);
    if (request > 0) {
        *reinterpret_cast<volatile uint32_t *>(base + TICK_RATE_RVA) = (uint32_t)request;
        ++s_speedWrites;
        force = true;
    }
    InterlockedExchange(&s_speed, (LONG)load32(base + TICK_RATE_RVA));

    if (!s_visible) {
        return;
    }
    const DWORD now = GetTickCount();
    if (!force && now - s_lastPoll < POLL_MS) {
        return;
    }
    s_lastPoll = now;
    ++s_polls;
    EnterCriticalSection(&s_lock);
    pollPlayers(base);
    relayout();
    LeaveCriticalSection(&s_lock);
}

// ── Painting (render thread) ────────────────────────────────────────────────

static void paint(HDC dc) {
    overlayPanelFill(dc, 0, 0, s_panel.w, s_panel.h, CLR_BACKGROUND);
    overlayPanelFrame(dc, 0, 0, s_panel.w, s_panel.h, CLR_BORDER);
    SelectObject(dc, overlayPanelFont(FONT_POINTS, false, s_scale));
    SetBkMode(dc, TRANSPARENT);
    const int margin = S(D_MARGIN);
    int y = margin;
    for (const Row &row : s_rows) {
        if (!row.label.empty()) {
            SetTextColor(dc, row.labelColor);
            TextOutW(dc, margin, y, row.label.c_str(), (int)row.label.size());
        }
        if (row.kind == RowKind::Slider) {
            const int left = s_panel.w - margin - S(D_SLIDER_W);
            const int right = s_panel.w - margin;
            const int mid = y + (row.height - S(D_SLIDER_H)) / 2;
            const int speed = s_speed < SPEED_MIN ? SPEED_MIN : s_speed > SPEED_MAX ? SPEED_MAX : s_speed;
            const int thumb = left + (right - left) * (speed - SPEED_MIN) / (SPEED_MAX - SPEED_MIN);
            overlayPanelFill(dc, left, mid, right, mid + S(D_SLIDER_H), CLR_TRACK);
            overlayPanelFill(dc, left, mid, thumb, mid + S(D_SLIDER_H), CLR_TRACK_FILL);
            overlayPanelFill(dc, thumb - S(D_THUMB_W) / 2, y + S(2), thumb + S(D_THUMB_W) / 2,
                             y + row.height - S(2), CLR_THUMB);
            s_sliderL = left;
            s_sliderR = right;
            s_sliderT = y;
            s_sliderB = y + row.height;
        }
        SetTextColor(dc, row.cellColor);
        for (int p = 0; p < s_columns; ++p) {
            if (row.cells[p].empty()) {
                continue;
            }
            RECT cell;
            cell.left = margin + s_labelColW + p * s_valueColW;
            cell.right = cell.left + s_valueColW - S(D_CELL_PAD) / 2;
            cell.top = y;
            cell.bottom = y + row.height;
            DrawTextW(dc, row.cells[p].c_str(), (int)row.cells[p].size(), &cell,
                      DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        }
        y += row.height;
    }
}

static void viewerRender(IDirect3DDevice9 *device) {
    if (!s_visible || !spectatorGuardActive()) {
        return;
    }
    EnterCriticalSection(&s_lock);
    if (s_layoutReady) {
        overlayPanelEnsureDevice(s_panel, device);
        if (s_panel.dirty) {
            HDC dc = overlayPanelBeginPaint(s_panel);
            if (dc) {
                paint(dc);
                overlayPanelEndPaint(s_panel);
            }
        }
        overlayPanelDraw(s_panel, device);
    }
    LeaveCriticalSection(&s_lock);
}

// ── Input (window procedure, via the spectator guard) ───────────────────────

static void speedFromPanelX(int lx) {
    const int span = s_sliderR - s_sliderL;
    if (span <= 0) {
        return;
    }
    const int offset = lx < s_sliderL ? 0 : lx > s_sliderR ? span : lx - s_sliderL;
    replayViewerRequestSpeed((uint32_t)(SPEED_MIN + offset * (SPEED_MAX - SPEED_MIN) / span));
}

static bool onSlider(int lx, int ly) {
    const int slack = S(D_THUMB_W);
    return ly >= s_sliderT && ly < s_sliderB && lx >= s_sliderL - slack && lx <= s_sliderR + slack;
}

bool replayViewerHandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (hotkeyMatchesKeyDown(s_toggle, (int)wparam)) {
            if (!(lparam & (1 << 30))) { // ignore auto-repeat
                replayViewerRequestToggle();
            }
            return true;
        }
        if (wparam == (WPARAM)PAUSE_VK) {
            if (!(lparam & (1 << 30))) {
                InterlockedExchange(&s_pauseToggle, 1);
            }
            return true;
        }
        return false;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return (hotkeyIsBound(s_toggle) && wparam == (WPARAM)s_toggle.vk) ||
               wparam == (WPARAM)PAUSE_VK;
    default:
        break;
    }
    if (!s_visible || !s_layoutReady) {
        return false;
    }
    int lx = 0, ly = 0;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const bool inside = overlayPanelMapPoint(s_panel, hwnd, (short)LOWORD(lparam),
                                                 (short)HIWORD(lparam), lx, ly);
        if (inside && onSlider(lx, ly)) {
            s_dragging = true;
            speedFromPanelX(lx);
        }
        return inside;
    }
    case WM_MOUSEMOVE:
        if (s_dragging) {
            if (!(wparam & MK_LBUTTON)) {
                s_dragging = false;
                return false;
            }
            overlayPanelMapPoint(s_panel, hwnd, (short)LOWORD(lparam), (short)HIWORD(lparam), lx, ly);
            speedFromPanelX(lx);
            return true;
        }
        return false;
    case WM_LBUTTONUP:
        if (s_dragging) {
            s_dragging = false;
            return true;
        }
        return overlayPanelMapPoint(s_panel, hwnd, (short)LOWORD(lparam), (short)HIWORD(lparam), lx, ly);
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return overlayPanelMapPoint(s_panel, hwnd, (short)LOWORD(lparam), (short)HIWORD(lparam), lx, ly);
    case WM_MOUSEWHEEL: {
        POINT pt = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
        ScreenToClient(hwnd, &pt);
        if (!overlayPanelMapPoint(s_panel, hwnd, pt.x, pt.y, lx, ly)) {
            return false;
        }
        const int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        replayViewerRequestSpeed((uint32_t)(s_speed + notches * SPEED_STEP));
        return true;
    }
    default:
        return false;
    }
}

bool replayViewerMenuOpen() {
    return s_menuOpen != 0;
}

void replayViewerSetEnded(bool ended) {
    InterlockedExchange(&s_ended, ended ? 1 : 0);
}

bool replayViewerEnded() {
    return s_ended != 0;
}

void replayViewerSetReplayName(const wchar_t *name) {
    size_t i = 0;
    for (; name && name[i] && i + 1 < sizeof(s_replayName) / sizeof(s_replayName[0]); ++i) {
        s_replayName[i] = name[i];
    }
    s_replayName[i] = 0;
}

void replayViewerRequestPause() {
    InterlockedExchange(&s_pauseToggle, 1);
}

void replayViewerRequestToggle() {
    InterlockedExchange(&s_toggleRequest, 1);
}

void replayViewerRequestSpeed(uint32_t speed) {
    InterlockedExchange(&s_speedRequest, (LONG)clampSpeed((int)speed));
}

void replayViewerGetStatus(ReplayViewerStatus *out) {
    out->visible = s_visible ? 1 : 0;
    out->players = (uint32_t)s_playerCount;
    out->rows = (uint32_t)s_rows.size();
    out->speed = (uint32_t)s_speed;
    out->polls = s_polls;
    out->speedWrites = s_speedWrites;
    out->panelW = (uint32_t)s_panel.w;
    out->panelH = (uint32_t)s_panel.h;
    out->panelX = (uint32_t)s_panel.x;
    out->panelY = (uint32_t)s_panel.y;
    out->sliderL = (uint32_t)(s_panel.x + s_sliderL);
    out->sliderR = (uint32_t)(s_panel.x + s_sliderR);
    out->sliderMidY = (uint32_t)(s_panel.y + (s_sliderT + s_sliderB) / 2);
    out->paused = s_paused ? 1 : 0;
    out->menuOpen = s_menuOpen ? 1 : 0;
}

static void installLoopSiteHook() {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint8_t *site = (uint8_t *)(base + LOOP_SITE_RVA);
    if (memcmp(site, LOOP_SITE_STOCK, sizeof(LOOP_SITE_STOCK)) != 0) {
        return; // unknown build: P and the menu then do not pause
    }
    DWORD old = 0;
    if (!VirtualProtect(site, sizeof(LOOP_SITE_STOCK), PAGE_EXECUTE_READWRITE, &old)) {
        return;
    }
    g_viewerLoopResume = (uintptr_t)site + sizeof(LOOP_SITE_STOCK);
    g_viewerLoopSkip = base + LOOP_SKIP_RVA;
    const int32_t rel = (int32_t)((uintptr_t)&loopSiteHook - (uintptr_t)site - 5);
    site[0] = 0xe9;
    memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    VirtualProtect(site, sizeof(LOOP_SITE_STOCK), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(LOOP_SITE_STOCK));
    s_loopHooked = true;
}

Hotkey replayViewerBinding() {
    return s_toggle;
}

void replayViewerSetBinding(const Hotkey &hk) {
    s_toggle = hk;
}

bool replayViewerInstalled() {
    return true;
}

void installReplayViewer() {
    s_toggle = hotkeyLoad("hotkeys", "ReplayPanel", s_toggle);
    installLoopSiteHook();
    InitializeCriticalSection(&s_lock);
    overlayPanelInit(s_panel, 0, 0, 1, 1); // real bounds are set on first show
    overlayPanelSetAlpha(s_panel, PANEL_ALPHA);
    registerFrameTick(viewerTick);
    registerD3DRender(viewerRender);
}
