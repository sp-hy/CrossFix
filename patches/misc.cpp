#include "misc.h"
#include <iostream>

// Apply the vertical border remover patch
bool ApplyVerticalBorderRemoverPatch(uintptr_t base) {
  // Calculate the address to patch
  uintptr_t addr = base + 0xE2F44C;

  // Prepare the patch value: write a float of 0.0
  float patchValue = 0.0f;

  // Apply the patch
  if (!WriteMemory(addr, &patchValue, sizeof(float))) {
    std::cout << "Failed to patch vertical border remover address (0x"
              << std::hex << addr << ")" << std::dec << std::endl;
    return false;
  }

#ifdef _DEBUG
  std::cout << "  Patched 0x" << std::hex << addr << std::dec
            << " with float value 0.0" << std::endl;
#endif
  return true;
}
