# Stronghold 2 Unofficial Patch — Player Changelog

---

## [Unreleased]

### [Feature] - Shift-Click to Recruit Several Units at Once

- **What the issue was:** Recruiting an army meant clicking the same unit icon over and over — once per unit — in the barracks, mercenary post, monastery, engineers guild and siege camp.
- **What we changed:** Holding **Shift** while clicking a unit icon now queues up to 20 units in one click (configurable in `sh2-unofficial-patch.ini` via `RecruitmentShiftMultiplier`, or set it to `0` to turn the feature off). This covers regular troops, monks, mercenaries, engineers, laddermen and all siege equipment. The game still checks gold, weapons and peasants for every single unit, so the batch simply stops when you run out — you can never be overcharged. Works in multiplayer and behaves exactly like clicking quickly by hand, so it stays compatible with unpatched players.

---

## [0.5.0]

### [Improvement] - More Reliable End-of-Game Statistics

- **What the issue was:** The end-of-game statistics could occasionally show wrong names or numbers for some players, because information kept being gathered even while you were in the menus between games and leftovers from a finished match could bleed into the next one.
- **What we changed:** Statistics are now only gathered while a match is actually being played, and everything is fully cleared as soon as you return to the main menu — including when you quit a match early. Each match starts with a completely clean slate.

### [Fix] - Short Player Names Shown as Chinese Characters

- **What the issue was:** Players with short names (7 characters or fewer) could show up on the end-of-game statistics as a jumble of Chinese-looking characters instead of their actual name.
- **What we changed:** Short names are now read correctly, so every player's real name appears regardless of its length.

### [Fix] - End-of-Game Statistics Panel Now Goes Away

- **What the issue was:** The statistics panel stayed on screen after leaving the victory or defeat screen, with no way to dismiss it.
- **What we changed:** The panel now disappears automatically when you return to the main menu.

### [Improvement] - End-of-Game Statistics Screen Polish

- **What the issue was:** Switching to another window (for example alt-tabbing to chat) while the stats were showing made them disappear for good, numbers were hard to compare because they weren't lined up, and the panel used a dated system font.
- **What we changed:** The stats now hide while you're in another window and come right back when you return to the game. Numbers are neatly right-aligned in their columns, a popularity row was added, the text uses a cleaner font, and the panel sizes itself to fit its contents and your game window.

### [Fix] - Crash When Troops Are Moved Off the Edge of the Map

- **What the issue was:** Pushing a selected group of troops out of bounds — most easily by dragging their formation into a corner or off any edge of the map — could crash the game to desktop.
- **What we changed:** Troops and their formations are now handled safely at and beyond the map edges, so this no longer crashes the game.

### [Feature] - Optional Configuration File

- **What the issue was:** The patch's hotkeys were fixed (`H` to stop troops, `Mouse4` for attack-move), with no way to change them if they clashed with your habits or your mouse had no extra buttons, and no way to turn individual conveniences off.
- **What we changed:** You can now place an optional `sh2-unofficial-patch.ini` file (included in the download) next to `d3d9.dll` to pick your own keys for both shortcuts — or disable them — and to speed up the camera zoom. Every option is explained inside the file. Without the file, everything behaves exactly as before.

### [Improvement] - Faster Camera Zoom (Opt-In)

- **What the issue was:** Zooming the camera in and out with the mouse wheel was very slow, especially on high-resolution monitors, taking many turns of the wheel to move between close and far views.
- **What we changed:** You can now make the zoom faster (or slower) by setting a zoom speed multiplier in the configuration file — for example `2.5` for two-and-a-half times the speed. The zoom is unchanged unless you set it. The closest and farthest zoom limits always stay the same.

### [Feature] - Mouse Button Shortcut for Attack-Move

- **What the issue was:** Turning on the "Attack" stance for selected troops — so a move order makes them engage enemies along the way — could only be done by clicking the Attack button on the command panel, with no shortcut.
- **What we changed:** Pressing the fourth mouse button (`Mouse4`) now toggles the Attack stance on your selected troops, just like clicking the Attack button. Turn it on, then move your troops.

---

## [0.4.0]

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
