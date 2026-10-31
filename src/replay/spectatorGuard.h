#pragma once

#include <cstdint>

// Playback is a non-authoritative spectator session: the spectator camera and
// inspection must not emit or alter simulation commands.
//
// Two independent mechanisms, because neither alone is trustworthy:
//
// 1. Prevention. While playback is armed, a window subclass swallows the
//    message classes that can reach a UI pane and post a Chore -- mouse
//    buttons and the keyboard -- while leaving camera-only input (mouse move,
//    wheel, arrow keys, W/A/S/D, Q/E, Space, Tab) through, so the viewer can still
//    watch the match. The replay viewer (replayViewer.h) is offered each
//    message first.
//
// 2. Proof. The admission hook reports any UI-origin command admitted while
//    playback is armed. If the game reads a device directly rather than
//    through the window queue, prevention silently fails and only this counter
//    would show it. A nonzero count is a hard FAIL of the spectator contract,
//    not a warning.
//
// The guard is inert unless playback is armed: recording sessions and ordinary
// play are untouched.

void installSpectatorGuard();

// Called by the replay player when playback is armed (1) or stopped (0).
void spectatorGuardSetActive(uint32_t active);

// True while playback is armed. Read by the admission hook on the simulation
// thread; the flag is a single aligned byte written only by the replay player.
bool spectatorGuardActive();

// Counted by the admission hook when a UI-origin command is admitted while the
// guard is active. Any nonzero value means live local input reached the
// simulation during playback.
void spectatorGuardNoteLocalCommand();
uint32_t spectatorGuardLocalCommands();

// Messages the guard actually swallowed, so a zero local-command count can be
// distinguished from "the guard was never exercised".
uint32_t spectatorGuardBlockedMessages();

// True once the window subclass is in place. Until then prevention is not
// active and only the proof counter is meaningful.
bool spectatorGuardHooked();

// Presses suppressed on the direct-device path. The game does not read input
// only through the window queue: four functions poll USER32!GetAsyncKeyState,
// including the hotkey dispatcher that can post MoveTroopsChore,
// PlaceBuildingChore, GameGroundFeatureChore and PlaceTowerEquipmentChore. A
// window subclass cannot affect a polled key state, so prevention needs this
// second mechanism. Counts actual presses suppressed, not polls taken.
uint32_t spectatorGuardBlockedPolls();

// How many GetAsyncKeyState import thunks are redirected, across every loaded
// module. Zero means the direct-device path is unguarded; the executable and
// dxrenderer.dll both import it, so the expected value is at least two.
uint32_t spectatorGuardPollThunks();

// For input the instrumentation DLL polls itself. The patch hotkeys
// (stop-troops, attack-toggle, siege-camp) and shift-recruit call
// GetAsyncKeyState from inside this module, so neither the window subclass nor
// the executable's import thunk can see them -- and stop-troops posts
// StopAllTroopsChore. Returns true while playback is armed, counting the
// suppression into spectatorGuardBlockedPolls(). Call it only once the key is
// known to be held, or the count becomes a per-frame statistic.
bool spectatorGuardSuppressOwnPoll();
