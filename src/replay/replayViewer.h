#pragma once

#include <cstdint>
#include <windows.h>

#include "../core/hotkey.h"

// Replay viewer: a live stats and game-speed panel for someone watching a
// playback. `]` (rebindable) toggles it while playback is armed (spectatorGuardActive());
// it is inert and invisible otherwise.
//
// Pause and exit. `P` toggles a pause and the ESC menu (stock popup) pauses
// while it is open. Pausing skips whole simulation updates in the main loop
// (the stock pause flag would desync the job counter); see replayViewer.cpp.
// Leaving the match from that menu ends the playback (the replay session does
// that), which releases the spectator guard.
//
// Content follows the end-of-game statistics screen (title, gold, honour,
// popularity, troops, siege, gold and honour by source, units recruited), plus
// units killed per type, refreshed live, plus a Game Speed slider that
// writes the stock tick-rate setting (Stronghold2.exe+0xe46518). The setting
// is put back when the playback ends.
//
// Sources: player object fields (endgameStats/gameOffsets.h, the same offsets
// the endgame overlay reads), the unitTracker and killTracker tallies.
// Everything is read and written on the game thread from the frame tick; the
// window procedure only records requests.

void installReplayViewer();

// The show / hide key ([hotkeys] ReplayPanel, `]` by default), for the settings
// panel. The setter applies at once and does not write the ini.
Hotkey replayViewerBinding();
void replayViewerSetBinding(const Hotkey &hk);
bool replayViewerInstalled();

// Called first by the spectator guard's window procedure while playback is
// armed. Returns true when the message was consumed by the viewer (the toggle
// key, or mouse input over the visible panel).
bool replayViewerHandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

void replayViewerRequestToggle();

// True while an in-match popup (the ESC menu) is up during playback. The
// spectator guard lets mouse buttons through then, so the menu is usable.
bool replayViewerMenuOpen();
// Requests a Game Speed value exactly as the slider would (clamped).
void replayViewerRequestSpeed(uint32_t speed);

// The replay session reports the end of the recording: the viewer holds the
// simulation paused and shows the panel with an end notice. Cleared when the
// playback ends.
void replayViewerSetEnded(bool ended);
bool replayViewerEnded();
// Shown in the panel title (the replay save's name). Empty = "Replay viewer".
void replayViewerSetReplayName(const wchar_t *name);
