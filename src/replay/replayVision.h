#pragma once

#include "../core/hotkey.h"

// Replay vision: what a spectator sees while a replay plays.
//
// 1. Ally vision ([replay] AllyVision, default on). The game draws some things
//    only for the local player and its allies: hidden Assassins and Outlaws,
//    Thieves out of disguise, building interiors (when the camera is close
//    enough), and Man Traps / Killing Pits. Each of those draw paths asks
//    allied(local, owner) (RVA 0x219e0). During playback those call sites,
//    and only those, answer "allied", so every player's buildings and units
//    look the way they look to that player's allies. The game's other
//    conditions (camera distance, hidden state) are unchanged.
//
// 2. Free view ([hotkeys] ReplayFreeView, default F). Toggles the camera
//    panel's Free View (the eye button), which hides the HUD and lets the
//    middle mouse button tilt and turn the camera. The replay blocks all
//    clicks, so the panel itself cannot be used during playback.
//
// Both are inert unless a replay is playing (spectatorGuardActive()).

void installReplayVision();

// The free-view key, for the settings panel. The setter applies at once and
// does not write the ini.
Hotkey replayFreeViewBinding();
void replayFreeViewSetBinding(const Hotkey &hk);
bool replayFreeViewInstalled();
