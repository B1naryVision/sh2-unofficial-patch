# Replay Seeking (fast-forward and rewind)

**Status:** Phase 0 (measurements) complete, 2026-11-05. Nothing player-facing yet.
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** Debug-only probe (`make debug`); release builds contain only the
three small APIs listed under [Code](#code), unused.

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
| `F11` | Restart the replay at the next job with no queued command and no deferred placement: `LoadGame` of its own save, as the pause menu does it, then the playback epoch at once (`replayPlayerStartNow`). `Shift+F11` restarts at once, without waiting |

`F12` is avoided because Windows reserves it for debuggers.

### What each log line records

| Line | Recorded |
|------|----------|
| `J <run> <job> <iter0> <hash> <worldTick>` | Every job of every playback, at the pump, before that job's injections. `iter0` is the simulation-only random cursor. `hash` is FNV-1a over each player's colour, honour, troops, siege, castle, gold, popularity, incomes and honour sources. Two runs of one replay must agree on every common job. |
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

The probe adds no hook sites. It rides the viewer's loop site (`0x21ea2`), the
player's pump (`0x367a03`) and the shared frame tick.

## Multiplayer Compatibility

**Safe for version mismatch.** The probe works only during an offline playback
under the spectator guard. It touches no live match.
