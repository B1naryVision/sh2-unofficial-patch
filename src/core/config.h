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

int configInt(const char *section, const char *key, int defaultValue);

// Enumerates all ini section names into buf as a double-NUL-terminated list
// (walk with strlen+1 until an empty string). Returns chars written; 0 if none.
int configSectionNames(char *buf, int bufLen);

// Reads a whole ini section as a double-NUL-terminated list of "key=value"
// entries. Returns chars written; 0 if the section is absent/empty.
int configSection(const char *section, char *buf, int bufLen);
