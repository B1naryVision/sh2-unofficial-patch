#pragma once
#include "snapshot.h"

// Renders a finished EndgameSnapshot in a layered overlay window owned by the
// game window. Knows nothing about game memory — presentation only.
//
// The window class is registered lazily on the first show (a user32 call is
// not loader-lock-safe, so it must not happen at patch install — see
// src/dllmain.cpp).

void showStatsOverlay(const EndgameSnapshot &snap);
void closeStatsOverlay();
