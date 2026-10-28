#pragma once
#include "../core/hotkey.h"

// Read-only panel on the multiplayer lobby screen listing each player's patch
// version and the host, so everyone can see before the match starts whether
// patch-only rules are possible. See docs/features/lobby-panel.md.
//
// Install after the handshake (it reads its snapshot).
void installLobbyPanel();

// Live binding of the show/hide key, for the settings overlay. The setter takes
// effect on the next frame and does not persist the value.
Hotkey lobbyPanelBinding();
void lobbyPanelSetBinding(const Hotkey &hk);
bool lobbyPanelInstalled();
