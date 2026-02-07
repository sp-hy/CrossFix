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

// Return address for battle UI element position hook (set at patch time)
static uintptr_t g_battleUIElementPosReturnAddr = 0;

__declspec(naked) static void BattleUIElementPositionHook() {
	__asm {
		// Original: movups [eax], xmm0
		movups [eax], xmm0
		push eax
		push ecx
		// Scale word at [eax]: position *= aspect ratio
		movzx ecx, word ptr [eax]
		cvtsi2ss xmm1, ecx
		mulss xmm1, dword ptr [g_aspectRatioMultiplier]
		cvttss2si ecx, xmm1
		mov word ptr [eax], cx
		// Scale word at [eax+4]: width *= aspect ratio
		movzx ecx, word ptr [eax+4]
		cvtsi2ss xmm1, ecx
		mulss xmm1, dword ptr [g_aspectRatioMultiplier]
		cvttss2si ecx, xmm1
		mov word ptr [eax+4], cx
		pop ecx
		pop eax
		// Execute overwritten instruction: mov [eax+10], 0
		mov dword ptr [eax+10h], 0
		mov eax, dword ptr [g_battleUIElementPosReturnAddr]
		jmp eax
	}
}

// Return address for battle dialog text position hook (set at patch time)
static uintptr_t g_battleDialogTextPosReturnAddr = 0;

__declspec(naked) static void BattleDialogTextPositionHook() {
	__asm {
		movss xmm1, [ebp-90h]
		mulss xmm1, dword ptr [g_aspectRatioMultiplier]
		mov eax, dword ptr [g_battleDialogTextPosReturnAddr]
		jmp eax
	}
}

// Battle dialog cursor: add 30*aspectRatio to [base+0x186DCB0] after the original add (2-byte value)
static uintptr_t g_battleDialogCursorAddr = 0;       // address of the word (set at patch time)
static int32_t g_battleDialogCursorAddend = 30;     // 30 * aspectRatio, flattened to whole number
static uintptr_t g_battleDialogCursorReturnAddr = 0;

__declspec(naked) static void BattleDialogCursorHook() {
	__asm {
		// Load actual game address (value of our pointer), not the address of our global
		push edx
		mov edx, dword ptr [g_battleDialogCursorAddr]
		// Original: add [addr], eax (target is 2-byte)
		add word ptr [edx], ax
		// Additional add: 30 * aspectRatio (flattened integer)
		push ecx
		mov ecx, dword ptr [g_battleDialogCursorAddend]
		add word ptr [edx], cx
		pop ecx
		pop edx
		mov eax, dword ptr [g_battleDialogCursorReturnAddr]
		jmp eax
	}
}

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
	// Patch 5: CHRONOCROSS.exe+17BEA8 - Shop Menu
	// Original: movd xmm0,[CHRONOCROSS.exe+E2F444] { (960) }
	// Instruction: 66 0F6E 05 44F40301 (movd xmm0,[abs32])
	// ============================================================
	uintptr_t addr5 = base + 0x17BEA8 + 4; // +4 to skip opcode (66 0F 6E 05), points to address operand
	uint32_t newAddress5 = (uint32_t)(uintptr_t)&g_baseRes;

	if (!WriteMemory(addr5, &newAddress5, sizeof(uint32_t))) {
		std::cout << "Failed to apply battle UI/menu patch 5 (shopMenu)" << std::endl;
		success = false;
	}

	// ============================================================
	// Patch 6: CHRONOCROSS.exe+294E4 - Menu Compass Hand
	// Original: movd xmm1,[CHRONOCROSS.exe+E2F440] { (1280) }
	// Instruction: 66 0F6E 0D 40F40301 (movd xmm1,[abs32])
	// ============================================================
	uintptr_t addr6 = base + 0x294E4 + 4; // +4 to skip opcode (66 0F 6E 0D), points to address operand
	uint32_t newAddress6 = (uint32_t)(uintptr_t)&g_baseRes;

	if (!WriteMemory(addr6, &newAddress6, sizeof(uint32_t))) {
		std::cout << "Failed to apply battle UI/menu patch 6 (menu compass hand)" << std::endl;
		success = false;
	}

	// ============================================================
	// Patch 7: CHRONOCROSS.exe+336FA - Menu Containers
	// Original: movd xmm1,[CHRONOCROSS.exe+E2F440] { (1280) }
	// Instruction: 66 0F6E 0D 40F49A01 (movd xmm1,[abs32])
	// Base value 1280 for 4:3, decreases with wider aspect ratios
	// ============================================================
	uintptr_t addr7 = base + 0x336FA + 4; // +4 to skip opcode (66 0F 6E 0D), points to address operand
	uint32_t newAddress7 = (uint32_t)(uintptr_t)&g_baseRes;

	if (!WriteMemory(addr7, &newAddress7, sizeof(uint32_t))) {
		std::cout << "Failed to apply battle UI/menu patch 7 (movd xmm1)" << std::endl;
		success = false;
	}

	// ============================================================
	// Patch 8: Battle UI Element Position at CHRONOCROSS.exe+1CF28
	// Original: movups [eax],xmm0 (3 bytes) + start of mov [eax+10],0 (we overwrite 10 bytes).
	// Scale position at [eax] and width at [eax+4] by aspect ratio, then run overwritten instruction.
	// ============================================================
	{
		uintptr_t hookAddr8 = base + 0x1CF28;
		g_battleUIElementPosReturnAddr = base + 0x1CF32;

		uint8_t jmpPatch8[10];
		jmpPatch8[0] = 0xE9;
		int32_t hookRel8 = (int32_t)((uintptr_t)&BattleUIElementPositionHook - (hookAddr8 + 5));
		memcpy(&jmpPatch8[1], &hookRel8, 4);
		jmpPatch8[5] = 0x90;
		jmpPatch8[6] = 0x90;
		jmpPatch8[7] = 0x90;
		jmpPatch8[8] = 0x90;
		jmpPatch8[9] = 0x90;

		if (!WriteMemory(hookAddr8, jmpPatch8, 10)) {
			std::cout << "Failed to apply battle UI/menu patch 8 (battle UI element position)" << std::endl;
			success = false;
		}
	}

	// ============================================================
	// Patch 9: CHRONOCROSS.exe+1D8BF - Battle dialog text position
	// Original: movss xmm1,[ebp-00000090]. Scale by aspect ratio, jump back.
	// ============================================================
	{
		uintptr_t hookAddr9 = base + 0x1D8BF;
		g_battleDialogTextPosReturnAddr = base + 0x1D8C7;

		uint8_t jmpPatch9[8];
		jmpPatch9[0] = 0xE9;
		int32_t hookRel9 = (int32_t)((uintptr_t)&BattleDialogTextPositionHook - (hookAddr9 + 5));
		memcpy(&jmpPatch9[1], &hookRel9, 4);
		jmpPatch9[5] = 0x90;
		jmpPatch9[6] = 0x90;
		jmpPatch9[7] = 0x90;

		if (!WriteMemory(hookAddr9, jmpPatch9, 8)) {
			std::cout << "Failed to apply battle UI/menu patch 9 (battle dialog text position)" << std::endl;
			success = false;
		}
	}

	// ============================================================
	// Patch 10: CHRONOCROSS.exe+1CAFC - Battle dialog cursor
	// Original: add [CHRONOCROSS.exe+186DCB0], eax (6 bytes). We add 30*aspectRatio after it.
	// ============================================================
	{
		uintptr_t hookAddr10 = base + 0x1CAFC;
		g_battleDialogCursorAddr = base + 0x186DCB0;
		g_battleDialogCursorReturnAddr = base + 0x1CB02;

		uint8_t jmpPatch10[6];
		jmpPatch10[0] = 0xE9;
		int32_t hookRel10 = (int32_t)((uintptr_t)&BattleDialogCursorHook - (hookAddr10 + 5));
		memcpy(&jmpPatch10[1], &hookRel10, 4);
		jmpPatch10[5] = 0x90;

		if (!WriteMemory(hookAddr10, jmpPatch10, 6)) {
			std::cout << "Failed to apply battle UI/menu patch 10 (battle dialog cursor)" << std::endl;
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
			g_battleDialogCursorAddend = 30;  // 30 * 1.0
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
		int32_t newCursorAddend = (int32_t)(30.0f * newAspectMultiplier + 0.5f);

		if (g_baseRes != newBaseRes || g_aspectRatioMultiplier != newAspectMultiplier || g_battleDialogCursorAddend != newCursorAddend) {
			g_baseRes = newBaseRes;
			g_aspectRatioMultiplier = newAspectMultiplier;
			g_battleDialogCursorAddend = newCursorAddend;
#ifdef _DEBUG
			std::cout << "Battle UI/Menu baseRes updated: " << g_baseRes 
					  << ", aspect multiplier: " << g_aspectRatioMultiplier
					  << " for aspect ratio " << aspectRatio << std::endl;
#endif
		}
	}
}
