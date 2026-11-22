# Units Answer Attack Orders

**Status:** Added in v0.9.0. Playtested 2026-11-14: works as expected.
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** Two `call` operands retargeted (recipe A in `CLAUDE.md`), client-local.
**Source:** `src/patches/attackVoices.cpp`
**Setting:** `[sound] AttackVoices`, default `1`; `0` installs nothing. Also an
"Attack voices" switch in the settings panel (`Ctrl+Shift+O`).

---

## Symptom

Selected troops answer a move order with a spoken line ("Moving out!"), but an
attack order gets no voice at all. Only the marching or hooves effect plays, so
a small group ordered to attack sounds exactly like one ordered to walk.

This is stock behaviour, not a patch regression: the unpatched game is silent
too.

---

## Root cause

The game loads the attack lines and never plays them.

Every voiced unit type's class object (`actor+0x174`) holds a table of speech
handles filled by its class constructor through the handle-from-name ctor
(VA `0x42e410`, which calls `Dragonfly::ResourceMgr::resolveIndex`). Field
`+0x124` is the move line, `+0x144` `FXT_<UNIT>_ATTACK_STRONGER` and `+0x148`
`FXT_<UNIT>_ATTACK_WEAKER`. All 32 `SoundUtil::playStream` call sites (IAT VA
`0x9008a4`) were checked: none reads `+0x144` or `+0x148`.

The move voice is not played by the simulation. The move poster
(`S2ActorHandler`, VA `0x4f3ae0`) plays it on the click, before posting the
`MoveTroopsChore`:

1. `0x4efac0(&out)` picks the speaker from the selection: the unit whose class
   has the highest priority at `class+0xa0`, preferring one that is not busy
   (vtable `+0xd0`) and whose class kind (`class+0x34`) is not `0x20`. It
   writes `{Actor *, handle}`, or zeros.
2. It calls the speaker's move-voice virtual, vtable `+0x1dc` (Soldier
   `0x4f5360`, Knight `0x5042f0`), which calls
   `playStream(1, handle, 1, false)` with one of the move lines.

The two attack posters, `0x4f38a0` (`AttackActorChore`) and `0x4f39c0`
(`AttackBuildingChore`), have no such step. `AttackActorChore::execute`
(`0x5e3400`) plays only the group effect `0x5993d0(0)` and calls an empty stub
(`0x599b10`). That group effect differs from a move's only for big groups
(`FX_MANYHRS_ATTACK` with 8 or more horses, `FX_LARGEGROUP_ATTACK` above 20
troops).

### The recordings

`fx/sh2_speech.csv` in the install is the speech table (id, volume, variant
files), and the files ship as `.mp3` under `fx/military speech/` (181 `ATK`
files). Of the exe's 40 `ATTACK_STRONGER/WEAKER` names, 34 have rows. The six
gaps are filled from the recordings that exist:

| Voice set | STRONGER | WEAKER | Used instead |
| --- | --- | --- | --- |
| Archer | none | none | `FXT_ARCHER_ATTACK_MISSLE` (12 variants) |
| Crossbowmen | none | none | `FXT_CROSSBOWMEN_ATTACK_MISSLE` (6) |
| Horse archer | none | `Horsearcher_ATKH1` (a hand-to-hand line) | `FXT_HORSEARCHER_ATTACK_MISSLE` (6) |
| Knight | none | 10 variants | `FXT_KNIGHT_ATTACK_WEAKER` for both |

The ranged sets also have `_HAND`, `_AREA`, `_OUTRANGE`, `_EQUIP` and `_PITCH`
lines, which the exe never names either. They are not used.

---

## The patch

Each attack poster has exactly one caller, in the troop-order dispatcher:

| Site (VA) | RVA | Before | After |
| --- | --- | --- | --- |
| `0x6316a1` | `0x2316a1` | `e8 fa 21 ec ff` (`call 0x4f38a0`) | `e8 <rel32 to attackActorPost>` |
| `0x6319ac` | `0x2319ac` | `e8 0f 20 ec ff` (`call 0x4f39c0`) | `e8 <rel32 to attackBuildingPost>` |

Both are preceded by `mov ecx, 0x11b8cb8` (the `S2ActorHandler` static,
RVA `0xdb8cb8`, base-relocated). The posters are thiscall
`(slot, target *, target handle)`, `ret 0xc`, and the stubs keep that
convention, so neither site needs a fixup. Each stub calls the stock poster
first, so the order is posted exactly as before, and then plays the voice:

1. Pick the speaker with the stock `0x4efac0`, the same unit the move voice
   would use.
2. Accept it only if its vtable `+0x1dc` is Soldier's or Knight's move voice.
   An RTTI census of every vtable found exactly 21 such classes: `Soldier`,
   its 19 voiced subclasses and `Knight`. Only their class objects have the
   voice table, so a selected siege engine, say, is never read as one.
3. Identify the voice set by `class+0x124` (the move line's handle) against
   the 20 move names in a fixed table. Each of the 20 class constructors stores
   its `FXT_<UNIT>_MOVE` there (checked for all 20).
4. Choose STRONGER when the target's maximum health (`actor+0x48`) is above
   the speaker's, otherwise WEAKER. Both are positive floats, compared as their
   int32 bits, so the path has no float arithmetic. A building target always
   gets WEAKER.
5. `playStream(1, handle, 1, false)`, the arguments the stock move voice
   passes.

The table's 56 names are resolved through the stock `0x42e410` on the first
attack order, when the sound resources are certainly loaded; `DllMain` is too
early. Every name was checked against `sh2_speech.csv` before shipping, so no
unknown handle ever reaches `playStream`.

Install checks, all-or-nothing: both sites are `b9 <relocated handler>` +
`e8` resolving to the expected poster; both posters and `0x42e410` begin with
`55 8b ec 6a ff`; `0x4efac0` with `55 8b ec 83 ec 2c 83 c8 ff`; the two voice
virtuals with their stock prologues.

No voice plays while a replay is being watched (`spectatorGuardActive()`), since
the spectator guard refuses the click's command anyway.

Switching it on from the settings panel installs the hooks on the next frame
tick. That tick runs between frames, when the troop-order dispatcher is not
executing. Switching it off leaves the hooks in and silences the voice.

### Judgment calls

- **Which line counts as "stronger".** The ids name the target ("attack a
  stronger / weaker enemy"). The playtest should confirm it by ear: compare
  `Peasant_ATKS1.mp3` with `Peasant_ATKW1.mp3`. If the meaning is the other
  way round, swap the comparison in `targetIsStronger`.
- **Maximum health as strength.** It is one unit against one unit. A whole
  army ordered onto a lone archer hears the WEAKER line, which is the intent.
- **Buildings get WEAKER.** A building cannot fight back.

---

## Multiplayer Compatibility

**Safe for version mismatch.** The patch only adds a sound on the clicking
client, after the stock poster has posted the unchanged command. It posts
nothing and writes no simulation state, and an unpatched opponent's game is
unaffected.

Randomness was checked, because a lockstep game must not draw from a shared
random sequence on one client only:

- `playStream` (dragonfly.dll export `0x1001b820`) reaches no
  `Dragonfly::Random`/`RandomIter` call. Its only random source is the C
  runtime's `rand()` (MSVCR100, called at `0x1001286c`), while stream variants
  rotate through a counter (`CompositeSoundInfo::getNextName`, `0x1000b050`).
  That walk followed direct calls only (64 functions); 55 indirect calls were
  not followed.
- Every stock sound, including the move voice on each move click, already
  draws from that `rand()` independently on each client. The exe has 12
  `rand`/`srand` call sites (IAT `0x9002b4`/`0x9002d4`), not classified, but
  any simulation reader among them would already be exposed to the stock
  sounds. Replays support this: playback stays job-exact against the recorded
  iterator-0 cursor at different speeds, which play different sounds, so the
  simulation does not consume `rand()` in practice.

Not a match rule, and replays need nothing: no simulation change.

---

## Testing

1. Select each unit type in turn, right-click an enemy unit: one voice line
   per order, from the same unit type that answers a move.
2. Mixed selection: the voice belongs to the unit type that answers a move for
   that selection.
3. Archers, crossbowmen and horse archers use their missile lines. **The horse
   archer row has a blank volume in `sh2_speech.csv`.** If it plays silent,
   switch that set to `FXT_HORSEARCHER_ATTACK_WEAKER`.
4. Attack a building: the WEAKER line.
5. Select only siege equipment and attack: silence, no crash.
6. Switch "Attack voices" off in the settings panel: silence; on again: voices.
7. Multiplayer against an unpatched player: no desync.
