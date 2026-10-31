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
//    message first. The game also polls GetAsyncKeyState directly, so the
//    import of the two modules that do (the exe and dxrenderer.dll) is
//    redirected for the duration of the playback and put back after it.
//
// 2. Enforcement. The admission hook refuses any UI-origin command that
//    reaches it while playback is armed and the in-game menu is closed. If the
//    game reads a device another way, prevention silently fails and this is
//    what still holds the line. Commands posted from the open in-game menu are
//    admitted.
//
// The guard is inert unless playback is armed: recording sessions and ordinary
// play are untouched.

void installSpectatorGuard();

// Called by the replay player when playback is armed (1) or stopped (0), on the
// game thread.
void spectatorGuardSetActive(uint32_t active);

// True while playback is armed. Read by the admission hook on the simulation
// thread; the flag is a single aligned word written only by the replay player.
bool spectatorGuardActive();
