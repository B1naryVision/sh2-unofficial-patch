#pragma once

// Hides multiplayer lobbies whose game has already started from the lobby
// browser. See docs/bugs/in-progress-lobbies.md.
void installLobbyInProgressFilter();

// Live on/off, for the settings overlay. Enabling installs the patch if it is
// not already in — the write happens on the game thread at the next frame.
// Disabling leaves the hooks in place and lets every row through, so the switch
// is safe to flip at any time; it takes effect on the next list refresh.
// lobbyFilterFailed() is true only when an install was rejected because the
// game's bytes did not match, which a restart would not fix.
bool lobbyFilterEnabled();
void lobbyFilterSetEnabled(bool enabled);
bool lobbyFilterFailed();
