# Multiplayer Replays

**Status:** Unreleased  
**Affected version:** Stronghold 2 Steam v1.5.0  
**Patch type:** Opt-in (`[replay] RecordMultiplayer=1`). Hooks are installed at startup so a received replay can be played with recording off; in a match that is neither recorded nor replayed they return after one or two compares (see [Footprint when idle](#footprint-when-idle))

---

## Symptom / Motivation

Stronghold 2 has no replay system. Once a multiplayer match ends, the only
record of it is whatever the players remember. A save only captures a single
moment; nothing of what the players did afterwards is kept.

## What This Patch Does

With recording switched on, every multiplayer match is recorded. After the
match, the recording shows up as an ordinary save in the game's Load menu; loading it
plays the whole match back in the game's own engine, from the first job to the
end, with every player's commands applied exactly when they ran live.

While a replay plays, the viewer is a spectator:

- The camera moves as usual (mouse, wheel, arrow keys, W/A/S/D, Q/E, Space,
  Tab). Clicks and command keys are blocked, so nothing the viewer does can
  change the match.
- **`]`** shows or hides the **replay panel**: live per-player stats (gold,
  honour, popularity, troops, siege, gold and honour by source, units recruited,
  units killed by type) and a **Game Speed** slider.
- **`P`** pauses and resumes. **`Esc`** opens the game's menu (the replay pauses
  while it is open); leaving the match from there ends the replay.
- **`F`** toggles **free view** (the camera panel's eye button): the HUD is
  hidden and holding the middle mouse button turns and tilts the camera.
- **Ally vision** (on by default): every player's buildings and units are drawn
  the way that player's allies see them - building interiors when the camera is
  close enough, hidden Assassins and Outlaws, Thieves out of disguise, and
  unsprung Man Traps and Killing Pits.

When the recording runs out, the replay holds on the last recorded moment and
the panel says so. A match that ended on a victory or defeat screen plays into
that same screen instead.

## Usage

Recording is **off by default** while mid-match checkpoints are still being
validated (see Known Issues).

1. Switch on **Multiplayer replays** in the settings panel, or set
   `[replay] RecordMultiplayer=1`.
2. Play a multiplayer match.
3. From the main menu, open **Load** and pick the save named `replay-...`.
4. Watch. Press `]` for the panel.

All settings live in the `[replay]` and `[hotkeys]` sections of
`sh2-unofficial-patch.ini`, and the main ones are also in the in-game settings
panel (`Ctrl+Shift+O`):

| Key | Default | Meaning |
|-----|---------|---------|
| `[replay] RecordMultiplayer` | `0` | `1` = record every multiplayer match. `0` = never. Takes effect from the next match. |
| `[replay] RecordSinglePlayer` | `0` | Also record single-player sessions. |
| `[replay] MaxReplays` | `10` | Replays this install recorded that are kept in the Saves folder (1-99). When a new match would go past this, the oldest are deleted first. Replays received from someone else do not count and are never deleted. |
| `[replay] AllyVision` | `1` | Ally vision while a replay plays. |
| `[replay] RecorderId` | generated | Written by the patch the first time it records; identifies this install's replays for `MaxReplays`. Do not copy it to another install. |
| `[hotkeys] ReplayPanel` | `RBracket` | Show / hide the replay panel. |
| `[hotkeys] ReplayFreeView` | `F` | Free view while a replay plays. |

The settings panel has rows for **Multiplayer replays**, **Replays to keep**,
**Replay panel** and **Replay free view**.

---

## Files It Creates

Each recorded match is two files in `Documents\Stronghold 2\Saves`:

| File | What it is |
|------|------------|
| `replay-<m>.<d>.<yy>-<h><mm><am/pm>-<xx>.sh2` | A normal game save, taken by the game's own save routine a moment after the match starts. This is the replay's starting point and what the Load menu lists. The name is the local date and time the match started, plus two random characters so two matches started in the same minute never collide. |
| `replay-....s2replay` | The **sidecar**, with the same name as the save. It holds everything the save cannot carry: the command log and the timing data that makes playback exact. |

A replay needs both files. To share one, send both; to delete one, delete both.
Without its sidecar the save is just an ordinary save and loads normally.

`MaxReplays` cleanup only ever deletes a `replay-*.s2replay` sidecar and the
`.sh2` save beside it, and only when the sidecar header carries this install's
recorder id (`REPLAY_BUNDLE_RECORDER_ID`, `reserved[1]`). The id is 32 random
bits from the OS generator, written once to `[replay] RecorderId` the first time
a match is recorded; if the ini cannot be written it lasts for that game session
only, which errs toward keeping files. (A hash of the computer name and Saves
path, used before, was guessable and collided between machines set up alike, so
a received replay could be pruned.) Age is the header's `recordedUtc`, not a
file time: copying, unzipping or downloading a replay keeps the sender's write
time, so ordering by mtime made a received replay look oldest and deleted it
first. A replay without the id (received, or from another install), an
unreadable sidecar, and a save without a sidecar are never touched, even if
named `replay-...`. Pruning happens only once a match's checkpoint is known to
be good, so a match that is never recorded never costs the oldest replay.

The `.s2replay` extension is deliberately not `.sh2...`: with 8.3 short names a
`*.sh2` search would also match it, and the game would list it as a save.

Debug builds (`make debug`) also write `killtrace.txt` to the game directory.
Release builds write no log files.

### Sidecar format

Little-endian, packed. A 144-byte header (`ReplayBundleHeader` in
`src/replay/replayFormat.h`) is followed by tagged sections
`{tag, bytes, data}`:

| Section | Contents |
|---------|----------|
| `CMDS` | The command log: every command the match admitted, in the game's own wire encoding, each scheduled at the job it executed live. |
| `PRNG` | The shared random-number cursor at the start of every simulation update (format v5; v2–v4 hold one per job). |
| `TILE` | The order of each tile and region occupancy list at the checkpoint. |
| `EXOR` | Per-job execution-order plans, for jobs where a player's command ran before an autonomous one. |
| `FLYS` | Damage values of projectiles in flight at the checkpoint. |

The header records the format version, a 64-bit hash of the checkpoint save,
a hash of the whole body, the executable's image size and timestamp, the epoch
job and last recorded job, command counts, why the recording ended, when it
was recorded and last written (UTC), the iterator-0 cursor either side of the
checkpoint save (`reserved[0]`, flag `0x40`), the recorder id
(`reserved[1]`, flag `0x80`), and the [match rules](match-rules.md) at the
checkpoint (flag `0x100`: `reserved[2]` = the rule bits on, `reserved[3]` = the
host's peer id; flag `0x200` = a rules line had already executed). A sidecar
without `FLYS`, the iterator-0 flag or the rules flag still loads; the missing
data is simply not restored.

A sidecar is meant to be shared, so it is read as hostile input. Every section
goes through a validator in `src/replay/replayFormat.cpp` before anything is
copied out of it: the whole-body hash and length, a section table that stays
inside the body, and for each section its magic, version, counts and every
length. `CMDS` must account for every byte, its ticks may not go backwards or
precede the epoch, and every class index must be in the current table. The
player then injects only registered `*Chore` classes that are not lobby or
session machinery (`StartGameChore`, `UpdateGameDescriptionChore`,
`GameReadyChore`, `EndJobChore`, the peer and invite classes), checked by name
(`choreCodecIsInjectable`). `FLYS` writes only the three known
(class, offset) pairs; tile relinking checks live chain membership first.

---

## How It Works

Stronghold 2 multiplayer is lockstep: every peer runs the same simulation, and
only the players' commands travel over the network. So a match can be replayed
from a starting save plus the commands, as long as the replaying process
re-derives everything else - AI decisions, economy passes, random rolls -
exactly as the live one did. Most of the replay code exists to make that true.

### Recording

1. **Checkpoint.** About 30 frames into a match, the session calls the game's
   own `SaveGame` (the same call the Esc-menu save makes) to write the
   `replay-...sh2` save into Saves, with the x87/SSE state saved and reset
   around it (`ObserverFpScope`), since the frame tick is a mid-function site. In a networked session the save's
   "game state" announcement is suppressed for the duration of the call, the
   same way the [multiplayer save recovery](mp-save-recovery.md) save is made,
   so the match is not interrupted. The occupancy-list order, projectile damage and
   the match rules in force, which the save format does not keep, are captured in
   the same frame. The save waits for
   a frame with no command in the lockstep queue and no deferred building
   placement (a save taken then would lose them); if that never happens
   within 900 frames, the match is simply not recorded.
2. **Command log.** A hook on the game's command admission records every
   command - human, remote, AI and system - with its job, its order within the
   job, where it came from, and the payload in the game's own wire encoding
   (each command class's own `serial(Stream&)`). Autonomous commands are kept
   too: playback does not inject them, but they show whether the replay
   re-derived them.
3. **Execution witness.** Hooks on both execution paths pair each admitted
   command with the job it actually ran in. In live multiplayer a command does
   not always run one job after it arrives: a packet arriving mid-job can run in
   that same job, and a late burst from the host can run two jobs later.
4. **Random cursor.** One of the game's two random-number iterators is shared
   between simulation and rendering (sound, particles and screen effects draw
   from it once per frame). A spectator renders different frames from a player,
   so the cursor is recorded at the entry of every simulation update, to be
   restored at the same point during playback. Once per job was not enough:
   multiplayer runs five updates per job (six across the update counter's
   rollover), and how many frames render between them depends on the Game
   Speed. A replay watched faster than it was played gave the AI's draws in a
   job's later updates different values, diverged, and held at the recorded
   end with the match unfinished. Recordings in the measured matches moved the
   cursor between jobs' simulation windows in 99% of jobs, by 5-10 draws.
5. **Sidecar writes.** The sidecar is rebuilt and written atomically (temp file
   plus rename) every 10 seconds, with the interval growing as it gets bigger
   (up to 60 seconds), once more when the match ends, and once when the game
   window closes (the first of `WM_CLOSE` / `WM_DESTROY` / `WM_ENDSESSION`,
   seen by the spectator guard's window subclass on the game thread). The
   periodic rewrites run on a worker thread: the game thread only takes a
   snapshot of the buffer counts, and the worker retimes, sorts and writes.
   The recording buffers are append-only and each count is published after
   the data it covers, so the worker never reads a torn entry while the match
   keeps appending. Final writes wait for the worker and run on the game
   thread. Nothing is written from `DllMain`: file I/O under the loader lock,
   after other threads are gone, can hang the exit. A match that ends by
   leaving, a desync drop, lord death or a crash still leaves a usable replay.
   A hard crash, or an exit path that never closes the window, loses at most
   the last interval. A match that ends before its checkpoint is taken leaves
   nothing behind, and its pending checkpoint is cancelled so it can never fire
   in the next match or during a playback.
6. **Buffers.** The command log (16 MB), the execution witness (16 MB), the
   random-cursor record (14.5 MB) and the retimer's working storage (about
   8 MB) are allocated when a match starts recording and freed when it ends.
   Playback allocates the log and cursor stream of the replay it loads and
   frees them when it ends.

At each write, **retiming** (`replayRetime.cpp`) moves every injected command
to the job before the one it executed in live, and builds `EXOR` plans for jobs
where the live order was not the default. Admissions are paired with their
executions in one pass through a hash of the commands still awaiting
execution. A command not executed within 64 jobs of its admission (live
multiplayer never took more than 4) was dropped on every peer -- for example,
deletes that arrived during a network catch-up -- and is left out; it is also
expired from the pairing, so its address can be reused by a later command and
any number of them leave retiming on. If a buffer filled up, the log is cut at
the last job whose timing is fully known, so everything kept is exact.

### Playback

1. **Detection.** A hook on `World::LoadGame` checks every load. If the save has
   a sidecar whose checkpoint hash matches the file being loaded, playback is
   armed before the load applies the multiplayer rules. Any other save loads
   untouched.
2. **Injection.** Recorded human and remote commands are injected from a hook
   on the `call FUN_007676c0` inside NetGameMgr's job function (RVA
   `0x367a03`), which runs after the job's autonomous admissions and before its
   drain. Each command therefore lands after that job's autonomous commands, in
   the same order as live. Injecting earlier in the job (the tick-body entry,
   or after its subsystem updates) put a player's command ahead of the AI's and
   changed the outcome; the tick-counter bump at the end of the job function
   (`0x367da5`) got the order right but is after the drain, so every injected
   command executed a job late. Everything the simulation derives for itself
   is left to the simulation. The playback epoch is the first in-match frame
   after the load, sampled by the player itself; it shares no state with the
   recorder.
3. **Determinism guards** (`prngGuard.cpp`). Hooks inside the simulation tick
   put the recorded random cursor back at the start of each simulation update,
   so render-side draws in between cannot shift the simulation's rolls at any
   Game Speed. A v2–v4 sidecar only restores the first update of each job and
   so stays exact only at the speed it was recorded at. Debug builds show
   "RNG updates restored N, corrected M" in the replay panel; `corrected`
   counts later updates whose live cursor had been moved by rendering. The checkpoint's
   occupancy-list order, projectile damage and iterator-0 cursor are written
   back at the playback epoch. The Monastery prayer-stage clock (`+0x164`),
   which the constructor never initialises and the serializer never saves, is
   zeroed by the constructor hook **only while a replay is armed** and then
   overwritten with the recorded value at the epoch. In live play the stock
   code zeroes it itself on entering stage 2 (VA `0x474339`) before its only
   read, so the hook leaves every live match on the stock constructor.
4. **Match rules** (`matchRulesReplayBegin`). The rules a match was played
   under (ballista auto-fire, no market, the simulation fixes) are patch
   state, not game state: no save carries them, and live they are decided
   outside the simulation, from the lobby and by the frame tick. Playback is
   offline with no Steam lobby, which the live logic reads as single player:
   it switched every option off and every fix on, and rejected the replayed
   rules line because its sender is not the (absent) network host. Field
   ballistae then stood idle in a replay of a match where they had fired.
   Playback therefore takes the rules from the sidecar instead: the state at
   the checkpoint is applied at load, and the live frame-tick logic is
   suspended until the playback ends. A rules line that executed after the
   checkpoint is an ordinary recorded command (human origin on the host, remote
   on everyone else), so it is injected and commits on the job it did live,
   checked against the recorded host peer id. The rebuilt line carries its
   live sender: `ChatChore`'s `serial` (VA `0x5e8a10`) writes `+8` before the
   text. A rule this build cannot run stays off, and that replay diverges.
5. **Spectator guard** (`spectatorGuard.cpp`). A window subclass swallows mouse
   buttons, character messages and keys that could post a command, and lets
   camera keys through. Because the game also polls keys directly, the
   `GetAsyncKeyState` import of the executable and of `dxrenderer.dll` -- the
   two modules that read the game's input -- is redirected when a playback
   starts and put back when it ends. No other module is touched, so the Steam
   overlay, ReShade or a D3D wrapper keep their own hotkeys, and nothing walks
   the loader's module list. The patch's own hotkeys (stop troops, attack move,
   siege camp, shift-recruit) and the auto-market check the guard. As a
   backstop, the admission hook **refuses** any UI-origin command that reaches
   it during playback while the in-game menu is closed: it releases the
   chore's by-value reference exactly as `S2Game::post`'s epilogue does
   (VA `0x77f022`..`0x77f057`) and returns without admitting it. Commands from
   the open in-game menu, whose clicks are let through so the viewer can leave,
   are admitted and counted.
6. **Viewer and vision** (`replayViewer.cpp`, `replayVision.cpp`). The panel is
   drawn in the frame like the other overlays. Pausing skips whole simulation
   updates rather than using the game's pause flag, which would put the job
   counter out of step. The Game Speed slider writes the game's own speed
   setting; the value it had when the playback started is put back when it
   ends. Ally vision answers "allied" only at the draw-path call sites of the
   game's `allied(local, owner)` check, and only while the spectator guard is
   up and the session is not networked.
7. **End.** Playback holds at the last recorded job, or plays into the
   victory/defeat screen if the match ended on one. The session alone ends a
   playback: when the match is left (World+4 falls), after the same 30-frame
   grace it gives a recording to report a victory or defeat screen.

### Kill tracking

`src/patches/endgameStats/killTracker.cpp` counts kills per player by victim
type for the replay panel. It observes the lethal branch of the shared actor
damage sink, ballista launches, pit falls, and ground fires (crediting whoever
lit the tile). The killer is the first healthy actor owned by another player in
the attack's caller frames. Repeat notifications for the same victim are
counted once. The counts feed only the replay panel; the end-of-game screen is
unchanged. Counts are reset on returning to the main menu and when a save is
loaded (from the replay session's `World::LoadGame` hook).

The tracker counts **only while a replay plays** (the flag follows the
spectator guard). Each hook tests the flag behind `pushfl`/`popfl` and goes
straight to the re-emitted stock bytes otherwise, so a live match pays one
compare per hook rather than the observer FPU save, ~97 `VirtualQuery` calls
and the registry scan. Kills are re-simulated during playback, so nothing is
lost.

Attribution is a stack-scan heuristic. It reconciled exactly with observed
survivors in the validation battle, but treat the counts as best-effort.

---

## Architecture (code layout)

| File | Role |
|------|------|
| `src/replay/replaySession.*` | Lifecycle: recording and playback start and end, sidecar writes (and the worker thread), `World::LoadGame` hook, retention. |
| `src/replay/replayFormat.*` | The sidecar layout and the validator of every section. Pure; covered by `make test`. |
| `src/replay/replayTrace.*` | Command admission, execution and network enqueue hooks. |
| `src/replay/replayOriginTables.h` | Command provenance by posting routine (see [Command origin](#command-origin)). |
| `src/replay/replayRecorder.*` | The command log arena (16 MB, while recording) and the checkpoint arm. |
| `src/replay/choreCodec.*` | Reuses the game's own command codec to encode and rebuild commands. |
| `src/replay/replayRetime.*` | Moves commands to their execution job; builds order plans. |
| `src/replay/replayPlayer.*` | Schedules and injects commands during playback; restores checkpoint state. |
| `src/replay/prngGuard.*` | Records and restores the shared random cursor per simulation update. |
| `src/replay/spectatorGuard.*` | Blocks viewer input during playback. |
| `src/replay/replayViewer.*` | The replay panel, pause and game speed. |
| `src/replay/replayVision.*` | Ally vision and free view. |
| `src/replay/replayCommon.*` | Shared helpers: in-game flag, popup check, checkpoint save, tile order, projectile state. |
| `src/patches/endgameStats/killTracker.*` | Per-player kill counts. |
| `src/core/observerContext.h`, `src/core/readableMemory.h` | Register/FPU save-restore for observation hooks; guarded memory reads. |

Install order is set in `src/patches/registry.cpp`: trace, player, random
guard, spectator guard, viewer, vision, session. Each module verifies the stock
bytes at every site before patching, and installs all of its sites or none. The
session installs last and refuses to record at all unless its own `LoadGame`
hook, the trace, the player and the random guard are all in: a replay this
build could not play back is not worth writing.

`make test` builds `tests/replayTests.cpp` with the host compiler under
AddressSanitizer and UBSan and runs it: the retimer (delays 0/1/2, autonomous
commands, never-executed commands, thousands of them, reuse of a dropped
command's address, missing hooks, witness overflow, the event cap, an external
limit, execution-order plans) and every sidecar validator, with table tests for
each refusal plus 20,000 random mutations and truncations of valid images.

## Footprint when idle

The hooks have to be in place for a received replay to play with recording
off, so they are installed at startup. In a match that is neither recorded nor
replayed:

| Site | What runs |
|------|-----------|
| Command admission, both executions | The lockstep clock lift and the within-job order counter, then two compares. No `VirtualQuery`, no allocation. |
| Network enqueue | One compare. |
| Simulation tick entry / sim end / sim done | One call and two compares per simulation update. |
| Monastery constructor | One compare per Monastery built. |
| Job pump, MP cadence, MP rules byte, frame pacer | One compare each. |
| `allied` / `enemy` draw-path calls (12) | One compare per call. |
| Kill tracker (10 sites) | One compare per site behind `pushfl`/`popfl`. |
| Main-loop pause site | One compare per frame. |
| `World::LoadGame` | Runs on every load: an ordinary save without a sidecar costs one failed file open. |
| Frame ticks (player, viewer, vision, session) | A handful of compares per frame. |

Nothing else: no buffers (about 1 MB of static tables in all), no threads, no
import-table writes. The recording buffers, the worker thread, the kill
tracker's 1 MB ballista-flight table and the `GetAsyncKeyState` redirection
exist only while a match is recorded or replayed.

## Command origin

Playback must inject exactly the commands a player made and leave the rest to
the simulation, so every admission is classified when it is recorded: AI or
system by its RTTI class name, and otherwise by its caller and one frame up
against whole function bodies in `src/replay/replayOriginTables.h`. A poster is
never sufficient evidence of a human origin on its own: the autonomous
per-player pass `FUN_00467190` shares four posters with the UI
(`PlaceBuildingChore` 45 UI admissions against 128 from the pass in one match,
`PlaceWallsChore` 3 against 24, `GameGroundFeatureChore` 4 against 213, and
`PlaceTowerEquipmentChore`). How the tables were built:

- **AI.** `FUN_0059e1a0` posts an `AIMoveTroopsChore` and then a
  `SetTroopStanceChore` (stance 2) from one body, so the whole body is
  autonomous.
- **UI.** The mouse dispatcher `FUN_005f6710` and the `GetAsyncKeyState` hotkey
  dispatcher `FUN_006af0d0` reach posters no pane process encloses; every call
  path into each listed body was walked in Ghidra against the recorded
  caller/parent pairs of human sessions. Pane processes are credited for their
  first `0x600` bytes; the Carter post pane is larger (a switch with its jump
  table at `0x20eb38`, `StopCarterTradeChore` posted `0x694` bytes in) and is
  listed as a whole body rather than widening the window for every pane. The
  shared recruit poster `FUN_004ef700` is listed because shift-recruit
  retargets its five pane call sites to a DLL stub, which leaves no pane in its
  parent frame. The chat send `FUN_0064d8e0` is reached only from the chat pane
  and the keyboard dispatcher; the lobby's own `ChatChore` post (`0x6a2e2d`) is
  before the epoch and not listed.
- **Deliberately absent.** The `PlaceWallsChore` (`0x1c9cd0`) and
  `DeleteWallsChore` (`0x1c9ee0`) posters are also reached from the autonomous
  `FUN_004564c0` under `FUN_00467190`; listing them would label an autonomous
  wall command human. Their UI parent frames are listed instead.
- **System.** The economy pass `FUN_0045b9b0`, the policy pass `FUN_0045d9b0`,
  the per-player pass and what they reach. The construction tree under
  `FUN_00457de0` reaches the post through the shared trampoline
  `FUN_0077e9e0`, so the trampoline itself is listed; all eight of its callers
  were walked and none is input code. `FUN_004578c0` and `FUN_00457780` were
  added after recordings stopped on their `PlaceBuildingChore` admissions.

An admission that matches nothing stays at origin 0, and playback stops there
rather than guess: injecting a command the simulation also makes runs it twice,
and dropping one it does not make loses it. At load, an origin-0 event is put
through the current tables again, so a sidecar from a build that did not know a
route yet still plays.

## Patch Offsets

All RVAs (VA - `0x400000`). The main sites; each file documents its own in full.

| Site | RVA | Use |
|------|-----|-----|
| `World::LoadGame` | `0x2ac60` | Detect replay loads |
| `SaveGame` | `0x2a2e0` | Called for the checkpoint (`0x2a37d` state announcement suppressed in MP) |
| World command admission | `0x37efb0` | Command log |
| Queued execution (NetGameMgr tick list) | `0x367c61` | Execution witness |
| Direct execution (World) | `0x37f018` | Execution witness |
| Network enqueue | `0x366c00` | Remote-command origin |
| `call FUN_007676c0` in the job function | `0x367a03` | Playback injection point |
| MP job cadence / load's MP rules byte / frame pacer | `0x3679cc` / `0x2aec5` / `0x20e4e` | Offline MP timing for playback |
| Simulation tick body: entry / sim end / sim done | `0x36aca0` / `0x36acf5` / `0x36acff` | Random cursor record / restore |
| Main-loop update test | `0x21ea2` | Replay pause |
| Monastery constructor | `0x739ba` | Initialise an unsaved field (playback only) |
| `allied(local, owner)` callers | `0x219e0` | Ally vision |
| Actor damage sink, lethal branch | `0x386a7f` | Kill tracking |
| `Fire::addGroundFire` | `0x1bfac0` | Fire kill attribution |

## Known Issues

- **Multiplayer recording is off by default.** The checkpoint is a `SaveGame`
  call in the middle of a live match, with its session-ending state
  announcement suppressed. That has been verified in one live two-player
  match. Before it can default to on:
  - It needs validating in 3-8 player matches, including one with an
    unpatched peer.
  - It must be confirmed that `SaveGame` draws no shared simulation RNG on the
    recording client. A recording's header already answers this: if the
    iterator-0 cursor in `reserved[0]` differs between its low half (before
    the save) and high half (after), the save drew from it, and that would be
    a desync against every peer.
- The checkpoint save is blocking disk I/O on the game thread. In lockstep
  every peer waits for the slowest client, so a slow disk is a short hitch for
  the whole match. It has not been measured yet. (The periodic sidecar writes
  run on a worker thread and do not stall the match; the final one, at match
  end, does run on the game thread.)
- The `GetAsyncKeyState` redirection, the `WM_CHAR` block and the separate
  playback epoch need a playtest: Esc, Space and Tab must still work during a
  playback, and the Steam overlay's `Shift+Tab` must work during one.
- A replay only plays on the same game executable it was recorded with. Any
  other build is refused, and the save loads as a normal save.
- A sidecar written before the match rules were recorded (no flag `0x100`)
  starts multiplayer playback vanilla and takes the first replayed rules line
  from any sender, since the host's peer id is unknown. That recovers a match
  whose rules line came after the checkpoint; one whose line came before it
  plays vanilla and diverges. Single-player sidecars of that age get every fix,
  as live.
- Replaying match rules needs a playtest: a match with ballista auto-fire on,
  recorded on both the host and a joining client, must show the ballistae
  firing in both replays.
- A replay cannot be loaded from inside a live networked session.
- Recording buffers hold about two hours of an 8-player match. A longer match
  is recorded up to that point, and the replay ends there.
- A hard crash loses up to the last sidecar write interval (10-60 seconds).
- Single-player recording is off by default.
- A replay is watched from a single viewpoint. Ally vision shows every player's
  hidden units and interiors, but there is no per-player fog of war.
- Untested in multiplayer games with computer players (the patch's MP AI
  option) or with custom rules from mods. Command origin is classified by
  return-address ranges. A misclassified AI command would be injected and
  re-derived, running it twice; an unclassified one stops playback instead.

## Multiplayer Compatibility

**Safe for version mismatch, pending the checkpoint validation in Known
Issues.** If the mid-match `SaveGame` turns out to draw shared simulation RNG
on the recording client, recording would desync every peer; that is the open
question behind the off-by-default setting, and 3-8 player matches have to
confirm it before this classification is unconditional.

While recording, the patch only observes: it reads command and timing data and
copies it into its own memory, and never changes simulation state. The
Monastery constructor hook writes only while a replay is being loaded, never in
a live match, and the kill tracker does nothing outside playback. The one game
call recording makes during a match is the local checkpoint save, made the same
way as the multiplayer save recovery save. That call is the open question
behind the off-by-default setting (Known Issues). Peers without the patch see
nothing different; each patched player records their own copy of the match.

Playback runs offline in a single process: it refuses to start inside a
networked session, and the spectator guard keeps the viewer from sending any
command.
