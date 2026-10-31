# No Market — Match Option

**Status**: Built; the Market's building type id (`0x40`) was read from a dump on 2026-10-31
(below). Playtested 2026-10-31: with the rule on, a Market cannot be placed and the
auto-market editor will not open. Trading at a map-placed Market was not separately checked.
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

### The Market's type id: `0x40`

The type is a small integer, and it is not a constant anywhere near the `Market` class. The
class registers by name through Dragonfly (`S2BuildingClass` ctor `0x4718e0`, class object
`0x12a9a18`, mesh `meshes\buildings\market.bdg`). The ctor's arguments are the name, the
parent class, a static and the object size, so the id is assigned from data at runtime.

It was read from a dump taken in a match with a Market on the map (`Stronghold2.DMP`, base
`0x980000`):

- Every `S2BuildingClass` object (vtable `0x99e5b8`) holds its type at **`+0x3c`**, and every
  building copies it to its own **`+0x1c`**. The class at `+0xa0` of each of the two
  `Market` instances is `0x12a9a18`, whose `+0x3c` is `0x40`, as is each instance's `+0x1c`.
- That field is the placement id space: placement (`0x471260`) singles out `0x46` and `0x16`,
  which are SiegeCamp and Keep3 under `+0x3c`, and the [siege camp hotkey](siege-camp-hotkey.md)
  finds siege camps with `getBuildingOfType(…, 0x46, …)`, independently of this dump.
- `+0x44` is a second, similar sequence (Market `0x45`) and is not the type: under it `0x46` is
  MercPost and no class has `0x16`.

The full table (106 classes) is sequential from StockPile `0x1`: Granary `0x7`, Market `0x40`,
MercPost `0x41`, SiegeCamp `0x46`, CarterPost `0x53`. Non-placeable bases (`S2Building`,
`Keep`, `Tower`, `Barracks`, `GateHouse`, `Torture`, `ConstructingBuilding`) read `0`.

Two leads that did not give it:

- **The class-pointer lists at `0xaa77f4`** are several null-terminated lists
  (`[0xae5e8c] = 0xaa77f4`, `[0xae5f58] = 0xaa7810`), not one table indexed by type, so the
  market's position in them means nothing.
- **The map editor's availability array** (`BuildingAvailabilityScreen`: `0x434450` writes
  `byte [[0x12c6844] + type + 0x14]`, over the id list at `0xac4be0`) belongs to the editor.
  `[0x12c6844]` is null in a match dump. The in-game availability state, which would also grey
  the build button the stock way, has not been found.

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
