#pragma once
#include <Windows.h>
#include <cstdint>

// Aspect ratio constants
constexpr float BASE_ASPECT_RATIO = 4.0f / 3.0f;
constexpr float WIDESCREEN_THRESHOLD = 1.4f;

// Helper function to write memory with proper protection
bool WriteMemory(uintptr_t address, const void *data, size_t size);

// Install JMP (0xE9) at addr, pad remainder with NOPs. Returns addr after
// overwrite, or 0 on failure.
uintptr_t InstallJmpHook(uintptr_t addr, void *hookFunc, int totalSize);

// Install CALL (0xE8) at addr, pad remainder with NOPs. Returns addr after
// overwrite, or 0 on failure.
uintptr_t InstallCallHook(uintptr_t addr, void *hookFunc, int totalSize);

// Redirect a 4-byte absolute address operand to point at newTarget.
bool RedirectOperand(uintptr_t operandAddr, const void *newTarget);

// Safe read of a game byte (returns defaultVal if addr is 0).
uint8_t ReadGameByte(uintptr_t addr, uint8_t defaultVal = 0);

// Thread-safe D3D11 vtable hook (handles VirtualProtect, MemoryBarrier,
// FlushInstructionCache).
bool InstallVtableHook(void **vtable, int vtableSize, int index,
                       void *hookFunc, volatile void **outOriginal,
                       volatile LONG *outReady);
