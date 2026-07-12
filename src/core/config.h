#pragma once

// Optional user configuration read from sh2-unofficial-patch.ini next to the
// patch DLL in the game directory. A missing file or missing key falls back
// to the built-in default. See docs/features/configuration.md.

// Resolves the ini path. Must run before any of the getters (registry.cpp
// calls it first).
void loadConfig();

// Virtual-key code for a [hotkeys] entry. Returns 0 when the user disabled
// the key ("None"), the default when the key is missing or unparseable.
int configHotkey(const char *key, int defaultVk);

float configFloat(const char *section, const char *key, float defaultValue);
