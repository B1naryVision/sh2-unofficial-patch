# Antivirus False-Positive Reports

**Status:** Process document. The binary-side work it refers to landed in v0.7.0
(version resource, ASLR/DEP bits, stripped release build — see `src/d3d9.rc`
and the `Sanity-check the binary` step in `.github/workflows/release.yml`).

---

## Why this happens

The patch is a `d3d9.dll` proxy that rewrites game code in memory, subclasses
the game window and polls the keyboard. Every one of those behaviours is
necessary, and every one of them is also a line item in the heuristics
antivirus engines are built around. The combination — DLL search-order load,
runtime code patching, input hooking, and (since the `!ping` command) peer
network traffic — is close to a textbook remote-access-tool profile.

This cannot be engineered away without dropping the features, and it should not
be worked around by obfuscating or packing the binary: that raises the
heuristic score rather than lowering it, and it destroys the one asset the
project actually has, which is that anyone can rebuild the exact shipped file
from public source.

So the strategy is provenance, not evasion:

1. Make the binary look like real software (version resource, ASLR/DEP,
   stripped release build) — done in v0.7.0.
2. Publish verifiable build provenance — the release workflow already emits
   SHA-256 sums and a GitHub build-provenance attestation.
3. Report each false positive to the vendor that raised it.
4. Long term, code signing. An OV certificate removes most of the heuristic
   weight; an EV certificate additionally grants SmartScreen reputation
   immediately. Nothing below is a substitute for it.

## Before submitting: collect these

A report without the first two items is not actionable and will usually be
closed.

| Item | Where it comes from |
| --- | --- |
| Exact detection name | The reporting player's AV alert or quarantine log |
| Which file | `d3d9.dll`, `sh2-map-unlocker.exe`, or the release `.zip` |
| SHA-256 of that exact file | `SHA256SUMS.txt` on the release page |
| VirusTotal link | Upload the file to VirusTotal; the report URL shows every engine |
| Vendor, product and engine/definition version | The player's AV UI |
| Download URL the player used | Release page, or the site |

The detection name is what separates a generic machine-learning verdict
(`Wacatac`, `Wacapew`, `Zusy`, anything suffixed `!ml`) from a specific
signature. Generic ML verdicts are the common case for unsigned MinGW binaries
and are usually cleared quickly. A named family is rarer and worth a closer
look at the build before reporting.

Check the VirusTotal result before submitting. One or two engines out of ~70 is
noise, and often clears itself on the next definition update. Ten or more is a
cluster worth reporting to each of them.

## Where to submit

- **Microsoft Defender** — the one that matters for most players. Use the
  Microsoft Security Intelligence file submission portal and choose the
  **software developer** submission type, not the end-user one; it routes to
  analysts and expects exactly the detail below.
- **Other vendors** — each runs its own form or mailbox. Search
  "<vendor> false positive submission". Submitting to one vendor does nothing
  for the others; they share samples, not verdicts.

Turnaround is typically a few days. A cleared detection applies to that exact
file hash, so **every release needs its own report** until the binary is
signed. That is the main practical argument for signing.

---

## Report template

Fill the bracketed fields. Keep it factual and specific; disclose the
suspicious behaviours yourself rather than letting an analyst discover them
unexplained.

```text
Subject: False positive report - Stronghold 2 Unofficial Patch (open source)

SUMMARY
[PRODUCT] detects [FILENAME] as [DETECTION NAME]. This is a false positive.
The file is part of an open-source, public-domain community patch for the 2005
video game Stronghold 2. It is built in public from public source, and the
exact shipped binary can be independently reproduced and cryptographically
verified against the commit it was built from (see VERIFICATION below).

FILE
  Name:          [d3d9.dll | sh2-map-unlocker.exe | sh2-unofficial-patch-vX.Y.Z.zip]
  SHA-256:       [HASH from SHA256SUMS.txt]
  Version:       [X.Y.Z]
  Download URL:  https://github.com/B1naryVision/sh2-unofficial-patch/releases
  VirusTotal:    [VT REPORT URL]

DETECTION
  Product:       [e.g. Microsoft Defender Antivirus]
  Detection:     [exact detection name]
  Engine/defs:   [version, if the reporter has it]
  First seen:    [date]

WHAT THE SOFTWARE IS
An unofficial community bug-fix and quality-of-life patch for Stronghold 2
(Firefly Studios, 2005). It fixes crashes still present in the final official
patch and adds optional conveniences such as keyboard shortcuts and camera
settings. It is released into the public domain under the Unlicense. It is not
commercial, contains no advertising, no bundled software, no installer and no
telemetry.

Source:  https://github.com/B1naryVision/sh2-unofficial-patch
Licence: Unlicense (public domain)

WHY IT LOOKS SUSPICIOUS TO A SCANNER
Disclosed deliberately; all of it is visible in the public source.

1. The file is named d3d9.dll and is placed in the game's own folder, so the
   game loads it instead of the system Direct3D 9 library. It resolves the
   real system d3d9.dll via GetSystemDirectoryA + LoadLibraryA +
   GetProcAddress and forwards all 13 exports to it. This resembles DLL
   search-order hijacking; it is the standard and only practical way to load a
   mod into a closed-source game of this era.
   Source: src/proxy/d3d9Proxy.cpp

2. It rewrites instructions in the loaded game executable using VirtualProtect
   + memcpy + FlushInstructionCache. This resembles runtime code injection; it
   is how a binary patch applies fixes to a game whose source is unavailable.
   Every patch site is documented with its offset and before/after bytes under
   docs/.

3. It subclasses the game window (SetWindowLongW / CallWindowProcW) and polls
   GetAsyncKeyState and GetForegroundWindow. This resembles a keylogger. It
   drives the patch's optional in-game hotkeys and the panel that lets players
   rebind them. No keystroke is recorded, stored or transmitted; the polling
   tests specific configured virtual-key codes on the game's own frame tick.
   Source: src/core/hotkey.cpp, src/core/keybindWidget.cpp

4. In multiplayer it can send small packets to other players in the same match
   (the "!ping" chat command, which reports round-trip latency). It opens no
   sockets of its own — it calls the game's existing peer-transport function
   through the game's own import table.
   Source: src/patches/pingCommand.cpp

5. It reads and writes sh2-unofficial-patch.ini beside itself, to persist the
   user's own settings.

WHAT IT DOES NOT DO
Verifiable from the import table of the shipped binary, which references only
GDI32.dll, KERNEL32.dll, USER32.dll and msvcrt.dll:

  - No registry access at all (advapi32 is not imported), so no persistence
    via Run keys, services or scheduled tasks.
  - No sockets, HTTP or downloads (ws2_32, wininet and urlmon are not
    imported).
  - No manipulation of other processes (no OpenProcess, WriteProcessMemory or
    CreateRemoteThread). It only modifies the process that loaded it.
  - No process creation (no CreateProcess or ShellExecute).
  - No elevation request, no driver, no service.
  - No packing, encryption or obfuscation. The binary is an ordinary
    unpacked PE; all sections are plainly readable.
  - It does not modify any game file on disk. Uninstalling is deleting the DLL.

VERIFICATION
  - Source:      https://github.com/B1naryVision/sh2-unofficial-patch
  - Build:       GitHub Actions, .github/workflows/release.yml, using the
                 i686 MinGW-w64 cross-compiler. The build is deterministic
                 (the PE timestamp is zeroed), so rebuilding the tagged commit
                 reproduces the published file byte for byte.
  - Checksums:   SHA256SUMS.txt is published with every release.
  - Attestation: each release artifact carries a GitHub build-provenance
                 attestation, cryptographically binding the binary to the
                 workflow run and source commit that produced it. Verify with:
                   gh attestation verify [FILENAME] \
                     --repo B1naryVision/sh2-unofficial-patch

CONTACT
  [NAME OR HANDLE]
  [EMAIL]
  Issue tracker: https://github.com/B1naryVision/sh2-unofficial-patch/issues
```

## After submitting

- Record the vendor, date, file hash and ticket reference, so a recurrence on
  a later release can cite the earlier cleared report.
- If the detection persists after the vendor says it is cleared, ask the
  reporting player to update definitions before retesting — the verdict is
  usually cached locally.
- Add a short note to the site's FAQ telling players what to do, and asking
  them to send the **exact detection name**. Without that string, no report
  can be filed.
