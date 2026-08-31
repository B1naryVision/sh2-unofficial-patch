#include "config.h"
#include "hook.h"
#include <cstdlib>
#include <cstring>
#include <windows.h>

// Uses only kernel32 calls (GetModuleFileNameA, GetPrivateProfileStringA), so
// it is safe to run under the loader lock in DllMain like every other install
// step. Key/value format is documented in docs/features/configuration.md.

static const char *INI_FILENAME = "sh2-unofficial-patch.ini";

// Sentinel returned by GetPrivateProfileStringA when a key is absent, so a
// present-but-equal-to-default value is distinguishable from a missing one.
static const char *MISSING = "\x01";

static char s_iniPath[MAX_PATH] = "";

void loadConfig() {
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(g_patchModule, path, MAX_PATH);

    if (len == 0 || len >= MAX_PATH) {
        return;
    }

    char *lastSlash = strrchr(path, '\\');

    if (!lastSlash || (lastSlash - path) + 1 + strlen(INI_FILENAME) + 1 > MAX_PATH) {
        return;
    }

    strcpy(lastSlash + 1, INI_FILENAME);
    strcpy(s_iniPath, path);
}

// Returns false when the ini or the key is absent.
static bool readValue(const char *section, const char *key, char *buf, DWORD bufLen) {
    if (!s_iniPath[0]) {
        return false;
    }

    GetPrivateProfileStringA(section, key, MISSING, buf, bufLen, s_iniPath);

    return strcmp(buf, MISSING) != 0;
}

const char *configIniPath() { return s_iniPath; }

bool configString(const char *section, const char *key, char *buf, int bufLen) {
    if (bufLen <= 0) {
        return false;
    }

    buf[0] = 0;
    return readValue(section, key, buf, (DWORD)bufLen);
}

bool configSetString(const char *section, const char *key, const char *value) {
    if (!s_iniPath[0]) {
        return false;
    }

    if (!WritePrivateProfileStringA(section, key, value, s_iniPath)) {
        return false;
    }

    // Flushes the ini write cache so the file on disk is up to date even if the
    // game is killed rather than closed.
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, s_iniPath);
    return true;
}

int configSectionNames(char *buf, int bufLen) {
    if (bufLen <= 0) {
        return 0;
    }

    buf[0] = 0;

    if (!s_iniPath[0]) {
        return 0;
    }

    return (int)GetPrivateProfileSectionNamesA(buf, (DWORD)bufLen, s_iniPath);
}

int configSection(const char *section, char *buf, int bufLen) {
    if (bufLen <= 0) {
        return 0;
    }

    buf[0] = 0;

    if (!s_iniPath[0]) {
        return 0;
    }

    return (int)GetPrivateProfileSectionA(section, buf, (DWORD)bufLen, s_iniPath);
}

int configInt(const char *section, const char *key, int defaultValue) {
    char raw[64];

    if (!readValue(section, key, raw, sizeof(raw))) {
        return defaultValue;
    }

    char *end = nullptr;
    long value = strtol(raw, &end, 10);

    if (end == raw) {
        return defaultValue;
    }

    return (int)value;
}

float configFloat(const char *section, const char *key, float defaultValue) {
    char raw[64];

    if (!readValue(section, key, raw, sizeof(raw))) {
        return defaultValue;
    }

    char *end = nullptr;
    double value = strtod(raw, &end);

    if (end == raw) {
        return defaultValue;
    }

    return (float)value;
}
