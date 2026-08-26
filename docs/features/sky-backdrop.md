# Sky Backdrop Extension

**Status:** Added in v0.7.0; opt-in via `sh2-unofficial-patch.ini`
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** Trampoline inside the sky draw (RVA `0x34b540`, 7 bytes)

---

## Symptom / Motivation

With [ZoomOutLimit](zoom-limit.md) set to `Auto`, a black wedge creeps into the
frame along the map's border and grows as the camera pulls back. It is not the
map running out of terrain — the terrain draws correctly right up to its edge.
It is the sky failing to cover the area next to it.

## Fix

Make the sky's backdrop quad reach the bottom of the screen instead of stopping
at the map's far edge. Because the sky pass runs **before** the terrain, the
extra area is completely hidden by terrain wherever terrain exists, and shows
sky exactly where the black used to be. `[camera] ExtendSky=1`; `0`, the
default, leaves the game code untouched.

---

## How the sky is drawn

`drawSky` (VA `0x74b110`, one caller, no vtable reference) draws two things:
the land surround (`Landscape/LandSurround.fx`, handle `landscape+0x253944`)
and then the sky (`Landscape/Sky.fx`, handle `landscape+0x253948`). The sky is
**not** a dome or a 3D backdrop — it is a screen-space textured quad submitted
through the renderer's quad batcher at VA `0x731470`:

```
drawQuad(float x, float y, float w, float h,
         float u1, float v1, float u2, float v2, void *material)
```

The batcher subtracting the D3D half-texel constant (`0.5`, `ds:0x9054a8`) from
the first two arguments is what identifies them as **pixel coordinates**. Its
arguments are built at VA `0x74b50e`–`0x74b564`, and its geometry at
`0x74b407`–`0x74b462`:

| Value | Source | Meaning |
| --- | --- | --- |
| `w` | `(float)[0xac7818] * 2.01` | viewport width × 2.01 |
| `h` | `(float)[0xac781c] * 0.32` | viewport height × **0.32** |
| `y` | `horizonY - h` | bottom pinned to the projected map edge |
| `u1`,`u2` | `[0xac783c] * (1/180)` and `+const` | horizontal scroll with camera yaw |
| `v1`,`v2` | `0.0`, `1.0` | full texture height |

`[0xac7818]`/`[0xac781c]` are the renderer's viewport width/height as **ints**
(`fild`), and `horizonY` is the projected screen Y of the map's far edge.

So the quad covers the top of the screen down to a straight horizontal line at
the map's far edge, and it is only 32% of the screen tall. The map's border is
diagonal in screen space; the quad's bottom edge is not. **The difference
between the two is the black wedge** — the terrain does not reach there and the
sky stops short of it.

### The engine already handles the *top* edge

Worth knowing before touching this code, because it looks like the obvious bug
and is not one:

```asm
74b440: fld  [ebp-0x48]     ; top = horizonY - h
74b443: fcom st(2)          ; vs 0.0
74b44a: jne  0x74b464       ; top <= 0 -> already past the screen top, draw as-is
74b44c: fld  [ebp-0x60]     ; else scale the quad up until it reaches y = 0:
74b451: fdiv st,st(3)       ;   w += w * top/h
74b453: fmul st,st(2)
74b455: faddp st(1),st
74b457: fstp [ebp-0x60]
74b45a: faddp st(1),st      ;   h += top
74b45c: fstp [ebp-0x58]
74b45f: fstp [ebp-0x48]     ;   top = 0
```

The sky always reaches the top of the screen. Only the bottom falls short.

## Patch

| | Bytes at RVA `0x34b540` |
| --- | --- |
| Before | `d9 45 a8 d9 5c 24 0c` (`fld [ebp-0x58]` / `fstp [esp+0xc]` — writes `h`) |
| After | `e9 <rel32>` + 2 × `90` |

The stub rewrites two of the nine arguments:

```asm
mov  eax, [g_skyViewportHeight]
fild dword [eax]            ; viewport height
fsub dword [ebp-0x48]       ; - quad top
fst  dword [esp+0xc]        ; h  = viewportH - top  -> bottom lands exactly at the screen edge
fdiv dword [ebp-0x58]       ; / stock h
fstp dword [esp+0x1c]       ; v2 = the same ratio
```

Notes:

- **`v2` is rescaled by the same factor the height grew by**, so texels-per-
  pixel is unchanged and the sky above the horizon is drawn *identically* to
  stock. Only the strip that used to fall short is new. Stretching the texture
  instead (leaving `v2 = 1.0`) would have moved the bright horizon band down
  behind the terrain and visibly darkened the visible sky.
- **The height is derived, never clamped**, so the quad's bottom lands exactly
  on the screen edge whether the stock quad fell short *or* already overshot —
  there is no case that leaves a gap and no case that shrinks the visible sky.
- **The viewport height is read through a pointer.** The exe is base-relocated,
  so `0xac781c` is not valid at runtime; the install captures `base + 0x6c781c`
  into a global and the stub loads it into `eax` first.
- `eax`/`edx` are dead at this site (last used for the argument-block pointer
  store at `0x74b525`); `ecx` holds the renderer for the `call` at `0x74b564`
  and is untouched. `esp` still points at the argument block, and the x87 stack
  is balanced (`fild` pushes, `fstp` pops).
- The site sits inside the effect-pass loop, so the stub runs once per pass —
  it is idempotent, since it recomputes from `[ebp-0x58]`/`[ebp-0x48]`, which
  the loop does not modify.
- The loop's back-edge targets `0x74b4f0`, well outside the overwritten range.

## Rejected approach: hiding the sky

The first attempt removed the sky pass entirely (NOP over the `call drawSky` at
VA `0x7557c4`, which the engine itself guards with a never-written render-mode
flag at `byte 0x107fbe8`). It worked, and it was the wrong fix twice over: the
black area stayed black — merely joined by the rest of the sky — and because
`drawSky` also draws the land surround, hiding it removed that as well. Kept
here as the reason the sky pass is *not* the thing to switch off.

## Multiplayer Compatibility

**Safe for version mismatch.** Rendering only; the quad is screen-space and no
simulation state is read or written.
