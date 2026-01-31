#include "battleuimenu.h"
#include "../utils/memory.h"
#include <iostream>
#include <cstring>

static uintptr_t g_inBattleAddr = 0;

// Dynamic value for battle UI and menu aspect ratio scaling
// Base value is 1280 for 4:3, needs to be decreased with wider aspect ratios
static uint32_t g_baseRes = 1280;  // Base 1280 for 4:3

// Aspect ratio multiplier for battle UI element positioning
// 1.0 for 4:3, 1.333 for 16:9, etc.
static float g_aspectRatioMultiplier = 1.0f;

// Hook memory for the movups patch
static void* g_movupsHookMem = nullptr;

bool ApplyBattleUIAndMenuPatch(uintptr_t base) {
	g_inBattleAddr = base + 0x6A1389;
	bool success = true;

	// ============================================================
	// Patch 1: CHRONOCROSS.exe+29314 - Battle Elements Width
	// Original: adc eax,CHRONOCROSS.exe+E2F444 { (960) }
	// Instruction: 15 44F49A01 (adc eax,[abs32])
	// ============================================================
	uintptr_t addr1 = base + 0x29314 + 1; // +1 to skip opcode, points to address operand
	uint32_t newAddress1 = (uint32_t)(uintptr_t)&g_baseRes;

	if (!WriteMemory(addr1, &newAddress1, sizeof(uint32_t))) {
		std::cout << "Failed to apply battle UI/menu patch 1 (adc)" << std::endl;
		success = false;
	}

	// ============================================================
	// Patch 2: CHRONOCROSS.exe+1D21F - Battle Text Containers
	// Original: and eax,CHRONOCROSS.exe+E2F444 { (960) }
	// Instruction: 25 44F49A01 (and eax,[abs32])
	// ============================================================
	uintptr_t addr2 = base + 0x1D21F + 1; // +1 to skip opcode
	uint32_t newAddress2 = (uint32_t)(uintptr_t)&g_baseRes;

	if (!WriteMemory(addr2, &newAddress2, sizeof(uint32_t))) {
		std::cout << "Failed to apply battle UI/menu patch 2 (and #1)" << std::endl;
		success = false;
	}

	// ============================================================
	// Patch 3: CHRONOCROSS.exe+282A5 - Battle Elements Container / Menu frames
	// Original: movd mm1,[CHRONOCROSS.exe+E2F444] { (960) }
	// Instruction: 0F6E 0D 44F49A01 (movd mm1,[abs32])
	// ============================================================
	uintptr_t addr3 = base + 0x282A5 + 3; // +3 to skip opcode (0F 6E 0D), points to address operand
	uint32_t newAddress3 = (uint32_t)(uintptr_t)&g_baseRes;

	if (!WriteMemory(addr3, &newAddress3, sizeof(uint32_t))) {
		std::cout << "Failed to apply battle UI/menu patch 3 (movd)" << std::endl;
		success = false;
	}

	// ============================================================
	// Patch 4: CHRONOCROSS.exe+2215B - Menu / Post Battle UI
	// Original: and eax,CHRONOCROSS.exe+E2F444 { (960) }
	// Instruction: 25 44F49A01 (and eax,[abs32])
	// ============================================================
	uintptr_t addr4 = base + 0x2215B + 1; // +1 to skip opcode
	uint32_t newAddress4 = (uint32_t)(uintptr_t)&g_baseRes;

	if (!WriteMemory(addr4, &newAddress4, sizeof(uint32_t))) {
		std::cout << "Failed to apply battle UI/menu patch 4 (and #2)" << std::endl;
		success = false;
	}

	// ============================================================
	// Patch 5: CHRONOCROSS.exe+336FA - Menu Containers
	// Original: movd xmm1,[CHRONOCROSS.exe+E2F440] { (1280) }
	// Instruction: 66 0F6E 0D 40F49A01 (movd xmm1,[abs32])
	// Base value 1280 for 4:3, decreases with wider aspect ratios
	// ============================================================
	uintptr_t addr5 = base + 0x336FA + 4; // +4 to skip opcode (66 0F 6E 0D), points to address operand
	uint32_t newAddress5 = (uint32_t)(uintptr_t)&g_baseRes;

	if (!WriteMemory(addr5, &newAddress5, sizeof(uint32_t))) {
		std::cout << "Failed to apply battle UI/menu patch 5 (movd xmm1)" << std::endl;
		success = false;
	}

	// ============================================================
	// Patch 6: Battle UI Element Position Scaling at CHRONOCROSS.exe+1CF28
	// Original: movups [eax],xmm0 (0F 11 00) - 3 bytes
	// Next instruction at +1CF2B: mov [eax+10],00000000 (C7 40 10 00 00 00 00)
	// At this point, [eax] contains a 2-byte position value and [eax+4] contains
	// a 2-byte width value that both need to be multiplied by aspect ratio
	// (e.g., 1.333 for 16:9)
	// We need 5 bytes for JMP, so we'll overwrite 2 bytes of the next instruction
	// and restore them in our hook
	// ============================================================
	uintptr_t hookAddr6 = base + 0x1CF28;
	uintptr_t nextInstrAddr = base + 0x1CF2B; // Start of next instruction

	// Allocate executable memory for our hook
	g_movupsHookMem = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_movupsHookMem) {
		std::cout << "Failed to allocate movups hook memory" << std::endl;
		success = false;
	} else {
		// Build the hook code
		uint8_t hookCode[96];
		int offset = 0;

		// Execute original instruction: movups [eax],xmm0
		hookCode[offset++] = 0x0F;
		hookCode[offset++] = 0x11;
		hookCode[offset++] = 0x00;

		// Save registers we'll use
		hookCode[offset++] = 0x50; // push eax
		hookCode[offset++] = 0x51; // push ecx

		// Read the 2-byte value at [eax] (movzx ecx, word ptr [eax])
		hookCode[offset++] = 0x0F;
		hookCode[offset++] = 0xB7;
		hookCode[offset++] = 0x08;

		// Convert to float (cvtsi2ss xmm1, ecx)
		hookCode[offset++] = 0xF3;
		hookCode[offset++] = 0x0F;
		hookCode[offset++] = 0x2A;
		hookCode[offset++] = 0xC9;

		// Multiply by aspect ratio (mulss xmm1, [g_aspectRatioMultiplier])
		hookCode[offset++] = 0xF3;
		hookCode[offset++] = 0x0F;
		hookCode[offset++] = 0x59;
		hookCode[offset++] = 0x0D;
		uint32_t aspectRatioAddr = (uint32_t)(uintptr_t)&g_aspectRatioMultiplier;
		memcpy(&hookCode[offset], &aspectRatioAddr, 4);
		offset += 4;

		// Convert back to integer (cvttss2si ecx, xmm1)
		hookCode[offset++] = 0xF3;
		hookCode[offset++] = 0x0F;
		hookCode[offset++] = 0x2C;
		hookCode[offset++] = 0xC9;

		// Write the 2-byte value back to [eax] (mov word ptr [eax], cx)
		hookCode[offset++] = 0x66;
		hookCode[offset++] = 0x89;
		hookCode[offset++] = 0x08;

		// Now handle the width at [eax+4]
		// Read the 2-byte value at [eax+4] (movzx ecx, word ptr [eax+4])
		hookCode[offset++] = 0x0F;
		hookCode[offset++] = 0xB7;
		hookCode[offset++] = 0x48;
		hookCode[offset++] = 0x04;

		// Convert to float (cvtsi2ss xmm1, ecx)
		hookCode[offset++] = 0xF3;
		hookCode[offset++] = 0x0F;
		hookCode[offset++] = 0x2A;
		hookCode[offset++] = 0xC9;

		// Multiply by aspect ratio (mulss xmm1, [g_aspectRatioMultiplier])
		hookCode[offset++] = 0xF3;
		hookCode[offset++] = 0x0F;
		hookCode[offset++] = 0x59;
		hookCode[offset++] = 0x0D;
		memcpy(&hookCode[offset], &aspectRatioAddr, 4);
		offset += 4;

		// Convert back to integer (cvttss2si ecx, xmm1)
		hookCode[offset++] = 0xF3;
		hookCode[offset++] = 0x0F;
		hookCode[offset++] = 0x2C;
		hookCode[offset++] = 0xC9;

		// Write the 2-byte value back to [eax+4] (mov word ptr [eax+4], cx)
		hookCode[offset++] = 0x66;
		hookCode[offset++] = 0x89;
		hookCode[offset++] = 0x48;
		hookCode[offset++] = 0x04;

		// Restore registers
		hookCode[offset++] = 0x59; // pop ecx
		hookCode[offset++] = 0x58; // pop eax

		// Execute the 2 bytes we overwrote from the next instruction
		// mov [eax+10],00000000 starts with C7 40, we overwrote these
		// So we need to execute: C7 40 10 00 00 00 00 (full instruction)
		hookCode[offset++] = 0xC7; // mov dword ptr [eax+10], imm32
		hookCode[offset++] = 0x40;
		hookCode[offset++] = 0x10;
		hookCode[offset++] = 0x00;
		hookCode[offset++] = 0x00;
		hookCode[offset++] = 0x00;
		hookCode[offset++] = 0x00;

		// jmp to instruction after the one we just executed (+1CF32)
		hookCode[offset++] = 0xE9;
		uintptr_t jmpFromAddr = (uintptr_t)g_movupsHookMem + offset;
		uintptr_t returnAddr6 = base + 0x1CF32; // After mov [eax+10],00000000
		int32_t jmpRel = (int32_t)(returnAddr6 - (jmpFromAddr + 4));
		memcpy(&hookCode[offset], &jmpRel, 4);
		offset += 4;

		// Copy hook code to allocated memory
		memcpy(g_movupsHookMem, hookCode, offset);

		// Patch the original location: JMP (5 bytes) + NOPs (5 bytes) = 10 bytes total
		// This replaces: movups [eax],xmm0 (3 bytes) + mov [eax+10],00000000 (7 bytes)
		uint8_t jmpPatch[10];
		jmpPatch[0] = 0xE9; // JMP rel32
		int32_t hookRel = (int32_t)((uintptr_t)g_movupsHookMem - (hookAddr6 + 5));
		memcpy(&jmpPatch[1], &hookRel, 4);
		// Fill remaining bytes with NOPs
		jmpPatch[5] = 0x90;
		jmpPatch[6] = 0x90;
		jmpPatch[7] = 0x90;
		jmpPatch[8] = 0x90;
		jmpPatch[9] = 0x90;
		
		if (!WriteMemory(hookAddr6, jmpPatch, 10)) {
			std::cout << "Failed to apply battle UI/menu patch 6 (movups hook)" << std::endl;
			success = false;
		}
	}

	if (success) {
		std::cout << "Battle UI/Menu patch applied" << std::endl;
	}

	return success;
}

bool IsInBattle() {
	if (g_inBattleAddr == 0) return false;
	
	try {
		return (*(unsigned char*)g_inBattleAddr == 1);
	} catch (...) {
		return false;
	}
}

void UpdateBattleUIAndMenuValues(float aspectRatio) {
	const float BASE_ASPECT = 4.0f / 3.0f;

	if (aspectRatio < 1.4f) {
		// Reset to 4:3 default
		if (g_baseRes != 1280 || g_aspectRatioMultiplier != 1.0f) {
			g_baseRes = 1280;
			g_aspectRatioMultiplier = 1.0f;
#ifdef _DEBUG
			std::cout << "Battle UI/Menu baseRes restored to default (4:3): 1280, aspect multiplier: 1.0" << std::endl;
#endif
		}
	} else {
		// Scale down the baseRes for wider aspect ratios
		// Base 1280 for 4:3, divide by aspect ratio increase
		// For 16:9 (1.777): 1280 / (1.777 / 1.333) = 1280 / 1.333 = 960
		uint32_t newBaseRes = (uint32_t)(1280.0f / (aspectRatio / BASE_ASPECT));
		
		// Calculate aspect ratio multiplier
		// For 4:3: 1.333 / 1.333 = 1.0
		// For 16:9: 1.777 / 1.333 = 1.333
		float newAspectMultiplier = aspectRatio / BASE_ASPECT;

		if (g_baseRes != newBaseRes || g_aspectRatioMultiplier != newAspectMultiplier) {
			g_baseRes = newBaseRes;
			g_aspectRatioMultiplier = newAspectMultiplier;
#ifdef _DEBUG
			std::cout << "Battle UI/Menu baseRes updated: " << g_baseRes 
					  << ", aspect multiplier: " << g_aspectRatioMultiplier
					  << " for aspect ratio " << aspectRatio << std::endl;
#endif
		}
	}
}
