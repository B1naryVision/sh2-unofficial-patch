# Auto-Market QoL — Implementation Plan

## Context

Players micro-manage market buy/sell constantly to keep resource stocks in a
useful band. This feature automates that: per-resource **Min** (auto-buy up to)
and **Max** (auto-sell down to) thresholds, evaluated periodically, executing
real market trades constrained by gold and stock — fully stock-compatible and
vanilla-save-safe.

The patch adds **no new game state** and stores nothing in save files. All
patch state (thresholds, enabled flags) lives in DLL-owned globals + the ini,
completely separate from game structures.

## Decisive gate — does market buy/sell route through the networked command layer?

This is the whole feature's foundation and is **not yet proven**. Everything
below assumes it holds; **Phase 0 must confirm it before any code is written.**

- Stronghold 2 MP is deterministic lockstep. A patch is MP-safe **only** if it
  either touches no simulation state, or issues an *existing player command*
  through the game's own networked command path (an input broadcast to all
  clients with an execution tick — see `CLAUDE.md` "Player command vs
  autonomous logic"). This is exactly how `shiftRecruit` stays "safe for
  version mismatch".
- Auto-market mutates **gold + goods stock** = simulation state. So the ONLY
  MP-safe design is to **replicate the market's own buy/sell command post**,
  never to call a low-level mutation directly.
- Strong prior that this works: players trade in MP without desyncing, so a
  buy/sell click *must* already feed a networked command. But this is a prior,
  not a fact. **Phase 0 proves it by tracing `TraderScreen`/`SubPanelStockPile`
  buy-button handling and confirming it posts an event to the command layer
  (like recruit) rather than mutating gold/stock inline.**
- **Go/no-go:** if buy/sell is a direct local mutation (no networked command),
  autonomous auto-buy is a one-sided state change → desync. In that case the
  feature is at best "requires all clients", and we stop and re-decide scope
  with the user before proceeding.

## What already exists (reuse — do not rebuild)

| Need | Reuse | Location |
| --- | --- | --- |
| Periodic sim-thread execution | `registerFrameTick(fn)` dispatcher (RVA `0x300c0`, sim thread, MP-safe timing) | `src/core/frameTick.h` |
| Command-post replication pattern (recipe A: retarget shared helper; recipe B: vtable-slot wrap) | `shiftRecruit.cpp` | `src/patches/shiftRecruit.cpp` |
| Local player object + gold (`+0x1010` float) | `PlayerView`, `getPlayer` (RVA `0x11360`), world singleton (RVA `0xd781d0`) | `src/patches/endgameStats/gameOffsets.h`, `shiftRecruit.cpp:24-26` |
| ini config (defaults on bad values, zero-footprint when off) | `configInt`/`configFloat`/`loadConfig` | `src/core/config.h` |
| Overlay window scaffolding (font, layout, double-buffer paint, game-window anchoring) | `showStatsOverlay` / `paintOverlay` | `src/patches/endgameStats/overlay.cpp` |
| Registration | add `install…()` call | `src/patches/registry.cpp` |

**Overlay caveat:** the endgame overlay is deliberately **click-through and
never-activating** (`WS_EX_TRANSPARENT | WS_EX_NOACTIVATE`, GDI). An
*interactive* threshold editor needs a real input-capable window (drop those
flags; add `WM_KEYDOWN`/`WM_CHAR`/`WM_LBUTTONDOWN`, a focus/field model, and
number parsing). This is a separate sub-project (Phase 3) with a known
exclusive-fullscreen focus caveat.

## RTTI anchors already located (in `Stronghold2.exe`)

`Market`, `StockPile`, `Granary`, `TraderScreen`, `SubPanelStockPile`,
`SubPanelGranary`, `TradeChore`/`CarterTradeChore` (carter logistics, not the
instant trade). No dedicated `Buy/SellGoodsChore` class exists → the buy/sell
command is built **inline** in the trade UI handler (recipe-B shape, like the
siege camp). Resolve the exact handler via the static RTTI walk in `CLAUDE.md`
("Static RTTI Mapping"): `TraderScreen`/`SubPanelStockPile` vtable → `process`
slot → the buy/sell subid dispatch.

## Phased implementation (vertical slice first)

### Phase 0 — RE spike (de-risk; gates everything)
Deliverable: a short doc + confirmed offsets, **no game code merged yet**.
1. Static RTTI walk to `TraderScreen`/`SubPanelStockPile::process`; find the
   buy/sell button subid case(s) and the payload layout (good-id, buy vs sell,
   quantity).
2. Confirm the handler **posts a networked command event** (trace to the same
   command layer recruit uses) — the MP gate above. Cross-check with a runtime
   minidump taken with the trade panel open if static tracing is ambiguous.
3. Find **per-player per-good stock storage**: the goods array on the player
   object (offset + good-id ordering + element stride/type). Anchor from the
   known player base (gold `+0x1010`) and the goods-acquired trigger classes.
4. Identify how to build/post the command *without a triggering click*
   (recruit's post helper needs a `ctx`; find the equivalent for trade, or the
   minimal event object the handler consumes).
Output: `docs/features/auto-market.md` (offsets, evidence, MP classification).

### Phase 1 — Logic engine + vertical slice (Wood, buy-only)
- New `src/patches/autoMarket/` module (mirrors `endgameStats/` split):
  `autoMarket.cpp/.h` (install + frame-tick callback + threshold engine),
  `marketOffsets.h` (Phase-0 offsets), `trade.h/.cpp` (post-command wrappers).
- Frame-tick callback throttled to every N ticks (config `TickInterval`,
  default e.g. 30). **Float-free** on the tick path (read gold as raw bits per
  the frame-tick rule); do gold/threshold compares in integer or defer the
  float convert to a prologue-safe helper.
- Slice: if `Wood.min > 0` and `stock(Wood) < min`, post one buy command per
  eligible tick (or a bounded batch), relying on the sim to re-validate and
  drop unaffordable commands (same safety model as shift-recruit — never
  pre-trust local affordability as the gate).
- Config-gated: absent/zeroed `[AutoMarket]` section → zero-footprint, no hook.

### Phase 2 — Full breadth
- All tradeable goods; both auto-buy (`stock < min`) and auto-sell
  (`stock > max`); disabled-state = unset/zero threshold ignores that good.
- Per-good enumeration table (good-id ↔ display name ↔ tradeable flag).

### Phase 3 — Interactive overlay
- New input-capable overlay window (not the click-through one): toggle hotkey
  (config `[hotkeys] AutoMarketToggle`, default e.g. `M`+modifier via
  `configHotkey`), a per-good row grid with editable Min/Max number fields,
  keyboard focus + click-to-focus + type-to-edit, live stock readout.
- Writes thresholds into the same DLL globals the engine reads; optionally
  persists back to the ini on close. Document the exclusive-fullscreen focus
  caveat.

## Files

**Create:** `src/patches/autoMarket/autoMarket.{cpp,h}`,
`.../marketOffsets.h`, `.../trade.{cpp,h}`, `.../overlay.{cpp,h}` (Phase 3),
`docs/features/auto-market.md`.
**Modify:** `src/patches/registry.cpp` (+`installAutoMarket()`), `Makefile`
(add new `.cpp` to `SRCS`), `CHANGELOG.md` (`[Unreleased]`), `README.md`
(Current Fixes and Features), `docs/features/configuration.md` (new keys).

Follow the 6-step "Adding a New Patch" checklist in `CLAUDE.md` — none optional.

## Verification

- **Build:** `make` (MinGW i686), then `make deploy`.
- **Byte-safety:** every patch site verifies expected stock bytes before
  writing (all-or-nothing), like `shiftRecruit` — a build shift must skip, not
  corrupt.
- **SP functional:** set `Wood min:20`, drop stock below 20 with a market
  present → auto-buys until 20 or gold exhausted; set `max:100`, overstock →
  auto-sells down to 100; zeroed good is ignored; no market building → no-op.
- **Gold safety:** never overdraws (sim re-validation drops the remainder);
  watch treasury never goes negative.
- **MP (the critical test):** two clients, one patched host issues auto-buys →
  **no desync** across many trades (proves the networked-command routing). Then
  patched-vs-unpatched to confirm "safe for version mismatch" if Phase 0
  supports that classification.
- **Debug logging:** `make debug` for the ring-buffer trace during bring-up.

## MP classification (to be finalized by Phase 0)

Target: **safe for version mismatch** — issues existing stock buy/sell player
commands through the networked command path (input, broadcast + executed
identically on every client; sim re-validates and drops unaffordable ones),
run on the sim thread via the frame-tick dispatcher. Falls back to "requires
all clients" / re-scope only if Phase 0 disproves networked-command routing.

## Open risks

1. **MP gate (Phase 0)** — the go/no-go above; highest risk.
2. **Timer-driven post without a click context** — recruit's helper takes a
   `ctx` from the live click; the trade equivalent may need a panel/handle we
   must synthesize or fetch. If unresolvable cleanly, fall back to recipe-B
   (invoke the stock `process` with a hand-built event, as siege camp does).
3. **Good-id ordering / stock array layout** — must be nailed from the dump,
   not guessed (CJK-garbage / wrong-offset failure modes in `CLAUDE.md`).
4. **Interactive overlay in exclusive fullscreen** — focus/input caveat;
   document and test windowed + fullscreen.
