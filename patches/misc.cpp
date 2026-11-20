#include "misc.h"
#include "../utils/rde_symbols.h"
#include <iostream>

// Apply the vertical border remover patch
bool ApplyVerticalBorderRemoverPatch(uintptr_t base) {
  // Calculate the address to patch
  uintptr_t addr = base + rde::render_crop_band;

  // Prepare the patch value: write a float of 0.0
  float patchValue = 0.0f;
  const int32_t originalValue = 8;

  if (!VerifyBytes(addr, &originalValue, sizeof(originalValue),
                   "Vertical border remover", &patchValue)) {
    return false;
  }

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
