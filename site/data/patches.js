/* ==================================================================
   SITE DATA — this is the only file you need to edit.
   ------------------------------------------------------------------
   Nothing here is compiled or built. Edit it, save it, push it.
   Open site/index.html in a browser to check your change.

   HOW TO ADD A PICTURE OR A VIDEO TO A FEATURE
   --------------------------------------------
   Every entry in PATCHES accepts an optional `media` object. Leave it
   out and the card shows a tasteful "picture coming" panel, so the
   site never looks broken while you are still capturing footage.

     media:{ type:"youtube", id:"dQw4w9WgXcQ", cap:"Watch it in action" }
     media:{ type:"image",   src:"assets/media/auto-market.jpg",
                             cap:"The auto-market panel" }
     media:{ type:"ba",      before:"assets/media/zoom-before.jpg",
                             after:"assets/media/zoom-after.jpg",
                             beforeLabel:"Before", afterLabel:"After",
                             cap:"Drag the handle to compare" }

   `type:"ba"` is the before/after slider. Use it only where the two
   pictures are taken from the same spot and differ in one visible way
   (zoom distance, black edges, panel size). For a crash fix or an
   animation, a short YouTube clip says far more than two stills.

   The YouTube `id` is the part after `v=` in the address bar:
   https://www.youtube.com/watch?v=dQw4w9WgXcQ  ->  id:"dQw4w9WgXcQ"

   Giving a fix its own video is the single biggest upgrade you can make
   to a card: the reader watches the problem happen and then watches it
   not happen. When you record one, add it to the playlist AND put its id
   on that fix's entry here -- the card then shows the video thumbnail in
   the grid and plays it in the panel, instead of the "screenshot on the
   way" strip.

   FIELD REFERENCE
   ---------------
   id        short slug, used for deep links (#fix-knight-catapult)
   cat       crashes | multiplayer | camera | controls | interface
   kind      fix | feature | improvement       (drives the colour tag)
   version   "0.7.0" etc, or "next" for something not yet released
   title     short, plain English, no jargon
   was       the problem, from the player's chair. One sentence.
   now       what happens instead. One or two sentences.
   how       optional: how to actually use it. Keys, settings, clicks.
   mp        solo | partial | host | all   -> the multiplayer badge
               solo    works even if nobody else has the patch
               partial works either way, but is better if both have it
               host    only the person hosting needs it
               all     everyone in the game needs the same version
   optIn     true if it does nothing until switched on in the ini file
   ================================================================== */

const SITE = {
  repo:        "B1naryVision/sh2-unofficial-patch",
  releases:    "https://github.com/B1naryVision/sh2-unofficial-patch/releases/latest",
  issues:      "https://github.com/B1naryVision/sh2-unofficial-patch/issues",
  discord:     "BinaryVision",
  fallbackVer: "v0.7.0",   /* shown if GitHub cannot be reached */

  /* The video playlist: release walkthroughs, embedded in its own
     section. Clear playlistId to hide that section entirely. */
  playlist:   "https://www.youtube.com/playlist?list=PLfgvJ-29Gb70",
  playlistId: "PLfgvJ-29Gb70",

  /* Optional: a short install walkthrough video. Paste a YouTube id
     here and it appears at the top of the Install section. Leave ""
     to hide it. */
  installVideo: "",

  /* Optional: a hero picture — a wide, good-looking in-game shot.
     Drop the file in assets/media/ and put its path here. */
  heroImage: "",
  heroCaption: "Stronghold 2 · Steam v1.5.0"
};

const CATEGORIES = [
  { id:"all",         label:"Everything" },
  { id:"crashes",     label:"Crash fixes" },
  { id:"multiplayer", label:"Multiplayer" },
  { id:"controls",    label:"Controls" },
  { id:"camera",      label:"Camera" },
  { id:"interface",   label:"Interface" }
];

const PATCHES = [

  /* ---------- CRASH FIXES ---------------------------------------- */
  {
    id:"knight-catapult", cat:"crashes", kind:"fix", version:"0.1.0", mp:"solo",
    title:"Crash when a catapult hits a knight getting on a horse",
    was:"If a catapult stone struck a knight at the exact moment he was mounting up, the game closed instantly to the desktop.",
    now:"The knight is simply thrown as normal (usually) and the game carries on. Nothing else about knights or catapults changes."
  },
  {
    id:"mp-connect-crash", cat:"crashes", kind:"fix", version:"0.3.0", mp:"solo",
    title:"Crash while a player was connecting",
    was:"A multiplayer game could drop to the desktop while somebody was joining or re-connecting, taking everyone's match down with it.",
    now:"The stray message that caused it is ignored and the match keeps going."
  },
  {
    id:"barracks-lord-death", cat:"crashes", kind:"fix", version:"0.4.0", mp:"solo",
    title:"Crash when your Lord dies with the barracks open",
    was:"Having the barracks or a recruitment screen open at the moment your Lord was killed crashed the game instead of showing the defeat screen.",
    now:"You get the defeat screen as intended, whatever menu you happen to have open."
  },
  {
    id:"map-edge-crash", cat:"crashes", kind:"fix", version:"0.5.0", mp:"solo",
    title:"Crash when troops are pushed off the edge of the map",
    was:"Dragging a formation into a map corner or over the edge could close the game to the desktop.",
    now:"Troops and formations are handled safely at the map edge, so it no longer crashes."
  },

  /* ---------- MULTIPLAYER ---------------------------------------- */
  {
    id:"mp-ai-enable", cat:"multiplayer", kind:"feature", version:"0.2.0", mp:"host",
    title:"AI opponents in multiplayer lobbies",
    was:"The lobby had an option to add AI opponents, but it was switched off and never did anything.",
    now:"You can add AI lords to a multiplayer match.",
    how:"Only the person hosting the game needs the patch. Everyone else can join as normal."
  },
  {
    id:"in-progress-lobbies", cat:"multiplayer", kind:"fix", version:"0.7.0", mp:"solo",
    title:"Games already under way clogging the game list",
    was:"The multiplayer list mixed games you could join with games that had already started, and clicking a started one did nothing at all — no message, no explanation.",
    now:"Games in progress are left out, so everything you can see is something you can actually join.",
    how:"Only you need the patch. The list is tidied on your own machine, so games hosted by players without the patch are filtered out too."
  },
  {
    id:"ping-command", cat:"multiplayer", kind:"feature", version:"next", mp:"partial",
    title:"See who is lagging, with !ping",
    was:"When a game turned choppy there was no way to tell who was struggling, so it came down to guesswork and finger-pointing. The game's own Latency column has never shown anything.",
    now:"Everyone sees a chat message listing the round-trip time to each player, so the lobby can decide together what to do about it.",
    how:"Type !ping into the in-game chat during a multiplayer match. Players without the patch still see the results — their own time shows as n/a until they install it."
  },

  /* ---------- CONTROLS ------------------------------------------- */
  {
    id:"stop-troops", cat:"controls", kind:"feature", version:"0.4.0", mp:"solo",
    title:"A key to stop your troops",
    was:"Halting a group of troops meant finding and clicking the Stop button on the command panel every single time.",
    now:"One key stops whatever you have selected, exactly as the Stop button does.",
    how:"Press H. You can change it to any key, in game, with Ctrl+Shift+O."
  },
  {
    id:"attack-move", cat:"controls", kind:"feature", version:"0.5.0", mp:"solo",
    title:"A shortcut for attack-move",
    was:"Putting troops into the Attack stance — so they engage enemies on the way to where you send them — could only be done by clicking the Attack button.",
    now:"One button toggles the Attack stance on your selected troops. Turn it on, then give the move order.",
    how:"Press the fourth mouse button (Mouse4). No spare mouse buttons? Change it to any key with Ctrl+Shift+O."
  },
  {
    id:"shift-recruit", cat:"controls", kind:"feature", version:"0.6.0", mp:"solo",
    title:"Recruit a whole squad in one click",
    was:"Building an army meant clicking the same unit icon over and over — once per soldier — in the barracks, mercenary post, monastery, engineers guild and siege camp.",
    now:"Hold Shift and click a unit to queue up to twenty at once. It covers troops, monks, mercenaries, engineers, laddermen and every piece of siege equipment.",
    how:"Hold Shift, click the unit. The game still checks gold, weapons and peasants for each one, so it simply stops when you run out — you can never be overcharged."
  },
  {
    id:"siege-camp-hotkey", cat:"controls", kind:"improvement", version:"0.6.0", mp:"solo",
    title:"The siege camp key no longer yanks your view away",
    was:"Pressing the siege camp key opened its panel and immediately threw the camera across the map, so you lost sight of the battle just to queue up a catapult.",
    now:"The first press opens the panel and leaves your view where it is. Press it again to travel there — the same way the barracks and granary keys already work.",
    how:"Press J. Prefer the old behaviour? Set SiegeCampJumpOnSecondPress to 0 in the settings file."
  },
  {
    id:"rebind", cat:"controls", kind:"feature", version:"0.6.0", mp:"solo",
    title:"Change any of the patch's keys, in game",
    was:"Changing a shortcut meant quitting, opening a settings file in a text editor and typing the right key name — awkward for anyone who would rather not touch config files.",
    now:"A panel inside the game lists every shortcut the patch adds. Click one, press the key you want, and it is saved for next time.",
    how:"Press Ctrl+Shift+O in game. [x] clears a shortcut, Esc closes the panel, and any two shortcuts sharing a key are highlighted so you can spot the clash. Combinations like Ctrl+Shift+H work too."
  },

  /* ---------- CAMERA --------------------------------------------- */
  {
    id:"zoom-speed", cat:"camera", kind:"improvement", version:"0.5.0", mp:"solo", optIn:true,
    title:"Faster camera zoom",
    was:"Zooming with the mouse wheel was painfully slow, especially on a big monitor — many turns of the wheel to get from a close view to a wide one.",
    now:"You can make the wheel zoom as fast (or as slow) as you like. The closest and furthest points stay exactly where they were.",
    how:"Set ZoomSpeedMultiplier in the settings file — 2.5 means two and a half times the normal speed."
  },
  {
    id:"zoom-limit", cat:"camera", kind:"feature", version:"0.7.0", mp:"solo", optIn:true,
    title:"Zoom out much further",
    was:"The camera stopped pulling back very early, so on a wide monitor you saw only a corner of your castle and had to scroll constantly to follow a battle.",
    now:"You can pull back far enough to take in the whole fight, stopping at the point where the view stops holding up.",
    how:"Set ZoomOutLimit to Auto in the settings file. There is nothing to tune — the stopping point is worked out from the size of the map and the angle of your camera, so it suits small and large maps alike."
  },

  /* ---------- INTERFACE ------------------------------------------ */
  {
    id:"intro-skip", cat:"interface", kind:"improvement", version:"0.2.0", mp:"solo",
    title:"Straight to the main menu",
    was:"Every launch made you sit through the Firefly Studios logo before you could touch anything.",
    now:"The game opens directly on the main menu."
  },
  {
    id:"endgame-stats", cat:"interface", kind:"feature", version:"0.3.0", mp:"solo",
    title:"End-of-game statistics",
    was:"When a match ended there was no summary at all — gold, honour, army size and recruitment numbers were shown nowhere.",
    now:"A summary appears on the victory and defeat screens with gold, honour, popularity, army size, income and units recruited for every player in the match.",
    how:"Nothing to switch on. It appears by itself when a match ends, is drawn into the game's own picture so it shows up in screenshots, and clears itself the moment you leave the screen."
  },
  {
    id:"auto-market", cat:"interface", kind:"feature", version:"0.6.0", mp:"solo",
    title:"Automatic buying and selling",
    was:"Keeping a stockpile healthy meant opening the market again and again all match long, buying what was running low and selling off surpluses by hand.",
    now:"Set a minimum and a maximum for each good, and the game keeps you between them by itself — buying up to the minimum, selling down to the maximum, at the normal price, only when you can afford it.",
    how:"Press the ` key (just left of the 1 key) to open the panel. You can save named presets — Start Game, War — and load them from the list. Settings reset at the start of each match."
  },
  {
    id:"ui-scale", cat:"interface", kind:"fix", version:"0.6.1", mp:"solo",
    title:"Patch panels were unreadable on some screens",
    was:"The patch's panels looked right for some players and badly wrong for others — text spilling out of boxes, labels running into each other, rows pushed off the edge. Two players on the same 1920x1080 could see completely different results.",
    now:"Panels size themselves to the text they contain and follow the game's resolution rather than your Windows display scaling, so they look the same everywhere and nothing clips.",
    how:"Automatic. If you want them larger or smaller, set Scale under [ui] to a percentage between 50 and 300."
  }
];

/* ==================================================================
   VERSION HISTORY — the full list, newest first, shown collapsed at
   the bottom of the page. Keep it in step with CHANGELOG.md.
   ================================================================== */
const HISTORY = [
  { v:"next", date:"In development", items:[
    "See who is lagging with the !ping chat command"
  ]},
  { v:"0.7.0", date:"", items:[
    "Games already in progress no longer clutter the multiplayer browser",
    "The statistics panel no longer stays on screen in the campaign",
    "Zoom out much further (opt-in)",
    "No more black edges when zoomed far out (opt-in)",
    "Keep the landscape drawn when zoomed far out (opt-in)"
  ]},
  { v:"0.6.1", date:"", items:[
    "Patch panels are readable on every screen and display-scaling setting"
  ]},
  { v:"0.6.0", date:"", items:[
    "Auto-market: automatic buying and selling",
    "Shift-click to recruit several units at once",
    "In-game settings panel for the patch's shortcuts",
    "Key combinations (Ctrl / Shift / Alt) and many more key names",
    "The siege camp shortcut no longer yanks the camera away",
    "End-of-game statistics draw more reliably, and appear in screenshots"
  ]},
  { v:"0.5.0", date:"", items:[
    "Fixed a crash when troops are moved off the edge of the map",
    "Optional settings file for keys and camera options",
    "Faster camera zoom (opt-in)",
    "Mouse button shortcut for attack-move",
    "Short player names no longer show as Chinese characters",
    "End-of-game statistics: cleaner layout, and they clear properly"
  ]},
  { v:"0.4.0", date:"", items:[
    "Keyboard shortcut to stop selected troops",
    "Fixed a crash when your Lord dies with the barracks open",
    "All players now appear in the multiplayer end-of-game statistics",
    "Real player names on the statistics screen, and easier to read"
  ]},
  { v:"0.3.0", date:"", items:[
    "Fixed a multiplayer crash while a player was connecting",
    "Added the end-of-game statistics screen"
  ]},
  { v:"0.2.0", date:"", items:[
    "Skip the intro video on launch",
    "AI opponents can be added to multiplayer lobbies"
  ]},
  { v:"0.1.0", date:"", items:[
    "Fixed the crash when a knight is hit by a catapult while mounting"
  ]}
];

/* ==================================================================
   SETTINGS FILE — plain-English version of sh2-unofficial-patch.ini.
   ================================================================== */
const SETTINGS = [
  { group:"Shortcuts", rows:[
    ["SettingsPanel","Ctrl+Shift+O","Opens the panel where you can change all of these keys in game."],
    ["StopTroops","H","Stops the troops you have selected."],
    ["AttackToggle","Mouse4","Turns the Attack stance on and off for selected troops."],
    ["AutoMarketPanel","Backtick","Opens the automatic buying and selling panel."]
  ]},
  { group:"Camera", rows:[
    ["ZoomSpeedMultiplier","1.0","How fast the mouse wheel zooms. 2.5 is two and a half times normal."],
    ["ZoomOutLimit","Vanilla","Set to Auto to pull the camera back much further than the game normally allows."]
  ]},
  { group:"Everything else", rows:[
    ["SiegeCampJumpOnSecondPress","1","1 opens the panel first and travels on a second press. 0 restores the old jump-at-once behaviour."],
    ["Scale","Auto","Size of the patch's panels, as a percentage between 50 and 300."],
    ["HideInProgressLobbies","1","Hides games that have already started from the multiplayer list."],
    ["RecruitmentShiftMultiplier","20","How many units a Shift-click queues. 0 turns the feature off."]
  ]}
];

/* ==================================================================
   FREQUENTLY ASKED — the questions that actually get asked.
   ================================================================== */
const FAQ = [
  { q:"Is this safe? Will it get me banned or break my game?",
    a:["<p>The patch does not modify a single one of your game's files. It is one extra file that sits next to the game and corrects problems in memory while you play.</p>",
       "<p><strong>To undo it completely, delete that one file.</strong> Your game is then exactly as it was. There is no installer, nothing is written to the registry, and your saved games, campaigns and maps are untouched.</p>",
       "<p>Stronghold 2 has no anti-cheat, and there is no account to ban. This is a community project, not an official Firefly release.</p>"] },
  { q:"Windows or my antivirus warned me about the file. Why?",
    a:["<p>That is expected, and it is not a sign of anything wrong. Windows shows a warning for any small program that has not been signed by a company that paid for a certificate — which no free community project does.</p>",
       "<p>If you would rather check for yourself: every release is built automatically by GitHub from the public source code, and each download is published with a checksum and a build record that proves which code produced it. Both are on the release page.</p>"] },
  { q:"Do my friends need it too, for multiplayer?",
    a:["<p>For nearly everything, <strong>no</strong> — you can install it and play with people who have not. Each feature on this page carries a badge saying exactly which case it falls into, so you never have to guess.</p>",
       "<p>Only two things need more than that: adding AI opponents to a lobby, where just the <strong>host</strong> needs the patch, and anything marked <strong>everyone needs it</strong>, which today is nothing in the released version.</p>"] },
  { q:"Will it work with my existing saved games?",
    a:["<p>Yes. Saves, campaign progress and custom maps all keep working, with or without the patch installed. Nothing about the save format changes.</p>"] },
  { q:"Which version of Stronghold 2 does it need?",
    a:["<p>The Steam version, v1.5.0 — the current one. Disc and other releases of the game are built differently and are not supported.</p>"] },
  { q:"Where exactly do I put the file?",
    a:["<p>In the folder the game itself lives in — the one containing <code>Stronghold2.exe</code>. The Install section above shows how to find it in two clicks from Steam.</p>",
       "<p>Not in a subfolder, not in your documents, not in the saves folder. Next to the game.</p>"] },
  { q:"Do I have to use the settings file?",
    a:["<p>No. Without it everything works with sensible defaults. It is there if you want to change the keys or switch on the optional camera features, and every option is explained inside the file itself.</p>",
       "<p>If it is only the keys you want to change, you never need to open it at all — press <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>O</kbd> in game instead.</p>"] },
  { q:"I found a bug, or the game still crashes. What should I do?",
    a:["<p>Please report it — crash reports are what make these fixes possible in the first place.</p>",
       "<p>Say what you were doing when it happened and, if you can, how somebody else could make it happen again. &ldquo;It crashed when I fired the catapult at knights getting on horses&rdquo; can be fixed; &ldquo;it crashed randomly&rdquo; usually cannot.</p>",
       "<p>Windows also keeps a record of every crash, and it helps enormously. Press <kbd>Win</kbd>+<kbd>R</kbd>, type <code>eventvwr</code>, press Enter, then open <strong>Windows Logs &rarr; Application</strong> and find the red Error entry from around the time it happened. Copy the text and paste it into your report.</p>"] }
];
