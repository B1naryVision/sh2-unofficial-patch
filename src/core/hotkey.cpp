#include "hotkey.h"
#include "config.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

// Names are matched case-insensitively with every space removed, and written
// back exactly as spelled here. The first entry for a given VK code is the
// canonical one hotkeyToString writes; later entries for the same code are
// accepted aliases, kept so ini files written against the older key-name list
// keep working.
struct KeyName {
    const char *name;
    int vk;
};

static const KeyName KEY_NAMES[] = {
    {"Mouse3", VK_MBUTTON},
    {"Mouse4", VK_XBUTTON1},
    {"Mouse5", VK_XBUTTON2},
    {"Space", VK_SPACE},
    {"Tab", VK_TAB},
    {"Enter", VK_RETURN},
    {"Return", VK_RETURN},
    {"Backspace", VK_BACK},
    {"Insert", VK_INSERT},
    {"Delete", VK_DELETE},
    {"Home", VK_HOME},
    {"End", VK_END},
    {"PageUp", VK_PRIOR},
    {"PageDown", VK_NEXT},
    {"Escape", VK_ESCAPE},
    {"Esc", VK_ESCAPE},
    {"Up", VK_UP},
    {"Down", VK_DOWN},
    {"Left", VK_LEFT},
    {"Right", VK_RIGHT},
    {"Backtick", VK_OEM_3},
    {"Grave", VK_OEM_3},
    {"Tilde", VK_OEM_3},
    {"Semicolon", VK_OEM_1},
    {"Plus", VK_OEM_PLUS},
    {"Equals", VK_OEM_PLUS},
    {"Comma", VK_OEM_COMMA},
    {"Minus", VK_OEM_MINUS},
    {"Period", VK_OEM_PERIOD},
    {"Slash", VK_OEM_2},
    {"LBracket", VK_OEM_4},
    {"Backslash", VK_OEM_5},
    {"RBracket", VK_OEM_6},
    {"Quote", VK_OEM_7},
    {"CapsLock", VK_CAPITAL},
    {"ScrollLock", VK_SCROLL},
    {"NumLock", VK_NUMLOCK},
    {"Pause", VK_PAUSE},
    {"PrintScreen", VK_SNAPSHOT},
    {"NumpadPlus", VK_ADD},
    {"NumpadMinus", VK_SUBTRACT},
    {"NumpadMultiply", VK_MULTIPLY},
    {"NumpadDivide", VK_DIVIDE},
    {"NumpadDecimal", VK_DECIMAL},
};

// Compares an already-lowercased token against a display-cased table name.
static bool tokenEquals(const char *lowerTok, const char *name) {
    while (*lowerTok && *name) {
        if (*lowerTok != (char)tolower((unsigned char)*name)) {
            return false;
        }

        ++lowerTok;
        ++name;
    }

    return *lowerTok == 0 && *name == 0;
}

// Keys the keyboard driver reports with the extended-key bit, needed to get the
// right name out of GetKeyNameTextA (its scan codes collide with the numpad).
static const int EXTENDED_VKS[] = {
    VK_INSERT, VK_DELETE, VK_HOME,  VK_END,   VK_PRIOR,    VK_NEXT,    VK_UP,
    VK_DOWN,   VK_LEFT,   VK_RIGHT, VK_RMENU, VK_RCONTROL, VK_NUMLOCK, VK_DIVIDE,
};

bool hotkeyIsBound(const Hotkey &hk) { return hk.vk != 0; }

bool hotkeySame(const Hotkey &a, const Hotkey &b) { return a.vk == b.vk && a.mods == b.mods; }

// The modifier bit a key *is*, so a binding on a bare modifier key does not
// fail its own exact-modifier test. 0 for every ordinary key.
static int modBitForVk(int vk) {
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        return HK_CTRL;
    }

    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        return HK_SHIFT;
    }

    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        return HK_ALT;
    }

    return 0;
}

bool hotkeyIsModifierVk(int vk) { return modBitForVk(vk) != 0; }

// ── string → Hotkey ─────────────────────────────────────────────────────────────

// Lowercases `text` into `out`, dropping whitespace. Returns false if it does
// not fit, so an over-long value falls back to the caller's default.
static bool normalize(const char *text, char *out, int outLen) {
    int n = 0;

    for (const char *p = text; *p; ++p) {
        if (isspace((unsigned char)*p)) {
            continue;
        }

        if (n + 1 >= outLen) {
            return false;
        }

        out[n++] = (char)tolower((unsigned char)*p);
    }

    out[n] = 0;
    return n > 0;
}

// One normalized token → VK code. Returns 0 if the token names no key.
static int parseKeyToken(const char *tok) {
    size_t len = strlen(tok);

    if (len == 0) {
        return 0;
    }

    // Single letter or digit: the VK code is the ASCII uppercase character.
    if (len == 1 && isalnum((unsigned char)tok[0])) {
        return toupper((unsigned char)tok[0]);
    }

    if (tok[0] == 'f' && isdigit((unsigned char)tok[1])) {
        int fn = atoi(tok + 1);

        if (fn >= 1 && fn <= 24) {
            return VK_F1 + fn - 1;
        }

        return 0;
    }

    if (!strncmp(tok, "numpad", 6) && len == 7 && isdigit((unsigned char)tok[6])) {
        return VK_NUMPAD0 + (tok[6] - '0');
    }

    for (const KeyName &k : KEY_NAMES) {
        if (tokenEquals(tok, k.name)) {
            return k.vk;
        }
    }

    // Raw virtual-key code, e.g. 0x48, for anything with no name above.
    if (tok[0] == '0' && tok[1] == 'x') {
        long vk = strtol(tok + 2, nullptr, 16);

        if (vk > 0 && vk < 255) {
            return (int)vk;
        }
    }

    return 0;
}

// Modifier bit for a normalized token, or 0 if it is not a modifier name.
static int parseModToken(const char *tok) {
    if (!strcmp(tok, "ctrl") || !strcmp(tok, "control")) {
        return HK_CTRL;
    }

    if (!strcmp(tok, "shift")) {
        return HK_SHIFT;
    }

    if (!strcmp(tok, "alt")) {
        return HK_ALT;
    }

    return 0;
}

bool hotkeyParse(const char *text, Hotkey &out) {
    if (!text) {
        return false;
    }

    char norm[HOTKEY_NAME_MAX];

    if (!normalize(text, norm, sizeof(norm))) {
        return false;
    }

    if (!strcmp(norm, "none") || !strcmp(norm, "off") || !strcmp(norm, "disabled")) {
        out.vk = 0;
        out.mods = 0;
        return true;
    }

    Hotkey hk = {0, 0};
    const char *p = norm;

    while (*p) {
        char tok[HOTKEY_NAME_MAX];
        int n = 0;

        while (*p && *p != '+') {
            tok[n++] = *p++;
        }

        tok[n] = 0;

        // A '+' with nothing after it (or two in a row) is malformed.
        bool isLast = (*p != '+');

        if (*p == '+') {
            ++p;
        }

        if (isLast) {
            hk.vk = parseKeyToken(tok);

            if (hk.vk == 0) {
                return false;
            }
        } else {
            int mod = parseModToken(tok);

            if (mod == 0) {
                return false; // only modifiers may precede the key
            }

            hk.mods |= mod;
        }
    }

    if (hk.vk == 0) {
        return false;
    }

    out = hk;
    return true;
}

// ── Hotkey → string ─────────────────────────────────────────────────────────────

// Appends as much of `s` as fits and keeps `buf` NUL-terminated.
static void appendStr(char *buf, int bufLen, int &len, const char *s) {
    while (*s && len + 1 < bufLen) {
        buf[len++] = *s++;
    }

    buf[len] = 0;
}

static bool isExtendedVk(int vk) {
    for (int ext : EXTENDED_VKS) {
        if (ext == vk) {
            return true;
        }
    }

    return false;
}

// Name of the key itself (no modifiers). `friendly` allows the layout-specific
// GetKeyNameTextA name for keys the table does not cover; without it those fall
// back to a raw hex code, which hotkeyParse accepts.
static void keyName(int vk, char *buf, int bufLen, bool friendly) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        buf[0] = (char)vk;
        buf[1] = 0;
        return;
    }

    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(buf, bufLen, "F%d", vk - VK_F1 + 1);
        return;
    }

    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(buf, bufLen, "Numpad%d", vk - VK_NUMPAD0);
        return;
    }

    for (const KeyName &k : KEY_NAMES) {
        if (k.vk == vk) {
            snprintf(buf, bufLen, "%s", k.name);
            return;
        }
    }

    if (friendly) {
        // user32 call — fine from the UI/draw path, never from DllMain.
        LONG lparam = (LONG)(MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC) << 16);

        if (isExtendedVk(vk)) {
            lparam |= 1 << 24;
        }

        if (lparam != 0 && GetKeyNameTextA(lparam, buf, bufLen) > 0) {
            return;
        }
    }

    snprintf(buf, bufLen, "0x%02X", vk);
}

static void formatHotkey(const Hotkey &hk, char *buf, int bufLen, const char *sep, bool friendly) {
    if (bufLen <= 0) {
        return;
    }

    buf[0] = 0;
    int len = 0;

    if (!hotkeyIsBound(hk)) {
        appendStr(buf, bufLen, len, "None");
        return;
    }

    if (hk.mods & HK_CTRL) {
        appendStr(buf, bufLen, len, "Ctrl");
        appendStr(buf, bufLen, len, sep);
    }

    if (hk.mods & HK_SHIFT) {
        appendStr(buf, bufLen, len, "Shift");
        appendStr(buf, bufLen, len, sep);
    }

    if (hk.mods & HK_ALT) {
        appendStr(buf, bufLen, len, "Alt");
        appendStr(buf, bufLen, len, sep);
    }

    char key[HOTKEY_NAME_MAX];
    keyName(hk.vk, key, sizeof(key), friendly);
    appendStr(buf, bufLen, len, key);
}

void hotkeyToString(const Hotkey &hk, char *buf, int bufLen) {
    formatHotkey(hk, buf, bufLen, "+", false);
}

void hotkeyDisplayName(const Hotkey &hk, char *buf, int bufLen) {
    formatHotkey(hk, buf, bufLen, " + ", true);
}

// ── ini ─────────────────────────────────────────────────────────────────────────

Hotkey hotkeyLoad(const char *section, const char *key, const Hotkey &def) {
    char raw[HOTKEY_NAME_MAX];

    if (!configString(section, key, raw, sizeof(raw))) {
        return def;
    }

    Hotkey hk = {0, 0};

    if (!hotkeyParse(raw, hk)) {
        return def;
    }

    return hk;
}

bool hotkeySave(const char *section, const char *key, const Hotkey &hk) {
    const char *ini = configIniPath();

    if (!ini || !ini[0]) {
        return false;
    }

    char text[HOTKEY_NAME_MAX];
    hotkeyToString(hk, text, sizeof(text));

    return configSetString(section, key, text);
}

// ── live key state ──────────────────────────────────────────────────────────────

static int modsFromState(bool async) {
    int mods = 0;
    int ctrl = async ? GetAsyncKeyState(VK_CONTROL) : GetKeyState(VK_CONTROL);
    int shift = async ? GetAsyncKeyState(VK_SHIFT) : GetKeyState(VK_SHIFT);
    int alt = async ? GetAsyncKeyState(VK_MENU) : GetKeyState(VK_MENU);

    if (ctrl & 0x8000) {
        mods |= HK_CTRL;
    }

    if (shift & 0x8000) {
        mods |= HK_SHIFT;
    }

    if (alt & 0x8000) {
        mods |= HK_ALT;
    }

    return mods;
}

bool hotkeyHeld(const Hotkey &hk) {
    if (!hotkeyIsBound(hk)) {
        return false;
    }

    if (!(GetAsyncKeyState(hk.vk) & 0x8000)) {
        return false;
    }

    return (modsFromState(true) & ~modBitForVk(hk.vk)) == hk.mods;
}

bool gameWindowFocused() {
    HWND foreground = GetForegroundWindow();

    if (!foreground) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);

    return pid == GetCurrentProcessId();
}

bool hotkeyMatchesKeyDown(const Hotkey &hk, int vk) {
    if (!hotkeyIsBound(hk) || hk.vk != vk) {
        return false;
    }

    return (modsFromState(false) & ~modBitForVk(hk.vk)) == hk.mods;
}
