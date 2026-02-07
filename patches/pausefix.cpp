#include "pausefix.h"
#include <iostream>

// Apply the disable pause on focus loss patch
bool ApplyDisablePausePatch(uintptr_t base) {
  // Calculate the address to patch
  uintptr_t addr = base + 0x184096;

  // Prepare the patch bytes: change 0F 85 (JNE) to 0F 84 (JE)
  // This inverts the logic so the game doesn't pause when losing focus
  unsigned char patchBytes[2] = {0x0F, 0x84};

  // Apply the patch
  if (!WriteMemory(addr, patchBytes, 2)) {
    std::cout << "Failed to patch address (0x" << std::hex << addr << ")"
              << std::dec << std::endl;
    return false;
  }

  std::cout << "Pause fix patch applied" << std::endl;
#ifdef _DEBUG
  std::cout << "  Patched 0x" << std::hex << addr << std::dec << std::endl;
#endif
  return true;
}
