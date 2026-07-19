#include "config.h"
#include "hook.h"
#include <cctype>
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

struct NamedKey {
    const char *name;
    int vk;
};

static const NamedKey NAMED_KEYS[] = {
    {"mouse3", VK_MBUTTON}, {"mouse4", VK_XBUTTON1}, {"mouse5", VK_XBUTTON2}, {"space", VK_SPACE},
    {"tab", VK_TAB},        {"enter", VK_RETURN},    {"backspace", VK_BACK},  {"insert", VK_INSERT},
    {"delete", VK_DELETE},  {"home", VK_HOME},       {"end", VK_END},         {"pageup", VK_PRIOR},
    {"pagedown", VK_NEXT},
};

int configHotkey(const char *key, int defaultVk) {
    char raw[32];

    if (!readValue("hotkeys", key, raw, sizeof(raw))) {
        return defaultVk;
    }

    char name[32];
    size_t n = 0;

    for (const char *p = raw; *p && n + 1 < sizeof(name); p++) {
        if (!isspace((unsigned char)*p)) {
            name[n++] = (char)tolower((unsigned char)*p);
        }
    }
    name[n] = 0;

    if (n == 0) {
        return defaultVk;
    }

    if (!strcmp(name, "none") || !strcmp(name, "off") || !strcmp(name, "disabled")) {
        return 0;
    }

    // Single letter or digit: the VK code is the ASCII uppercase character.
    if (n == 1 && isalnum((unsigned char)name[0])) {
        return toupper((unsigned char)name[0]);
    }

    // F1..F24
    if (name[0] == 'f' && isdigit((unsigned char)name[1])) {
        int fn = atoi(name + 1);

        if (fn >= 1 && fn <= 24) {
            return VK_F1 + fn - 1;
        }
    }

    for (const NamedKey &k : NAMED_KEYS) {
        if (!strcmp(name, k.name)) {
            return k.vk;
        }
    }

    // Raw virtual-key code, e.g. 0x48
    if (name[0] == '0' && name[1] == 'x') {
        long vk = strtol(name + 2, nullptr, 16);

        if (vk > 0 && vk < 255) {
            return (int)vk;
        }
    }

    return defaultVk;
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
