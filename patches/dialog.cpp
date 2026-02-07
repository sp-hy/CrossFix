#include "dialog.h"
#include "../utils/memory.h"
#include <Windows.h>
#include <cstring>
#include <iostream>

// Dynamic values for dialog scaling
static float g_xScale =
    1.0f; // X scale for dialog text (1.0 = 4:3, 0.75 = 16:9)
static float g_letterSpacing = 0.45f;   // Letter spacing (0.45 = 4:3 base)
static uint32_t g_portraitWidth = 960;  // Base 960
static float g_lastCursorWidth = 70.0f; // Base 70.0

// Track last modified values to avoid re-scaling
static float g_lastDialogWidthScaled = 0.0f;
static float g_lastCursorXScaled = 0.0f;

// Memory addresses
static uintptr_t g_cursorWidthAddr = 0;
static uintptr_t g_mainMenuOpenAddr = 0;

// Check if the main menu is currently open
bool IsMainMenuOpen() {
  if (g_mainMenuOpenAddr == 0)
    return false;
  try {
    return (*(uint8_t *)g_mainMenuOpenAddr == 1);
  } catch (...) {
    return false;
  }
}

// Hook memory
static void *g_hookMem = nullptr;
static void *g_cursorPosHookMem = nullptr;

// Helper function called by the hook to scale values only if they've changed
// We track the ORIGINAL unscaled values and only scale if the value has changed
// from last time
extern "C" __declspec(naked) void __stdcall
ScaleDialogValues(float *dialogWidth, float *cursorX) {
  __asm {
		push ebp
		mov ebp, esp
		sub esp, 0x20 // Reserve space for XMM register saves
		push ebx
		push esi
		push edi

          // Save XMM registers that we'll use
		movups [ebp-0x10], xmm0
		movups [ebp-0x20], xmm1

          // Get parameters
		mov esi, [ebp+8] // dialogWidth pointer
		mov edi, [ebp+12] // cursorX pointer

      // Handle dialogWidth
		movss xmm0, [esi] // Load current dialogWidth
		movss xmm1, dword ptr [g_lastDialogWidthScaled] // Load last scaled value

      // Check if this value is already scaled (equals our last scaled output)
		ucomiss xmm0, xmm1
		je skip_dialog_width // If equal to last scaled value, skip

          // This is a new/different value, scale it
		mulss xmm0, dword ptr [g_xScale]
		movss [esi], xmm0 // Write back scaled value
		movss dword ptr [g_lastDialogWidthScaled], xmm0                          // Remember this scaled value
		
	skip_dialog_width:
      // Handle cursorX
		movss xmm0, [edi] // Load current cursorX
		movss xmm1, dword ptr [g_lastCursorXScaled] // Load last scaled value

      // Check if this value is already scaled (equals our last scaled output)
		ucomiss xmm0, xmm1
		je skip_cursor_x // If equal to last scaled value, skip

          // This is a new/different value, scale it
		mulss xmm0, dword ptr [g_xScale]
		movss [edi], xmm0 // Write back scaled value
		movss dword ptr [g_lastCursorXScaled], xmm0       // Remember this scaled value
		
	skip_cursor_x:
      // Restore XMM registers
		movups xmm1, [ebp-0x20]
		movups xmm0, [ebp-0x10]
		
		pop edi
		pop esi
		pop ebx
		add esp, 0x20 // Clean up XMM save space
		pop ebp
		ret 8 // stdcall cleanup (2 params * 4 bytes)
  }
}

bool ApplyDialogPatch(uintptr_t base) {
  g_cursorWidthAddr = base + 0x415F8;
  g_mainMenuOpenAddr = base + 0x18B2C5D;
  bool success = true;

  // ============================================================
  // Patch 1: Dialog Text X Scale Hook at CHRONOCROSS.exe+195CD6
  // Original: mulps xmm1,xmm4 (0F 59 CC) + 3 more bytes
  // We hook to scale the X component of dialog text positions
  // ============================================================
  uintptr_t hookAddr = base + 0x195CD6;
  uintptr_t returnAddr = hookAddr + 6; // After jmp(5) + nop(1)

  // Allocate executable memory for our hook
  g_hookMem =
      VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
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

  if (!WriteMemory(hookAddr, jmpPatch, 6)) {
    std::cout << "Failed to apply dialog X scale hook" << std::endl;
    success = false;
  }

  // ============================================================
  // Patch 2: Letter Spacing
  // CHRONOCROSS.exe+44D11 - F3 0F59 15 E4B84D00 - mulss
  // xmm2,[CHRONOCROSS.exe+2CB8E4] Redirect to our g_letterSpacing variable
  // ============================================================
  uintptr_t addr2 = base + 0x44D11 + 4;
  uint32_t newAddress2 = (uint32_t)(uintptr_t)&g_letterSpacing;

  if (!WriteMemory(addr2, &newAddress2, sizeof(uint32_t))) {
    std::cout << "Failed to apply letter spacing redirection patch"
              << std::endl;
    success = false;
  }

  // ============================================================
  // Patch 3: Character Portrait Width
  // CHRONOCROSS.exe+415B9 - 66 0F6E 0D 48F40B01 - movd xmm1,
  // [CHRONOCROSS.exe+E2F448]
  // ============================================================
  uintptr_t portraitAddr = base + 0x415B9 + 4;
  uint32_t portraitVal = (uint32_t)(uintptr_t)&g_portraitWidth;

  if (!WriteMemory(portraitAddr, &portraitVal, sizeof(uint32_t))) {
    std::cout << "Failed to apply portrait width patch" << std::endl;
    success = false;
  }

  // ============================================================
  // Patch 4: Cursor Position and Dialog Width Hook at CHRONOCROSS.exe+433BA
  // Original: movss xmm0,[eax+CHRONOCROSS.exe+1089E14] (F3 0F 10 80 14 9E 08
  // 01) We hook to scale cursor X position and dialog width based on aspect
  // ratio
  // ============================================================
  uintptr_t cursorPosHookAddr = base + 0x433BA;
  uintptr_t cursorPosReturnAddr =
      cursorPosHookAddr + 8; // After the 8-byte instruction

  // Allocate executable memory for cursor position hook
  g_cursorPosHookMem =
      VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!g_cursorPosHookMem) {
    std::cout << "Failed to allocate cursor position hook memory" << std::endl;
    return false;
  }

  // Build the hook code
  uint8_t cursorHookCode[128];
  int cursorOffset = 0;

  uint32_t dialogWidthOffset = base + 0x1089E10;
  uint32_t cursorXOffset = base + 0x1089E14;
  uint32_t scaleFunc = (uint32_t)(uintptr_t)&ScaleDialogValues;

  // Save all registers that we'll modify
  // pushad - push all general purpose registers (60)
  cursorHookCode[cursorOffset++] = 0x60;

  // Calculate addresses: [eax+base+0x1089E10] and [eax+base+0x1089E14]
  // We need to pass these addresses to our function

  // lea edx,[eax+base+0x1089E14] - calculate address of cursorX (8D 90
  // [offset])
  cursorHookCode[cursorOffset++] = 0x8D;
  cursorHookCode[cursorOffset++] = 0x90;
  memcpy(&cursorHookCode[cursorOffset], &cursorXOffset, 4);
  cursorOffset += 4;

  // lea ecx,[eax+base+0x1089E10] - calculate address of dialogWidth (8D 88
  // [offset])
  cursorHookCode[cursorOffset++] = 0x8D;
  cursorHookCode[cursorOffset++] = 0x88;
  memcpy(&cursorHookCode[cursorOffset], &dialogWidthOffset, 4);
  cursorOffset += 4;

  // push edx - push cursorX address as 2nd parameter
  cursorHookCode[cursorOffset++] = 0x52;

  // push ecx - push dialogWidth address as 1st parameter
  cursorHookCode[cursorOffset++] = 0x51;

  // call ScaleDialogValues (E8 [rel32])
  cursorHookCode[cursorOffset++] = 0xE8;
  uintptr_t callFromAddr = (uintptr_t)g_cursorPosHookMem + cursorOffset;
  int32_t callRel = (int32_t)(scaleFunc - (callFromAddr + 4));
  memcpy(&cursorHookCode[cursorOffset], &callRel, 4);
  cursorOffset += 4;

  // Restore all registers
  // popad - pop all general purpose registers (61)
  cursorHookCode[cursorOffset++] = 0x61;

  // Execute original instruction: movss xmm0,[eax+base+0x1089E14]
  cursorHookCode[cursorOffset++] = 0xF3;
  cursorHookCode[cursorOffset++] = 0x0F;
  cursorHookCode[cursorOffset++] = 0x10;
  cursorHookCode[cursorOffset++] = 0x80;
  memcpy(&cursorHookCode[cursorOffset], &cursorXOffset, 4);
  cursorOffset += 4;

  // jmp return (E9 [rel32])
  cursorHookCode[cursorOffset++] = 0xE9;
  uintptr_t cursorJmpFromAddr = (uintptr_t)g_cursorPosHookMem + cursorOffset;
  int32_t cursorJmpRel =
      (int32_t)(cursorPosReturnAddr - (cursorJmpFromAddr + 4));
  memcpy(&cursorHookCode[cursorOffset], &cursorJmpRel, 4);
  cursorOffset += 4;

  // Copy hook code to allocated memory
  memcpy(g_cursorPosHookMem, cursorHookCode, cursorOffset);

  // Patch the original location to jump to our hook
  uint8_t cursorJmpPatch[8];
  cursorJmpPatch[0] = 0xE9; // JMP rel32
  int32_t cursorHookRel =
      (int32_t)((uintptr_t)g_cursorPosHookMem - (cursorPosHookAddr + 5));
  memcpy(&cursorJmpPatch[1], &cursorHookRel, 4);
  cursorJmpPatch[5] = 0x90; // NOP
  cursorJmpPatch[6] = 0x90; // NOP
  cursorJmpPatch[7] = 0x90; // NOP

  if (!WriteMemory(cursorPosHookAddr, cursorJmpPatch, 8)) {
    std::cout << "Failed to apply cursor position hook" << std::endl;
    success = false;
  }

  if (success) {
    std::cout << "Dialog patch applied" << std::endl;
  }

  return success;
}

void UpdateDialogValues(float aspectRatio, bool isInBattle) {
  const float BASE_ASPECT = 4.0f / 3.0f;

  bool isMenuOpen = IsMainMenuOpen();

  if (aspectRatio < 1.4f || isMenuOpen) {
    // Reset to 4:3 defaults
    if (g_xScale != 1.0f || g_letterSpacing != 0.45f ||
        g_portraitWidth != 960 || g_lastCursorWidth != 70.0f) {
      g_xScale = 1.0f;
      g_letterSpacing = 0.45f;
      g_portraitWidth = 960;
      g_lastCursorWidth = 70.0f;

      // Reset tracking variables so new dialogs get scaled
      g_lastDialogWidthScaled = 0.0f;
      g_lastCursorXScaled = 0.0f;

      WriteMemory(g_cursorWidthAddr, &g_lastCursorWidth, sizeof(float));

#ifdef _DEBUG
      std::cout << "Dialog values restored to default (4:3)" << std::endl;
#endif
    }
  } else {
    float wideRatio = BASE_ASPECT / aspectRatio;

    // X Scale: compress horizontally for widescreen (1.0 for 4:3, 0.75 for
    // 16:9)
    float newXScale = wideRatio;

    // Letter Spacing: compress for widescreen (0.45 for 4:3, 0.3375 for 16:9)
    float newLetterSpacing = 0.45f * wideRatio;

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

      // Reset tracking variables so new dialogs get scaled
      g_lastDialogWidthScaled = 0.0f;
      g_lastCursorXScaled = 0.0f;

      WriteMemory(g_cursorWidthAddr, &g_lastCursorWidth, sizeof(float));

#ifdef _DEBUG
      std::cout << "Dialog updated: XScale=" << g_xScale
                << ", LetterSpacing=" << g_letterSpacing
                << ", Portrait=" << g_portraitWidth
                << ", Cursor=" << g_lastCursorWidth << " for aspect ratio "
                << aspectRatio << std::endl;
#endif
    }
  }
}
