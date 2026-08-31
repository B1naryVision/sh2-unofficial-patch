#pragma once

void installSiegeCampHotkey();

// Live on/off, for the settings overlay. Enabling installs the patch if it is
// not already in — the write happens on the game thread at the next frame.
// Disabling leaves the hooks in place and lets the stock path run, so the
// switch is safe to flip at any time.
// siegeCampFailed() is true only when an install was rejected because the
// game's bytes did not match, which a restart would not fix.
bool siegeCampTwoStep();
void siegeCampSetTwoStep(bool twoStep);
bool siegeCampFailed();
