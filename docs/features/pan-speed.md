# Camera Pan Speed (configurable multiplier)

**Status:** Added in v0.7.0 (offsets from static analysis of the camera controller); opt-in via `sh2-unofficial-patch.ini`
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** In-place byte rewrite (RVA `0x1f9d86`, 6 bytes) — no trampoline

---

## Symptom / Motivation

Panning the camera across the map — with the keyboard pan keys or by
pushing the mouse against a screen edge — is slow, and slower still than it
looks because the camera takes over a second to reach its top speed. Crossing a
255-tile map at a normal working zoom is a multi-second affair, and there is no
in-game option or config-file value for it. This came out of the
[zoom limit](zoom-limit.md) work: once the view reaches much further, the mismatch
between how much map you can see and how fast you can move across it stands out.

## Fix

Multiply the camera's pan speed constant where the controller turns the
frame time into a target velocity. This scales the whole motion profile
uniformly — the top speed and the acceleration ramp alike — for every pan
trigger, keyboard and mouse edge both, while the map-bounds clamps still apply.

The factor comes from `[camera] PanSpeedMultiplier` in
`sh2-unofficial-patch.ini` (see [configuration.md](configuration.md)), accepted
range 0.1–10.0. The default is **1.0, which leaves the game code completely
untouched**.

---

## Terminology: the engine's names run the other way

Player-facing wording in this project calls this **panning** — moving the view
across the map — because "scrolling" reads as zooming to most players. The
engine's own symbols are the opposite way round, and this document keeps them
verbatim:

| Symbol | What it actually is |
| --- | --- |
| `Camera::scroll` (`0x761c70`) | the mover this patch scales — what we call panning |
| `Camera::pan` (`0x761f30`) | **not** the pan path: the map-bounds push-back clamp (see the dead-ends below) |

So "the pan speed constant" and "`Camera::scroll`" refer to the same thing here.

## How panning works in the engine

Three functions, one per layer:

| VA | Role |
| --- | --- |
| `0x5fe010` | camera controller update — reads input, builds four direction flags |
| `0x5f9ca0` | applies the frame's camera deltas, then pushes the camera back inside the map bounds |
| `0x761c70` | `Camera::scroll` — ramps a velocity and moves the camera |

### Where the direction flags come from (VA `0x5feede`–`0x5fef90`)

Four bools are zeroed and then set from two independent sources, which is why a
single constant covers both input methods:

- **Mouse edge pan** (`0x5feeec`–`0x5fef17`): cursor x `<= 0` or
  `>= viewportWidth - 1`, cursor y `<= 0` or `>= viewportHeight - 1`.
- **Keyboard** (`0x5fef1b`–`0x5fef90`): bits in `Dragonfly::EventMgr::ms_keyStateSet`
  (IAT slot `0x90072c`), two bits tested per direction — dword `+0x8` bits
  `0x800000` / `0x80000` / `0x2` / `0x10`, dword `+0x20` bits `0x10000` /
  `0x40000` / `0x8000` / `0x20000`.

### Where the speed comes from (VA `0x5f9d63`–`0x5f9d9e`)

`0x5f9ca0` converts the engine's frame time into a target velocity and passes it
to `Camera::scroll` along with the four flags:

```asm
5f9d63: df 2d f8 81 17 01  fild qword [0x11781f8]   ; frame time, microseconds
5f9d6f: dc 0d b0 80 9c 00  fmul qword [0x9c80b0]    ; * 1e-6  -> seconds
5f9d7c: d9 5d 14           fstp [ebp+0x14]
5f9d80: d9 45 14           fld  [ebp+0x14]
5f9d86: dc 0d e0 81 9c 00  fmul qword [0x9c81e0]    ; * 76800.0
...
5f9d9e: e8 cd 7e 16 00     call 0x761c70            ; Camera::scroll
```

`0x9c81e0` holds the double `76800.0` and **has exactly one reference in the
whole exe**, this one — so it is unambiguously the camera pan speed, in world
units per second before the distance scaling below.

### `Camera::scroll` (VA `0x761c70`)

`scroll(float speed, bool west, bool east, bool north, bool south, bool clamp)`,
thiscall on the camera. It keeps a velocity in `+0x28` (x) and `+0x2c` (z):

- **Acceleration** is `0.8 × speed × dt` per frame (the `0.8` is the double at
  VA `0x99e078`), clamped to `±speed`. Since `speed` is itself `76800 × dt`,
  that works out to a **~1.25 s ramp to full speed**, independent of framerate.
- **Release stops instantly**: at `0x761ca6`–`0x761cf0` a velocity component
  whose direction flag is not held is written straight back to zero. There is no
  deceleration to match the ramp.
- Opposite directions held together cancel each other (`0x761c87`, `0x761c93`).
- The frame delta is clamped to 0.2 s (`0x9a8bf8` / `0x9a1b5c`) so an alt-tab
  cannot fling the camera.
- The displacement is finally scaled by `distance / 16000` (the camera's zoom
  distance at `+0x20`, divisor at VA `0x9f3210`), so **panning is faster the
  further out you are zoomed** — the patch inherits that, which is what keeps a
  raised multiplier usable at both ends of the zoom range.

Scaling the one constant scales `speed`, and therefore the acceleration, the top
speed and the distance-scaled result together: the shape of the motion is
unchanged, only its size.

## Patch bytes

`fmul qword ptr` (`dc /1`, the game's double) and `fmul dword ptr` (`d8 /1`, our
float) share a ModRM byte and a length, so this is an in-place 6-byte rewrite
with no code cave and no trampoline:

| | Bytes at RVA `0x1f9d86` |
| --- | --- |
| Before | `dc 0d <abs32 → 0x9c81e0>` (`fmul qword [76800.0]`) |
| After | `d8 0d <abs32 → g_panSpeed>` (`fmul dword [g_panSpeed]`) |

`g_panSpeed` is a static `float` in the patch DLL holding
`76800.0f × multiplier`, and its absolute address is written into the patch
bytes at install time. The DLL is never unloaded (it is the game's `d3d9.dll`),
so the address stays valid for the process lifetime, and the value is written
once before the patch is applied, so there is no tearing risk. Dropping from a
double to a float costs nothing here: `76800 × multiplier` is far inside a
float's exact-integer range for the whole accepted multiplier range.

The stock operand is an **absolute address, which the loader relocates**, so the
installer cannot `memcmp` the six bytes against a literal (see CLAUDE.md). It
verifies the `dc 0d` opcode and then rebuilds the expected operand from the
running module base (`base + 0x5c81e0`); a future game build that relocates this
code is left untouched rather than corrupted. No branch inside `0x5f9ca0`
targets the rewritten range.

---

## Note on the choppiness

Raising the speed does not smooth the motion, and the two are separate causes:

- The **ramp/stop asymmetry** above (1.25 s up, instant stop) is most of what
  reads as "sluggish then abrupt" rather than "smooth". Changing it means
  rewriting the acceleration constant and the zeroing branches in
  `Camera::scroll`, not this patch's one constant, and it is a change to feel
  rather than to speed — deliberately left alone here.
- The per-frame delta itself is the qword of microseconds at VA `0x11781f8`.
  **Nothing in the exe writes it** — all 23 references are `fild` reads — so it
  is published by `dragonfly.dll`, and its resolution (and therefore any
  frame-to-frame jitter in it) is not something this patch can see statically.

## Investigation dead-ends worth knowing

- **`Camera::pan` (VA `0x761f30`) is not the pan path, despite the name.** It
  looks like the obvious mover — it is the only function that writes the camera's `+0xc`/`+0x14`
  position directly — but all four of its call sites (`0x5f9f33`, `0x5f9f6f`,
  `0x5f9fa2`, `0x5f9fd6`) are the **map-bounds push-back** at the tail of
  `0x5f9ca0`: each is gated on a camera coordinate having fallen outside
  `1024 … mapTiles × 1024 − 1024`, and nudges it back. The real panning runs
  through `Camera::scroll`, which moves the camera via its own velocity fields.
  The tell is the argument shape — `pan` is called with `±magnitude` on one axis
  and a hard `0.0` on the other, four times, which is a clamp and not an input.
- The `0.125` constant multiplied into that push-back magnitude (VA `0x9a8b60`,
  used at `0x5f9efd`) has nine references across the exe and is a shared
  literal — it is not a camera constant, and rewriting it would reach unrelated
  code.
- `0x5fe010` also computes a rotation delta from its own frame-time product
  (`× 76800` at `0x5f9d86` is the pan speed; `× 131072 × 0.125` at `0x5f9ee5` is
  the
  push-back; the block at `0x5ff0ae` is rotation, with the `Shift` modifier read
  through `[0x900408]` selecting between 50 and 25). Getting these three
  frame-time products confused is easy — they sit within a few hundred bytes of
  each other and start with identical `fild`/`fmul 1e-6` pairs.

## Multiplayer Compatibility

**Safe for version mismatch.** The camera position and its pan velocity are
client-local view state and are never part of the deterministic lockstep
simulation; the patched client simply moves its own view faster. No unit
position, health or command is touched. Same classification as
[zoom speed](zoom-speed.md) and [zoom limit](zoom-limit.md).
