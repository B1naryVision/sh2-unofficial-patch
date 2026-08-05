#pragma once

// A key binding: a virtual-key code plus optional Ctrl/Shift/Alt modifiers,
// stored in the ini in human-readable form ("Ctrl+Shift+F5", "Mouse4", "None").
// Shared by the hotkey patches, the config layer and the in-game keybind
// widget. See docs/features/keybinding.md.

enum HotkeyMod {
    HK_CTRL = 1,
    HK_SHIFT = 2,
    HK_ALT = 4,
};

struct Hotkey {
    int vk; // virtual-key code; 0 = unbound
    int mods; // OR of HotkeyMod
};

// Buffer size that fits any name the writers below produce, including the NUL.
const int HOTKEY_NAME_MAX = 64;

bool hotkeyIsBound(const Hotkey &hk);

bool hotkeySame(const Hotkey &a, const Hotkey &b);

// True for VK_CONTROL/VK_SHIFT/VK_MENU and their L/R variants — keys that act
// only as modifiers and are never accepted as the bound key itself.
bool hotkeyIsModifierVk(int vk);

// Parses "Ctrl+Shift+F5" / "Mouse4" / "None". Case-insensitive; whitespace is
// ignored, so "Ctrl + Shift + F5" works too. Returns false and leaves `out`
// untouched when the text names no key, letting callers keep their default —
// a typo never silently unbinds a feature ("None" must be explicit).
bool hotkeyParse(const char *text, Hotkey &out);

// Canonical ini form ("Ctrl+Shift+F5"). Always round-trips through hotkeyParse.
void hotkeyToString(const Hotkey &hk, char *buf, int bufLen);

// Friendlier UI form ("Ctrl + Shift + F5"), falling back to the active keyboard
// layout's own name (GetKeyNameTextA) for keys with no canonical name. That
// name is localized and may not parse back — never write this to the ini.
void hotkeyDisplayName(const Hotkey &hk, char *buf, int bufLen);

// `[section] key = <binding>` in sh2-unofficial-patch.ini. hotkeyLoad falls
// back to `def` for a missing or unparseable value. hotkeySave returns false
// when the file could not be written (no ini path resolved, or a read-only
// game directory).
Hotkey hotkeyLoad(const char *section, const char *key, const Hotkey &def);

bool hotkeySave(const char *section, const char *key, const Hotkey &hk);

// True while the key and exactly its modifiers are held, for frame-tick
// polling (GetAsyncKeyState). The modifier match is exact — "H" does not fire
// while Ctrl is down, which is what makes "H" and "Ctrl+H" distinct bindings.
bool hotkeyHeld(const Hotkey &hk);

// True when a WM_KEYDOWN/WM_SYSKEYDOWN for `vk` completes this binding, with
// the modifier state read from GetKeyState (valid inside a window procedure).
bool hotkeyMatchesKeyDown(const Hotkey &hk, int vk);

// True while a window of this process has the foreground. Polled hotkeys must
// check it: GetAsyncKeyState reports keys pressed in *other* applications, so
// without this a hotkey fires while the player is typing in a browser.
bool gameWindowFocused();
