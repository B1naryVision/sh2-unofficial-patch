#pragma once

#include <cstdint>

// Replay session: recording and in-game replay loading.
//
// Recording. Every match of the enabled kinds (multiplayer by default; single
// player off) is recorded without any tooling: at match start the recorder
// arms against a fresh checkpoint save in the player's own Saves folder, named
// like `replay-1.31.26-905pm-k3.sh2`. Beside it the session writes a sidecar,
// `<same name>.s2replay`, holding everything playback needs that the save
// cannot carry (replayFormat.h). The sidecar is rewritten atomically every few
// seconds while the match runs -- on a worker thread, from a snapshot taken on
// the game thread -- and once more when it ends, so a match that ends by
// leaving, by a desync drop to the menu, by lord death / game over or by a hard
// crash still leaves a replay (a hard crash loses at most the last flush
// interval). A match that outlasts the recording buffers (about two hours of 8
// players) is recorded up to that point: the recording ends there, exactly
// timed. The recording buffers (about 60 MB) exist only while a match is being
// recorded.
//
// Playback. Loading a save through the game's own Load menu passes through a
// hook on stock World::LoadGame (RVA 0x2ac60). When the save has a sidecar
// whose checkpoint hash matches the file being loaded, playback is armed right
// there -- before the load applies the multiplayer rules byte -- and the match
// that comes up is the replay: spectator input guard, replay viewer (`]`), and
// a hold at the end of the recording, or -- for a match that ended on a
// victory/defeat screen -- that same screen. Any other save loads untouched.
// The session alone decides when a playback ends.

void installReplaySession();

// The local player's victory (won) or defeat screen activated. Called from the
// Win/LoseScreen::OnActivate hook owner (endgameStats); only latches -- the
// frame tick ends the recording there (END_WIN/END_LOSE) or finishes a replay
// that reached its own screen.
void replaySessionNoteOutcome(bool won);

// The game window is closing (WM_CLOSE / WM_DESTROY / WM_ENDSESSION, seen by
// the spectator guard's window subclass on the game thread): write a last
// sidecar if a match is being recorded, once. Never called from DllMain, where
// file I/O under the loader lock can hang the exit.
void replaySessionOnWindowClosing();

// Settings panel: whether multiplayer matches are recorded ([replay]
// RecordMultiplayer; takes effect from the next match), and how many replays
// to keep in Saves ([replay] MaxReplays, 1..99, default 10). When a new match
// would go past the limit, the oldest replays (save and sidecar) are deleted
// first. The setters do not write the ini -- the panel does.
const int REPLAY_KEEP_MIN = 1;
const int REPLAY_KEEP_MAX = 99;
const int REPLAY_KEEP_DEFAULT = 10;
bool replaySessionRecordMultiplayer();
void replaySessionSetRecordMultiplayer(bool on);
int replaySessionMaxReplays();
void replaySessionSetMaxReplays(int keep);
