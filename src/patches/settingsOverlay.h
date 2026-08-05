#pragma once

// In-game settings panel: rebind the patch's hotkeys with the mouse and
// keyboard instead of editing sh2-unofficial-patch.ini by hand. Changes apply
// immediately and are written back to the ini.
//
// Must be installed *after* every feature it lists (it reads their loaded
// bindings) and after the auto-market overlay (window-subclass order decides
// which panel sees input first). See docs/features/settings-overlay.md.

void installSettingsOverlay();
