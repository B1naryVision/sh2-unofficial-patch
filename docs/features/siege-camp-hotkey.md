# Siege Camp Hotkey — Open Panel First, Jump Camera on Second Press (`J`)

**Status:** Implemented — confirmed in-game (panel opens without moving the view, a second press travels there)
**Affected version:** Stronghold 2 Steam v1.5.0 (32-bit, `Stronghold2.exe`)
**Config:** `[interface] SiegeCampJumpOnSecondPress` in `sh2-unofficial-patch.ini` (default `1`; `0` restores stock behaviour)
**Patch type:** Two trampolines inside `GameScreen::onKeyDown`'s siege camp case — one latches
the panel state (RVA `0x1f2c65`), one gates the case's camera block (RVA `0x1f2c99`)

---

## Symptom / Motivation

The stock siege camp key (`J`) does two things in one press: it opens the siege camp
recruitment panel **and** it throws the camera across the map to the siege camp. Players who
only want the panel — to queue engines while watching a fight elsewhere — lose their view
every time.

The stock game already has the two-step idiom: the barracks (`B`) and granary (`G`) keys open
their panel on the first press and only move the camera on a second press while the panel is
up. This patch gives the siege camp key the same behaviour.

Patched behaviour:

| Press | Siege camp panel state | Result |
| --- | --- | --- |
| 1st | closed | Panel opens on the first siege camp, camera does not move |
| 2nd | open | Camera jumps to that siege camp, panel stays up |
| 3rd+ | open | Camera jumps again (stock behaviour) |

Closing the panel — ESC, clicking elsewhere, selecting another building — resets the sequence
by itself, because the patch reads the panel's own visibility rather than keeping a private
"pressed once" flag.

---

## How the game handles the key (reverse-engineering trail)

All addresses are VAs at the preferred image base `0x400000` (RVA = VA − `0x400000`).

Keyboard shortcuts are **not** polled from `Dragonfly::EventMgr::ms_keyStateSet` (its ~20 call
sites are all modifier tests and camera scrolling). They arrive as UI events:

```text
Dragonfly EventMgr
  └─ GameScreen::process(Event&)            0x5f75b0   (Pane vtable 0x9c76ec, slot 0)
       ├─ event type 0x14  → 0x5f2510       GameScreen::onKeyDown
       ├─ event type 0x17  → 0x5f3540       (key up)
       └─ default          → 0x5f6710       (mouse / drag handling)
```

`GameScreen::onKeyDown` reads the **virtual-key code as a word at `event+0xe`** (modifier bits
live at `event+0xc`) and dispatches through a two-level MSVC switch:

```asm
5f2564: movzx ecx, WORD PTR [esi+0xe]      ; VK code
5f256a: cmp   eax, 0x54                    ; 'T'
5f2579: sub   eax, 0x30                    ; '0'
5f257c: cmp   eax, 0x22
5f2585: movzx eax, BYTE PTR [eax+0x5f340c] ; case index table
5f258c: jmp   DWORD PTR [eax*4+0x5f33d8]   ; jump table
```

Decoding both tables gives the building shortcuts:

| Key | VK | Case | Building |
| --- | --- | --- | --- |
| `B` | `0x42` | `0x5f2a66` | Barracks (two-step) |
| `G` | `0x47` | `0x5f29f8` | Granary (two-step) |
| `J` | `0x4a` | `0x5f2c3b` | **Siege camp (panel + camera in one press)** |
| `K` | `0x4b` | `0x5f295b` | Keep (camera only) |

### The siege camp case (`0x5f2c3b`)

```text
if (byte 0x1246309) return;                       ; UI blocked (menu/cutscene)
player = World::getLocalPlayer(0x11781d0)         ; 0x411360
if (!player) return;
registry = player + 0x20;                          ; per-type building registry
index    = ds:0x12c0eb0;                           ; "next siege camp" cycle index
retries  = 2;
do {
    handle = getBuildingOfType(registry, &out, 0x46, index);   ; 0x46aec0, ret 0xc
    if (isValid(&handle)) {                                    ; 0x4113c0
        ++ds:0x12c0eb0;
        building->getWorldPos(&pos);                           ; 0x777810
        Camera::setPosition(0xae5a70, &pos);                   ; 0x761890
        ds:0xae5a8c = building[+0x90];                         ; camera angle  (camera +0x1c)
        ds:0xae5a88 = <const 0x907684>;                        ; camera angle  (camera +0x18)
        ds:0xae5adc = 1;                                       ; "camera moved, refresh world"
        SubPanelSiegeCamp::setBuilding(GameScreen+0xdacec, building, id);  ; 0x628c30
        GameInterface->vt[0x40](0x1e);                         ; show the siege camp page
    }
    index = 0; ds:0x12c0eb0 = 0;
} while (--retries);
```

Two things fall out of this that matter for the patch:

- **The camera move is the position and angle writes, not the flag.** `0xae5a70` is the camera
  (a `Dragonfly::Classified`, vtable `0x9054d8`): `+0xc`/`+0x10`/`+0x14` are its position,
  written by `0x761890` (which also zeroes its velocity at `+0x24`..`+0x2c`), and `+0x18`/`+0x1c`
  — the globals `0xae5a88`/`0xae5a8c` — are its view angles. Its per-frame update (`0x762480`,
  called from `0x5ff4f5`) feeds all five straight into the engine's set-view call
  (`0x733310`/`0x735e10`), so the move is immediate; there is no arm-and-animate step.
- **`byte 0xae5adc` is a "the camera moved, refresh the world" flag, not the trigger.** It is
  read at `0x5ff4fa` — `if (0xae5adc || 0x1be6068) { rebuild the scene via 0x78b620 / 0x7575a0
  on 0x135bdb0 }` — and cleared elsewhere at `0x60099f`.

  **Dead end worth recording:** the first version of this patch suppressed only that flag.
  The camera still jumped (of course — the position write was untouched) *and* the terrain
  around the new position stopped being rebuilt, so navigating away left the surroundings
  unrendered. The flag is a consequence of a camera move, not its cause.
- **The case body runs twice per keypress.** The success path falls straight through into
  `0x5f2d02` (`xor eax,eax / dec ebx / mov ds:0x12c0eb0,eax / jne loop`) — there is no `break`.
  So the cycle index is reset to `0` at the end of every press and the second iteration finds
  the same building again. This is why `J` never cycles in the shipped build and always lands
  on the *first* siege camp, despite the documented "Siege Camp (Cycle)". The patch does not
  change this; it is recorded here because it dictates the hook design (below).

### Panel identification

Recovered via static MSVC RTTI (type descriptor → complete object locator → vtable):
`SubPanelSiegeCamp` has a single vtable at `0x9ccba4` (slot 0 = `process` = `0x628c50`, the
handler already wrapped by [shift-click-recruitment.md](shift-click-recruitment.md)). Its
layout matches the `Pane` vtable shape, i.e. single inheritance, so the object base is also
the `Pane` base and `Pane::isVisible` can be called on it directly.

`GameScreen + 0xdacec` is that panel: the case calls `SubPanelSiegeCamp::setBuilding`
(`0x628c30`, writes `[this+0x4c40]`/`[this+0x4c44]`) on exactly that address. The sibling keys
use the same idiom on their own panels — `B` on `GameScreen+0x83024` (`SubPanelBarracks`) and
`G` on `GameScreen+0x73e8c` — and both gate their camera jump on
`Pane::isVisible` (`0x40cfa0`, `return visible(+0xc8) && (parent(+0x80) ? isVisible(parent) : true)`).

---

## The patch

### Hook 1 — latch the panel state (RVA `0x1f2c65`, VA `0x5f2c65`)

```text
before:  bb 02 00 00 00        mov ebx,0x2
after :  e9 <rel32>            jmp latchHook
```

`mov ebx,2` is the loop counter initialiser at the head of the case, after the "UI blocked"
and "no local player" guards and before the search loop. It is exactly 5 bytes and fully
position-independent (no relocated immediate), and nothing branches into it — the only inbound
edges in the case are `0x5f2c6a` (loop back) and `0x5f2d02` (retry). `edi` still holds the
`GameScreen` `this` for the whole of `onKeyDown` (`mov edi,ecx` at `0x5f253b`).

The trampoline stores `edi`, calls `siegeCampLatch()` inside a `pushal`/`pushfl` frame,
re-emits `mov ebx,0x2` verbatim and resumes at `0x5f2c6a`. `siegeCampLatch()` validates the
panel's vtable pointer, then records `Pane::isVisible(GameScreen+0xdacec)`.

**Why latch instead of reading the panel live in hook 2:** the case body runs twice per press.
On the first pass the panel is opened by `vt[0x40](0x1e)`; a live read on the second pass would
see it visible and let the camera through, defeating the patch. Latching once, before the loop,
gives both passes the pre-keypress answer.

### Hook 2 — gate the camera block (RVA `0x1f2c99`, VA `0x5f2c99`)

```text
before:  8d 8d 78 ff ff ff     lea ecx,[ebp-0x88]
after :  e9 <rel32> 90         jmp cameraGateHook ; nop
```

`0x5f2c99` is the first instruction of the case's camera block. Not suppressing means
re-emitting `lea ecx,[ebp-0x88]` verbatim and resuming at `0x5f2c9f` — the stock block runs
untouched. Suppressing means jumping the whole block and resuming at `0x5f2cdf`.

The block is not purely camera code: MSVC interleaved three instructions of the *panel* call's
argument setup into it, so the skip path performs exactly those, plus the world-refresh flag
the stock code always sets:

```asm
mov  ecx, [ebp-0x1c]          ; 0x5f2cbe — building handle id (panel arg)
sub  esp, 8                   ; 0x5f2ccd — panel argument block
mov  eax, esp                 ; 0x5f2cd0
mov  BYTE PTR [refresh], 1    ; 0x5f2cd8 — kept: a redundant refresh is harmless,
                              ;            a missing one is not (see dead end above)
```

Skipping the block as a unit is what keeps the x87 stack balanced: both `fld`/`fstp` pairs
inside it (`0x5f2cb8`–`0x5f2cd2`) are skipped together, so no float state is disturbed and the
gate itself needs no `fxsave`. `getWorldPos` is skipped too — it only writes the local
`[ebp-0x88]`, which nothing else reads.

At `0x5f2c99` `eax`, `ecx`, `edx` and the flags are all dead (the last flag producer is
`inc ds:0x12c0eb0`; the next consumer is `dec ebx` at `0x5f2d04`), and `esi`/`edi`/`ebx`/`ebp`
are untouched by the gate — so it needs no save/restore at all.

### Install-time verification

Both sites are `memcmp`'d against their stock bytes before either is written (all-or-nothing);
both are 5/6 bytes of fully position-independent code, so the expected bytes are plain literals.
The latch is installed first: on its own it only sets a flag nothing reads.

`siegeCampLatch()` also leaves suppression off (i.e. stock always-jump behaviour) when the
vtable check fails, so a future game build that moves the panel degrades to vanilla rather than
to a camera that never moves.

---

## Resolved offsets (RVAs)

| Name | RVA | VA | Meaning |
| --- | --- | --- | --- |
| Latch hook site | `0x1f2c65` | `0x5f2c65` | `mov ebx,2` at the head of the siege camp case |
| Gate hook site | `0x1f2c99` | `0x5f2c99` | `lea ecx,[ebp-0x88]`, head of the camera block |
| Gate resume | `0x1f2cdf` | `0x5f2cdf` | First instruction after the camera block |
| Camera refresh flag | `0x6e5adc` | `0xae5adc` | Byte: the camera moved, rebuild the world |
| `Pane::isVisible` | `0xcfa0` | `0x40cfa0` | thiscall, no args, returns bool |
| Siege panel offset | `0xdacec` | — | `SubPanelSiegeCamp` inside `GameScreen` |
| `SubPanelSiegeCamp` vtable | `0x5ccba4` | `0x9ccba4` | Validation |

Context (not patched): `GameScreen::onKeyDown` `0x1f2510`, key jump table `0x1f33d8`, case index
table `0x1f340c`, siege camp case `0x1f2c3b`, cycle index `0x12c0eb0`, camera object `0xae5a70`
(position `+0xc`/`+0x10`/`+0x14`, angles `+0x18`/`+0x1c`, velocity `+0x24`..`+0x2c`),
`Camera::setPosition` `0x361890`, camera per-frame update `0x362480`, screen update `0x1ff3c0`,
siege camp building type id `0x46`.

---

## Multiplayer Compatibility

**Safe for version mismatch — the other player does not need the patch.**

The patch changes only when the *local* camera moves and never touches the command path. No
game entity changes state: `SubPanelSiegeCamp::setBuilding`, the interface page switch and the
recruit commands posted from the panel all run exactly as before, and the camera controller at
`0xae5a70` is pure client-side presentation. A patched and an unpatched client differ only in
where their own camera is pointing.

---

## Notes and dead ends

- **Display strings lead nowhere for hotkeys.** There is no key-binding table in
  `stronghold2.configuration.xml`, `options_override.xml` or the localisation files, and the exe
  contains no `hotkey`/`keybind` strings at all. The bindings are hardcoded in the switch above.
- **`ms_keyStateSet` is a red herring.** The imported `Dragonfly::EventMgr::ms_keyStateSet`
  bitset (`bitset<299>`, IAT slot `0x90072c`) has ~20 call sites; all of them read dword `+0x20`
  (bit indices 256+, i.e. modifier pseudo-keys) except two camera-rotate polls. Shortcut keys
  never go through it.
- **The `J` "cycle" is broken in the shipped build** (missing `break`, see above) — every press
  targets the first siege camp. Fixing that is a separate change; this patch deliberately
  preserves "first siege camp" focus.
