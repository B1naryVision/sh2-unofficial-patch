#pragma once

// Which game screen is on display. Each screen is registered once at startup and
// kept for the process lifetime; the one on display is the one whose Pane parent
// (+0x80) is set. Two plain reads each, so safe from any hook.
// See docs/features/lobby-panel.md.

// The multiplayer lobby (ScenarioLobbyScreen).
bool lobbyScreenUp();

// A match in progress (GameScreen), single- or multiplayer.
bool gameScreenUp();
