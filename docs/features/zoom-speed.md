# Camera Zoom Speed (3×)

**Status:** Implemented (offsets confirmed from two process dumps + static analysis)
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** In-place byte rewrite (RVA `0x3623D8`, 15 bytes) — no trampoline

---

## Symptom / Motivation

The mouse-wheel camera zoom moves in very small increments per frame. On a
high-resolution monitor (2560×1440) it feels sluggish — it takes many wheel
notches / a long hold to move between the closest and farthest zoom. The zoom
speed is hardcoded in the binary; there is no in-game option or config-file
value for it (`config.dat`, `Stronghold2.GraphicsSettings.xml`, and the game's
XML config files contain no zoom/scroll entry).

## Fix

Triple the per-frame zoom **delta** just before it is applied to the camera
distance. This makes every zoom trigger (mouse wheel, keyboard, and edge zoom)
move three times as fast, while all existing min/max clamps still apply.

---

## How zoom works in the engine

The active game camera is a **static global** at RVA `0x6e5a70`
(VA `0x400000 + 0x6e5a70 = 0xae5a70`; in the analysed run it loaded at
`0xe45a70`). It derives from `GlowWorm::Camera` (vtable RVA `0x5f3244`). Relevant
fields (offsets from the camera object):

| Offset | Meaning |
| --- | --- |
| `+0x20` | current zoom **distance** (the value the wheel changes) |
| `+0xd4` | mirror copy of the distance in the second camera state block |
| `+0x48` | min distance clamp (≈ 7000 in the analysed session) |
| `+0x4c` | max distance clamp (75000) |

The two process dumps (fully zoomed out vs. fully zoomed in) showed the distance
at `+0x20`/`+0xd4` change from `75000.0` → `21903.3`, and **only** those two
fields changed — confirming `+0x20` is the zoom distance.

### The zoom function — RVA `0x3623c0` (`thiscall`, `ecx` = camera)

```
CameraZoom(float delta /*[ebp+8]*/, bool doClamp /*[ebp+0xc]*/, float ref /*[ebp+0x10]*/)
```

It is called from 6 sites inside the camera input dispatcher (RVA
`0x1f99xx`–`0x1fa1xx`), each of which sets `ecx = 0xae5a70` and pushes a
time-scaled `delta` (`fild [frame timer]` × a speed constant → smooth,
framerate-independent zoom). The core of the function:

```asm
; RVA 0x3623d8 (VA 0x8623d8), original 15 bytes:
d9 45 08        fld   [ebp+8]        ; st0 = delta
d8 41 20        fadd  [ecx+0x20]     ; st0 = delta + distance
d9 5d 08        fstp  [ebp+8]        ; (redundant round-trip)
d9 45 08        fld   [ebp+8]
d9 51 20        fst   [ecx+0x20]     ; distance = delta + distance
```

## Patch bytes

The redundant `fstp [ebp+8]; fld [ebp+8]` round-trip frees enough room to build
`3*delta` by repeated addition, with no code cave and no data constant. Because
the original store-back to `[ebp+8]` is removed, `[ebp+8]` still holds the
untouched `delta` and can be added a second time:

```asm
; RVA 0x3623d8, patched 15 bytes:
d9 45 08        fld   [ebp+8]        ; st0 = delta
d8 c0           fadd  st, st(0)      ; st0 = 2*delta
d8 45 08        fadd  [ebp+8]        ; st0 = 3*delta
d8 41 20        fadd  [ecx+0x20]     ; st0 = 3*delta + distance
d9 51 20        fst   [ecx+0x20]     ; distance = 3*delta + distance
90              nop                  ; pad to 15 bytes
```

| | Bytes |
| --- | --- |
| Before | `d9 45 08 d8 41 20 d9 5d 08 d9 45 08 d9 51 20` |
| After  | `d9 45 08 d8 c0 d8 45 08 d8 41 20 d9 51 20 90` |

Stack contract is preserved: on entry the x87 stack is empty; on exit `st0` holds
the new distance (as the original `fst` left it) for the subsequent min/max clamp
code at RVA `0x3623e7`. `[ebp+8]` is read twice here and not again after this
block. No branch inside the function targets the middle of the rewritten range.
The installer verifies the 15 stock bytes are present before patching, so a
future game build that relocates this code is left untouched rather than
corrupted.

### Tuning the factor

The multiplier is built from x87 `fadd`s (`×2` = `fadd st,st(0)`; `×3` = a further
`fadd [ebp+8]`), so any small integer factor is a self-contained edit needing no
data constant. A **non-integer** factor (e.g. ×2.5) would require multiplying by a
`float` stored somewhere the exe can reach (a data cave or the DLL) via `fmul`,
i.e. a trampoline rather than this in-place edit.

---

## Investigation dead-ends worth knowing

- **`TUTORSTATE_USING_CAMERA_ZOOM`** (string VA `0x9d2dd8`) has a single XREF,
  and it is the tutorial-state *registration* table — not the zoom logic (the
  usual "string XREF → registrar" trap).
- **`75000.0`** (the max, VA `0x9a2228`) has one static occurrence, used only as
  a constructor default in an unrelated object; the runtime clamp reads the
  per-camera field `+0x4c`, so there is no literal-`75000` reference in the zoom
  path.
- A read-modify-write of a `+0xd4` field at RVA `0xe18e1` looked like the zoom
  updater but is an **angle wrap to [0,360)** on a different object — `+0xd4`
  is a common offset; the collision was coincidental.

## Multiplayer Compatibility

**Safe for version mismatch.** The camera zoom distance is client-local view
state; it is never part of the deterministic lockstep simulation. A patched
client simply renders its own view at a different distance. Nothing about unit
positions, health, or commands changes, so a patched and an unpatched client
stay in sync.
