#pragma once

// Order sounds during a replay.
//
// When a move or attack command executes, the game plays the group's marching,
// hooves and siege-engine rolls for the local player's own orders (FUN_005993d0,
// called by the move and attack executors after `byte [S2Game+5] == 0` and
// `chore player == local slot`). In a replay the local player is whoever
// recorded it, so a viewer watching the far side of the map still heard every
// order that player gave. While a replay plays the function returns at once.
//
// It only plays sounds: its stores are the argument blocks of its own
// playSound calls, and its callees are handle resolves into locals and
// read-only predicates. It runs only for the local player, who differs on every
// client, so it can never be part of the simulation; leaving it out changes
// nothing the replay reproduces.

void installReplayOrderSounds();

// The spectator guard calls this when a playback is armed (true) and when it
// stops (false), on the game thread.
void replayOrderSoundsSetMuted(bool muted);
