#include "ballistaAutoFire.h"
#include <cstring>
#include <windows.h>

void installBallistaAutoFire() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    DWORD oldProtect;

    // Patch 1 — Code cave: validate manual-target cell before routing.
    //   Site: 13 bytes at RVA 0x180c48 (JMP to cave + NOP×8).
    //   Cave: 57 bytes, allocated at runtime with VirtualAlloc.
    //
    // Original code at 0x180c48: cmp byte [esi+0x308],0; jne 0x180f39
    //   [this+0x308] set to 0x01 after first shot → jne always taken from tick 2
    //   onward → manual-target handler exits → auto-fire fires once, then stops.
    //
    // Simple CMP [esi+0x198] fix (previous iteration) broke moved ballistae:
    //   movement writes the destination cell to [this+0x198]; after arrival the
    //   entity lookup in the manual handler finds the ballista at its own cell
    //   (non-NULL) so [this+0x198] is never cleared → auto-fire blocked forever.
    //
    // Cave logic ([esi+0x30]=own X, [esi+0x34]=own Y, confirmed from 0x177b90):
    //   [esi+0x198]==0              → no target, fall through to auto-fire
    //   target X == [esi+0x30]
    //     AND target Y == [esi+0x34] → movement destination: clear [esi+0x198],
    //                                   fall through to auto-fire
    //   otherwise                  → real manual target: jmp to manual handler
    //
    // Cave layout (57 bytes):
    //   +00  83 be 98 01 00 00 00  cmp dword [esi+0x198],0
    //   +07  74 2b                 jz  auto_fire (+0x34)
    //   +09  0f b7 86 98 01 00 00  movzx eax,word [esi+0x198]  (target X)
    //   +10  8b 4e 30              mov ecx,[esi+0x30]           (own X)
    //   +13  39 c8                 cmp eax,ecx
    //   +15  75 0e                 jne not_own_cell (+0x25)
    //   +17  0f b7 86 9a 01 00 00  movzx eax,word [esi+0x19a]  (target Y)
    //   +1e  8b 4e 34              mov ecx,[esi+0x34]           (own Y)
    //   +21  39 c8                 cmp eax,ecx
    //   +23  74 05                 je  is_own_cell (+0x2a)
    //   +25  e9 [rel32]            jmp manual_handler (not_own_cell)
    //   +2a  c7 86 98 01 00 00 00 00 00 00  mov dword [esi+0x198],0
    //   +34  e9 [rel32]            jmp 0x180c55 (auto_fire)
    //
    // Before (site): 80 be 08 03 00 00 00  0f 85 e4 02 00 00
    // After  (site): e9 [rel32 to cave]  + 90 90 90 90 90 90 90 90
    {
        unsigned char *cave = (unsigned char *)VirtualAlloc(
            NULL, 57, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        );
        if (!cave) {
            return;
        }

        static const unsigned char tmpl[57] = {
            0x83, 0xbe, 0x98, 0x01, 0x00, 0x00, 0x00, // +00 cmp dword [esi+0x198],0
            0x74, 0x2b, // +07 jz auto_fire
            0x0f, 0xb7, 0x86, 0x98, 0x01, 0x00, 0x00, // +09 movzx eax,word[esi+0x198]
            0x8b, 0x4e, 0x30, // +10 mov ecx,[esi+0x30]
            0x39, 0xc8, // +13 cmp eax,ecx
            0x75, 0x0e, // +15 jne not_own_cell
            0x0f, 0xb7, 0x86, 0x9a, 0x01, 0x00, 0x00, // +17 movzx eax,word[esi+0x19a]
            0x8b, 0x4e, 0x34, // +1e mov ecx,[esi+0x34]
            0x39, 0xc8, // +21 cmp eax,ecx
            0x74, 0x05, // +23 je is_own_cell
            0xe9, 0x00, 0x00, 0x00, 0x00, // +25 jmp manual_handler (disp@+26)
            0xc7, 0x86, 0x98, 0x01, 0x00, 0x00, // +2a mov dword[esi+0x198],0 ...
            0x00, 0x00, 0x00, 0x00, //     ... imm32=0
            0xe9, 0x00, 0x00, 0x00, 0x00 // +34 jmp return_site (disp@+35)
        };
        memcpy(cave, tmpl, 57);

        *(int *)(cave + 0x26) = (int)((base + 0x180f39) - ((uintptr_t)cave + 0x2a));
        *(int *)(cave + 0x35) = (int)((base + 0x180c55) - ((uintptr_t)cave + 0x39));

        void *site = (void *)(base + 0x180c48);
        uintptr_t cave_disp = (uintptr_t)cave - ((uintptr_t)site + 5);
        unsigned char p[13] = {
            0xe9,
            (unsigned char)(cave_disp),
            (unsigned char)(cave_disp >> 8),
            (unsigned char)(cave_disp >> 16),
            (unsigned char)(cave_disp >> 24),
            0x90,
            0x90,
            0x90,
            0x90,
            0x90,
            0x90,
            0x90,
            0x90
        };
        VirtualProtect(site, 13, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(site, p, 13);
        VirtualProtect(site, 13, oldProtect, &oldProtect);
    }

    // Patch 2 — NOP the player-active je (6 bytes, RVA 0x180c71).
    // FireBallista::think reads [this+0x58] (player index), looks it up in
    // the player table at 0xae8bd8, and calls 0x415f20.  That function returns
    // 0 for human players and 1 for AI.  For a human-owned ballista the je is
    // always taken → manual-target handler → exits.  Ballista::think (tower)
    // has no equivalent check.  NOP the je so execution always reaches the
    // rotation check regardless of player type.
    // Before: 0f 84 c2 02 00 00  (je +0x2c2 → 0x180f39)
    {
        static const unsigned char p[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        void *site = (void *)(base + 0x180c71);
        VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(site, p, 6);
        VirtualProtect(site, 6, oldProtect, &oldProtect);
    }

    // Patch 3 — je → jmp short (1 byte, RVA 0x180c92).
    // cmp dword [esi+0xb4], ebx precedes this.  [this+0xb4] is a float field
    // initialised to 1536.0 in the constructor and never cleared.  The je
    // (zero-flag branch) is never taken, so execution falls through to an
    // epilogue that resets [this+0x352] and returns — bypassing the tick gate
    // completely.  Change je to jmp short so execution always reaches the tick
    // gate.  The preceding xor ebx,ebx is kept; bl=0 is required by the
    // cmp byte [esi+0x352], bl inside the tick gate.
    // Before: 74  (je)   After: eb  (jmp short)
    {
        static const unsigned char p[] = {0xeb};
        void *site = (void *)(base + 0x180c92);
        VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(site, p, 1);
        VirtualProtect(site, 1, oldProtect, &oldProtect);
    }

    // Patch 4 — redirect fire-path CALL (5 bytes, RVA 0x180ef9).
    // The tick gate reaches a fire path at 0x180ef3 on T%60==0 or when
    // [this+0x352]==0.  The CALL at 0x180ef9 originally targeted 0x17f8c0
    // (a cached-target retriever that never populates its cache, returning
    // empty results).  Redirect to 0x177b90, the same proven fresh-search-
    // and-fire function used by Ballista::think (tower).  Both share the same
    // thiscall signature: ECX=this, [ESP+4]=result_buf, returns buf pointer.
    // Before: e8 c2 e9 ff ff  (call 0x17f8c0)
    // After:  e8 92 6c ff ff  (call 0x177b90)
    {
        void *site = (void *)(base + 0x180ef9);
        uintptr_t disp = (uintptr_t)(base + 0x177b90) - (uintptr_t)site - 5;
        unsigned char p[5] = {
            0xe8, (unsigned char)(disp), (unsigned char)(disp >> 8), (unsigned char)(disp >> 16),
            (unsigned char)(disp >> 24)
        };
        VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(site, p, 5);
        VirtualProtect(site, 5, oldProtect, &oldProtect);
    }
}
