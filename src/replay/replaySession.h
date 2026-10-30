#pragma once

#include <cstdint>

// Replay session: continuous recording and in-game replay loading.
//
// Recording. Every match of the enabled kinds (multiplayer by default; single
// player off) is recorded without any tooling: at match start the recorder
// auto-arms against a fresh checkpoint save in the player's own Saves folder,
// named like `replay-1.31.26-905pm-k3.sh2`. Beside it the session writes a
// sidecar, `<same name>.s2replay`, holding everything playback needs that the
// save cannot carry: the command log (retimed to each command's recorded
// execution job), the
// iterator-1 cursor stream, the checkpoint occupancy-list order and the
// intra-drain execution-order plans. The sidecar is rewritten atomically every
// few seconds while the match runs and once more when it ends, so a match that
// ends by leaving, by a desync drop to the menu, by lord death / game over or by
// a hard crash still leaves a replay (a hard crash loses at most the last flush
// interval). Recording re-arms itself for the next match in the same process.
// A match that outlasts the recording buffers (about two hours of 8 players)
// is recorded up to that point: the recording ends there, exactly timed.
//
// Playback. Loading a save through the game's own Load menu passes through a
// hook on stock World::LoadGame (RVA 0x2ac60). When the save has a sidecar
// whose checkpoint hash matches the file being loaded, playback is armed right
// there -- before the load applies the multiplayer rules byte -- and the match
// that comes up is the replay: spectator input guard, replay viewer (`]`), and
// a hold at the end of the recording, or -- for a match that ended on a
// victory/defeat screen -- that same screen. Any other save loads untouched.
//
// Everything runs on the game thread (frame tick and the LoadGame hook), except
// the crash/exit flush, which reads the recording buffers as they stand.

void installReplaySession();

// The local player's victory (won) or defeat screen activated. Called from the
// Win/LoseScreen::OnActivate hook owner (endgameStats); only latches -- the frame tick ends the recording there
// (END_WIN/END_LOSE) or finishes a session replay that reached its own screen.
void replaySessionNoteOutcome(bool won);

// DLL_PROCESS_DETACH: write a last sidecar if a match is being recorded.
void replaySessionOnProcessExit();

// Runtime override of the recording kinds (tooling); the INI supplies defaults.
// fingerprintInterval 0 keeps the current one.
void replaySessionConfigure(bool recordMultiplayer, bool recordSinglePlayer,
                            uint32_t fingerprintInterval = 0);

// Tooling: end recordings at the capacity limit after this many recorded
// commands (0 = only when a buffer is nearly full), to exercise that path.
void replaySessionSetCommandBudget(uint32_t commands);

// Settings panel: whether multiplayer matches are recorded ([replay]
// RecordMultiplayer; takes effect from the next match), and how many replays
// to keep in Saves ([replay] MaxReplays, 1..99, default 10). When a new match would go past
// the limit, the oldest replays (save and sidecar) are deleted first. The
// setters do not write the ini -- the panel does.
const int REPLAY_KEEP_MIN = 1;
const int REPLAY_KEEP_MAX = 99;
const int REPLAY_KEEP_DEFAULT = 10;
bool replaySessionRecordMultiplayer();
void replaySessionSetRecordMultiplayer(bool on);
int replaySessionMaxReplays();
void replaySessionSetMaxReplays(int keep);

enum ReplaySessionMode : uint32_t {
    REPLAY_SESSION_IDLE = 0,
    REPLAY_SESSION_RECORDING = 1,
    REPLAY_SESSION_PLAYBACK = 2,
};

enum ReplaySessionLoadResult : uint32_t {
    REPLAY_LOAD_NONE = 0,         // no load seen yet
    REPLAY_LOAD_NOT_REPLAY = 1,   // no sidecar beside the save: a normal load
    REPLAY_LOAD_ARMED = 2,
    REPLAY_LOAD_BAD_SIDECAR = 3,  // unreadable, wrong magic/version, bad hash
    REPLAY_LOAD_CHECKPOINT = 4,   // the .sh2 is not the recorded checkpoint
    REPLAY_LOAD_NETWORKED = 5,    // refused inside a live networked session
    REPLAY_LOAD_COMMANDS = 6,     // command log refused (class table differs)
    REPLAY_LOAD_CONTROL = 7,      // the player refused to arm
    REPLAY_LOAD_EXE = 8,          // recorded by a different executable
};

#pragma pack(push, 1)
struct ReplaySessionStatus {
    uint32_t mode;
    uint32_t recordMultiplayer;
    uint32_t recordSinglePlayer;
    uint32_t hooks;          // bit0 LoadGame hook, bit1 crash filter
    uint32_t recordings;     // matches finalized this process
    uint32_t flushes;        // sidecar writes this process
    uint32_t flushFailures;
    uint32_t lastFlushBytes;
    uint32_t lastFlushMs;
    uint32_t lastEndReason;
    uint32_t lastRetimed;    // injected commands moved to their execution job
    uint32_t lastUnretimed;  // injected commands never executed, left out
    uint32_t lastExecPlans;
    uint32_t lastRetimeFail; // 0 ok; 1/3/4 retiming skipped; 2/5 truncated (replayRetime.h)
    uint32_t loads;          // LoadGame calls seen
    uint32_t lastLoadResult;
    uint32_t playbacks;      // replays armed from a load
    uint32_t endRelative;    // playback: last recorded job
    uint32_t ended;          // playback finished: 1 end hold, 2 win, 3 lose screen, 4 left
    uint32_t checkpointHashLo;
    uint32_t checkpointHashHi;
};
#pragma pack(pop)

void replaySessionGetStatus(ReplaySessionStatus *out);
// The current (or last) sidecar path, UTF-8.
const char *replaySessionSidecarPath();
