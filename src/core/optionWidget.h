#pragma once
#include <windows.h>

// A value control for the in-game settings panel: either a draggable slider
// over an integer range, or a click-to-cycle choice between named options.
//
// Like KeybindWidget it owns no window and no drawing surface: the host overlay
// gives it a panel-local rectangle, draws it into its own GDI panel bitmap on
// repaint, and forwards mouse messages to it from the WndProc it already
// subclasses.
//
// Values are integers throughout, including the ones the ini stores as decimals
// — a multiplier of 1.5 is carried as 15 tenths. That keeps every value the
// widget touches free of float arithmetic, which matters because the host draws
// from inside the EndScene detour. The host converts to a float only when it
// hands the value to a feature, on the WndProc path.
//
// See docs/features/settings-overlay.md.

enum OptionKind {
    OPTION_SLIDER, // drag or click anywhere on the track
    OPTION_CHOICE, // click cycles to the next named option
};

struct OptionWidget {
    OptionKind kind;
    int value; // slider: min..max in steps; choice: index into choices
    int min, max, step; // slider range
    const char *const *choices; // choice: option names
    int choiceCount;
    RECT bounds; // panel-local control box
    bool dragging; // slider handle is being dragged
    bool changed; // an accepted edit is pending a save
};

void optionWidgetInitSlider(
    OptionWidget &w, int value, int min, int max, int step, int left, int top, int right, int bottom
);

void optionWidgetInitChoice(
    OptionWidget &w, int value, const char *const *choices, int choiceCount, int left, int top,
    int right, int bottom
);

// True if a panel-local point is inside the control box.
bool optionWidgetHit(const OptionWidget &w, int x, int y);

// Sets the value directly (a host restoring a default). Clamped and snapped
// like a real edit, but does not raise `changed` — the host asked for it.
void optionWidgetSetValue(OptionWidget &w, int value);

// Left button down inside the control: a slider jumps to the point and starts
// dragging, a choice advances one option. Returns true when the value changed.
bool optionWidgetOnMouseDown(OptionWidget &w, int x, int y);

// Drag tracking. Does nothing unless this widget is mid-drag, so a host can
// call it unconditionally. Returns true when the value changed.
bool optionWidgetOnMouseMove(OptionWidget &w, int x);

// Ends a drag. Returns true if this widget was the one dragging, which is also
// the host's cue that a drag has finished and the value is ready to persist.
bool optionWidgetOnMouseUp(OptionWidget &w);

// True while any widget is being dragged, so the host knows to swallow mouse
// movement rather than letting the game act on it.
bool optionWidgetDragActive();

// Draws the control. A choice renders as a button with its option name; a
// slider renders as a track with a filled portion and a handle, and the host
// draws the formatted value in its own column beside it.
void optionWidgetDraw(const OptionWidget &w, HDC dc);

// One-shot read of the changed flag, for the host's "persist it" step.
bool optionWidgetTakeChanged(OptionWidget &w);
