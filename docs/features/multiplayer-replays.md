# Multiplayer Replays

**Status:** Unreleased  
**Affected version:** Stronghold 2 Steam v1.5.0  
**Patch type:** Observation hooks during play; playback hooks active only while a replay is loaded

---

## Symptom / Motivation

Stronghold 2 has no replay system. Once a multiplayer match ends, the only
record of it is whatever the players remember. A save only captures a single
moment; nothing of what the players did afterwards is kept.

## What This Patch Does

Every multiplayer match is recorded automatically. After the match, the
recording shows up as an ordinary save in the game's Load menu; loading it
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

Nothing to do: with the default settings every multiplayer match is recorded.

1. Play a multiplayer match.
2. From the main menu, open **Load** and pick the save named `replay-...`.
3. Watch. Press `]` for the panel.

All settings live in the `[replay]` and `[hotkeys]` sections of
`sh2-unofficial-patch.ini`, and the main ones are also in the in-game settings
panel (`Ctrl+Shift+O`):

| Key | Default | Meaning |
|-----|---------|---------|
| `[replay] RecordMultiplayer` | `1` | Record every multiplayer match. `0` = never. Takes effect from the next match. |
| `[replay] RecordSinglePlayer` | `0` | Also record single-player sessions. |
| `[replay] MaxReplays` | `10` | Replays kept in the Saves folder (1-99). When a new match would go past this, the oldest replays are deleted first. |
| `[replay] AllyVision` | `1` | Ally vision while a replay plays. |
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
`.sh2` save beside it. A save without a sidecar is never touched, even if it is
named `replay-...`.

The `.s2replay` extension is deliberately not `.sh2...`: with 8.3 short names a
`*.sh2` search would also match it, and the game would list it as a save.

Debug builds (`make debug`) also write `killtrace.txt` to the game directory.
Release builds write no log files.

### Sidecar format

Little-endian, packed. A 144-byte header (`ReplayBundleHeader` in
`src/replay/replaySession.cpp`) is followed by tagged sections
`{tag, bytes, data}`:

| Section | Contents |
|---------|----------|
| `CMDS` | The command log: every command the match admitted, in the game's own wire encoding, each scheduled at the job it executed live. |
| `PRNG` | The shared random-number cursor, once per simulation job. |
| `TILE` | The order of each tile and region occupancy list at the checkpoint. |
| `EXOR` | Per-job execution-order plans, for jobs where a player's command ran before an autonomous one. |
| `FLYS` | Damage values of projectiles in flight at the checkpoint. |

The header records the format version, a 64-bit hash of the checkpoint save,
a hash of the whole body, the executable's image size and timestamp, the epoch
job and last recorded job, command counts, why the recording ended, and when it
was recorded and last written (UTC). Older sidecars without `FLYS` or the
iterator-0 flag still load; the missing data is simply not restored.

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
   `replay-...sh2` save into Saves. In a networked session the save's
   "game state" announcement is suppressed for the duration of the call, the
   same way the [multiplayer save recovery](mp-save-recovery.md) save is made,
   so the match is not interrupted. The occupancy-list order and projectile damage, which the save
   format does not keep, are captured in the same frame.
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
   so the cursor is recorded once per simulation job, to be restored at the
   same point during playback.
5. **Sidecar writes.** The sidecar is rebuilt and written atomically (temp file
   plus rename) every 10 seconds, with the interval growing as it gets bigger
   (up to 60 seconds), and once more when the match ends. A match that ends by
   leaving, a desync drop, lord death or a crash still leaves a usable replay. A
   hard crash loses at most the last interval. Recording re-arms for the next
   match in the same game session.

At the end, **retiming** (`replayRetime.cpp`) moves every injected command to
the job before the one it executed in live, and builds `EXOR` plans for jobs
where the live order was not the default. A command admitted live but never
executed on any peer (for example, deletes that arrived during a network
catch-up and were dropped) is left out. If a buffer filled up, the log is cut at
the last job whose timing is fully known, so everything kept is exact.

### Playback

1. **Detection.** A hook on `World::LoadGame` checks every load. If the save has
   a sidecar whose checkpoint hash matches the file being loaded, playback is
   armed before the load applies the multiplayer rules. Any other save loads
   untouched.
2. **Injection.** Recorded human and remote commands are injected at the end of
   the job's admission window (the tick-counter bump), so each lands after that
   job's autonomous commands, in the same order as live. Injecting earlier in
   the job puts a player's command ahead of the AI's and changes the outcome.
   Everything the simulation derives for itself is left to the simulation.
3. **Determinism guards** (`prngGuard.cpp`). Hooks inside the simulation tick
   put the recorded random cursor back at the start of each job, so render-side
   draws in between cannot shift the simulation's rolls. The checkpoint's
   occupancy-list order, projectile damage and iterator-0 cursor are written
   back at the playback epoch. A Monastery field the save leaves uninitialised
   is set to zero.
4. **Spectator guard** (`spectatorGuard.cpp`). A window subclass swallows mouse
   buttons and keys that could post a command, and lets camera input through.
   Because the game also polls keys directly, every `GetAsyncKeyState` import
   is redirected too, and the patch's own hotkeys (stop troops, attack move,
   siege camp, shift-recruit) and the auto-market check the guard. Any player
   command admitted during playback is counted as a failure.
5. **Viewer and vision** (`replayViewer.cpp`, `replayVision.cpp`). The panel is
   drawn in the frame like the other overlays. Pausing skips whole simulation
   updates rather than using the game's pause flag, which would put the job
   counter out of step. Ally vision answers "allied" only at the draw-path call
   sites of the game's `allied(local, owner)` check.
6. **End.** Playback holds at the last recorded job, or plays into the
   victory/defeat screen if the match ended on one.

### Kill tracking

`src/patches/endgameStats/killTracker.cpp` counts kills per player by victim
type for the replay panel. It observes the lethal branch of the shared actor
damage sink, ballista launches, pit falls, and ground fires (crediting whoever
lit the tile). The killer is the first healthy actor owned by another player in
the attack's caller frames. Repeat notifications for the same victim are
counted once. The counts feed only the replay panel; the end-of-game screen is
unchanged. Counts are reset on returning to the main menu and when a save is
loaded.

---

## Architecture (code layout)

| File | Role |
|------|------|
| `src/replay/replaySession.*` | Lifecycle: checkpoint, sidecar read/write, `World::LoadGame` hook, retention, end handling. |
| `src/replay/replayTrace.*` | Command admission, execution and network enqueue hooks. |
| `src/replay/replayRecorder.*` | The command log arena (16 MB). |
| `src/replay/choreCodec.*` | Reuses the game's own command codec to encode and rebuild commands. |
| `src/replay/replayRetime.*` | Moves commands to their execution job; builds order plans. |
| `src/replay/replayPlayer.*` | Schedules and injects commands during playback; restores checkpoint state. |
| `src/replay/prngGuard.*` | Records and restores the shared random cursor per job. |
| `src/replay/spectatorGuard.*` | Blocks viewer input during playback. |
| `src/replay/replayViewer.*` | The replay panel, pause and game speed. |
| `src/replay/replayVision.*` | Ally vision and free view. |
| `src/replay/replayCommon.*` | Shared helpers: in-game flag, popup check, checkpoint save, tile order, projectile state. |
| `src/patches/endgameStats/killTracker.*` | Per-player kill counts. |
| `src/core/observerContext.h`, `src/core/readableMemory.h` | Register/FPU save-restore for observation hooks; guarded memory reads. |

Install order is set in `src/patches/registry.cpp`: trace, recorder, player,
random guard, spectator guard, viewer, vision, session. Each module verifies
the stock bytes at every site before patching, and installs all of its sites
or none.

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
| Tick-counter bump | `0x367a03` | Playback injection point |
| Simulation tick body: entry / sim end / sim done | `0x36aca0` / `0x36acf5` / `0x36acff` | Random cursor record / restore |
| Monastery constructor | `0x739ba` | Initialise an unsaved field |
| `allied(local, owner)` callers | `0x219e0` | Ally vision |
| Actor damage sink, lethal branch | `0x386a7f` | Kill tracking |
| `Fire::addGroundFire` | `0x1bfac0` | Fire kill attribution |

## Known Issues

- A replay only plays on the same game executable it was recorded with. Any
  other build is refused, and the save loads as a normal save.
- A replay cannot be loaded from inside a live networked session.
- Recording buffers hold about two hours of an 8-player match. A longer match
  is recorded up to that point, and the replay ends there.
- A hard crash loses up to the last sidecar write interval (10-60 seconds).
- Single-player recording is off by default.
- A replay is watched from a single viewpoint. Ally vision shows every player's
  hidden units and interiors, but there is no per-player fog of war.
- Untested in multiplayer games with computer players or with custom rules from mods

## Multiplayer Compatibility

**Safe for version mismatch.**

While recording, the patch only observes: it reads command and timing data and
copies it into its own memory, and never changes simulation state. The one game
call it makes during a match is the local checkpoint save, made the same way as
the multiplayer save recovery save. Peers without the patch see nothing
different; each patched player records their own copy of the match.

Playback runs offline in a single process: it refuses to start inside a
networked session, and the spectator guard keeps the viewer from sending any
command.
