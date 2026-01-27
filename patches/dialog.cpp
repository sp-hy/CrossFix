#include "dialog.h"
#include "../utils/memory.h"
#include <iostream>
#include <cstring>
#include <Windows.h>

// Dynamic values for dialog scaling
static float g_xScale = 1.0f;            // X scale for dialog text (1.0 = 4:3, 0.75 = 16:9)
static float g_letterSpacing = 640.0f;   // Letter spacing (640 = 4:3 base)
static uint32_t g_portraitWidth = 960;   // Base 960
static float g_lastCursorWidth = 70.0f;  // Base 70.0

// Memory addresses
static uintptr_t g_cursorWidthAddr = 0;
static uintptr_t g_letterSpacingAddr = 0;

// Hook memory
static void* g_hookMem = nullptr;

bool ApplyDialogPatch(uintptr_t base) {
	g_cursorWidthAddr = base + 0x415F8;
	g_letterSpacingAddr = base + 0x2CBC90;
	bool success = true;

	// ============================================================
	// Patch 1: Dialog Text X Scale Hook at CHRONOCROSS.exe+195CD6
	// Original: mulps xmm1,xmm4 (0F 59 CC) + 3 more bytes
	// We hook to scale the X component of dialog text positions
	// ============================================================
	uintptr_t hookAddr = base + 0x195CD6;
	uintptr_t returnAddr = hookAddr + 6; // After jmp(5) + nop(1)

	// Allocate executable memory for our hook
	g_hookMem = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_hookMem) {
		std::cout << "Failed to allocate hook memory" << std::endl;
		return false;
	}

	// Build the hook code
	uint8_t hookCode[64];
	int offset = 0;

	// mulps xmm1,xmm4 - original instruction (0F 59 CC)
	hookCode[offset++] = 0x0F;
	hookCode[offset++] = 0x59;
	hookCode[offset++] = 0xCC;

	// movaps xmm0,xmm1 - copy positions (0F 28 C1)
	hookCode[offset++] = 0x0F;
	hookCode[offset++] = 0x28;
	hookCode[offset++] = 0xC1;

	// mulss xmm0,[g_xScale] - scale X component (F3 0F 59 05 [abs32])
	hookCode[offset++] = 0xF3;
	hookCode[offset++] = 0x0F;
	hookCode[offset++] = 0x59;
	hookCode[offset++] = 0x05;
	uint32_t xScaleAddr = (uint32_t)(uintptr_t)&g_xScale;
	memcpy(&hookCode[offset], &xScaleAddr, 4);
	offset += 4;

	// movss xmm1,xmm0 - put compressed X back into xmm1 (F3 0F 10 C8)
	hookCode[offset++] = 0xF3;
	hookCode[offset++] = 0x0F;
	hookCode[offset++] = 0x10;
	hookCode[offset++] = 0xC8;

	// jmp return (E9 [rel32])
	hookCode[offset++] = 0xE9;
	uintptr_t jmpFromAddr = (uintptr_t)g_hookMem + offset;
	int32_t jmpRel = (int32_t)(returnAddr - (jmpFromAddr + 4));
	memcpy(&hookCode[offset], &jmpRel, 4);
	offset += 4;

	// Copy hook code to allocated memory
	memcpy(g_hookMem, hookCode, offset);

	// Patch the original location to jump to our hook
	uint8_t jmpPatch[6];
	jmpPatch[0] = 0xE9; // JMP rel32
	int32_t hookRel = (int32_t)((uintptr_t)g_hookMem - (hookAddr + 5));
	memcpy(&jmpPatch[1], &hookRel, 4);
	jmpPatch[5] = 0x90; // NOP

	if (WriteMemory(hookAddr, jmpPatch, 6)) {
		std::cout << "Dialog X scale hook applied at " << std::hex << hookAddr << std::dec << std::endl;
	} else {
		std::cout << "Failed to apply dialog X scale hook" << std::endl;
		success = false;
	}

	// ============================================================
	// Patch 2: Letter Spacing at CHRONOCROSS.exe+2CBC90
	// Base value = 640 for 4:3, scale up for wider aspects
	// ============================================================
	// This is a direct memory value we'll update dynamically
	// Initial write to set up
	if (WriteMemory(g_letterSpacingAddr, &g_letterSpacing, sizeof(float))) {
		std::cout << "Letter spacing patch applied" << std::endl;
	} else {
		std::cout << "Failed to apply letter spacing patch" << std::endl;
		success = false;
	}

	// ============================================================
	// Patch 3: Character Portrait Width
	// CHRONOCROSS.exe+415B9 - 66 0F6E 0D 48F40B01 - movd xmm1, [CHRONOCROSS.exe+E2F448]
	// ============================================================
	uintptr_t addr3 = base + 0x415B9 + 4;
	uint32_t newAddress3 = (uint32_t)(uintptr_t)&g_portraitWidth;

	if (WriteMemory(addr3, &newAddress3, sizeof(uint32_t))) {
		std::cout << "Portrait width patch applied" << std::endl;
	} else {
		std::cout << "Failed to apply portrait width patch" << std::endl;
		success = false;
	}

	return success;
}

void UpdateDialogValues(float aspectRatio, bool isInFmvMenu, bool isInBattle) {
	const float BASE_ASPECT = 4.0f / 3.0f;

	if (isInFmvMenu || aspectRatio < 1.4f) {
		// Reset to 4:3 defaults
		if (g_xScale != 1.0f || g_letterSpacing != 640.0f || g_portraitWidth != 960 || g_lastCursorWidth != 70.0f) {
			g_xScale = 1.0f;
			g_letterSpacing = 640.0f;
			g_portraitWidth = 960;
			g_lastCursorWidth = 70.0f;

			WriteMemory(g_letterSpacingAddr, &g_letterSpacing, sizeof(float));
			WriteMemory(g_cursorWidthAddr, &g_lastCursorWidth, sizeof(float));

#ifdef _DEBUG
			std::cout << "Dialog values restored to default (FMV/Menu or 4:3)" << std::endl;
#endif
		}
	} else {
		float wideRatio = BASE_ASPECT / aspectRatio;

		// X Scale: compress horizontally for widescreen (1.0 for 4:3, 0.75 for 16:9)
		float newXScale = wideRatio;

		// Letter Spacing: expand for widescreen (640 for 4:3, ~853 for 16:9)
		float newLetterSpacing = 640.0f * aspectRatio / BASE_ASPECT;

		// Portrait Width: compress for widescreen
		uint32_t newPortrait = (uint32_t)(960.0f * wideRatio);

		// Cursor Width: compress for widescreen
		float newCursorWidth = 70.0f * wideRatio;

		if (g_xScale != newXScale || g_letterSpacing != newLetterSpacing || 
			g_portraitWidth != newPortrait || g_lastCursorWidth != newCursorWidth) {
			
			g_xScale = newXScale;
			g_letterSpacing = newLetterSpacing;
			g_portraitWidth = newPortrait;
			g_lastCursorWidth = newCursorWidth;

			WriteMemory(g_letterSpacingAddr, &g_letterSpacing, sizeof(float));
			WriteMemory(g_cursorWidthAddr, &g_lastCursorWidth, sizeof(float));

#ifdef _DEBUG
			std::cout << "Dialog updated: XScale=" << g_xScale 
					  << ", LetterSpacing=" << g_letterSpacing 
					  << ", Portrait=" << g_portraitWidth 
					  << ", Cursor=" << g_lastCursorWidth 
					  << " for aspect ratio " << aspectRatio << std::endl;
#endif
		}
	}
}
