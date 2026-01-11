#include "widescreen.h"
#include <iostream>

// 16:9 widescreen hook - multiply by 3/4
__declspec(naked) void WidescreenHook_16_9() {
	__asm {
		// Widescreen modification code for 16:9
		push eax
		push edx
		mov eax, edi
		imul eax, eax, 3
		sar eax, 2           // divide by 4 (shift right 2 bits)
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

// 21:9 ultrawide hook - multiply by 4/7
__declspec(naked) void WidescreenHook_21_9() {
	__asm {
		// Widescreen modification code for 21:9
		push eax
		push edx
		push ebx              // need to save ebx for division
		
		mov eax, edi          // eax = result * IR1
		imul eax, eax, 4      // eax = (result * IR1) * 4
		mov ebx, 7            // divisor = 7
		cdq                   // sign extend eax into edx:eax for division
		idiv ebx              // eax = (result * IR1) * 4 / 7
		mov edi, eax          // edi = modified value
		
		pop ebx               // restore ebx
		pop edx               // restore edx
		pop eax               // restore eax
		
		// Execute the original instructions we replaced
		mov eax, [esi+0x60]   // original: read OFX
		cdq                   // original: sign extend
		add edi, eax          // original: add OFX
		adc ecx, edx          // original: add with carry
		ret
	}
}

// 32:9 super ultrawide hook - multiply by 3/8
__declspec(naked) void WidescreenHook_32_9() {
	__asm {
		// Widescreen modification code for 32:9
		push eax
		push edx
		
		mov eax, edi          // eax = result * IR1
		imul eax, eax, 3      // eax = (result * IR1) * 3
		sar eax, 3            // eax = (result * IR1) * 3 / 8 (shift right 3 bits)
		mov edi, eax          // edi = modified value
		
		pop edx               // restore edx
		pop eax               // restore eax
		
		// Execute the original instructions we replaced
		mov eax, [esi+0x60]   // original: read OFX
		cdq                   // original: sign extend
		add edi, eax          // original: add OFX
		adc ecx, edx          // original: add with carry
		ret
	}
}

// Apply the widescreen patches
bool ApplyWidescreenPatch(uintptr_t base, WidescreenMode mode) {
	// Select the appropriate hook function based on mode
	void* hookFunction = nullptr;
	const char* modeName = "";
	
	switch (mode) {
		case WIDESCREEN_16_9:
			hookFunction = (void*)WidescreenHook_16_9;
			modeName = "16:9";
			break;
		case WIDESCREEN_21_9:
			hookFunction = (void*)WidescreenHook_21_9;
			modeName = "21:9";
			break;
		case WIDESCREEN_32_9:
			hookFunction = (void*)WidescreenHook_32_9;
			modeName = "32:9";
			break;
		default:
			std::cout << "Invalid widescreen mode!" << std::endl;
			return false;
	}
	
	// Calculate the addresses to patch
	uintptr_t addr1 = base + 0x18EE7B;
	uintptr_t addr2 = base + 0x18AD25;
	
	// Calculate relative call address for addr1
	int32_t relativeAddr1 = (int32_t)((uintptr_t)hookFunction - (addr1 + 5));
	// Calculate relative call address for addr2
	int32_t relativeAddr2 = (int32_t)((uintptr_t)hookFunction - (addr2 + 5));
	
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
	
	std::cout << "Widescreen patch (" << modeName << ") applied successfully!" << std::endl;
#ifdef _DEBUG
	std::cout << "  Patched 0x" << std::hex << addr1 << std::dec << std::endl;
	std::cout << "  Patched 0x" << std::hex << addr2 << std::dec << std::endl;
#endif
	return true;
}
