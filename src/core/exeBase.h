#pragma once
#include <cstdint>
#include <windows.h>

// Load address of Stronghold2.exe. Every patch site is this plus an RVA; the
// exe is base-relocated, so never assume the preferred 0x400000.
inline uintptr_t exeBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL)); }
