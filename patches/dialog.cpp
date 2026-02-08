#include "dialog.h"
#include "../utils/memory.h"
#include <Windows.h>
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
// Main menu flag: when true, dialog patches are disabled (values reset to 4:3).
// Set to same as game menu below if no separate main-menu address is known.
static uintptr_t g_mainMenuOpenAddr = 0;
// Game menu flag: byte at base+0x82F120 is 1 when in-game menu is open.
static uintptr_t g_gameMenuOpenAddr = 0;

bool IsMainMenuOpen() {
  return g_mainMenuOpenAddr && ReadGameByte(g_mainMenuOpenAddr) == 1;
}
bool IsGameMenuOpen() {
  return g_gameMenuOpenAddr && ReadGameByte(g_gameMenuOpenAddr) == 1;
}

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

// Return address for dialog X scale hook (set at patch time)
static uintptr_t g_dialogXScaleReturnAddr = 0;

// Patch 4: offsets and return address (set at patch time)
static uint32_t g_cursorPosDialogWidthOffset = 0;
static uint32_t g_cursorPosCursorXOffset = 0;
static uintptr_t g_cursorPosReturnAddr = 0;

// Naked hook: original mulps + scale X component, then jmp back
extern "C" __declspec(naked) void DialogTextXScaleHook() {
  __asm {
		mulps xmm1, xmm4
		movaps xmm0, xmm1
		mulss xmm0, dword ptr [g_xScale]
		movss xmm1, xmm0
		jmp dword ptr [g_dialogXScaleReturnAddr]
  }
}

// Naked hook: scale dialog width + cursor X via ScaleDialogValues, then
// original movss, jmp back
extern "C" __declspec(naked) void CursorPosDialogWidthHook() {
  __asm {
		pushad
		mov ecx, dword ptr [g_cursorPosDialogWidthOffset]
		add ecx, eax
		mov edx, dword ptr [g_cursorPosCursorXOffset]
		add edx, eax
		push edx
		push ecx
		call ScaleDialogValues
		popad
		mov ecx, dword ptr [g_cursorPosCursorXOffset]
		movss xmm0, dword ptr [eax+ecx]
		jmp dword ptr [g_cursorPosReturnAddr]
  }
}

bool ApplyDialogPatch(uintptr_t base) {
  g_cursorWidthAddr = base + 0x415F8;
  g_gameMenuOpenAddr = base + 0x82F120;
  g_mainMenuOpenAddr = base + 0x18B2C5D;
  bool success = true;

  // ============================================================
  // Patch 1: Dialog Text X Scale Hook at CHRONOCROSS.exe+195CD6
  // Original: mulps xmm1,xmm4 (0F 59 CC) + 3 more bytes
  // We hook to scale the X component of dialog text positions
  // ============================================================
  uintptr_t hookAddr = base + 0x195CD6;
  g_dialogXScaleReturnAddr = hookAddr + 6; // After jmp(5) + nop(1)

  if (!InstallJmpHook(hookAddr, (void *)&DialogTextXScaleHook, 6)) {
    std::cout << "Failed to apply dialog X scale hook" << std::endl;
    success = false;
  }

  // ============================================================
  // Patch 2: Letter Spacing
  // CHRONOCROSS.exe+44D11 - F3 0F59 15 E4B84D00 - mulss
  // xmm2,[CHRONOCROSS.exe+2CB8E4] Redirect to our g_letterSpacing variable
  // ============================================================
  if (!RedirectOperand(base + 0x44D11 + 4, &g_letterSpacing)) {
    std::cout << "Failed to apply letter spacing redirection patch"
              << std::endl;
    success = false;
  }

  // ============================================================
  // Patch 3: Character Portrait Width
  // CHRONOCROSS.exe+415B9 - 66 0F6E 0D 48F40B01 - movd xmm1,
  // [CHRONOCROSS.exe+E2F448]
  // ============================================================
  if (!RedirectOperand(base + 0x415B9 + 4, &g_portraitWidth)) {
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
  g_cursorPosDialogWidthOffset = (uint32_t)(base + 0x1089E10);
  g_cursorPosCursorXOffset = (uint32_t)(base + 0x1089E14);
  g_cursorPosReturnAddr = cursorPosHookAddr + 8; // After the 8-byte instruction

  if (!InstallJmpHook(cursorPosHookAddr, (void *)&CursorPosDialogWidthHook,
                      8)) {
    std::cout << "Failed to apply cursor position hook" << std::endl;
    success = false;
  }

  if (success) {
    std::cout << "Dialog patch applied" << std::endl;
  }

  return success;
}

void UpdateDialogValues(float aspectRatio, bool isInBattle) {
  bool mainMenuOpen = IsMainMenuOpen();
  bool gameMenuOpen = IsGameMenuOpen();

  // Disable dialog patches when main menu is open or in 4:3
  if (aspectRatio < WIDESCREEN_THRESHOLD || mainMenuOpen || gameMenuOpen) {
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
    float wideRatio = BASE_ASPECT_RATIO / aspectRatio;

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
