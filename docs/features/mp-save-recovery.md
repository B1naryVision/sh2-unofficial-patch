# Multiplayer Save Recovery (desync insurance)

**Status: ON HOLD — deferred to a future release.** The source
(`src/patches/mpSaveRecovery.cpp/.h`) is kept in the repo but is **excluded
from the build** (not in the Makefile `SRCS`, not registered) — same convention
as ballista-auto-fire and unit-cap-raise. The last built form was a
*diagnostic* (samples flags, performs no save); to resume, see "Runtime
verification log" and "Diagnostic status file" below.
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** No byte patches. Frame-tick callback that calls the game's own
`SaveGame` routine, plus a background worker that converts the save into a
rehostable `.s2m` map with rolling retention. **Timer-based live saving does
not work in multiplayer** (kicks the session to the lobby); the intended
redesign is to save at session-end.

---

## Symptom / Motivation

Multiplayer desyncs kill the match with no way to resume. A multiplayer save
converted to a map (`.sh2` → rename → `.s2m`) can be rehosted from the moment
of the last save. But the vanilla game blocks saving during multiplayer, so
there is nothing to convert. This patch periodically forces the engine's own
save routine during MP matches and maintains the two newest recovery maps in
the game's `maps\` folder (`MP_Recovery_YYYYMMDD_HHMM.s2m`).

---

## Why no byte patch is needed

The MP restriction is a **UI gate, not a save-engine gate**: `SaveGame` (VA
`0x42a2e0`) has no multiplayer check that could be found, and the MP ESC menu
simply never offers Save. Calling `SaveGame` directly bypasses the restriction
without touching game code.

A note on a false lead: the in-game ESC menu's command dispatcher (function at
VA `0x6837e0`, command ids `0x144e`–`0x1461`) swallows its Save/Load commands
when the byte at VA `0x11b8483` is nonzero (checks at VA `0x683839`,
`0x68386a`, `0x68389d`). This was initially assumed to be the "network game
active" flag — **runtime testing (2026-07) disproved that: the byte reads 0
during an MP match**. It gates the menu for some other mode (likely
cutscene/tutorial playback); its two known writers both clear it
(`MainMenuScreen::OnActivate` at VA `0x67dd64`, and VA `0x6567a1`), and its
setter writes through a register-based address, so its true meaning was never
pinned down. The patch no longer reads it.

---

## Resolved offsets (all RVAs = VA − 0x400000)

| Item | RVA | Notes |
| --- | --- | --- |
| `SaveGame(std::wstring path)` | `0x2a2e0` | `thiscall`; one **by-value** MSVC wstring arg (0x1c-byte stack slot, callee pops via `ret 0x1c`); returns `bool` in `al`. Writes the full save incl. header keys `version`, `maxplayers`, `balanced`, `lastsave`, and a game-mode char (`k/h/w/p/f` from `[this+0x40284]`) |
| Save/profile manager (`this`) | `0xd781d0` | global object; all call sites do `mov ecx, offset` (relocated under ASLR) |
| wstring copy-ctor | `0x1d120` | `thiscall(dest)` + 1 stack arg (src), `ret 4`; used to construct the by-value arg with the **game's allocator** |
| `GetSavesFolder(wstring *out)` | `0x28880` | `thiscall` on the same manager; constructs `out` in place with `<Documents>\Stronghold 2\Saves\` |
| ESC-menu command gate (byte) | `0xdb8483` | **not used by the patch.** Blocks the menu's Save/Load commands when nonzero, but reads 0 during MP matches (measured) — it is *not* an is-multiplayer flag. Kept here as negative knowledge |
| Save-kind flag (byte) | `0xe46594` | the stock save handler zeroes it immediately before calling `SaveGame` (VA `0x683eab`); mirrored for exactness |

### The stock call pattern being reproduced (VA `0x683e99`–`0x683ebb`)

```asm
sub  esp, 0x1c            ; stack slot for the by-value wstring
lea  edx, [ebp-0x430]     ; source path wstring
mov  ecx, esp
push edx
call 0x41d120             ; wstring copy-ctor (ret 4) — game heap owns the copy
mov  byte [0x1246594], 0
mov  ecx, 0x11781d0       ; save manager
call 0x42a2e0             ; SaveGame — pops its own 0x1c-byte argument
```

The patch replays this byte-for-byte from inline asm
(`src/patches/mpSaveRecovery.cpp`, `callSaveGame`). The by-value wstring
**must** be built with the game's copy-ctor: `SaveGame` destroys its argument
with the game CRT's allocator, so handing it a buffer allocated by the patch
DLL's CRT would corrupt a heap. Our side only supplies a read-only *source*
struct (pointer/size/capacity) that the ctor copies from.

MSVC wstring layout used (see CLAUDE.md): 16-byte SSO union, `_Mysize` at
+0x10, `_Myres` at +0x14; `_Myres >= 8` means the union holds a heap pointer.
The source struct sets `res >= 8` to force the pointer path.

`GetSavesFolder`'s returned heap buffer is deliberately leaked (once per
process, ~100 bytes) rather than risking a misidentified destructor.

---

## How the save-routine chain was found

1. UTF-16 string scan → `\Stronghold 2\Saves\` (VA `0x9086f8`), single code
   xref → `GetSavesFolder` (VA `0x428880`); its 6 callers are the save/load
   screens and the writers.
2. The `Saves\Restarts\` sibling (VA `0x428920`, one caller) exposed the
   restart-save writer, which assembles `<name>_Restart_<n>.sh2` and calls
   VA `0x42a2e0` — same callee as the ESC-menu save handler (`0x683ebb`).
   Six total callers (menu save, restart save, two save screens, two
   wrappers) confirm `0x42a2e0` is the save entry point.
3. String-table dead ends worth remembering: xrefs of display strings ("Game
   saved") lead to a **localization table loader** and SEH unwind funclets,
   not logic. The localization key `MPADVANCEDOPTIONS072` = "Auto Save Game"
   suggests Firefly had (or planned) native MP autosave — supporting evidence
   that the save engine works in MP.
4. The MP gate byte was found from the menu dispatcher's guard
   (`cmp byte [0x11b8483], 0`) and its writer inside
   `MainMenuScreen::OnActivate` — the same lifecycle hook the endgame-stats
   tracker uses.

---

## Runtime design

- **Trigger (game thread):** a `registerFrameTick` callback. Gated on
  `sessionInGame()` only (endgame-stats lifecycle — true only after the first
  unit recruit). There is deliberately **no is-multiplayer gate**: the only
  candidate flag turned out not to be one (see above), and saving in
  single-player games too is harmless — retention keeps the clutter at two
  maps. Every 3 minutes (`SAVE_INTERVAL_MS`) it calls `SaveGame` with
  `<Documents>\Stronghold 2\Saves\MP_Recovery.sh2` and, on success, signals
  the worker. Integer-only code (mid-function hook: x87 rule).
- **Worker (background thread):** created lazily on the first successful save
  (never under loader lock). Waits on an auto-reset event; on wake sleeps
  1.5 s for the engine to finish flushing, then copies the `.sh2` to
  `<gamedir>\maps\MP_Recovery_YYYYMMDD_HHMM.s2m` (`std::filesystem`,
  overwrite, 10×500 ms retries against transient locks) and deletes all but
  the `KEEP_LATEST_MAPS = 2` newest `MP_Recovery_*.s2m` (zero-padded
  timestamps sort lexicographically = chronologically).

The toolchain is `--enable-threads=win32` (no `std::thread`), hence Win32
`CreateThread`/`CreateEventW` for threading while file handling stays
`std::filesystem`.

---

## Multiplayer Compatibility

**Safe for version mismatch.** Saving serializes simulation state to local
disk; it does not mutate any entity, so lockstep state is identical whether or
not the opponent runs the patch. Two caveats:

- The synchronous save causes a **frame hitch** (sub-second to a few seconds
  on large maps) every interval on the patched client; in lockstep the other
  client simply waits, same as any hiccup.
- Rehosting a recovery map is a new lobby — all players rejoin manually.

---

## Runtime verification log

1. ~~The full chain works in single player~~ — **verified 2026-07**:
   `SaveGame` succeeded from the frame tick and the recovery `.s2m` appeared
   with the current game state.
2. ~~Byte `0xdb8483` is an is-multiplayer flag~~ — **disproved 2026-07**: it
   read 0 for an entire MP match. It gates the ESC menu for some other mode.
3. ~~`SaveGame` is safe to call during a live MP match~~ — **disproved
   2026-07, the decisive finding.** Status file showed `saves_attempted=1`,
   `last_save_ok=1`, and the recovery map was written — i.e. the save
   *succeeded* — but the act of calling it **kicked the whole session back to
   the multiplayer lobby** (no crash). `SaveGame` is not side-effect-free: at
   VA `0x42a37e` it writes game-state global `0xa892ac`/`0xa892b0 = 6` and
   dispatches through `0x40c2a0` before serializing. Single-player absorbs that
   transition; a networked match tears down to the lobby. This is *why* the
   stock game never offers Save in MP, and it means **no save can be produced
   during a live networked match by any path** — every save entry
   (menu save, restart save `0x683600`, autosave `0x42a930`, scenario-editor
   save `0x42a850`) funnels through `SaveGame` at `0x42a2e0`.

### Consequence for the feature

Because a real desync **crashes both clients instantly** and you cannot save
in the seconds before it without ending the session, there is **no pre-desync
recovery point possible in MP**. The feature is therefore being redesigned to
**save at session-end** (graceful drops — a peer's connection dies, host quits)
rather than on a timer. This covers the common "someone dropped" case but not a
hard desync. Requires: (a) a reliable is-networked-game signal, and (b) a
leave-game hook where the world is still in memory (note: by the time any
menu/lobby screen's `OnActivate` fires, the world is already destroyed — this
is why endgame-stats polls continuously instead of reading at the end).

## Diagnostic status file (current build)

The current build is a **diagnostic that performs no save at all** (so it
cannot disturb a match). Each in-game session it samples candidate flags into
`<gamedir>\mp_save_recovery_status.txt`:

```text
gate_byte_0xdb8483=<v>   # menu gate (0 in MP; want SP value)
net_a_0x1245745=<v>      # network-manager flag serialized into saves
net_b_0x11b845c=<v>      # set from a network-object call (VA 0x6caeea)
net_c_0x1246305=<v>      # written by save / end-of-game handlers
mode_char=<v>            # game-mode char k/h/w/p/f off the local player
```

Play one single-player skirmish and one multiplayer match and compare the two
files: the field that differs SP-vs-MP is the discriminator to gate the
session-end save on. This replaces guessing a flag's polarity (which the
`0xdb8483` mistake cost us a test round).
