# No Market — Match Option

**Status**: Built, **waiting on one number**: the Market's building type id, which is captured
from a debug build (below). Until it is filled in, the rule reports itself unavailable, so it
is never advertised or offered. The lobby panel shows it as `unavailable` to everyone.
**Config**: a host [match option](match-rules.md) (`[matchrules] NoMarket`), clicked on in
the lobby panel. Rule bit `0x4`.

---

## What it does

While on, for the whole match and for every player, human or AI:

- a Market cannot be built;
- no market trade happens, including at a Market the map places itself;
- the auto-market stands down: it posts nothing, and its editor will not open.

The Carter's Post (moving goods between estates and allies) is unaffected.

---

## How

Both blocks act on lockstep commands as they **execute**, through vtable slot swaps, so every
client drops the same command on the same tick. That is the same principle as the
[match-rules commit](match-rules.md#why-a-chat-line-committing-through-lockstep).

| Command | `execute` (slot 2) | Slot VA | Block |
| --- | --- | --- | --- |
| `PlaceBuildingChore` (vtable `0x9c5ab4`) | `0x5de770` | `0x9c5abc` | drop when the type word `+0x10` is the Market's |
| `TradeChore` (vtable `0x9c6190`) | `0x5e0fa0` | `0x9c6198` | drop all |

Both are thiscall with no stack arguments, and each slot is its function's only reference.
Dropping at execute happens before placement charges anything: the stock execute calls
`0x471260` on `0x11b8c10` with `(byte +0x13, word +0x10 type, &position +0x08, byte +0x15,
byte +0x12, byte +0x14, 0)`, and that call is where the cost and validation live.

### Who produces these commands

- `PlaceBuildingChore` is built by `S2BuildingHandler` (vtable slot 6, the player's placement)
  and by `0x5a4320`, reached from `S2Governor` slots 5 and 7, which is **the AI**. So the
  placement block covers AI players too.
- `TradeChore`'s constructor (`0x5e1160`) has four callers: the Market panel's buy and sell
  buttons in `SubPanelTrading::process` (`0x61dbdf`, `0x61dcb4`) and `0x45be80`, reached from
  `S2Governor` slot 5, the **AI's** trading. The Carter's Post builds `CarterTradeChore`
  instead. The earlier note in [auto-market.md](auto-market.md) that both buildings share
  `TradeChore` is wrong.
- `0x471260` has one other caller, `0x4714f0`, with no direct callers. It is probably
  map-load placement. A Market placed that way still exists, but nothing can trade at it.

### Finding the Market's type id

The type is a small integer: placement itself singles out `0x16` and `0x46`. It is not a
constant anywhere near the `Market` class. The class registers by name through Dragonfly
(`S2BuildingClass` ctor `0x4718e0`, class object `0x12a9a18`, mesh
`meshes\buildings\market.bdg`). Two leads that did not give it:

- **The class-pointer lists at `0xaa77f4`** are several null-terminated lists
  (`[0xae5e8c] = 0xaa77f4`, `[0xae5f58] = 0xaa7810`), not one table indexed by type, so the
  market's position in them means nothing.
- **The map editor's availability array** (`BuildingAvailabilityScreen`: `0x434450` writes
  `byte [[0x12c6844] + type + 0x14]`, over the id list at `0xac4be0`) belongs to the editor.
  `[0x12c6844]` is null in a match dump. The in-game availability state, which would also grey
  the build button the stock way, has not been found.

So the id is captured at runtime instead. In a `make debug` build every executed placement
pushes `0xb0000000 | type` to the debug ring, written to `patch_debug.txt` when the game
exits. Place a Market, then a Granary as a cross-check, quit, and read the last two entries.

---

## Not done

- The build menu still offers the Market. Placing one does nothing, because the command is
  dropped. Greying the button would need the in-game availability state (above).

---

## Multiplayer Compatibility

**Requires all clients, self-enforced**, like every [match rule](match-rules.md): it is only
offered when every player in the lobby advertises it, and switched on through the host's
rules line on the same tick everywhere. The auto-market suppression is local and only
removes commands that would be dropped anyway.
