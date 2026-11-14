#pragma once

void installAttackVoices();

// Live on/off, for the settings overlay. Enabling installs the patch if it is
// not already in; the write happens on the game thread at the next frame.
// Disabling leaves the hooks in place and only silences the voice, so the switch
// is safe to flip at any time. attackVoicesFailed() is true only when an install
// was rejected because the game's bytes did not match.
bool attackVoicesEnabled();
void attackVoicesSetEnabled(bool enabled);
bool attackVoicesFailed();
