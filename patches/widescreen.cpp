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

// Auto-detect aspect ratio and apply the appropriate widescreen patch
// Optionally returns the detected mode via outMode pointer
bool ApplyWidescreenPatchAuto(uintptr_t base, WidescreenMode* outMode) {
	// Memory addresses for screen resolution
	// CHRONOCROSS.exe+E2F488 - screen res X
	// CHRONOCROSS.exe+E2F48C - screen res Y
	uintptr_t resXAddr = base + 0xE2F488;
	uintptr_t resYAddr = base + 0xE2F48C;
	
	// Read the resolution values from memory with retry logic
	// The game might not have initialized these values yet
	int resX = 0;
	int resY = 0;
	const int maxRetries = 10;
	const int retryDelayMs = 100;
	
	for (int attempt = 0; attempt < maxRetries; attempt++) {
		try {
			resX = *(int*)resXAddr;
			resY = *(int*)resYAddr;
			
			// Check if we got valid values
			if (resX > 0 && resY > 0) {
				break; // Success!
			}
			
			// Values not initialized yet, wait and retry
			if (attempt < maxRetries - 1) {
#ifdef _DEBUG
				std::cout << "Screen resolution not initialized yet (attempt " << (attempt + 1) << "/" << maxRetries << "), retrying..." << std::endl;
#endif
				Sleep(retryDelayMs);
			}
		} catch (...) {
			std::cout << "Failed to read screen resolution from memory!" << std::endl;
			return false;
		}
	}
	
	// Validate the resolution values
	if (resX <= 0 || resY <= 0) {
		std::cout << "Invalid screen resolution detected: " << resX << "x" << resY << std::endl;
		std::cout << "The game may not have initialized the resolution yet. Try setting widescreen_mode manually (1=16:9, 2=21:9, 3=32:9)" << std::endl;
		return false;
	}
	
#ifdef _DEBUG
	std::cout << "Detected screen resolution: " << resX << "x" << resY << std::endl;
#endif
	
	// Calculate aspect ratio
	float aspectRatio = (float)resX / (float)resY;
	
#ifdef _DEBUG
	std::cout << "Calculated aspect ratio: " << aspectRatio << std::endl;
#endif
	
	// Determine the appropriate widescreen mode based on aspect ratio
	WidescreenMode mode;
	const char* modeName = "";
	
	// 16:9 = 1.777...
	// 21:9 = 2.333...
	// 32:9 = 3.555...
	
	// Use tolerance ranges to determine the best match
	if (aspectRatio >= 3.2f) {
		// 32:9 or wider (e.g., 3840x1080, 5120x1440)
		mode = WIDESCREEN_32_9;
		modeName = "32:9";
	} else if (aspectRatio >= 2.1f) {
		// 21:9 ultrawide (e.g., 2560x1080, 3440x1440)
		mode = WIDESCREEN_21_9;
		modeName = "21:9";
	} else if (aspectRatio >= 1.6f) {
		// 16:9 widescreen (e.g., 1920x1080, 2560x1440, 3840x2160)
		mode = WIDESCREEN_16_9;
		modeName = "16:9";
	} else {
		// Aspect ratio is too narrow for widescreen (4:3 or similar)
		std::cout << "Aspect ratio " << aspectRatio << " is not widescreen (< 16:9). Widescreen patch not applied." << std::endl;
		return false;
	}
	
	std::cout << "Auto-detected widescreen mode: " << modeName << std::endl;
	
	// Return the detected mode if requested
	if (outMode != nullptr) {
		*outMode = mode;
	}
	
	// Apply the patch with the detected mode
	return ApplyWidescreenPatch(base, mode);
}

// Auto-detect aspect ratio and apply the appropriate widescreen patch
bool ApplyWidescreenPatchAuto(uintptr_t base) {
	return ApplyWidescreenPatchAuto(base, nullptr);
}

// Get the 2D ratio for a given widescreen mode
float GetWidescreenRatio2D(WidescreenMode mode) {
	switch (mode) {
		case WIDESCREEN_AUTO:
			return 0.75f;  // Default to 16:9 if called with AUTO (shouldn't happen)
		case WIDESCREEN_16_9:
			return 0.75f;  // 3/4
		case WIDESCREEN_21_9:
			return 0.571428f;  // 4/7
		case WIDESCREEN_32_9:
			return 0.375f;  // 3/8
		default:
			return 0.75f;  // Default to 16:9
	}
}
