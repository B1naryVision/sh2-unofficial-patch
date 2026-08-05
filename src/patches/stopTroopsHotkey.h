#pragma once
#include "../core/hotkey.h"

void installStopTroopsHotkey();

// Live binding, for the settings overlay. The setter takes effect on the next
// frame and does not persist the value — the overlay writes the ini itself.
// stopTroopsInstalled() is false when the hotkey was unbound at startup, in
// which case nothing was hooked and a new binding applies only after a restart.
Hotkey stopTroopsBinding();
void stopTroopsSetBinding(const Hotkey &hk);
bool stopTroopsInstalled();
