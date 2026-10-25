# Intro Skip

**Status:** Added in v0.2.0; timer-resolution fix and byte checks added after v0.7.0  
**Affected version:** Stronghold 2 Steam v1.5.0  
**Patch type:** In-memory write (2 sites) + one `timeBeginPeriod(1)` call

---

## Symptom

On launch the game plays three Bink videos before reaching the player-name
screen: the Firefly Studios logo, a trailer, and the Stronghold 2 opening FMV.
Each has to be clicked through by hand.

---

## Root Cause

The intro is managed by an `IntroBinkScreen` class. It owns a video-player
object at `this+0xe0` (the game's own Bink wrapper; the `HBINK` lives at
player `+0xe4`, i.e. screen `+0x1c4`). A counter at `[this+0x1e4]`, initialised
to 0, selects which video is playing:

| Counter after increment | Action in `Update` |
| --- | --- |
| 1 | open `ui\frontend\SHW_trailer.bik` |
| 2 | open `ui\frontend\stronghold2 fmv.bik` |
| 3 | transition to the player-name screen |

`OnActivate` opens the first video, `ui\frontend\firefly_logo.bik`. Every
frame, `Update` asks the player whether its video has finished
(`0x7fe600`: true when there is no handle, or the current frame has reached the
frame count). If it has, `Update` increments the counter and acts on the table
above.

Two sites control this:

| Site | VA | Bytes (original) | Role |
| --- | --- | --- | --- |
| Counter init | `0x8DA9F8` | `00 00 00 00` | imm32 of `mov dword [esi+0x1e4], 0` in the factory |
| Video-open call | `0x67BB0D` | `E8 1E 2F 18 00` | `call 0x7fea30` — opens `firefly_logo.bik` |

`0x7fea30` is the player's open method, not `BinkOpen` itself. It is thiscall
(`ecx` = player) with six stack arguments (path plus five flag bytes) and ends
with `ret 0x18`. It closes any handle already open and then calls
`BinkOpen` (IAT `0x9004a4`). Neither patched site holds an absolute address,
so both are compared against the stock bytes directly. The call's rel32 is
rebuilt from the two RVAs rather than written as a literal.

---

## Fix

**Patch 1 — `base+0x4DA9F8` (4 bytes)**

Write `02 00 00 00`, so the counter starts at 2.

**Patch 2 — `base+0x27BB0D` (5 bytes)**

Replace the call with `83 C4 18 90 90` (`add esp,0x18 ; nop ; nop`). That pops
the six pushed arguments the callee would have popped with `ret 0x18`. No video
is opened and the handle stays NULL.

**Combined effect:** on the first `Update` tick the player reports "finished"
(no handle), the counter goes from 2 to 3, and the screen moves straight to
the player-name screen. None of the three videos is opened.

Both sites are checked before either is written (all-or-nothing). If either
does not match, the patch does nothing.

**Patch 3 — `timeBeginPeriod(1)`** (see below)

---

## Side Effect: Timer Resolution and Stuttering

Reported as occasional in-game stuttering with the patch installed. Cause, from
static analysis:

1. **Bink raises the process's timer resolution.** `binkw32.dll`'s DirectSound
   audio backend starts a mixing thread (`CreateThread` at
   `0x18012540`) and calls `timeBeginPeriod(1)` there (`0x1801267a`). The
   matching `timeEndPeriod(1)` is in its teardown (`0x18012a20`; calls at
   `0x18012a9c`/`0x18012abd`), reached when the last sound track is released.
   No other module in the install imports `timeBeginPeriod`: not the exe, not
   `dragonfly.dll`, not any other shipped DLL.
2. **The stock intro never closes its last video.** When a video plays to its
   end, the counter-3 path in `Update` (`0x67bb91`) changes screen without
   closing the player. Only the click handler, `process(Event&)` (vtable slot 0,
   `0x67bd10`), closes it (`0x67be28` → `0x7fe840` → `BinkClose`). Screen
   objects persist for the process lifetime, so a player who lets the intro run
   out keeps Bink's 1 ms request for the whole session.
3. **The simulation tick pacer depends on it.** `0x420e30` runs the sim at
   40/45 ticks per second (`1000 / rate` ≈ 22–25 ms per tick). It keeps a
   running balance at `[obj+0xce34c]` of elapsed time minus tick time, and when
   the game is more than 5 ms ahead it calls `Sleep(-balance)` (`0x420f13`). At
   the default ~15.6 ms timer granularity, any `Sleep(6..15)` rounds up to about
   a full tick, and the next frame has to catch up, so tick cadence becomes
   uneven.

With the intro skipped, no Bink video is ever opened before the first match,
so the request is never made and the pacer runs at default granularity. Why it
shows only some of the time:

- The pacer sleeps only when frames finish early (fast machine, light scene).
- On Windows older than 10 version 2004, any other program that raises the
  timer resolution (browsers, voice chat) masks the problem for every process.
- A stock player who clicks through the final video also loses the request.
  This is a latent vanilla behaviour that the patch made universal.

**Fix:** `installIntroSkip` calls `timeBeginPeriod(1)` once, after both writes
succeed. This reproduces what a stock player who watches the intro gets. The
call is never undone: Windows drops a process's request when it exits, and the
stock game never undoes it either. It also covers stock players who clicked
through. Bink's own begin/end pairs for later in-game videos nest on top of it
and stay balanced.

**Not yet confirmed at runtime.** To check, run `powercfg /energy /duration 10`
(elevated) during a skirmish and look at "Platform Timer Resolution:
Outstanding Timer Request". With the fix, `Stronghold2.exe` should show a 1 ms
(10000 × 100 ns) request. With the patch DLL removed and the intro left to play
out, it should show the same. Before the fix, the patched game showed none.

---

## Disassembly Notes

`IntroBinkScreen` vtable (VA `0x9DB784`):

| Slot | VA | Role |
| --- | --- | --- |
| [0] | `0x67BD10` | `process(Event&)` — on event type 2: if counter ≥ 2, go to the player-name screen; then close the video |
| [1] | `0x67BC90` | scalar deleting destructor |
| [2] | `0x67BA90` | `OnActivate` — opens `firefly_logo.bik` |
| [4] | `0x67BB30` | `Update` — completion check and counter state machine |

The factory at `0x8DA970` allocates `0x1e8` bytes, constructs the player at
`this+0xe0` (`0x7fe700`, vtable `0x9f9a84`), names the screen
`"intro bink screen"`, and writes the counter at `0x8DA9F2`
(`c7 86 e4 01 00 00 00 00 00 00`).

Video-player methods (vtable `0x9f9a84`): `0x7fe600` "finished?",
`0x7fea30` open (closes previous first; `ret 0x18`), `0x7fe840` close
(`BinkClose` + release texture), `0x7fe620` decode/blit a frame.

---

## Multiplayer Compatibility

Safe for version mismatch. Skipping the videos affects only startup. The timer
request changes how precisely this client's `Sleep` wakes, which is local frame
pacing; it is the same state an unpatched client reaches by watching the intro.
The lockstep simulation's results do not depend on it.

---

## Notes

- No saved game compatibility concerns.
