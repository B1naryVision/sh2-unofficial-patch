# Wheat Farmers and Candle Makers Turning to Crime Inside Their Buildings

**Status:** Fixed in v0.8.0 as an automatic [match fix](../features/match-rules.md#options-and-fixes): on in every
multiplayer match where every player supports it, and always on in single player. Built
2026-10-31, playtested 2026-11-01: works as expected.
**Affects:** Stronghold 2 Steam v1.5.0 (32-bit, `Stronghold2.exe`)
**Config:** none. A bug fix is never a choice.

---

## Symptom

Wheat farmers and candle makers (the Chandler's Workshop worker) tend to end up inside their
building's geometry. When the crime system picks one of them to turn criminal there, Parish
Guards cannot reach the criminal. The only remedy is to demolish the building so the
criminal comes out.

Firefly fixed the same soft-lock for two other buildings in an official update. The Gong Pit
and the Falconer's Post are "no longer affected by crime". This patch extends that same fix
to the Wheat Farm and the Chandler's Workshop.

---

## Root cause and how Firefly fixed it

All addresses are VAs at the preferred image base `0x400000` (RVA = VA − `0x400000`).

### The crime routine

`0x41ec30` (thiscall on a player object) turns one worker of that player into a criminal. It
has two callers:

- **the per-player tick**, at `0x420083`. A counter at `player+0x10c8` counts to `0x12c0`.
  When the player has fewer criminals than their cap (`player+0x10cc`, derived from
  `player+0x1034`), the routine runs once.
- **`CreateCriminalsAction`**, the scripted event, at `0x446940` (referenced from `0x90f684`).
  It calls the routine as many times as the script asks.

The routine:

1. walks a static table at **`0xa8d4a8`** of 12-byte records
   `{u32 buildingType, u32 otherFlag, u32 crimeEligible}`, ending at a type of 0;
2. for each record with `crimeEligible != 0`, read at `0x41ec83` as
   `cmp dword [eax+0xa8d4b0], 0`, collects every building of that type the player owns.
   The iterator is `0x46bb20`, and handles resolve through `0xae5f40` against the
   `S2Building` class `0x129c930`. A building whose vfunc `+0x68` returns non-zero is skipped;
3. shuffles the candidates with the simulation PRNG (one draw per candidate, cursor
   `[[0xae8bcc]+0x40078]`);
4. calls building vfunc `+0x7c(0x37, 1)` on each candidate in turn until one returns true.
   `0x37` is the Criminal unit type. It then calls `+0x78`, and shows the
   `FXT_TURN_TO_CRIME_*` floating text through a switch on the building class's worker
   type (`0x41eefb`).

### Firefly's fix was data, not code

In the shipped table, the Gong Pit and Falconer's Post records hold `crimeEligible = 0`, so
the routine never collects them. Their floating-text keys,
`FXT_TURN_TO_CRIME_GONG_FARMER` (id `0x2b`) and `FXT_TURN_TO_CRIME_FALCONER` (id `0x2c`),
are still in the exe. They are leftovers from before the exclusion. The Wheat Farm and the
Chandler's Workshop are still eligible.

The building type ids come from `S2BuildingClass+0x3c`, read in a match dump. The method and
the full id map are in [no-market.md](../features/no-market.md#the-markets-type-id-0x40).
The dump's copy of the table matches the exe on disk exactly, so nothing rewrites it at
runtime.

| # | Record VA | Type | Building | `+4` | `+8` crime |
| --- | --- | --- | --- | --- | --- |
| 0 | `0xa8d4a8` | `0x02` | SawPit | 0 | 1 |
| 1 | `0xa8d4b4` | `0x2a` | AppleFarm | 0 | 1 |
| 2 | `0xa8d4c0` | `0x29` | DairyFarm | 0 | 1 |
| 3 | `0xa8d4cc` | `0x2b` | HuntersPost | 0 | 1 |
| 4 | `0xa8d4d8` | `0x04` | StoneQuarry | 1 | 0 |
| 5 | `0xa8d4e4` | `0x0f` | IronMine | 1 | 0 |
| 6 | `0xa8d4f0` | `0x11` | OxTether | 0 | 0 |
| 7 | `0xa8d4fc` | `0x43` | PitchRig | 0 | 1 |
| 8 | `0xa8d508` | `0x0b` | Inn | 0 | 1 |
| 9 | `0xa8d514` | `0x50` | Church | 0 | 0 |
| **10** | `0xa8d520` | `0x03` | **WheatFarm** | 0 | **1 → 0** |
| 11 | `0xa8d52c` | `0x06` | Bakery | 0 | 1 |
| 12 | `0xa8d538` | `0x05` | Mill | 1 | 0 |
| 13 | `0xa8d544` | `0x45` | GongPit | 0 | 0 (Firefly) |
| 14 | `0xa8d550` | `0x42` | FalconersPost | 0 | 0 (Firefly) |
| 15 | `0xa8d55c` | `0x2e` | Well | 0 | 1 |
| 16 | `0xa8d568` | `0x34` | WatchPost | 0 | 0 |
| 17 | `0xa8d574` | `0x31` | CourtHouse | 0 | 0 |
| 18 | `0xa8d580` | `0x0c` | HopFarm | 0 | 1 |
| 19 | `0xa8d58c` | `0x0d` | Brewery | 0 | 1 |
| **20** | `0xa8d598` | `0x48` | **ChandlerWorkshop** | 0 | **1 → 0** |
| 21 | `0xa8d5a4` | `0x08` | FletcherWorkshop | 0 | 1 |
| 22 | `0xa8d5b0` | `0x09` | PoleturnerWorkshop | 0 | 1 |
| 23 | `0xa8d5bc` | `0x0e` | BlacksmithWorkshop | 0 | 1 |
| 24 | `0xa8d5c8` | `0x4a` | ArmourerWorkshop | 0 | 1 |
| 25 | `0xa8d5d4` | `0x4d` | TannerWorkshop | 0 | 1 |
| 26 | `0xa8d5e0` | `0x19` | Vineyard | 0 | 1 |
| 27 | `0xa8d5ec` | `0x1b` | VintnerWorkshop | 0 | 1 |
| 28 | `0xa8d5f8` | `0x1c` | FishPond | 0 | 1 |
| 29 | `0xa8d604` | `0x27` | PigFarm | 0 | 1 |
| 30 | `0xa8d610` | `0x2d` | GardenersHut | 0 | 1 |
| 31 | `0xa8d61c` | `0x4c` | SheepFarm | 0 | 1 |
| 32 | `0xa8d628` | `0x4b` | WeaverWorkshop | 0 | 1 |
| 33 | `0xa8d634` | `0x1a` | Kitchen | 0 | 0 |
| 34 | `0xa8d640` | `0x1d` | ServantsQuarters | 1 | 0 |
| 35 | `0xa8d64c` | `0x33` | TorturersGuild | 1 | 0 |
| 36 | `0xa8d658` | `0x10` | Treasury | 0 | 1 |
| 37 | `0xa8d664` | `0x53` | CarterPost | 0 | 0 |
| 38 | `0xa8d670` | `0x4f` | Apothecary | 0 | 0 |
| 39 | `0xa8d67c` | `0x54` | Monastery | 1 | 0 |
| 40 | `0xa8d688` | `0x55` | MusiciansGuild | 0 | 0 |

### Who else reads the table

Three other functions walk it: `0x4199dc`, `0x419d40` and `0x419ef0`. They read only `+0` (the
type) and `+4`, the latter together with a per-player byte at
`player + type + 0x1474`. None of them reads `+8`, so the crime flag has exactly one reader.
No code in `.text` writes to the table, and it holds no pointers, so there are no relocations
in it.

---

## The patch

Two dwords in `.data`, written while the fix is on and restored when it is off:

| VA | RVA | Stock | Patched |
| --- | --- | --- | --- |
| `0xa8d528` (WheatFarm `+8`) | `0x68d528` | `01 00 00 00` | `00 00 00 00` |
| `0xa8d5a0` (ChandlerWorkshop `+8`) | `0x68d5a0` | `01 00 00 00` | `00 00 00 00` |

The install writes nothing (`src/patches/crimeExclusion.cpp`). It verifies the following,
all-or-nothing:

- records 10 and 20 read `{0x03, 0, 1}` and `{0x48, 0, 1}`;
- records 13 and 14 read `{0x45, 0, 0}` and `{0x42, 0, 0}`. These are Firefly's exclusions,
  checked to fingerprint the table;
- the reader at `0x41ec83` is `83 b8 <table+8> 00`. Its operand is checked against
  `base + 0x68d4b0` rebuilt at runtime, because the loader relocates it and the disk bytes
  would never match.

`patches/matchRules.cpp` then calls `crimeExclusionSetEnabled` on the simulation thread when
the rules line executes, and again on the way back to the lobby. In single player it is called
when the match screen attaches.

### Side effects

- **Scripted criminals.** `CreateCriminalsAction` draws from the same table. In a campaign
  mission where the only eligible buildings are wheat farms and chandler's workshops, a
  scripted "criminals appear" event produces fewer criminals, or none. A mission with Gong
  Pits already behaves this way.
- **Where crime lands.** The per-player tick still runs at the same cadence and the cap is
  unchanged. When the player has other eligible buildings, the criminal comes from one of
  them. When the player has none, no criminal forms that tick.
- Criminals that already exist are not touched.

---

## Multiplayer Compatibility

**Requires all clients, self-enforced** (an automatic match fix). Removing two building types
from the candidate list changes how many PRNG draws the shuffle consumes, and which worker
is picked. A client with the fix and a client without it would diverge on the first crime
check. The fix is therefore advertised as rule bit `0x8` and only switched on through the
host's rules line, on the same lockstep tick everywhere. With any player lacking it, the
lobby panel shows it as `unavailable` and the match keeps the stock table. Replays record
and restore it with the other rules.

---

## Dead ends

- **The `FXT_TURN_TO_CRIME_*` strings** have one xref each, all inside a static initializer
  that fills floating-text records (`0x8e97f0`…). Nothing outside it references those
  records, so they lead only to the text ids (`0x29`…`0x3a`), not to the logic. The ids did
  pay off later: the `push 0x29`…`push 0x3a` switch was the way into the crime routine.
- **The Criminal constructor** (`0x518830`) is reached only through its `DynamicFactory`
  traits vtable (`0x9b1bbc`/`0x9b1bc0`). Most references to the Criminal class object
  `0x12b3e38` are handle resolves (`0x769360` on `0xae8728`). The class's index in the unit
  class list at `0xaa79ac` gives the unit type: list index + 1, so Criminal is `0x37`,
  WheatFarmer `0xf`, Falconer `0x39`, GongFarmer `0x3b` and Chandler `0x3c`.
- **Diffing worker vtables** (GongFarmer and Falconer against Chandler and WheatFarmer)
  finds no crime virtual. The only slot that differs in a suggestive way, `+0x130`, is a
  goods/state predicate that Falconer and Chandler share with the base class.
