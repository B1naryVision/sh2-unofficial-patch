#pragma once
#include "../core/hotkey.h"

void installAttackHotkey();

// Live binding, for the settings overlay — see stopTroopsHotkey.h for the
// contract (setter does not persist; installed() false means restart required).
Hotkey attackToggleBinding();
void attackToggleSetBinding(const Hotkey &hk);
bool attackToggleInstalled();
