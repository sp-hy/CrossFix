#include "memory.h"
#include <cstring>

// Helper function to write memory with proper protection
bool WriteMemory(uintptr_t address, const void *data, size_t size) {
  DWORD oldProtect;
  if (!VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE,
                      &oldProtect)) {
    return false;
  }
  memcpy((void *)address, data, size);
  VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
  return true;
}

uintptr_t InstallJmpHook(uintptr_t addr, void *hookFunc, int totalSize) {
  if (totalSize < 5)
    return 0;

  uint8_t buf[16];
  buf[0] = 0xE9; // JMP rel32
  int32_t rel = (int32_t)((uintptr_t)hookFunc - (addr + 5));
  memcpy(&buf[1], &rel, 4);
  for (int i = 5; i < totalSize; ++i)
    buf[i] = 0x90; // NOP

  if (!WriteMemory(addr, buf, totalSize))
    return 0;
  return addr + totalSize;
}

uintptr_t InstallCallHook(uintptr_t addr, void *hookFunc, int totalSize) {
  if (totalSize < 5)
    return 0;

  uint8_t buf[16];
  buf[0] = 0xE8; // CALL rel32
  int32_t rel = (int32_t)((uintptr_t)hookFunc - (addr + 5));
  memcpy(&buf[1], &rel, 4);
  for (int i = 5; i < totalSize; ++i)
    buf[i] = 0x90; // NOP

  if (!WriteMemory(addr, buf, totalSize))
    return 0;
  return addr + totalSize;
}

bool RedirectOperand(uintptr_t operandAddr, const void *newTarget) {
  uint32_t addr = (uint32_t)(uintptr_t)newTarget;
  return WriteMemory(operandAddr, &addr, sizeof(uint32_t));
}

uint8_t ReadGameByte(uintptr_t addr, uint8_t defaultVal) {
  if (addr == 0)
    return defaultVal;
  return *(volatile uint8_t *)addr;
}

bool InstallVtableHook(void **vtable, int vtableSize, int index,
                       void *hookFunc, volatile void **outOriginal,
                       volatile LONG *outReady) {
  if (!vtable || !vtable[index])
    return false;

  DWORD oldProtect;
  if (!VirtualProtect(vtable, sizeof(void *) * vtableSize,
                      PAGE_EXECUTE_READWRITE, &oldProtect))
    return false;

  if (vtable[index] != hookFunc) {
    *outOriginal = vtable[index];
    MemoryBarrier();
    vtable[index] = hookFunc;
    FlushInstructionCache(GetCurrentProcess(), vtable,
                          sizeof(void *) * vtableSize);
    MemoryBarrier();
    InterlockedExchange(outReady, 1);
  }

  VirtualProtect(vtable, sizeof(void *) * vtableSize, oldProtect, &oldProtect);
  return true;
}
