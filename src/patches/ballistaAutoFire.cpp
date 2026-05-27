#include "ballistaAutoFire.h"
#include <windows.h>
#include <cstring>

void installBallistaAutoFire() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    DWORD oldProtect;

    // Patch 1 — NOP the deployment-flag jne (6 bytes, RVA 0x180c4f).
    // cmp byte [esi+0x308],0 precedes this.  [this+0x308] starts at 0x00 on
    // first deployment and is set to 0x01 by the fire path after the first
    // shot.  From tick 2 onward the jne would be taken → manual-target handler
    // → exits without re-firing.  NOP it so execution always falls through.
    // Before: 0f 85 e4 02 00 00  (jne +0x2e4 → 0x180f39)
    {
        static const unsigned char p[] = { 0x90,0x90,0x90,0x90,0x90,0x90 };
        void *site = (void *)(base + 0x180c4f);
        VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(site, p, 6);
        VirtualProtect(site, 6, oldProtect, &oldProtect);
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
        static const unsigned char p[] = { 0x90,0x90,0x90,0x90,0x90,0x90 };
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
        static const unsigned char p[] = { 0xeb };
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
        unsigned char p[5] = { 0xe8,
            (unsigned char)(disp),
            (unsigned char)(disp >> 8),
            (unsigned char)(disp >> 16),
            (unsigned char)(disp >> 24) };
        VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(site, p, 5);
        VirtualProtect(site, 5, oldProtect, &oldProtect);
    }
}
