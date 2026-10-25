# D3D9 Chain-Loading (DXVK, ReShade, dgVoodoo)

**Status:** Unreleased  
**Affected version:** Stronghold 2 Steam v1.5.0  
**Patch type:** Proxy loader change + `EndScene` hook fallback (no exe bytes touched)

---

## Symptom

The patch is delivered as `d3d9.dll`, and so is every D3D9 wrapper players use
with this game: DXVK (D3D9 → Vulkan, a real performance win on modern GPUs),
ReShade, dgVoodoo, WineD3D builds for Windows. Only one file can have that name,
so players had to choose between the patch and their wrapper.

## Usage

Rename the wrapper's `d3d9.dll` to `d3d9_chain.dll` and put it in the game
directory next to the patch's `d3d9.dll`. Nothing else to configure; without
the file the patch behaves exactly as before.

To remove the patch but keep the wrapper, delete the patch's `d3d9.dll` and
rename `d3d9_chain.dll` back to `d3d9.dll`.

---

## Implementation

### Loading (`src/proxy/d3d9Proxy.cpp`)

`loadRealD3d9Dll` (run from `DllMain`) looks for `d3d9_chain.dll` in the
directory of the patch DLL itself (`GetModuleFileNameA(g_patchModule)`, the same
directory the ini is read from) and loads it by full path. If the file exists
but fails to load, a message box says so and the system library is used.

Export resolution is **two-pass**: every name is looked up in the chained DLL
first, and the system `d3d9.dll` is loaded only if one or more names are
missing, to fill exactly those slots. This matters because every proxied export
except `Direct3DCreate9` is a naked `jmp *slot` — an unresolved name would be a
jump to address 0, not a graceful failure. Wrappers implement what games call
and may lack the rarely-used ones (`PSGPError`, `PSGPSampleTexture`,
`DebugSetMute`, `Direct3DShaderValidatorCreate9`). Stubs are not an option: a
`__stdcall` stub must pop the right number of argument bytes and those
signatures are undocumented.

With no chain file the behaviour is identical to before: the chained pass is
skipped, every slot is missing, and all thirteen resolve from System32.

### Overlay hook (`src/core/d3dHook.cpp`)

The overlay panels (settings, endgame stats, auto-market editor) draw from a
hook on `IDirect3DDevice9::EndScene`. The game's `dxrenderer.dll` copies the
device vtable into private memory, which is why the stock path inline-patches
the `EndScene` *function* instead. That inline hook verifies Microsoft's SEH
prologue (`6a 14 b8 …`) and refuses anything else — correctly, since writing
five bytes over an unknown prologue is a crash — so under a wrapper it never
installs.

Fallback: when the prologue check fails, `hookCreateDevice` swaps vtable slot
42 (`EndScene`) on the returned device **before `CreateDevice` returns** to
the game. dxrenderer can only copy the vtable after it has the device, so its
copy should carry our pointer. The same naked detour serves both routes;
`g_endSceneTramp` holds the inline trampoline in one case and the wrapper's
original `EndScene` in the other.

**Untested assumption:** that dxrenderer copies the vtable after `CreateDevice`
returns, rather than reading a vtable obtained some other way. If panels do not
draw under DXVK, this is the first thing to check.

If neither route installs, the panels are simply absent: each panel subclasses
the game window from its first render callback, so with no render hook nothing
subclasses the window and **no input is swallowed**. The stop-troops and
attack-move hotkeys (frame-tick polling) are unaffected either way.

### Not affected

Every other patch writes to `Stronghold2.exe` (or granny2.dll) and does not
care which `d3d9.dll` sits underneath.

---

## Multiplayer Compatibility

**Safe for version mismatch.** Chain-loading only decides which D3D9
implementation renders the local client's frames; no simulation state is
touched, and the overlay hook is render-side only.

---

## Investigation notes

- A copy of the patch itself renamed to `d3d9_chain.dll` would load a second
  instance that chains to itself. Not guarded; nobody has a reason to do it.
