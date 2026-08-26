# Far Clip Plane (configurable multiplier)

**Status:** Added in v0.7.0; opt-in via `sh2-unofficial-patch.ini`
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** Two trampolines on the far-distance loads (RVA `0x1fa5e7`, `0x335dee`, 6 bytes each)

---

## Symptom / Motivation

With [ZoomOutLimit](zoom-limit.md) set to `Auto`, pulling the camera all the way
back makes the picture go completely black — not a missing backdrop, but
*nothing rendering at all* — and it comes back as soon as the camera moves in
again. It is most easily reproduced with the view tilted straight down, which
puts the whole map at its greatest distance from the camera at once.

## Fix

Multiply the far clip plane the projection matrix is built with, via
`[camera] FarPlaneMultiplier` (range 1.0–10.0; `1.0`, the default, leaves the
game code untouched). Raise it until the blackout stops: a large map's
whole-map zoom distance needs a good deal more than the stock 120000.

---

## How the far plane is set

The projection is built by a renderer method at VA `0x72d7a0`:

```
D3DXMatrixPerspectiveLH(&m, w = 1024.0, h = (viewport<<10)/aspect,
                        zNear = [ds:0x905480] = 1024.0,
                        zFar  = <caller-supplied>)
```

`zFar` comes from the renderer's own far-distance field — the global float at
VA `0xac782c`, which is renderer `+0x184` (the renderer object is the static
global `0xac76a8`). That field is produced by the view-distance setter at
VA `0x72d500`:

```asm
72d503: fld  [ebp+8]        ; requested view distance, 0..1
72d506: fst  [ecx+4]        ; kept as-is for other users
72d509: fld  [0x9c8200]     ; 120000.0
72d512: fld1
72d514: fcomp st(1)
72d51b: jne  0x72d53f       ; requested >= 1.0 -> far = 120000.0  (hard cap)
72d51d: fmul [0x9054a8]     ; 0.5
72d523: fmul [0x9c81f8]     ; 120000.0
72d529: fadd [0x99e6e0]     ; 60000.0
                            ; -> far = 60000 + 60000*requested
```

So the game's graphics view-distance option is a 0–1 slider mapped onto
**60000–120000 world units, hard-capped at 120000**. Stock maximum zoom
distance is 75000, comfortably inside it; a raised `ZoomOutLimit` is what
lets the camera cross it. The same 60000/120000 constants appear a second time
at VA `0x5fa583`–`0x5fa5b3`, so raising them in `.rdata` would have to be done
in two places and would still leave the cap semantics intact — scaling the
value on the way into the projection is both smaller and unambiguous.

## Patch

Two sites load that field to hand it to `setPerspective`, and each is a 6-byte
`fld` whose result is stored a few bytes later:

| Site | Before | Meaning |
| --- | --- | --- |
| RVA `0x1fa5e7` | `d9 05 2c 78 ac 00` | `fld [0xac782c]` — camera-side path |
| RVA `0x335dee` | `d9 86 84 01 00 00` | `fld [esi+0x184]` — renderer-side path |

Each becomes `e9 <rel32>` + one `90`, jumping to a stub that reloads the value
and scales it:

```asm
fld  dword [<reg>+0x184]
fmul dword [g_farPlaneMultiplier]
jmp  <site+6>
```

Notes:

- **Neither stub needs the absolute address.** The exe is base-relocated, so
  the on-disk `0xac782c` operand is stale at runtime — but at the camera site
  the stock code has just executed `mov ecx, <renderer>` (five bytes earlier),
  and at the renderer site `esi` is the renderer. Both stubs go through the
  register instead, which is position-independent and needs no captured global.
  The install verifies the `mov ecx, imm32` **opcode** at `site-5` for exactly
  this reason: the immediate itself cannot be compared against a disk value.
- Installation is all-or-nothing across both sites, so a build where one
  projection path has moved cannot end up with one scaled and the other not.
- The x87 stack is left exactly as the replaced `fld` left it — one value
  pushed, which the instruction after the site stores.
- **Depth precision cost.** `zNear` stays 1024, so the depth ratio grows from
  ~117 to ~470 at 4×. That is still well within a 24-bit depth buffer, but it
  is the reason the multiplier is capped at 10 rather than left open.

## Install bug worth remembering (fixed)

The first version of this patch verified both absolute sites with a literal
byte array containing the **on-disk** operand `2c 78 ac 00`. The loader
base-relocates that immediate — a process dump taken with the exe at `0x780000`
showed `d9 05 2c 78 e4 00`, i.e. `0xac782c + 0x380000` — so the `memcmp` never
matched and `installFarPlane` returned early on every launch. The patch looked
installed, the ini value looked applied, and nothing happened; the symptom was
"raising `FarPlaneMultiplier` changes nothing", which is indistinguishable from
"the far plane is not the cause".

Both absolute sites are now verified by opcode plus a **rebuilt** operand
(`base + 0x6c782c`), and the `mov ecx` guard checks its immediate against
`base + 0x6c76a8` the same way. The general rule, already in CLAUDE.md for
`mov ecx, imm32`, applies to **every** instruction carrying an absolute address:
never compare a relocated immediate against disk bytes — rebuild it from the
runtime base. Verifying an install against a process dump (`site[0] == 0xe9`)
is the cheap way to catch this class of failure.

## Multiplayer Compatibility

**Safe for version mismatch.** The projection matrix is client-local rendering
state; no simulation state is read or written.
