#include "fps.h"
#include <iostream>

// Apply the double FPS patch
bool ApplyDoubleFpsPatch(uintptr_t base) {
	std::cout << "Applying double FPS patch..." << std::endl;
	
	// Calculate the addresses to patch
	uintptr_t addr1 = base + 0x188A6D;
	uintptr_t addr2 = base + 0x18557E;
	
	// Prepare the patch bytes: 0D200000 (4 bytes)
	unsigned char patchBytes[4] = { 0x0D, 0x20, 0x00, 0x00 };
	
	// Apply the patches
	if (!WriteMemory(addr1, patchBytes, 4)) {
		std::cout << "Failed to patch address 1 (0x" << std::hex << addr1 << ")" << std::dec << std::endl;
		return false;
	}
	
	if (!WriteMemory(addr2, patchBytes, 4)) {
		std::cout << "Failed to patch address 2 (0x" << std::hex << addr2 << ")" << std::dec << std::endl;
		return false;
	}
	
	std::cout << "Double FPS patch applied successfully!" << std::endl;
	std::cout << "  Patched 0x" << std::hex << addr1 << std::dec << std::endl;
	std::cout << "  Patched 0x" << std::hex << addr2 << std::dec << std::endl;
	return true;
}
