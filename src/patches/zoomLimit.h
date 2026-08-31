#pragma once

void installZoomLimit();

// Live on/off, for the settings overlay. Enabling installs the patch if it is
// not already in — the write happens on the game thread at the next frame.
// Disabling hands the engine's own limit back on the next frame rather than
// unhooking, so the switch is safe to flip at any time.
// zoomLimitFailed() is true only when an install was rejected because the
// game's bytes did not match, which a restart would not fix.
bool zoomLimitEnabled();
void zoomLimitSetEnabled(bool enabled);
bool zoomLimitFailed();
