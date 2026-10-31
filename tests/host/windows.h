#pragma once

// Host-build stand-in for the only Win32 calls the pure replay modules make
// (replayRetime.cpp's working storage), so tests/replayTests.cpp can build them
// natively under AddressSanitizer. Never on the include path of the DLL.

#include <cstddef>
#include <cstdlib>

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04

inline void *VirtualAlloc(void *, size_t bytes, unsigned, unsigned) { return calloc(1, bytes); }

inline int VirtualFree(void *p, size_t, unsigned) {
    free(p);
    return 1;
}
