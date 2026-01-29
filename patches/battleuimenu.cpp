#include "battleuimenu.h"
#include "../utils/memory.h"
#include <iostream>
#include <cstring>

static uintptr_t g_inBattleAddr = 0;

// Dynamic value for battle UI and menu aspect ratio scaling
// Base value is 1280 for 4:3, needs to be decreased with wider aspect ratios
static uint32_t g_baseRes = 1280;  // Base 1280 for 4:3

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
		if (g_baseRes != 1280) {
			g_baseRes = 1280;
#ifdef _DEBUG
			std::cout << "Battle UI/Menu baseRes restored to default (4:3): 1280" << std::endl;
#endif
		}
	} else {
		// Scale down the baseRes for wider aspect ratios
		// Base 1280 for 4:3, divide by aspect ratio increase
		// For 16:9 (1.777): 1280 / (1.777 / 1.333) = 1280 / 1.333 = 960
		uint32_t newBaseRes = (uint32_t)(1280.0f / (aspectRatio / BASE_ASPECT));

		if (g_baseRes != newBaseRes) {
			g_baseRes = newBaseRes;
#ifdef _DEBUG
			std::cout << "Battle UI/Menu baseRes updated: " << g_baseRes 
					  << " for aspect ratio " << aspectRatio << std::endl;
#endif
		}
	}
}
