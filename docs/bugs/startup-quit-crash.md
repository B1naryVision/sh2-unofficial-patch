# Crash When the Game Closes During Startup

**Status**: Fixed in the next release (unreleased)
**Severity**: Low to medium. The crash happens on the way out, but it hides
whatever closed the game, and it leaves a WER report that looks like a game bug.
**Game version**: Steam build v1.5.0 (PE timestamp `0x5EA311C0`)

---

## Symptom

A player report, no dump:

```
Faulting module name: Stronghold2.exe, version: 1.5.0.0, time stamp: 0x5ea311c0
Exception code: 0xc0000005
Fault offset: 0x00344976
```

`0x344976` is an RVA (VA `0x744976`).

---

## Root Cause

The faulting instruction is inside a small `GlowWorm::Landscape` method at
VA `0x744960` (thiscall, no arguments) that hands the landscape's texture
handles back to the texture manager at `0xac76a8`:

```asm
00744960  push esi
00744961  push edi
00744962  mov  edi, ecx
00744964  xor  esi, esi
00744966  cmp  dword [edi+0x253900], esi   ; count
0074496c  jle  0x74499a
00744970  mov  eax, dword [edi+0x2130b8]   ; handle array
00744976  mov  ecx, dword [eax+esi*4]      ; <-- CRASH
00744979  push ecx
0074497a  mov  ecx, 0xac76a8
0074497f  call 0x733cc0                    ; release handle
...
```

The landscape is a static global at VA `0x135bdb0` (its vtable `0x9f28c4`
resolves by RTTI to `.?AVLandscape@GlowWorm@@`). Its two fields are set at
different times:

| Field | Written by | When |
| --- | --- | --- |
| count `+0x253900` | ctor, `mov [esi+0x253900], 0x29` at VA `0x755ab7` | static initialisation, before `WinMain` |
| array `+0x2130b8` | `0x756570` (allocates ten arrays, `0x75663f`) | scene setup, during the startup loading screen |

So between process start and scene setup, the landscape says it holds `0x29`
handles while its array pointer is still the zero from static storage.

**Setup and teardown.** The scene setup is `0x5f8090`, and its only caller is
`IntroScreen`'s per-frame update (vtable `0x9db8bc` slot 4 = `0x67c360`;
`IntroScreen` is the startup loading screen that shows
`ui\gold_edition\loadscreen.jpg`). Its `OnActivate` sets a countdown at
`[this+0xe0] = 10` (VA `0x67c20c`). Each frame `0x67c240` decrements it and
calls `0x5f8090` when it reaches 8 (VA `0x67c293`), so the landscape is built on
the second frame of the loading screen.

The matching teardown is `0x5f8240`. It undoes exactly what `0x5f8090` built
(`0x76c5d0`, `Landscape` `0x744960`, `Landscape` `0x756ee0` which frees the ten
arrays, three `0x759c30` releases, `0x407e30`), and its only caller is the main
loop, after the loop has exited (VA `0x430331`, after `0x430316`, the loop-back
on `EventMgr::processEvents`). The teardown runs once per process, at exit, and
never checks whether setup ran.

**The trigger.** If the main loop exits before the loading screen's second
frame, the teardown walks `0x29` entries of a null array and faults reading
address `0`. `processEvents` returns false when the window is closed, so any of
these does it:

- the player closes the window (Alt+F4, the taskbar, Steam's Stop button)
  while the game is still opening;
- something makes the game close itself during startup.

The window is a few frames long, so a player is unlikely to land in it by
quitting on purpose. A report of this offset is more likely the second case,
and the crash on exit is hiding the reason the game closed. Ask the reporter
whether the game closed by itself at launch, and whether they use
`d3d9_chain.dll` or other graphics add-ons.

**Not yet confirmed at runtime.** The analysis is static and the report had no
dump. With a dump, `eax == 0` at the fault and a zero dword at
`0x135bdb0 + 0x2130b8` (rebased) would confirm it.

---

## Fix

Retarget the main loop's call to the teardown at a stub that runs it only when
the landscape's handle array exists:

| Site | VA | Before | After |
| --- | --- | --- | --- |
| Teardown call | `0x430331` | `e8 0a 7f 1c 00` (`call 0x5f8240`) | `e8 <rel32>` (`call sceneTeardownIfBuilt`) |

The array pointer is the setup marker: its only writer is `0x756570`, its only
caller is `0x5f8090`, and it is never cleared afterwards (`0x756ee0` frees it
but leaves the pointer). When it is null, setup never ran and nothing needs to
be torn down, so the stub returns. Otherwise it tail-calls `0x5f8240`
unchanged.

Skipping the whole teardown rather than guarding the one loop also covers the
rest of `0x5f8240`, which releases objects that setup would have created. None
of it has been seen to crash, but none of it has anything to do when setup
never ran.

The install compares the stock call (its rel32 rebuilt from the two RVAs) before
writing, and does nothing if it does not match. `call` rel32 operands are not
base-relocated, so comparing them is safe.

---

## Multiplayer Compatibility

Safe for version mismatch. It only changes what happens after the main loop has
exited, when there is no session left. No simulation state is involved, so it
is not a match rule and replays are unaffected.

---

## Testing

1. Launch the game and press Alt+F4 as soon as the window appears, before the
   loading bar moves. Without the patch, Event Viewer shows an application
   error at fault offset `0x00344976`. With it, the process exits cleanly.
2. Launch normally, reach the main menu, quit. The teardown still runs
   (unchanged shutdown).
