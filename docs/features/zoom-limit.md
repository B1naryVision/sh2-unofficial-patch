# Camera Zoom-Out Limit (measured, pitch-aware)

**Status:** Added in v0.7.0 (offsets from static analysis of the camera class; builds on the [zoom speed](zoom-speed.md) investigation); playtest-confirmed; opt-in via `sh2-unofficial-patch.ini`
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** Trampoline on the camera update prologue (RVA `0x362480`, 6 bytes)

---

## Symptom / Motivation

The camera stops zooming out well before it shows a useful amount of the map —
on a widescreen monitor the maximum view is barely more than a couple of
building plots. The limit is a per-camera field, not a config value; the game
exposes no way to change it.

## Fix

Rewrite the camera's own maximum-distance field (`camera + 0x4c`) from the
camera's per-frame update, so that **every** engine path reading the limit sees
the same raised value. `[camera] ZoomOutLimit=Auto` (see
[configuration.md](configuration.md)); `Vanilla`, the default, leaves the game
code completely untouched.

The limit is **measured, not configured**. There is no multiplier and no margin
to tune: it comes from two process dumps taken at the furthest point the view
still holds up, interpolated by the camera's pitch and scaled to the map being
played.

### The calibration

Two dumps, same 255-tile map, 2560×1440, camera zoomed out until the view broke
down — once looking straight down, once at the shallow default pitch:

| | `+0x18` pitch | `+0x20` distance | `+0x48` min distance |
| --- | --- | --- | --- |
| `Stronghold2TopDownCap.DMP` | 90.0° | **234392.90625** | 16000 |
| `Stronghold2AngledCap.DMP` | 36.0° | **144000.0** | 7000 |

Both dumps read map bounds `+0x5c`…`+0x68` of 1024…261120 — a limiting extent
of **260096** world units — and a far distance (`renderer+0x184`) of 240000.

Note the engine varies the *minimum* distance with pitch too (16000 vs 7000),
which is independent confirmation that distance limits in this camera are
pitch-dependent by design rather than by accident.

### Deriving the limit

The limiting extent is `min(mapWidth, mapDepth × aspect)`, because the ground
width visible at distance `d` is exactly `d` — the projection is built with
`D3DXMatrixPerspectiveLH(w = 1024, zNear = 1024)`, so `tan(halfFOV) = 0.5`
horizontally (see [far-plane.md](far-plane.md)) — and the visible depth is that
divided by the viewport aspect. Whichever runs out first brings the map's edge
into view. On the calibration map that expression yields 260096, which is what
the two measurements are expressed against.

```cpp
t     = clamp((pitch - 36) / (90 - 36), 0, 1);
limit = lerp(144000, 234392.90625, t) * (extent / 260096);
limit = max(limit, engineInstalledLimit);
```

The camera pointer, viewport ints (RVA `0x6c7818`/`0x6c781c`) and map bounds all
come from the same places the [sky backdrop](sky-backdrop.md) patch reads.

### Caveat on the top-down figure

The 234392.9 measurement was taken **with the far-plane patch inactive** — it
had a byte-check bug (see [far-plane.md](far-plane.md)) and had never installed.
The runtime far distance in both dumps is 240000, and 234392.9 sits just under
it, so that capture is almost certainly the far plane clipping the ground rather
than the map's edge coming into view. Re-measuring top-down with the far-plane
fix live would likely support a larger figure. The angled capture at 144000 is
well clear of 240000 and is not suspect.

---

---

## How the limit works in the engine

The active game camera is the static global at RVA `0x6e5a70`
(VA `0xae5a70`), a `GlowWorm::Camera` (vtable RVA `0x5f3244`). Relevant fields:

| Offset | Meaning |
| --- | --- |
| `+0x20` | current zoom distance |
| `+0x38`, `+0x3c` | pitch ramp span and low reference distance |
| `+0x43`, `+0x44` | pitch-ramp enable flags |
| `+0x48` | min distance clamp |
| `+0x4c` | max distance clamp |
| `+0x54`, `+0x58` | min / max pitch angle |
| `+0x5c`…`+0x68` | map pan bounds |

The limits are installed by `Camera::setLimits` (VA `0x762880`, 10 float args →
`+0x48`…`+0x68`), called on the game camera from four sites in the camera setup
code (`0x5f79fe`, `0x5f7b46`, `0x5f7c1c`, `0x5f7d0e`) plus `0x762e6e`. The
in-game site `0x5f79fe` passes min `7000.0` (`ds:0x9c80c4`) and max `80000.0`
(`ds:0x99e9dc`); the value observed in a live session was `75000.0`, so a later
writer adjusts it — which is exactly why the patch **scales whatever is in the
field** rather than writing an absolute number or assuming a stock constant.

The distance clamp itself lives only in the zoom function `CameraZoom`
(VA `0x7623c0`):

```asm
762408: d9 41 4c        fld   [ecx+0x4c]      ; st0 = max, st1 = v
76240b: d8 d9           fcomp st(1)           ; compare max, v ; pop
76240d: df e0           fnstsw ax
76240f: f6 c4 05        test  ah,5
762412: 7a 0b           jp    0x76241f        ; max >= v -> nothing to do
762414: d8 69 4c        fsubr [ecx+0x4c]      ; st0 = max - v
762417: d8 41 20        fadd  [ecx+0x20]
76241a: d9 59 20        fstp  [ecx+0x20]      ; distance += max - v
```

`v` is the reference distance, not necessarily the new one: the minimum clamp
just above stores the corrected distance and leaves the reference on the x87
stack, so both clamps are written as corrections applied to `+0x20`.

## Patch

Hook the prologue of the camera update (VA `0x762480`), which runs once per
frame with the camera in `ecx`:

| | Bytes at RVA `0x362480` |
| --- | --- |
| Before | `55 8b ec 83 ec 08` (`push ebp; mov ebp,esp; sub esp,8`) |
| After | `e9 <rel32>` + `90` |

The stub brackets the callback in `pushal`/`pushfl`, re-emits the three stock
instructions and resumes at `+6`. The callback rewrites the field, and also
pulls the current distance in when the limit drops:

```cpp
if (camera != gameCamera) return;
if (*maxDistance != lastWrittenBits) stockMax = *maxDistance;  // engine installed one
else if (pitchBits == lastPitchBits)  return;                  // integer fast path
*maxDistance = bits(max(limitFor(pitch, extent), stockMax));
if (*distance > limit) *distance = limit;
```

The distance clamp matters because the engine only clamps distance when the
player *zooms*: tilting towards the horizon lowers the limit, and without this
the camera would sit parked past it until the next scroll of the wheel.

Notes on why it is shaped this way:

- **A function prologue is where the float multiply is legal.** The x87 stack is
  empty by calling convention there, so no `fxsave` bracketing is needed (see
  CLAUDE.md); a frame-tick or render callback could not do this arithmetic.
- **Two integer comparisons are the whole state machine.** The field not
  matching what we last wrote means the engine has installed its own limit, so
  that value is captured as the floor and the limit is recomputed; otherwise the
  pitch bits are compared and the callback returns unless the player has tilted.
  `setLimits` writes the distance limits and the map bounds in one call, so the
  bounds cannot change without tripping the first comparison, and nothing has to
  be remembered across maps.
- **The floor is the engine's own limit**, so a map small enough that the
  computed limit falls below vanilla still zooms out as far as it did before.
- **The camera pointer is checked**, so other cameras the same update serves are
  left alone.
- The installer verifies the 6 stock bytes before patching, so a future game
  build that relocates this code is left untouched rather than corrupted.

### Earlier approach (replaced): rewriting the clamp

The first version left the field alone and instead replaced the 23-byte max
clamp at `0x762408` with a stub comparing against `[ecx+0x4c] * multiplier`.
It worked for zooming — and then **the camera snapped back to the stock limit
as soon as the view was moved**, because some other engine path re-clamps the
distance against the unscaled field. Scaling the field itself is what makes all
of those paths agree, and it is also less code. Recorded here because the shape
of the failure ("the feature works until anything else touches the same state")
is the general argument for patching the *data* an engine shares rather than
the one *reader* of it you happened to find.

## Note on the pitch ramp

`CameraZoom` ends with a block that drives the camera pitch from the distance:

```text
t = (distance - [ecx+0x3c]) / ([ecx+0x4c] - [ecx+0x3c])
if (t > 0) [ecx+0x18] = t * [ecx+0x38] + [ecx+0x54]
```

It is gated on `[ecx+0x44]` and `[ecx+0x43]` (VA `0x762430`/`0x76243a`), and
`setLimits` only sets `+0x44` when its last argument equals the max distance —
which the in-game call site (`0x5f79fe`, last argument `0.0`) does not do, so
the ramp does not run for the game camera. Scaling `+0x4c` keeps `t` inside
`[0,1]` in any case, so wherever the ramp *is* active it stays proportional
rather than extrapolating past its end point.

## Investigation dead-ends worth knowing

- **The `75000.0` literal (VA `0x9a2228`) is not the camera's limit.** Its only
  two xrefs are a constructor at `0x497b63` that stores it into `[esi+0x40]` of
  a different class, and an unrelated `fmul` at `0x5f7c79`.
- **`Camera` VA `0x761760` looks like a limits setter and is not** — it takes
  three floats and writes `+0x20` (distance) and `+0x18`/`+0x1c` (angles); it is
  `setView`.
- **Panning cannot be what re-clamps the zoom.** `Camera::pan` (VA `0x761f30`,
  4 call sites in the input dispatcher) only moves `+0xc`/`+0x14` and clamps
  them to the map bounds at `+0x5c`…`+0x68`; it never touches `+0x20`. Tilting
  (`0x762250`) *does* recompute the distance from the new geometry, and rotating
  (`0x7620a0`, `0x762120`) does not.
- Searching for x87 stores to `[reg+0x4c]` across the exe is dominated by
  **4×4 matrix writers** (`+0x40`…`+0x54` written in sequence); filter them out
  by checking for an adjacent `+0x50` store.
- A useful sweep for "which camera methods can change field X": collect every
  `mov ecx, 0xae5a70` site, take the first `call` after each, and scan each
  unique target's body for stores to the offset. That enumerated the camera's
  whole per-input API (pan, tilt, two rotates, zoom, setView, setPosition,
  setState, setLimits, update) in one pass.

## Multiplayer Compatibility

**Safe for version mismatch.** The camera zoom distance and its limits are
client-local view state and are never part of the deterministic lockstep
simulation; the patched client simply renders its own view from further away.
No unit position, health or command is touched. Same classification as
[zoom speed](zoom-speed.md).
