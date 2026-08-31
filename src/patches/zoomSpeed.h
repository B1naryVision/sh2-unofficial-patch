#pragma once

void installZoomSpeed();

// Live value, for the settings overlay. The multiplier is carried in tenths
// (15 = 1.5x) so the overlay never does float arithmetic; the setter converts.
// Setting a multiplier other than 1.0 installs the patch if it is not already
// in — the write happens on the game thread at the next frame, so the setter
// takes effect within a frame rather than immediately.
// zoomSpeedFailed() is true only when that install was rejected because the
// game's bytes did not match, which a restart would not fix.
int zoomSpeedTenths();
void zoomSpeedSetTenths(int tenths);
bool zoomSpeedFailed();
