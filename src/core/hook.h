#pragma once
#include <cstddef>
#include <windows.h>

// The patch DLL's own module handle, captured in DllMain. Use this (not
// GetModuleHandleA with a DLL name) wherever an HINSTANCE for this DLL is
// needed, e.g. window class registration.
extern HMODULE g_patchModule;

void installHook(void *targetAddress, void *detourFunction, size_t instructionLength);
