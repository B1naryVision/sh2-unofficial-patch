# Stronghold 2 Unofficial Patch — Player Changelog

---

## [Unreleased]

### [Feature] - Keyboard Shortcut to Stop Selected Troops

- **What the issue was:** Stopping a selected group of troops could only be done by clicking the Stop button on the command panel, with no keyboard shortcut.
- **What we changed:** Pressing the `H` key now stops whatever troops you currently have selected, the same as clicking Stop.

### [Fix] - Crash When Your Lord Dies in the Barracks Menu

- **What the issue was:** If you had the barracks or recruitment screen open at the moment your Lord died, the game crashed to the desktop instead of showing the defeat screen.
- **What we changed:** The game now handles the Lord's death gracefully while the recruitment menu is open, so the defeat screen appears as intended.

### [Fix] - End-of-Game Stats Missing Most Players in Multiplayer

- **What the issue was:** The end-of-game statistics screen only showed a small fraction of the players from a multiplayer match.
- **What we changed:** All active players now appear in the final stats summary.

### [Improvement] - Player Names on End-of-Game Stats Screen

- **What the issue was:** The stats screen showed generic placeholders instead of real player names, and when you were not the host the names that did appear were often attached to the wrong player — sometimes shown as unreadable characters.
- **What we changed:** Each player's real name is now matched to their own column reliably, whether or not you are the host. Players who left the match before it ended show a colour label instead of a name.

### [Improvement] - Readability of End-of-Game Stats

- **What the issue was:** Stat values and player names were hard to read because they blended into the dark overlay background.
- **What we changed:** Player names and all stat values are now shown in white for much easier reading.

---

## [0.3.0]

### [Fix] - Multiplayer Crash During Player Connection

- **What the issue was:** The game could crash to desktop during a multiplayer session when a stray connection message arrived with no matching player entry.
- **What we changed:** The game now safely ignores these edge-case messages so the session continues without crashing.

### [Improvement] - End-of-Game Statistics Screen

- **What the issue was:** When a match ended there was no summary of how each player performed — gold, honour, army size, and recruitment numbers were not shown anywhere.
- **What we changed:** A stats overlay now appears on the victory and defeat screens showing gold, honour, army size, income sources, and unit recruitment counts for every player in the match.

---

## [0.2.0]

### [Improvement] - Skip Intro Video on Launch

- **What the issue was:** Every time you launched the game you were forced to watch the Firefly Studios logo video before the main menu appeared.
- **What we changed:** The game now loads directly to the main menu on startup, skipping the intro video entirely.

### [Improvement] - AI Opponents in Multiplayer Lobbies

- **What the issue was:** The option to add AI opponents to a multiplayer game existed in the lobby but was silently disabled and never worked.
- **What we changed:** AI opponents can now be configured and added to multiplayer matches. The host must have the patch installed; AI behaviour is the same as in single-player.

---

## [0.1.0]

### [Fix] - Crash When a Knight Is Hit by a Catapult While Mounting

- **What the issue was:** If a catapult projectile hit a knight at the exact moment they were getting on a horse, the game crashed to desktop instantly.
- **What we changed:** Knights can now be hit by projectiles while mounting without causing a crash; the unit is defeated normally instead.
