#pragma once
#include <cstdint>
#include <windows.h>

// Fail closed before speculative observer reads. MEM_COMMIT alone does not
// imply read access. In particular, never consume a game's stack guard page.
inline bool observerReadable(const void *address, size_t len) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (!start || !len || len > UINTPTR_MAX - start) return false;
    const uintptr_t end = start + len;
    uintptr_t cur = start;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void *>(cur), &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return false;
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & readable)) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= cur) return false;
        cur = next;
    }
    return true;
}
