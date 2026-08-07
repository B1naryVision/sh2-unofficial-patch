# Auto-Market (QoL feature)

Keeps each good's stock inside a `[min, max]` band by posting the Market's own
buy/sell commands at the good's real market price — the same result as a player
clicking Buy/Sell in the trade window, but automated. Thresholds are set through
an **in-game editor** (toggled with a hotkey), not the ini, because what needs
buying/selling changes throughout a match; they reset every game. The only ini
key is the toggle hotkey — see [configuration.md](configuration.md).

The feature has two halves: the **trade engine** (frame-tick evaluator that posts
`TradeChore` commands, below) and the **editor overlay** (a Direct3D-drawn panel,
further below).

## Market vs Carter's Post

The **Market** (on-demand buy/sell for gold) and the **Carter's Post / Trader**
(carters moving goods between estates and allies) are different buildings:

- Market UI = **`SubPanelTrading`** (a command sub-panel). ← the feature target
- Carter's Post UI = `SubPanelCarterPost` / `TraderScreen`.

Both build the shared `TradeChore` command class; this feature drives the
*Market* path only.

## Trade command path

All RVAs (VA = RVA + 0x400000). `SubPanelTrading::process` (vtable slot 0) =
RVA `0x21da50` is a jump table over button subids; the buy and sell buttons
build and dispatch a **`TradeChore`**:

| Element | Address / value | Notes |
| --- | --- | --- |
| BUY click handler | RVA `0x21db98` | sets event `+0x14 = 1` |
| SELL click handler | RVA `0x21dc6d` | sets event `+0x15 = 1` |
| `TradeChore` vtable | RVA `0x5c6190` | name string `"TradeChore"` follows vtable |
| `TradeChore::ctor` | RVA `0x1e1160` | thiscall; writes vtable, zeroes fields |
| Event size | `0x1c` bytes (+ 8-byte refcount header **before** the pointer) | refcount at `event-8` |
| Refcounting allocator | IAT `[0x900980]`, ctx `[0x90097c]` | **must** use this, not DLL `operator new` |
| Handle wrap / refcount++ | RVA `0x29e2c0` (ret 8) and `0x210d60` (ret 4) | `lock xadd [event-8]` |
| Handle release / refcount-- | RVA `0x19e9f0` | frees at 0 |
| World singleton | global RVA `0xd781d0` | same singleton shift-recruit uses |
| `getPlayer(World)` | RVA `0x11360` | returns local player; `player+0x8` = player id |
| **Dispatch** `World::vtbl[0x20]` | RVA `0x37efb0` | takes raw `Event*` (ret 4); enqueues to the networked command mgr |

### TradeChore field layout

| Offset | Field | Value posted |
| --- | --- | --- |
| `+0x00` / `+0x04` | vtables | ctor |
| `+0x08` | good id | the good being traded |
| `+0x0c` | amount | units to trade |
| `+0x10` | unit price | the good's real market price (see below) |
| `+0x14` | buy flag (byte) | 1 for buy |
| `+0x15` | sell flag (byte) | 1 for sell |
| `+0x18` | player id | `player+0x8` |

### Construction sequence (timer-driven, no live panel)

The auto path has no open panel to re-invoke, so it replicates the handler's own
build+dispatch sequence with the game's functions:

1. `player = getPlayer(World)`; bail if null.
2. `event = alloc([0x90097c], 0x1c)` — game refcounting allocator.
3. `TradeChore::ctor(event)` (`0x1e1160`).
4. Wrap in a local Handle (`0x29e2c0`) so the refcount is valid.
5. Fill good id / amount / unit price / buy or sell flag / player id.
6. Dispatch `World::vtbl[0x20](event)` (`0x37efb0`).
7. Release the local Handle (`0x19e9f0`); the command queue keeps its own ref.

The simulation re-validates each `TradeChore` (gold/stock) and drops what is
unaffordable, so overshooting a threshold is safe — the same graceful-degradation
model as shift-click recruitment.

The threshold engine runs on `registerFrameTick` (sim thread), throttled to a
fixed 30 frames (not user-configurable): for each active good it reads stock and
posts a buy toward `min` when below or a sell toward `max` when above. Float-free
on the tick path.

**Posting is closed-loop.** After posting for a good, it waits for that good's
stock to *move* (the command landed) before posting again, with only a long
frame timeout as a fallback retry. This matters under lag: when the game stalls
("Waiting for Players") the frame tick keeps firing but the simulation is frozen,
so stock never moves and commands don't execute. Open-loop posting would re-post
the same deficit every tick and then execute all of them at once when sync
resumed — badly overbuying (e.g. a min-5 target buying 35). The stock-move gate
posts at most one command per good across the whole stall.

## Per-player goods stock

`stock(goodId) = *(int *)(player + 0xf5c + goodId*4)` (int per good). Local
player pointer = global RVA `0x6e8c60`; gold (float) at `player + 0x1010`.

Good-id → name: 1 Wood, 2 Stone, 3 Iron, 4 Wheat, 5 Flour, 6 Hops, 7 Ale,
9 Pitch, 10 Candles, 11 Wool, 12 Cloth, 14 Eels, 15 Geese, 17 Pigs,
18 Vegetables, 19 Wine, 22 Apples, 23 Bread, 24 Cheese, 25 Meat, 30 Bows,
31 Crossbows, 32 Swords, 33 Maces, 34 Pikes, 35 Spears, 36 Armour,
37 Leather Armour. Ids 8/13/16/20/21/26–29 are unnamed (unused or non-market).

## Market prices

Each trade posts the good's **current market unit price**, read live from the
Market good-definition table at RVA `0x6bcfd8` (30 entries, stride `0x28`, **not**
in good-id order — searched by `entry[0] == goodId`). Within an entry: **sell
unit price at `+0x20`, buy unit price at `+0x24`**. These are exactly the values
the trade window displays, so gold settles identically to a manual trade
(e.g. Wood buy 3 / sell 1, Iron 45 / 25, Swords 58 / 30). A good with no valid
price entry is skipped.

## In-game editor overlay

The editor is a panel drawn over the game and edited with mouse/keyboard. It is
purely client-local UI — it never touches simulation state except by calling the
same threshold setters the trade engine reads, so it has no multiplayer effect.

**Getting a render hook (`src/core/d3dHook.cpp`).** The patch DLL *is* the d3d9
proxy, so its `Direct3DCreate9` wrapper hands the real `IDirect3D9` to
`installD3DHook`, which detours `IDirect3D9::CreateDevice` (vtable slot 16) to
capture the device. The obvious next step — patching the device's `EndScene`
vtable slot — **does not work**: Stronghold's `dxrenderer.dll` copies the device
vtable into private heap memory and inline-hooks d3d9's `Present`/`Reset`, and
the game renders through that copy, so our slot patch is never called. `EndScene`
itself is left un-hooked, so instead we install an **inline trampoline on the
real `d3d9.dll` `EndScene` function** (its address read from `vtable[42]`): a
7-byte position-independent SEH prologue (`push 0x14; mov eax, imm32`) is copied
to an allocated trampoline, and a `jmp` to a naked detour is written over it. The
detour saves state, calls the registered render callbacks with the device (scene
still open, so drawing is valid), then jumps to the trampoline. Render callbacks
must be **float-free** (the interrupted `EndScene` may hold live x87 state); the
overlay uses only compile-time-constant vertex coordinates to satisfy this.

**Drawing (`src/patches/autoMarket/overlay.cpp`).** The panel is rendered with
GDI into a 32-bit top-down DIB (same text-drawing approach as the endgame-stats
overlay), uploaded to a `D3DPOOL_MANAGED` texture (survives device reset), and
drawn as one alpha-blended pre-transformed quad. It is re-rendered only when a
dirty flag is set (toggle, selection or value change). Goods are grouped under
category headers; each Min/Max is a cell.

That plumbing now lives in `src/core/overlayPanel.cpp`, shared with the settings
overlay — this file keeps only the layout and the editing logic. The quad's
vertices are built at install time by `overlayPanelInit`, which is what keeps the
render path free of float arithmetic; see
[settings-overlay.md](settings-overlay.md).

**Input.** The overlay subclasses the game window's `WndProc`. The toggle hotkey
shows/hides it; while open it routes keys (arrows/Tab to move, digits to type,
Backspace/Delete, Esc) and mouse clicks (hit-tested to a cell, mapped through the
backbuffer size so it works windowed) to the editor and **swallows** them so the
game never sees them. Selecting a cell (click or arrow) starts "fresh entry" so
the next digit replaces the value. Thresholds are cleared and the panel hidden on
`MainMenuScreen::OnActivate` (reusing the endgame-stats return-to-menu hook).

**Presets.** `[preset:NAME]` ini sections define named threshold sets, loaded at
install into a fixed table (good names matched case-insensitively to the good
list). The editor shows a picklist bar (`‹ name ›  [Apply]`, cycled with
PageUp/PageDown or the arrows, loaded with Enter/click); applying is replace-all
— every threshold is cleared, then the preset's entries set. Presets are static
config and persist across games; only the live thresholds reset on return to menu.

## Multiplayer compatibility

**Safe for version mismatch.** The dispatch `World::vtbl[0x20]` (`0x37efb0`)
hands the `TradeChore` to the networked command manager at VA `0x1be6a30`
(via `0x767050`) — the same sync-checked command path a manual trade click uses.
The trade is therefore a normal lockstep command: broadcast with an execution
tick and executed identically on every client, exactly like the recruit /
shift-recruit path. Run on the sim thread via the frame-tick dispatcher to avoid
a client-local thread race. The editor overlay is client-local UI and has no
multiplayer effect.
