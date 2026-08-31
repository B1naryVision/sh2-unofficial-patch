#pragma once

void installPanSpeed();

// Live value, for the settings overlay. Carried in tenths (15 = 1.5x) so the
// overlay never does float arithmetic; the setter converts. See zoomSpeed.h for
// the shared semantics of the lazy install and the failure flag.
int panSpeedTenths();
void panSpeedSetTenths(int tenths);
bool panSpeedFailed();
