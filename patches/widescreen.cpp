#include "widescreen.h"
#include <iostream>

// Widescreen hook function - this replaces the 8 bytes at both hook locations
// Original bytes: 8B 46 60 99 03 F8 13 CA
// Which is: mov eax,[esi+60]; cdq; add edi,eax; adc ecx,edx
__declspec(naked) void WidescreenHook() {
	__asm {
		// Widescreen modification code
		push eax
		push edx
		mov eax, edi
		imul eax, eax, 3
		sar eax, 2
		mov edi, eax
		pop edx
		pop eax
		
		// Now execute the original instructions we replaced
		mov eax, [esi+0x60]  // original: read OFX
		cdq                  // original: sign extend
		add edi, eax         // original: add OFX
		adc ecx, edx         // original: add with carry
		ret
	}
}

// Apply the widescreen patches
bool ApplyWidescreenPatch(uintptr_t base) {
	// Calculate the addresses to patch
	uintptr_t addr1 = base + 0x18EE7B;
	uintptr_t addr2 = base + 0x18AD25;
	
	// Calculate relative call address for addr1
	int32_t relativeAddr1 = (int32_t)((uintptr_t)WidescreenHook - (addr1 + 5));
	// Calculate relative call address for addr2
	int32_t relativeAddr2 = (int32_t)((uintptr_t)WidescreenHook - (addr2 + 5));
	
	// Prepare the patch bytes for addr1: call + 3 nops
	unsigned char patch1[8] = {
		0xE8, // call
		(unsigned char)(relativeAddr1 & 0xFF),
		(unsigned char)((relativeAddr1 >> 8) & 0xFF),
		(unsigned char)((relativeAddr1 >> 16) & 0xFF),
		(unsigned char)((relativeAddr1 >> 24) & 0xFF),
		0x90, 0x90, 0x90 // nop nop nop
	};
	
	// Prepare the patch bytes for addr2: call + 3 nops
	unsigned char patch2[8] = {
		0xE8, // call
		(unsigned char)(relativeAddr2 & 0xFF),
		(unsigned char)((relativeAddr2 >> 8) & 0xFF),
		(unsigned char)((relativeAddr2 >> 16) & 0xFF),
		(unsigned char)((relativeAddr2 >> 24) & 0xFF),
		0x90, 0x90, 0x90 // nop nop nop
	};
	
	// Apply the patches
	if (!WriteMemory(addr1, patch1, 8)) {
		std::cout << "Failed to patch address 1 (0x" << std::hex << addr1 << ")" << std::dec << std::endl;
		return false;
	}
	
	if (!WriteMemory(addr2, patch2, 8)) {
		std::cout << "Failed to patch address 2 (0x" << std::hex << addr2 << ")" << std::dec << std::endl;
		return false;
	}
	
	std::cout << "Widescreen patch applied successfully!" << std::endl;
	std::cout << "  Patched 0x" << std::hex << addr1 << std::dec << std::endl;
	std::cout << "  Patched 0x" << std::hex << addr2 << std::dec << std::endl;
	return true;
}
