# tools

`win/sh2-map-unlocker.exe` — makes a Stronghold 2 map editable. Not part of
`d3d9.dll`.

Stronghold 2 marks official content with an `author` property in the plain
header block of a `.s2m` / `.sh2` file. The map browser hides an entry from the
editor's load list when that key is **present** — the value is never compared —
so removing it makes a map editable and adding it back makes it read-only.

## Building

```bash
make tool          # -> tools/win/sh2-map-unlocker.exe (~145 KB)
```

Cross-compiled from WSL with the same MinGW toolchain as the DLL, statically
linked and stripped: one dependency-free file, no Python, no runtime, no
installer. The build is reproducible — identical source and toolchain give a
bit-identical exe.

## Using it

**Drag a `.s2m` onto it**, or double-click it and pick a map. It then

- writes `<name> editable.s2m` into `Documents\Stronghold 2\Maps` and tells you
  to open it in the map editor — the original is never modified;
- offers to make a map read-only instead, if the map you gave it is already
  editable;
- asks before replacing an existing copy;
- offers to open the maps folder when it's done.

It finds the Documents folder through the shell, so a OneDrive-redirected
Documents works, and it creates `Stronghold 2\Maps` if the game hasn't yet.

## Why the copy must live in the user maps folder

This is not cosmetic. A map list entry stores only a filename, and the editor
resolves it against `Documents\Stronghold 2\Maps` — an unlocked map left in the
game's install `maps\` folder is listed in the editor but **opens as empty
terrain**. That is why the tool writes there rather than beside the original.

**The source can be anywhere.** Locked maps turn up in the user maps folder too
(a downloaded map that descends from official content keeps its author marker),
and the game's install `maps\` folder is only one possible origin. Give the tool
a map from any folder — install, Documents, Downloads — and the copy still lands
in the user maps folder. When the source is already there, the copy simply
appears beside it under the new name; nothing is overwritten and the original
stays listed.

## Safety properties

- The original file is never modified by an unlock.
- Every write is re-parsed and the compressed body compared byte-for-byte before
  the file is put in place, so a bad parse aborts instead of producing a corrupt
  map; writes go through a temp file and an atomic rename.
- A file whose body is under 16 KB is rejected as truncated. The smallest real
  body across 122 shipped, community and save files is 123,421 bytes. A header
  can be perfectly intact while the body is missing, so parsing alone does not
  prove a file is whole.
- Unlock and lock round-trip to a byte-identical file, so locking is exactly
  reversible and no backups are needed.

Caveat: re-saving a map in the in-game editor re-adds `author = Firefly`
whenever the map descends from official content, because that flag is also
serialized inside the compressed body. That is usually what you want — the
finished map ends up read-only again — but unlock it again for another pass.

## SmartScreen and antivirus

Two different warnings, with different causes:

- **SmartScreen** — blue box, "Windows protected your PC / unrecognized app",
  with Run anyway behind *More info*. This is reputation, not detection. It
  appears because the exe is unsigned and new. Only a code-signing certificate
  (or accumulated download reputation) removes it, and **reputation is tied to
  the file hash, so every rebuild starts over**.
- **Defender antivirus** — red, names a threat, quarantines the file. That is a
  heuristic false positive. Submit the exe to Microsoft's Security Intelligence
  portal; those are usually cleared within days, and it is free. Small,
  statically linked 32-bit MinGW binaries get flagged disproportionately,
  because that combination is common in malware.

The version resource, manifest and icon exist partly for this: an exe carrying
no metadata at all is the worst case for both, and it makes the SmartScreen
dialog show a real product name instead of a blank "Unknown publisher". The
manifest deliberately requests `asInvoker` — the tool only writes into the
user's own Documents folder, and an elevation prompt would look worse.

For distribution:

- Publish the SHA-256 next to the download.
- The build is byte-reproducible, so anyone with the source and the same
  toolchain can rebuild and compare hashes. For a public-domain community tool
  that is a stronger guarantee than a signature most players cannot check.
- Ship it inside the patch archive people already trust rather than as a
  separate loose download.
- Downloaded files carry a Mark-of-the-Web that survives zip extraction; users
  can clear it with right-click, Properties, Unblock. Never tell people to
  disable Defender.

Free signing routes worth checking, since terms change: SignPath Foundation
offers free certificates to open-source projects, and Microsoft's Azure Trusted
Signing has a low-cost individual tier with eligibility requirements. An EV
certificate grants SmartScreen trust immediately but is expensive and needs a
hardware token. A self-signed certificate does nothing for SmartScreen.

## Regression testing

```bash
make tool-test     # -> tools/win/test-s2m.exe
./tools/win/test-s2m.exe unlock "<in.s2m>" "<out.s2m>"
./tools/win/test-s2m.exe lock   "<out.s2m>" "<roundtrip.s2m>"
```

A console harness over the same parse/rebuild/verify code, so the file handling
can be checked without clicking through dialogs. The invariant is that unlock
followed by lock reproduces the input byte-for-byte; run it over the whole maps
folder to check a change against every real map at once.

Note that WSL passes Linux paths straight through to a Windows binary, so use
`wslpath -w` when driving the exe from a WSL shell.

Format and offset details are in `CLAUDE.md`, section
"`.s2m` / `.sh2` File Header".
