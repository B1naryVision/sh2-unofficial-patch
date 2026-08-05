#pragma once
#include "hotkey.h"
#include <windows.h>

// A reusable "click it, then press a key" rebind control for the in-game
// overlays. It owns no window and no drawing surface: the host overlay gives it
// a panel-local rectangle, draws it into its own GDI panel bitmap on repaint,
// and forwards window messages to it from the WndProc it already subclasses.
//
// See docs/features/keybinding.md for the host wiring and the input-swallowing
// contract.

struct KeybindWidget {
    Hotkey binding; // current binding; the host reads this after a change
    RECT bounds; // panel-local button box
    bool capturing; // listening for the next key
    bool changed; // an accepted rebind is pending a save
};

void keybindWidgetInit(
    KeybindWidget &w, const Hotkey &binding, int left, int top, int right, int bottom
);

// True if a panel-local point is inside the button.
bool keybindWidgetHit(const KeybindWidget &w, int x, int y);

// Sets the binding directly (a "clear to None" button, or a host restoring
// defaults). Cancels any capture and flags the change like a real rebind would,
// so the host's persist step picks it up.
void keybindWidgetSetBinding(KeybindWidget &w, const Hotkey &hk);

// Enters the listening state. Only one widget listens at a time — starting one
// cancels any other.
void keybindWidgetBeginCapture(KeybindWidget &w);

void keybindWidgetCancelCapture(KeybindWidget &w);

// Feeds one window message to the widget. Returns true when the message was
// consumed and the host must swallow it (return 0 from its WndProc). While
// listening that is *every* keyboard and mouse message, so no game action can
// fire behind the rebind prompt. Does nothing (returns false) otherwise, so a
// host can call it unconditionally.
bool keybindWidgetOnMessage(KeybindWidget &w, UINT msg, WPARAM wparam, LPARAM lparam);

// Draws the button into `dc` with the font the caller has selected: `caption`
// (may be null) right-aligned just left of the box, and inside the box either
// the binding name or the "Press any key..." prompt.
void keybindWidgetDraw(const KeybindWidget &w, HDC dc, const char *caption);

// One-shot read of the changed flag, for the host's "persist it" step.
bool keybindWidgetTakeChanged(KeybindWidget &w);

// True while any widget is listening. Frame-tick hotkeys poll the keyboard
// directly (GetAsyncKeyState) and so bypass the window procedure entirely —
// they must check this to stay quiet during a rebind.
bool keybindCaptureActive();
