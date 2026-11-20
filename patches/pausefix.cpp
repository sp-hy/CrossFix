#include "pausefix.h"
#include "../utils/rde_symbols.h"
#include <iostream>

// Apply the disable pause on focus loss patch
bool ApplyDisablePausePatch(uintptr_t base) {
  // Calculate the address to patch
  uintptr_t addr = base + rde::sys_window_proc_plus_86;

  // Prepare the patch bytes: change 0F 85 (JNE) to 0F 84 (JE)
  // This inverts the logic so the game doesn't pause when losing focus
  const unsigned char originalBytes[2] = {0x0F, 0x85};
  unsigned char patchBytes[2] = {0x0F, 0x84};

  if (!VerifyBytes(addr, originalBytes, 2, "No focus pause", patchBytes)) {
    return false;
  }

  // Apply the patch
  if (!WriteMemory(addr, patchBytes, 2)) {
    std::cout << "Failed to patch address (0x" << std::hex << addr << ")"
              << std::dec << std::endl;
    return false;
  }

  std::cout << "[Mod] No focus pause" << std::endl;
#ifdef _DEBUG
  std::cout << "  Patched 0x" << std::hex << addr << std::dec << std::endl;
#endif
  return true;
}
