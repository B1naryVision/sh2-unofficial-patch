# Camera Zoom Speed (configurable multiplier)

**Status:** Added in v0.5.0 (offsets confirmed from two process dumps + static analysis); opt-in via `sh2-unofficial-patch.ini`
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** In-place byte rewrite (RVA `0x3623D8`, 15 bytes) — no trampoline

---

## Symptom / Motivation

The mouse-wheel camera zoom moves in very small increments per frame. On a
high-resolution monitor (2560×1440) it feels sluggish — it takes many wheel
notches / a long hold to move between the closest and farthest zoom. The zoom
speed is hardcoded in the binary; there is no in-game option or config-file
value for it (`config.dat`, `Stronghold2.GraphicsSettings.xml`, and the game's
XML config files contain no zoom or camera-speed entry).

## Fix

Multiply the per-frame zoom **delta** just before it is applied to the camera
distance. This scales every zoom trigger (mouse wheel, keyboard, and edge zoom)
by the same factor, while all existing min/max clamps still apply.

The factor comes from `[camera] ZoomSpeedMultiplier` in
`sh2-unofficial-patch.ini` (see [configuration.md](configuration.md)),
accepted range 0.1–10.0. The default is **1.0, which leaves the game code
completely untouched** — an earlier hardcoded 3× version shipped enabled and
was judged too fast, so the feature is now strictly opt-in.

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

The redundant `fstp [ebp+8]; fld [ebp+8]` round-trip frees enough room for an
`fmul` against a `float` in the patch DLL's data section — still an in-place
15-byte rewrite, no code cave and no trampoline:

```asm
; RVA 0x3623d8, patched 15 bytes:
d9 45 08        fld   [ebp+8]           ; st0 = delta
d8 0d xx xx xx xx  fmul dword [g_zoomMultiplier]  ; st0 = mult*delta
d8 41 20        fadd  [ecx+0x20]        ; st0 = mult*delta + distance
d9 51 20        fst   [ecx+0x20]        ; distance = mult*delta + distance
```

| | Bytes |
| --- | --- |
| Before | `d9 45 08 d8 41 20 d9 5d 08 d9 45 08 d9 51 20` |
| After  | `d9 45 08 d8 0d <abs32> d8 41 20 d9 51 20` |

The `fmul` operand is the **absolute address** of `g_zoomMultiplier`, a static
`float` in the patch DLL, written into the patch bytes at install time. The
DLL is never unloaded (it is the game's `d3d9.dll`), so the address stays
valid for the process lifetime. The multiplier is written once before the
patch is applied and never changes afterwards, so there is no tearing risk.

An earlier version of this patch built a fixed 3× by repeated `fadd`
(`fadd st,st(0)` + `fadd [ebp+8]`); the `fmul` form replaced it to allow
arbitrary user-configured factors.

Stack contract is preserved: on entry the x87 stack is empty; on exit `st0` holds
the new distance (as the original `fst` left it) for the subsequent min/max clamp
code at RVA `0x3623e7`. The original store-back to `[ebp+8]` is dropped —
`[ebp+8]` is not read again after this block. No branch inside the function
targets the middle of the rewritten range. The installer verifies the 15 stock
bytes are present before patching, so a future game build that relocates this
code is left untouched rather than corrupted.

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
