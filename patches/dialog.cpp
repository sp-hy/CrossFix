#include "dialog.h"
#include "../utils/memory.h"
#include <iostream>

// The values that will be read by the game
static uint32_t g_dialogWidth = 1280;     // Base 1280
static uint32_t g_textXOffset = 1280;     // Base 1280
static uint32_t g_portraitWidth = 960;    // Base 960
static float g_lastCursorWidth = 70.0f;   // Base 70.0

// Memory addresses
static uintptr_t g_cursorWidthAddr = 0;

bool ApplyDialogPatch(uintptr_t base) {
	g_cursorWidthAddr = base + 0x415F8;
	bool success = true;

	// Patch 1: Dialog Text Width
	// CHRONOCROSS.exe+1AD4FA - 66 0F6E 05 40F40B01 - movd xmm0,[CHRONOCROSS.exe+E2F440]
	uintptr_t addr1 = base + 0x1AD4FA + 4;
	uint32_t newAddress1 = (uint32_t)&g_dialogWidth;
	
	if (WriteMemory(addr1, &newAddress1, sizeof(uint32_t))) {
		std::cout << "Dialog width patch applied" << std::endl;
	} else {
		std::cout << "Failed to apply dialog width patch" << std::endl;
		success = false;
	}

	// Patch 2: Text X Offset
	// CHRONOCROSS.exe+445AD - 66 0F6E 05 40F40B01 - movd xmm0,[CHRONOCROSS.exe+E2F440]
	uintptr_t addr2 = base + 0x445AD + 4;
	uint32_t newAddress2 = (uint32_t)&g_textXOffset;

	if (WriteMemory(addr2, &newAddress2, sizeof(uint32_t))) {
		std::cout << "Text X offset patch applied" << std::endl;
	} else {
		std::cout << "Failed to apply text X offset patch" << std::endl;
		success = false;
	}

	// Patch 3: Character Portrait Width
	// CHRONOCROSS.exe+415B9 - 66 0F6E 0D 48F40B01 - movd xmm1, [CHRONOCROSS.exe+E2F448]
	uintptr_t addr3 = base + 0x415B9 + 4;
	uint32_t newAddress3 = (uint32_t)&g_portraitWidth;

	if (WriteMemory(addr3, &newAddress3, sizeof(uint32_t))) {
		std::cout << "Portrait width patch applied" << std::endl;
	} else {
		std::cout << "Failed to apply portrait width patch" << std::endl;
		success = false;
	}
	
	return success;
}

void UpdateDialogValues(float aspectRatio, bool isInFmvMenu, bool isInBattle) {
	if (isInFmvMenu || aspectRatio < 1.4f) {
		if (g_dialogWidth != 1280 || g_textXOffset != 1280 || g_portraitWidth != 960 || g_lastCursorWidth != 70.0f) {
			g_dialogWidth = 1280;
			g_textXOffset = 1280;
			g_portraitWidth = 960;
			g_lastCursorWidth = 70.0f;
			
			WriteMemory(g_cursorWidthAddr, &g_lastCursorWidth, sizeof(float));

#ifdef _DEBUG
			std::cout << "Dialog values restored to default (FMV/Menu or 4:3)" << std::endl;
#endif
		}
	} else {
		float wideRatio = 4.0f / 3.0f / aspectRatio;

		// Dialog Width Calculation: 1280 * (aspectRatio / (4/3))
		uint32_t newWidth = (uint32_t)(1280.0f * (aspectRatio / (4.0f / 3.0f)));
		
		// Text X Offset Calculation: 1280 * ((4/3) / aspectRatio)
		uint32_t newOffset = (uint32_t)(1280.0f * wideRatio);

		// Portrait Width Calculation: 960 * ((4/3) / aspectRatio)
		uint32_t newPortrait = (uint32_t)(960.0f * wideRatio);

		// Cursor Width Calculation: 70.0 * ((4/3) / aspectRatio)
		float newCursorWidth = 70.0f * wideRatio;

		if (g_dialogWidth != newWidth || g_textXOffset != newOffset || g_portraitWidth != newPortrait || g_lastCursorWidth != newCursorWidth) {
			g_dialogWidth = newWidth;
			g_textXOffset = newOffset;
			g_portraitWidth = newPortrait;
			g_lastCursorWidth = newCursorWidth;
			
			WriteMemory(g_cursorWidthAddr, &g_lastCursorWidth, sizeof(float));

#ifdef _DEBUG
			std::cout << "Dialog updated: Width=" << g_dialogWidth << ", Offset=" << g_textXOffset << ", Portrait=" << g_portraitWidth << ", Cursor=" << g_lastCursorWidth << " (Battle: " << (isInBattle ? "YES" : "NO") << ") for aspect ratio " << aspectRatio << std::endl;
#endif
		}
	}
}
