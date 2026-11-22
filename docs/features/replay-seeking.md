# Replay Seeking (fast-forward and rewind)

**Status:** Phase 0 (measurements) complete, 2026-11-05. Phase 1 (skip ahead)
implemented and playtested, unreleased. Phase 2 (rewind by restart)
implemented and playtested, 2026-11-14. Phase 3 (keyframes) implemented and
playtested, 2026-11-22: automatic capture is on, and every run resumed from a
keyframe is checked against the recording and redone from an older point when it
differs.
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** Part of [multiplayer replays](multiplayer-replays.md): works only
while a replay plays. The phase 0 probe is debug-only (`make debug`).

---

## Goal

Let a replay viewer skip ahead (past peace time) and jump back (watch the last
30 seconds again). [Multiplayer replays](multiplayer-replays.md) can only play
forward from their checkpoint save today.

The plan is in four phases:

| Phase | What | Exact by construction? |
|-------|------|------------------------|
| 0 | Measurements (this document) | — |
| 1 | Forward seek: run hundreds of simulation updates per frame | Yes, given per-update RNG restore (PRNG v5) |
| 2 | Rewind by restart: reload the replay save, then forward seek | Yes: the validated playback path |
| 3 | Keyframes: mid-playback saves, restored with the state the save drops | Only once verified per keyframe |

## What is already known (static analysis)

All VAs at ImageBase `0x400000`.

### The update loop

`FUN_00421e00` (`this` = World `0x11781d0`) runs once per frame:

1. **Pacer.** `FUN_00420e30` converts the Game Speed (`World+0xce348`) into
   milliseconds per update, adds the frame's elapsed time to a debt at
   `World+0xce34c`, and returns `debt / msPerUpdate + 1`. That count has **no
   cap**. When the loop is ahead, the pacer calls `Sleep` and returns 1. The
   result goes to `World+0xce30c`.
2. **Warm-up clamp.** For the world's first 20 frames (`World+0xce350`), the
   count is clamped to 1.
3. **Update loop** at `0x421ea2`:
   `for (i = 0; i < [World+0xce30c]; ++i) if (NetGameMgr::tick()) world->vfunc[0x1c]();`.
   The bound is **re-read from memory every iteration** (`0x421ec8`).
4. Rendering, the frame tick and `processEvents` run once per frame, outside
   the loop.

Consequences:

- Fast-forward needs no render suppression. Overriding the count at the loop
  site, which the viewer already owns for pause, puts hundreds of updates into
  one frame.
- Writing `0` to `World+0xce30c` from a per-job hook ends the frame's loop
  exactly at a job.
- After an override, the pacer's debt must be zeroed, or the next frames
  burst.

### The particle gate

The particle flag (dword `0x10800b0`) has three readers, all in the particle
system:

- emitter spawn, `0x7bc330`: early out;
- update, `0x76acff`, inside the simulation tick after `SIM_END`;
- `0x75d36f`.

Setting it to 0 stops particle work only.

### Loading a save

`World::LoadGame(wstring, int)` (`0x42ac60`, `ret 0x20`) is called the same way
by the pause menu (`0x65ec3c`) and the main menu (`0x67d4a5`):

1. `LoadGame(path, 0)`
2. `byte [0x1246306] = 0`
3. `Screen::hidePopup(pane)`
4. `Screen::setWindow("in game interface", false, true)`

`LoadGame` does not switch screens itself. The string overload of `setWindow`
is dragonfly RVA `0x296b0`, `ret 0x24`: a 0x1c-byte VC10 `std::string`, with
the allocator *last*, plus two bools. The main menu additionally calls
`0x725590` on `0xac73dc` and World vfunc `+0x2c`.
`replayLoadSave` (`replayCommon.cpp`) reproduces the pause-menu sequence
without `hidePopup`.

### No in-memory save

`SaveGame` writes a file. `LoadGame` is a full world teardown and rebuild. A raw
memory snapshot is ruled out: heap pointers, D3D, Granny and Miles objects, and
a process with 2 GB of address space (not large-address-aware).

Measured from existing saves:

| Save | On disk | Decompressed |
|------|---------|--------------|
| Checkpoint (match start) | 0.38–0.57 MB | 10.3–10.7 MB |
| Developed SP save | 1.10 MB | 12.7 MB |

### Dead end: `GlowWorm::SyncCheck`

`GlowWorm::SyncCheck` (vtable `0x9f567c`, methods `0x7cd000..0x7cd800`) is an
event log of 8-byte records, gated by bytes `+0x64`/`+0x65`. It is not a world
hash usable as a fingerprint. The probe hashes player state instead.

## The phase 0 probe

`src/replay/seekProbe.cpp`, compiled only by `make debug`. It works only during
a playback. It writes `seek_probe.txt` next to `Stronghold2.exe`, appending one
session header per game launch.

| Key | Action |
|-----|--------|
| `F7` | Toggle the iterator-0 write watch: every draw of the simulation cursor, with its code site and whether it ran inside the update loop |
| `F8` | Turbo on / off. `Shift+F8` starts it without particles |
| `F9` | Save a keyframe probe into `%TEMP%\sh2up-seekprobe\kf-<job>.sh2`, timed |
| `Shift+F9` | Load the last keyframe probe (ends the playback; for timing only) |
| `F10` | Toggle: later skips (phase 1) run without the kill tracker, to measure its cost. Kills during such a skip are not counted |
| `F11` | Restart the replay at the next job with no queued command and no deferred placement: `LoadGame` of its own save, as the pause menu does it, then the playback epoch at once (`replayPlayerStartNow`). `Shift+F11` restarts at once, without waiting |

`F12` is avoided because Windows reserves it for debuggers.

### What each log line records

| Line | Recorded |
|------|----------|
| `J <run> <job> <iter0> <hash> <worldTick> <frame> <dtUs>` | Every job of every playback, at the pump, before that job's injections. `iter0` is the simulation-only random cursor. `hash` is FNV-1a over each player's colour, honour, troops, siege, castle, gold, popularity, incomes and honour sources. Two runs of one replay must agree on every common job. `frame` is the probe's frame counter and `dtUs` the time since the previous job's pump, so within a frame `dtUs` is the simulation's cost per job and the first job of a frame also carries the frame's drawing. |
| `SKIP …` | A player skip (phase 1): its start and target, its speed every 2 s, and its total jobs and wall time at the end. `fx` is the particle gate, `kills=0` when `F10` ran it without the kill tracker. |
| `RUN <n>` | Each new playback epoch, with the world tick (`0xae8bd4`), the job-phase byte (`NetGameMgr+4`) and the address space in use. After `F11`, also the time from the load call to the epoch. |
| `TURBO …` | Every 2 s: updates per frame, jobs/s, simulation updates/s, frame time and address space. Turbo aims for 100 ms frames. It switches off at the end of the recording, on a load, or when playback stops. |
| `SAVE …` | Time, file size, queued commands and pending placements (a keyframe needs both 0), both random cursors before and after, address space. Changed cursors are put back. |
| `RESTART` / `LOADKF` | Time of the synchronous load call, address space before, after, and 300 frames later. |
| `D <run> <job> <worldTick> <site> <flags>` | With `F7` on: one per iterator-0 draw. `site` is the RVA after the writing instruction; flags bit 0 = inside the update loop (its return address into `FUN_00421e00` is on the stack), bit 1 = outside the exe. A hardware write breakpoint (DR0) on the game thread, set from a vectored handler; no hooks. |
| `OUTSITE <site> chain …` | First sighting of a draw site outside the update loop, with up to eight return addresses found on the stack. |

### Measurement protocol

1. `make debug && make deploy`. Load a recorded replay, preferably a long one
   with a siege.
2. **Baseline.** Let it play at 1x for a few minutes. This logs run 1's
   fingerprints.
3. **Restart and turbo.**
   1. `F11`. Check that the replay restarts. This logs run 2 and the load time.
   2. `F8`, and let turbo run past where run 1 stopped. Note the `TURBO` lines.
   3. `F8` to stop; `Shift+F8` for the no-particles mode, `F8` to stop.
4. **Keyframe save.** `F9` a few times at different points, at least one during
   a battle. Then let the replay continue for a minute after each one. Run 2's
   fingerprints should still match run 1 past every save point.
5. **Leak.** `F11` ten to twenty times in a row. Watch the `va` figures.
6. **Mid-match load.** `Shift+F9`. This only records the load time; the replay
   ends.
7. Send `seek_probe.txt`.

### Results (2026-11-02 to 11-05, `replay-11.1.26-1115pm-e3`, 64 min)

| Question | Result |
|----------|--------|
| Headless speed, particles on | 620–655 jobs/s in the opening, 150–320 jobs/s through the mid game (20–43x live, ~7.4 jobs/s), falling to **23–75 jobs/s (3–10x) in the late-game fighting** around jobs 12,000–15,000 |
| Headless speed, particles off | 250–330 jobs/s around jobs 1300–2450; not measured at the same phase as particles on, so the gain is unknown |
| Turbo exact? | **Yes.** A restarted run in turbo for 18,083 jobs matched the menu-loaded run at every job, and both stayed in step with the recording throughout |
| Restart from the frame tick | `LoadGame` returns in 175–240 ms. **The first probe build restarted wrongly**: the epoch was sampled a frame late, the replay drifted from job 53, and the economies collapsed later. Fixed with `replayPlayerStartNow`. **A restart must also wait for a quiet job** (below). Done both ways, every restart was exact |
| Restart needs a quiet job? | **Yes.** `LoadGame` does not clear the lockstep queue: three immediate restarts carried 3, 3 and 1 queued commands across the load, and two of them diverged (from jobs 143 and 1800). Four restarts at a quiet job were all exact, after waiting 0–5 frames |
| A menu-loaded replay exact? | **Yes**: 5,731 jobs checked against the recording's own per-job iterator-0 cursor, with no job out of place |
| `SaveGame` mid-match | 200–212 ms, 0.94–1.04 MB. Draws neither cursor. Two of three saves fell on a job with queued commands (7 and 1), so a keyframe needs the quiet-boundary rule |
| `LoadGame` of a mid-match save | 187 ms |
| Address space per load | No growth over 20 restarts (1455–1456 MB) |

The divergence. A run restarted with `F11` agreed with a menu-loaded run until
job 53, where a batch of 24 iterator-0 draws landed one job earlier, then 59 and
64 (12 and 8 draws). The player hash differed from job 142. The jobs and values
were the same in two separate sessions, so the shift is systematic, not frame
noise. The recording settles which run is right: the sidecar's `PRNG` section
holds the live iterator-0 cursor at the first update of every job, and it lies
between the probe's pump samples of jobs J and J+1 for **every** job of the
menu-loaded run and for almost none of the restarted run after job 53.

Cause: a load from a menu runs in `processEvents`, so the next frame tick samples
the playback epoch before any update. `F11` loaded from inside the frame tick,
after the player's own tick had run, so that frame's update loop advanced the
world before the epoch was sampled on the next frame. Every command then landed
on a world one or more updates ahead of its schedule. How many updates ran
depends on the pacer, which is why 2 of 20 quick restarts shifted at jobs 5–6
and the rest did not. A seek implementation (phase 2) must load the same way:
sample the epoch straight after the load (`replayPlayerStartNow`).

The `F7` watch confirmed nothing else is involved: in 976,000 iterator-0 draws
over 5,700 jobs, every draw was inside the update loop. The only writes outside
it were the deserialisation of the cursor during `LoadGame`.

Commands carried across a load. One restart of the 2026-11-02 22:28 session
made one iterator-0 draw too many between the pumps of jobs 0 and 1, at the same
world ticks as the exact runs, so every later draw was one entry off. Playback
injects each command a job before it executes, so between jobs the lockstep
queue (`NetGameMgr+0x10`) usually holds the next job's commands, and the 11-05
session logged the queue either side of every load: **`LoadGame` leaves it as it
was** (`queued 3->3`, `3->3`, `1->1`). The new world then executes the old run's
commands as well as its own. Two of the three restarts that carried commands
diverged (jobs 143 and 1800); the third ran 1,921 jobs without a difference,
presumably because those commands found nothing to act on. Every restart taken
at a job with nothing queued and no deferred placement was exact. Waiting for
one costs 0–5 frames while playing, but never comes while paused, so a seek
must run the simulation until it reaches one.

Ruled out on the way: the building placer's retry queue (`0x4714f0`, reached
only from `S2Game::update`, vtable slot `+0x1c`, so once per update), and the
wall-clock gates in unit code (`now - last > 2000 ms`), which only rate-limit
positional sounds (`0x900890`).

`updates/s` in a `TURBO` line counts the updates turbo *asked for*, so it is
meaningless while the viewer is paused (the 36 s at job 2685 with `jobs=0`);
`jobs/s` is the real figure.

## What phase 0 decided

- **Forward seek (phase 1) is turbo**: the update-count override plus a per-job
  stop, with no other change. It is exact, and runs at 20–43x in the opening and
  mid game but only 3–10x in large late-game battles.
- **Rewind by restart (phase 2) works** with two rules, both learned here: load
  only at a quiet job (no queued command, no deferred placement), and sample the
  epoch straight after the load (`replayPlayerStartNow`). Each restart costs
  ~200 ms of load and leaks no address space, then a forward seek to the target.
  Its cost is the turbo time from the start of the match, which is fine early
  and slow late (a target 50 minutes in is minutes of turbo).
- **Keyframes (phase 3) are what make late rewinds fast**, and the costs are
  small: a mid-match save is 200–212 ms and about 1 MB, draws neither random
  cursor, and loads in 187 ms. What is not known yet is whether a mid-match
  save restores exactly. That needs the phase 3 machinery and is checked the
  same way as here: every job's iterator-0 cursor against the recording.
- **Keyframe interval**: with late-game turbo at 25–75 jobs/s, a 60 s interval
  (about 450 jobs) costs 6–18 s to cross in the worst case, versus about 2 s mid
  game. Keyframes every 30 s late in a match (when turbo is slow) and 60 s
  earlier keep a rewind within about 10 s; at 1 MB and 200 ms each, that is
  under 200 MB of temporary files for a two-hour match.

## Phase 1: skipping ahead

### What the player gets

The replay panel (`]`) shows the match time, `12:34 / 1:04:10`, above a
timeline. Clicking a point ahead on the timeline, the **+30 s**, **+1 min** and
**+5 min** buttons, or `.` (`[hotkeys] ReplaySkip`, a minute per press) start a
skip. While it runs, the title reads SKIPPING, the time reads `12:34 → 20:00`
and the target is marked on the timeline. Further presses add to the target. `P`
or the **Stop** button ends a skip where it is (`P` also pauses there). The
in-game menu holds a skip until it closes. A skip that reaches the end of the
recording stops there. Clicks behind the current point do nothing until rewind
exists.

Match time is estimated per replay: the recording's own wall-clock span
(`recordedUtc` to `updatedUtc` in the sidecar header) over its jobs, about 135
ms per job in a live multiplayer match. An implausible span (outside 50–400 ms
per job) falls back to 135 ms.

### How it works (`src/replay/replaySeeker.cpp`)

- **Speed.** Each frame the seeker sets the viewer's update-count override
  (`World+0xce30c`, the loop site `0x21ea2` the viewer already owns). The count
  starts at 20 and is scaled every frame toward a target frame length, at most
  doubling or halving per frame, capped at 5,000. The game draws one frame per
  batch, so the camera, the panel and the window stay responsive.
- **Frame length.** Each frame draws the world once, and what that costs
  depends on the view (below). The pump stamps the clock at every job of a
  frame (`QueryPerformanceCounter`, FPU-free), so the seeker knows the frame's
  simulation span and, by subtraction, its drawing overhead. The target frame
  length is eight times the smoothed overhead, between 100 ms and 400 ms: a cheap
  view keeps a smooth camera, a costly one is drawn less often.
- **Stopping at the target.** The player's pump (`0x367a03`, once per job) calls
  `replaySeekerOnJob` after the job's injections. At the target it writes `0` to
  `World+0xce30c`; the update loop re-reads its bound every iteration, so the
  frame ends after the current update, which runs as it always does. Float-free
  and call-free, as the pump is mid-function.
- **Afterwards.** The override goes back to 0 and the pacer's time debt
  (`World+0xce34c`) is zeroed, so the next frames do not burst.
- **Pause.** A skip runs through the player's own pause (`P` before the skip),
  which holds again once it ends; the menu and the end hold still win over it.
- **Sound.** `SoundUtil::setMuteSound(true)` (IAT VA `0x900618`, dragonfly
  `0x1001ace0`) mutes stream types 1 and 2, the ones the stock options toggle
  uses, presumably sound effects and speech. Music, type 0, is not touched. The
  end of the skip calls `setMuteSound(!soundOn)`, with `soundOn` the player's own
  setting (options object VA `0x1177d28` `+0x48`, the byte the options toggle at
  `0x67164d` flips). It is never written, so nothing persists, and a change made
  in the options during the skip is honoured. `setMuteAll` (`0x1001ac90`) would
  mute music too, and is imported but never called by the game.

- **Particles.** The particle gate (dword VA `0x10800b0`) is cleared for the
  length of the skip and restored after, if it is still clear. It has three
  readers: the emitter manager's per-frame pass (`0x75d350`, test at
  `0x75d36f`), which is the only caller of the spawn `0x7bc330` (test at
  `0x7bc340`), and the particle update in the simulation tick after `SIM_END`
  (`0x76acff`). Emitters are simulation objects that exist whatever the gate
  says; it only stops them emitting, so chimneys and fires emit again as soon as
  the skip ends. No code writes the gate by its address (it is a member of the
  object at VA `0x1080000`), so whether it is also an options setting is not
  known; the measurement below is what clears it.

### Speed (2026-11-07, the same 50 minutes of `replay-11.1.26-1115pm-e3` skipped twice)

| | Particles on | Particles off |
|---|---|---|
| Jobs 126–22,347 (about 50 min of match) | 265 s, 83 jobs/s | 236 s, 94 jobs/s |
| Exact against the recording | yes, every job | yes, every job |

Where the time goes, from the per-job timing (`J` lines):

| Jobs | Simulation per job | Jobs per frame | Per-frame overhead, particles on | off |
|------|-------------------|----------------|--------------------------------|-----|
| 0–2,000 (opening) | 2.0 ms | 43 | 12 ms | 10 ms |
| 2,000–8,000 | 4.7–5.0 ms | 16–19 | 4–14 ms | 14–20 ms |
| 8,000–14,000 | 5.7–9.4 ms | 10–16 | 6–15 ms | 5–34 ms |
| 14,000–20,000 (big battles) | 15–18 ms | 4–6 | 8–35 ms | 6–7 ms |

The simulation's own cost per job is the ceiling, and it is the game's code: at
17 ms a job in the late battles, even a skip with no overhead at all could not
pass about 58 jobs/s, roughly 8x real time. Particles are what the frame spends
most on when battles are large (up to 30 ms a frame); with them off the per-frame
overhead in those battles was 6–7 ms, under 10% of the frame.

A second session (same replay, particles off in both runs) settled two more
things:

- **The kill tracker costs nothing measurable.** With it switched off (`F10`) the
  simulation per job was the same as with it on (13.6–16.0 ms against
  13.7–15.9 ms in the battles), within run-to-run noise.
- **Drawing cost depends on the view.** The simulation per job matched the first
  session, but the per-frame overhead in the late game was 25–55 ms of every
  100 ms frame, against 6–7 ms before: the camera was over a busy town (the
  tester saw bakery smoke and windmills). Turning such things off one by one
  would take finding each system and proving it touches nothing; drawing less
  often during a skip covers all of them at once, which is what the adaptive
  frame length does. Bakery smoke kept showing with the particle gate off, so it
  is drawn by another system; with longer frames its cost shrinks with the rest.

A third session, with the adaptive frame length, measured where the remaining
time goes. Debug builds time each update at the random guard's three hooks
(entry, `SIM_END`, `SIM_DONE`; `prngGuardDebugTimes`):

| Stretch | Tick-body subsystems (buildings, actors, projectiles, …) | Emitter manager (`0x76a610`) | Everything else (rest of the world update, drawing, window) |
|---|---|---|---|
| Opening | 57% | 0% | 43% |
| Mid game | 70–78% | 0% | 22–30% |
| Late battles | 80–85% | 0% | 15–20% |

The emitter manager, the one presentation step inside the tick body, costs
nothing measurable, so skipping it would gain nothing. Four-fifths of a
late-game skip is the game's own simulation. The whole 50 minutes skipped in
220 s (100 jobs/s on average, 13x real time), 40–60 jobs/s (5–8x) through the
largest battles, and stayed exact. That is the practical limit of skipping by
running the simulation faster; making late targets quick is what keyframes
(phase 3) are for.

### Exactness

Nothing in the simulation changes: only how many updates run between two
frames. Phase 0 showed that is invisible to the simulation (a turbo run matched
the unskipped one at all 18,083 common jobs, and both matched the recording).
The skip uses the same mechanism with an exact stop.

### Verification (to do)

With a debug build, the probe's fingerprints check any skip against the
recording, as in phase 0:

1. Load a long replay from the Load menu. Skip with each control: timeline click,
   each button, `.` several times in a row, `P` and Stop mid-skip.
2. Skip to the end of the recording; confirm it stops there and the panel says
   ENDED, and that a replay ending on a victory screen still reaches it.
3. Open the ESC menu during a skip; confirm it holds and resumes.
4. Confirm sound effects are silent during a skip and come back after, music
   plays throughout, and a player who had sound off still has it off. Confirm
   smoke and fire show again right after a skip.
5. Skip the late game twice, once with the camera over a busy town and once
   over empty land, and compare the `SKIP` lines: with a busy view the frames
   should lengthen toward 400 ms and the jobs/s should hold up.
6. Check `seek_probe.txt`: every `J` line of every run stays in step with the
   recording (the bracket check described under Results above).

## Phase 2: jumping back

### What the player gets

A second row of buttons, **Jump back**: **−30 s**, **−1 min**, **−5 min** and
**Start**. Clicking behind the current point on the timeline jumps there, and `,`
(`[hotkeys] ReplaySkipBack`) jumps a minute per press. Presses add to a running
seek's target either way, so `,` three times goes three minutes back. The title
reads REWINDING until the replay has been reloaded, then SKIPPING while it runs
forward to the point. A jump back also works from the end-of-recording hold.

### How it works

A rewind is a restart plus a forward seek, so it is exact for the same reason
every playback is. `replaySeeker.cpp` runs it as one more phase of the seek:

1. **Wait for a quiet job.** `LoadGame` keeps the lockstep queue, and the new
   world would execute the old one's commands (phase 0: two of three restarts
   that carried queued commands diverged). The seeker runs the simulation at 10
   updates a frame, through the player's pause and the end hold, until a frame
   tick finds `replayNetQueuedCommands()` and `replayPendingPlacements()` both 0
   (logged waits in phase 0: 0–8 frames). It gives up after 600 frames. The
   in-game menu holds the wait.
2. **Reload.** `replayLoadSave(path)` with the save the playback came from
   (`replaySessionPlaybackPath()`), inside `ObserverFpScope`, from the seeker's
   frame tick. The session's `LoadGame` hook re-arms the playback as for any
   replay load: sidecar, random stream, tile order, match rules.
3. **Start at once.** `replayPlayerStartNow()` samples the epoch before this
   frame's update loop can run; leaving it to the next frame tick ran the world
   ahead of its schedule (phase 0: divergence from job 53). The spectator guard
   drops inside the load and is raised again here, all within the seeker's
   frame-tick call, so the viewer, whose tick comes later, sees an unbroken
   playback: its pause, speed and panel survive. Its tallies restart at the new
   epoch and are rebuilt by the forward seek, which counts kills as playback
   does.
4. **Seek forward** to the target as in phase 1. A target of 0:00 ends there.

A rewind costs the forward seek from the start: a few seconds early in a match,
about a minute at 25 minutes, over three minutes at 50 minutes in a large match.
Keyframes (phase 3) are what would make late rewinds quick; this path stays as
their fallback.

### Result (2026-11-14, a 3-player match of about 12,400 jobs)

Six rewinds in one session, from the timeline, the buttons and `,`, including a
rewind issued while a forward skip was running and one straight after another.
Every one of the seven runs (the first playback and each reload) stayed exact
against the recording at every job; the last run's final 29 jobs are the quit,
with the world tick frozen. Rewinds to points 3,400 and 8,000 jobs in (about 8
and 18 minutes of match) took 5 s and 21 s of forward skipping after the reload.

### Verification

With a debug build:

1. Jump back with each control: a timeline click behind, each button, `,`
   several times, Start. Check the title reads REWINDING, then SKIPPING, and the
   playback lands on the point.
2. Jump back while paused; it should land paused. Jump back from the end of the
   recording.
3. Open the ESC menu during the wait; it should hold and carry on.
4. Check `seek_probe.txt`: every run after a rewind (each reload is a new `RUN`)
   stays in step with the recording, with the bracket check described above.
5. Panel stats after a rewind should match what they showed at that point the
   first time (kills, units recruited).

## Phase 3: keyframes

### What changes for the player

Nothing to press. While a replay plays, watched or skipped, it saves itself
once per minute of match time. A jump back then loads the newest keyframe at or
before the point and skips the rest, so it costs a load (about 0.4 s with the
wait for a quiet job) plus at most a minute of skipping, anywhere in the match.
A skip ahead past a keyframe 600 or more jobs beyond the current point loads
that keyframe instead of simulating up to it. Without a keyframe (the first
minute, or a point the playback has not passed yet) a jump back restarts as in
phase 2.

### What a keyframe holds (`src/replay/replayKeyframes.cpp`)

A keyframe is the replay's own checkpoint taken later, so it carries what the
checkpoint carries, plus where the playback stands:

| Part | Taken by | Put back by |
|------|----------|-------------|
| The game's state | `replayCheckpointSave(path, 2)` (no state announcement) | the keyframe's `LoadGame` |
| Tile and region list order | `replayCaptureTileOrder`, stored as a `TILE` section | `replayPlayerLoadTileOrderBytes`, applied at the epoch |
| Projectile damage in flight | `replayCaptureFlyState`, as a `FLYS` section | `replayPlayerLoadFlyStateBytes`, at the epoch |
| Iterator-0 cursor (a load draws it) | read before the save | `replayPlayerSetIter0AtSave`, at the epoch |
| Monastery stage clocks | `prngGuardSaveClocks` | `prngGuardRestoreClocks`, at the next update |
| Relative job, command cursor, MP cadence byte (`NetGameMgr+4`) | `replayPlayerSavePosition` | `replayPlayerStartAt`, which sets the epoch back by the job |
| Random-stream position, render gates, the update's index in its job | `prngGuardSavePosition` | `prngGuardRestorePosition` |
| Match rules, with the replay's own host | `matchRulesReplaySave` | `matchRulesReplayRestore`, inside the load |
| Panel tallies (units recruited, kills) | `unitCountsSave`, `killCountsSave` | `…Restore`, plus `sessionRebindSlots` |

Taking one needs a frame with nothing queued and no deferred placement, like
the checkpoint; the update's index in its job is restored because a frame can
end between two updates of one job, and the next update after the resume must
read as the one that followed.

The keyframe's `LoadGame` goes through the session's hook like any load, but
`replaySessionExpectKeyframe` marks it first, so the hook keeps the playback:
it re-arms the player (`replayPlayerRearmForKeyframe`, every buffer stays
loaded), puts the rules back, clears the end hold and returns. The spectator
guard never drops, and the viewer resets its tallies only on a new playback
(`replayPlayerGeneration`), not on a resume.

### Storage

About 1 MB each, at most 150 (two and a half hours), in
`%TEMP%\sh2up-keyframes\<process id>`. They are kept while the same replay plays,
a restart included, and deleted when another save loads or the playback ends.
At startup, folders of processes that no longer run are deleted.

### First playtest (2026-11-15): taking a keyframe breaks the playback

Two replays, 17 runs. Every fresh playback that captured keyframes left the
recording shortly after its first capture, and a restart that captured none
stayed exact:

| Run | What | First keyframe | First job out of step |
|---|---|---|---|
| 1 | fresh load, skipping | 752 | 773 |
| 5 | fresh load, other replay | 527 | 564 |
| 12 | fresh load, same replay as 5 | 556 | 564 |
| 7 | restart; every window already had a keyframe, so none taken | — | none |

Every resumed run was out of step from its first job, as it must be when
resumed from a keyframe taken in a run already off, or from a save that itself
changed something. Runs 5 and 12 failing at the same job (564) from captures at
527 and 556 says the save changes some state that only matters when a
particular event comes round. The capture's own reads (tile order, projectile
damage, clocks, positions) were checked to write nothing, so `SaveGame` itself
changes live state the save file does not carry. The replay's own checkpoint
never showed this because there the live match carried on with the same side
effect the loaded save starts from; a keyframe changes the playback's world,
which the recording never had. Phase 0 had timed mid-playback saves but never
checked that the playback stayed exact after one: the run it saved in was
already broken by the old restart bug.

Automatic capture was switched off while the cause was found
(`replayKeyframesSetEnabled`; the probe's `F10` toggles it). The probe's `F9` diffs memory
around one save: the exe's writable sections, and the first 2 KB of every actor,
building and projectile, reported as `DIFF exe` runs (VA, length, first word
before and after) and `DIFF field` lines (class, field offset, how many objects
changed there).

### What the save changes (diff of 2026-11-15, one save at job 11441)

The `F9` diff of one stock save, `replay-11.14.26-634pm-5n`, 2,556 objects:

| Changed | Objects | Cause |
|---|---|---|
| `+0xdc` on every actor class (Mantlet 6.55 → 6.0, Child 4.05 → 4.0, Hunter 0.9 → 0) | 700+ | `GlowWorm::Actor::serialize` (VA `0x789600`, version-2 path `0x7896b8`–`0x7896fb`) writes the path progress as a whole number (`fistp` with truncation) and stores that number back into the live actor. The only float-to-int round trip of this shape in the exe |
| `{Actor*, id}` pairs zeroed (Knight `+0x194`/`+0x198`, the S2Actor target handle, and `+0x3e4`/`+0x3e8`; HorseArcher `+0x440`/`+0x444`) | ~30 | The handle serializer: when the stream is writing (`vt+0x18`, `isOutput`) and the id is set, it re-resolves the handle, stores the result as the cached pointer and **zeroes the id when it no longer resolves**. 148 inlined copies, each calling the actor resolver `0x769360` or the building resolver `0x768550` |
| 4 `Arrow` objects overwritten with the save's path strings | 4 | Freed during the save; not yet explained (see below) |
| `0x107faf8` in the exe | — | The delay-load import slot `SaveGame` calls (`0x42a327`), resolved on first use. Harmless |

Both named causes are invisible at the recorder's checkpoint: the live match
carries on from the same rounded values and cleared handles that the loaded
save starts from. In the middle of a playback, they change a world the
recording never changed. The rounded path progress alone moves units, so it
explains the drift. The cleared ids matter to any code that tests an id
without resolving it (the ballista target at `+0x198` is one such field).

Keeping a stale id is safe across a load. An id is `generation << 16 | slot`,
the generation coming from one 16-bit counter on the manager (`+0x4`,
incremented at each creation, `0x769e9b`), which the save carries. So a stale
id is just as unresolvable after the load as it is live.

**Keyframe mode** (`src/replay/keyframeSerial.cpp`), on only around a
keyframe's save and its load; every other save is stock:

- Path progress is written and read as its raw float bits: a jump at
  `0x7896b8` calls the stream's io on `+0xdc` and continues at `0x789701`.
  Keyframe files are therefore not stock saves, and only a keyframe load reads
  them.
- Both resolvers are hooked at entry. While a keyframe save runs (game thread
  only), the hook runs the stock body, then decodes the caller's next bytes. A
  caller of the serializer's shape (`mov [h],eax / test eax,eax / jne / mov
  [h+4],eax`, with `h` in `ebx`/`esi`/`edi`/`ebp`) has its handle's cached
  pointer and id logged. A handle that does not resolve gets a non-zero answer
  (its old cached pointer, or a zeroed static block), so the stock code skips
  the id store and the file holds the stale id, as the live world does. After
  the save, every logged handle gets both fields back. Handles on the stack
  (serializer temporaries) are answered the same way but not logged, since
  their frames are gone by then. Any other caller gets the stock answer.
- The log holds 128 K handles (1.5 MB, allocated for the save only; a late
  save logs about 8,000). On an overflow the rest get stock behaviour and the
  keyframe is discarded.

**Second playtest (2026-11-15): capture exact, restore crashed in our own
ballista cave.**

- **Capture was exact.** 19 keyframes taken from job 592 to 11442 while
  skipping, and the playback stayed in step with the recording through all of
  them. It left the recording only after a deliberate stock save (`Ctrl+F9`) at
  11503. The keyframe-mode diff changed 40 objects, all four arrows' (below),
  against 822 for the stock save.
- **Every restore late in the match crashed** on the world's first update:
  `0xc0000005` in `S2Actor::isAlive` (RVA `0x15ed0`) reading `0x98`, i.e.
  `isAlive(0)`. The first guess, that the loader relies on a stock save never
  writing an id that fails to resolve, was wrong: writing stock (cleared)
  handles crashed in the same way. The probe's crash report settled it. The
  return address on top of the stack was not in the exe, and the frame below
  was `Actor::tick`'s action update (`0x787d68`): the call came from the patch's
  **ballista auto-fire cave**, which passed the target's *cached* pointer to
  `isAlive`. A load restores only the id, so the cache is null until something
  resolves it. That is a real bug in ballista auto-fire: any save loaded while
  a field ballista holds a target crashed. Fixed in the cave
  (`docs/features/ballista-auto-fire.md`), and keyframe files keep stale ids
  again.

**Third playtest (2026-11-15): one restore exact in structure, capture still
drifts.** With the ballista cave fixed, a jump back from job 15,664 to 13,838
loaded and played without a crash; a second, to 11,267, crashed 21 jobs later in
the pathfinder, on a `TunnelSetTargetChore` whose tunnel hole's `Navigator`
(`+0xe8`) had a garbage vtable. That is a consequence, not a cause: the bracket
check shows the uninterrupted run left the recording at 11,381, 114 jobs after
its keyframe at 11,267, and the run restored from that keyframe was out of step
two jobs after the load. A recording fed to a world it no longer matches can
reach states the stock game never does.

Two leads:

- **The save still changed the live world.** A census of every serializer-shaped
  call site, grouped by callee, found five handle resolvers, not two: actors
  (107 sites), buildings (41), clans `0x774e20` (22), projectiles `0x76a000`
  (12) and elements `0x767e40` (6, a different prologue). Clan, projectile and
  element handles to dead objects were still being zeroed live. All five are
  hooked now.
- **The load loses something the live world keeps.** The restore drifted much
  sooner than the capture did, so the file itself lacks state. The probe's
  `Shift+F10` round trip finds it: at the next quiet job it copies every object
  of the five managers by slot (sized by `HeapSize` on the game CRT's heap, up to
  8 KB) and the 32 player objects, takes a keyframe, restores it at once, and
  reports every field that differs and is not merely a moved pointer
  (`ROUNDTRIP field`), plus objects missing, extra or of another class, and the
  exe's globals (`ROUNDTRIP exe`).

The four "arrows" in the keyframe-mode diff were a false lead: an arrow is far
smaller than the 2 KB window, which reached into the next heap blocks, where the
save's own strings were allocated.

Automatic capture is on in debug builds, and stays off in release until a
playtest confirms restores are exact. The probe's `Ctrl+F10` switches keyframe
files to stock (rounded) path progress, with the live value put back after the
save; it was added to isolate the crash and is kept for comparison. The probe's `F9` takes a keyframe-mode save with the
diff and `Ctrl+F9` a stock one (which changes the playback; use it last); `SAVE` lines
record the mode, `handles` (handles put back) and `complete`.

### Fourth and fifth playtests (2026-11-15, 2026-11-21): two separate problems

**The drift at job 11381 was the ballista fix, not the keyframes.** Recording
`634pm-5n` (ballista auto-fire on, recorded with the 82-byte cave) left the
bracket at job 11381 in every run with the first load-crash fix, with keyframes
on or off and with the skip or with turbo and particles on. That fix resolved the
held target afresh for every check instead of testing the cached pointer, which
is not the same test. The cave now uses the cache whenever it is set and resolves
only when it is null (after a load), and the same playback stayed exact to job
11895 (2026-11-21). Details in `ballista-auto-fire.md`.

**The reloaded world drifts on its first job.** Run B was exact up to a round
trip at job 11199, and was out of the bracket from job 11200, the first job after
the load. The round-trip diff is too noisy to name the cause (3553 of 3684
objects "differ": caches a load clears, the stale action mask `+0x16c`, padding
bytes compared as whole words). One lost state is now known from the tunnel crash
below: an Engineer that is busy in the live world is idle after the load.

**The tunnel crash is a stock out-of-bounds write that the load triggers.** The
probe watched each `TunnelHole`'s second-base vtable pointer (`+0xe8`) with a
hardware write watchpoint on the game thread (a vectored handler sets `Dr0..3`
from a raised exception). It caught the writer on the first tunnel update after
the load, at VA `0x4c8c41`:

- `TunnelHole` (0x160 bytes) holds five engineer handles at `+0xec + 8·i`
  (cache, id), five pending bytes at `+0x120`, and wants `+0x118` engineers.
- Its update (`0x4c9e90`, every 20 ticks) counts the slots that resolve or are
  pending, keeps the first free one in a local that starts at `-1`, and when
  the count is below `+0x118` calls the recruit (`0x4c8c50`) with that index.
  The recruit takes an idle engineer from the player (`0x41a960`, idle =
  `[eng+0x448] == 0 && [eng+0x300] == 0 && [eng+0x440] == 0`), or a new one from
  a guild, and stores it with `0x4c8c00` at `+0xec + 8·index`.
- The ctor (`0x4c8820`) sets `byte +0x114 = 1`, "send the first engineer". The
  first update recruits into slot 0, increments `+0x118` and clears the flag.
  The serializer (`0x4c9080`) does not write `+0x114`, so **every load
  re-arms it**: the first update after a load replaces slot 0's engineer and
  raises the wanted count by one. A tunnel with five engineers wants **6**
  after a load (both tunnels in this match), although the add-engineer chore
  (`0x4c8da0`) stops at 5.
- From then on every update finds no free slot and recruits into slot `-1` as
  soon as an engineer is idle: the cache goes to `+0xe4` and the id over the
  vtable pointer at `+0xe8`, and the next `TunnelSetTargetChore` calls through
  the broken pointer (`0x7cd99d`).

A live match cannot reach it (no saves in multiplayer); single player needs a
load of a tunnel with five engineers and an idle sixth, and it has never been
reported. No stock fix is planned. For keyframes it is a confirmed source of
post-load drift, and the first tunnel update runs on the first tick after the
load, which matches run B leaving the bracket at job 11200. Keyframes now
record `+0x114` per tunnel (building slot and id) at capture and write it back
straight after the load, before any update (`replayKeyframes.cpp`,
`saveTunnels`/`restoreTunnels`). As far as anyone knows no other building
behaves like this; the next round trip shows whether anything else is lost.

With the tunnel flag restored there was no corruption and no crash, but every
jump-back still drifted within a few jobs (2026-11-21: from keyframes at 5149,
8204, 8748, 10263, 11267, 10757; the restart run stayed exact). The probe's
`Ctrl+Shift+F10` draw trace looks for the cause: at a quiet job it takes a
keyframe, records every simulation draw of both random iterators for 40 jobs
(a hardware write watchpoint on each cursor; the draw is inlined at each call
site, so the trapping EIP is the code that drew), restores the keyframe and
records the same jobs again. The first differing draw (`TRACE first
difference`, with EIP and three callers) names the code whose input the load
lost. Only draws from update entry to SIM_END count (debug-only
`g_prngGuardInSimulation`), since the emitters and rendering draw iterator 1 on
a frame schedule.

### What else a load loses (2026-11-21 and 11-22)

The draw trace and a per-update record of actors in both passes (live and
restored, matched by actor slot, filtered to *words that agree at first and
then differ*) found three more things the save does not carry. Whole-object
diffs drown in padding and render state; that filter does not.

**Actor list order.** Each actor is in four intrusive lists, node `{owner,
next, back-link}`: `+0x64` (spatial grid cell, kept at node `+0xc`, grid
`0xae5440`), `+0x74` (tile, table `0xae8eec`), `+0x80` (16×16-tile block,
`0xba8eec`), `+0x8c` (region, `0x288d5f0+0x2c`). Every position update moves the
actor to the front of its lists. A load leaves every node unlinked and relinks
an actor only when it next moves, so standing actors drop out of their tile and
the order changes. The order is simulation input: the speed routine
`0x7875f0` multiplies by 0.8 for each actor ahead of this one on its tile (up
to three), which is what made a walking Mother 0.8x faster on the first update
after a load. The existing tile-order restore (`applyTileOrder`, made for the
checkpoint) only reorders lists the load had already linked, so it did nothing
here. Each keyframe now keeps every chain of all four lists by head slot (the
tables and the grid cells keep their addresses across a load) and rebuilds
them straight after the load, marking occupied grid cells with `0x787810`
(`saveChains`/`restoreChains`).

**Formation spots.** A movement cluster (`GlowWorm::Cluster`, vtable
`0x9f4bcc`, 0xc8 bytes, each clan's vector at `+0x14`) keeps its members'
formation spots in 16-byte records `{actor cache, handle, x, y}` at
`+0x18`/`+0x1c`/`+0x20`, and a flag byte at `+0x90`. Its serializer
(`0x79ddf0`) writes neither. A load leaves the records empty, and a moving
cluster rebuilds them from scratch, so its path progress (`+0x8c`, advanced in
`0x79d130` from the members' distances to their spots) and its members' next
paths come out on other ticks (eight Mantlets, 2026-11-21). Keyframes keep them
per clan slot and cluster index, checked against the member handles, and write
them back into a vector from the game's `operator new` (`0x80fbb6`), with each
cache resolved by the game's own resolver (`saveClusters`/`restoreClusters`).

**A Mother's sound timer** (`+0x2f8`, reset to `0x4b0` by a load) draws
iterator 1 inside the update when it runs out, which shifts any later draw of
that update. Kept per actor slot.

**Not restored: the pathfinder's region map.** Static reading suggests the
pathfinder (`0x2101788`; tile → region word table `0x2101c6c`, region graph at
`+0x4c4`) answers "can I get there" (`0x783f70`, reached from `0x45ad10`) from a
region map that a load rebuilds from scratch. A Child picking a stroll target
got "no" live and "yes" after a load, from the same random offsets (job 5127 of
`634pm-5n`). Whether the live map lags behind building changes was not
confirmed. Two restores of the same keyframe behaved identically, so this is
the keyframe's content, not leftover memory. Capturing the map is a larger job
and is left for later, because of the check below.

### The check against the recording

A keyframe may still leave the recording, so a run that starts from one is
checked. The recorder samples iterator 0 at the entry of each job's first
update (`ReplayPrngEntry::startCursor0`); during playback `prngGuardTickEntry`
compares the live cursor at the same point with the recorded one and keeps the
first job that differs (`prngGuardVerifyMismatch`). An exact playback matches
on every job, so the comparison is equality, not the probe's bracket. When the
current run came from a keyframe and a job differs, the seeker
(`verifyFrame`, every frame before any capture) deletes that keyframe and every
keyframe this run took from the differing job on, and seeks again to the same
target: an older keyframe, or the replay's own save, which is always exact. A
bad keyframe therefore costs a second load and a longer skip, never a wrong
picture, and divergences have so far appeared within 2 to 40 jobs of the load,
inside the skip to the target. With that in place automatic capture is on in
release.

The first release playtest showed why the skip has to be long enough: a
jump-back landing just after a keyframe was already playing when its run left
the recording, and the viewer saw one or two automatic re-jumps. A seek
therefore uses only a keyframe at least `VERIFY_JOBS` (60) jobs before its
target, unless that keyframe is already verified: a run resumed from it has
matched the recording for 60 jobs, which marks it trusted
(`replayKeyframesMarkVerified`). The check keeps running after that, so a later
divergence is still repaired, just visibly.

### Playtest (2026-11-22, `634pm-5n`)

| Run | What | Result |
|---|---|---|
| 1 | Plain playback to 6101 | Exact; the check reported nothing over every job |
| 2 | Jump back, keyframe 5183 | Left the recording at 5185; caught two jobs in, keyframe dropped, seek redone |
| 3 | The redo, keyframe 4667 | Exact to 8084 |
| 4 | Jump back, keyframe 7168 (taken in run 3) | Exact to 11286, then the game was closed |

The draw trace at job 7160 (after the list and cluster fixes) matched the live
pass on all 10,597 simulation draws of iterator 0 and all of iterator 1.

## Code

Release-visible additions, all inert unless called:

- `replayViewerSetUpdateOverride(n)`: the loop-site detour writes `n` to
  `World+0xce30c` before the stock compare. Pause still wins. It is cleared
  whenever the viewer releases its pause (playback end, leaving the match).
  Cost when unused: one compare per frame.
- `replayPlayerSetJobObserver(fn)`: called at the pump once per job of a
  playback. Cost when unset: one compare per job.
- `replaySessionPlaybackPath()`: the save the current playback was loaded from.
- `replayLoadSave(path)`: the pause-menu load sequence.
- Phase 1: `replaySeeker.*`; `replaySessionTimeline()` (end job, ms per job);
  the viewer's timeline, skip buttons and `[hotkeys] ReplaySkip`; one call per
  job from the pump to `replaySeekerOnJob` (one compare when no skip runs).

The probe adds no hook sites. It rides the viewer's loop site (`0x21ea2`), the
player's pump (`0x367a03`) and the shared frame tick.

## Multiplayer Compatibility

**Safe for version mismatch.** Skipping and the probe work only during an
offline playback under the spectator guard. They touch no live match, and a
replay plays the same whether or not it was skipped.
