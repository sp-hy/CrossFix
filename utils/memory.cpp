#include "memory.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

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

static std::string HexString(const void *data, size_t size) {
  std::string text;
  char byteText[4];
  for (size_t i = 0; i < size; ++i) {
    sprintf_s(byteText, "%02X", static_cast<const uint8_t *>(data)[i]);
    if (i > 0)
      text += ' ';
    text += byteText;
  }
  return text;
}

bool VerifyBytes(uintptr_t addr, const void *expected, size_t size,
                 const char *name, const void *alt) {
  const void *actual = reinterpret_cast<const void *>(addr);
  if (memcmp(actual, expected, size) == 0)
    return true;
  if (alt && memcmp(actual, alt, size) == 0)
    return true;

  std::cout << "[Check] " << name << ": unexpected bytes at 0x" << std::hex
            << addr << std::dec << ", expected " << HexString(expected, size)
            << ", found " << HexString(actual, size) << ". Patch skipped."
            << std::endl;
  return false;
}

bool VerifyOperand(uintptr_t addr, uintptr_t expectedAddress,
                   const char *name) {
  uint32_t expected = static_cast<uint32_t>(expectedAddress);
  return VerifyBytes(addr, &expected, sizeof(expected), name);
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

bool VerifyCallTo(uintptr_t site, uintptr_t target, const char *name) {
  uint8_t expected[5] = {0xE8};
  const int32_t relative = static_cast<int32_t>(target - (site + 5));
  memcpy(expected + 1, &relative, sizeof(relative));
  return VerifyBytes(site, expected, sizeof(expected), name);
}

void *MakeTrampoline(uintptr_t entry, const uint8_t *prologue, size_t size) {
  constexpr size_t kJumpSize = 5;
  uint8_t *trampoline = static_cast<uint8_t *>(
      VirtualAlloc(nullptr, size + kJumpSize, MEM_COMMIT | MEM_RESERVE,
                   PAGE_EXECUTE_READWRITE));
  if (!trampoline)
    return nullptr;
  memcpy(trampoline, prologue, size);
  trampoline[size] = 0xE9;
  const int32_t back = static_cast<int32_t>(
      (entry + size) -
      reinterpret_cast<uintptr_t>(trampoline + size + kJumpSize));
  memcpy(trampoline + size + 1, &back, sizeof(back));
  FlushInstructionCache(GetCurrentProcess(), trampoline, size + kJumpSize);
  return trampoline;
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
