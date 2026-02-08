#include "battleuimenu.h"
#include "../utils/memory.h"
#include <iostream>


static uintptr_t g_inBattleAddr = 0;

// Dynamic value for battle UI and menu aspect ratio scaling
// Base value is 1280 for 4:3, needs to be decreased with wider aspect ratios
static uint32_t g_baseRes = 1280; // Base 1280 for 4:3

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

// Battle dialog cursor: add 30*aspectRatio to [base+0x186DCB0] after the
// original add (2-byte value)
static uintptr_t g_battleDialogCursorAddr =
    0; // address of the word (set at patch time)
static int32_t g_battleDialogCursorAddend =
    30; // 30 * aspectRatio, flattened to whole number
static uintptr_t g_battleDialogCursorReturnAddr = 0;

__declspec(naked) static void BattleDialogCursorHook() {
  __asm {
    // Load actual game address (value of our pointer), not the address of our
    // global
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
  // Patches 1-7: Redirect operands to our dynamic g_baseRes
  // ============================================================
  struct {
    uintptr_t offset;
    int opcodeLen;
    const char *name;
  } operandPatches[] = {
      {0x29314, 1, "battle elements width (adc)"},
      {0x1D21F, 1, "battle text containers (and #1)"},
      {0x282A5, 3, "battle elements container (movd mm1)"},
      {0x2215B, 1, "menu / post battle UI (and #2)"},
      {0x17BEA8, 4, "shop menu (movd xmm0)"},
      {0x294E4, 4, "menu compass hand (movd xmm1)"},
      {0x336FA, 4, "menu containers (movd xmm1)"},
  };

  for (const auto &p : operandPatches) {
    if (!RedirectOperand(base + p.offset + p.opcodeLen, &g_baseRes)) {
      std::cout << "Failed to apply battle UI/menu patch: " << p.name
                << std::endl;
      success = false;
    }
  }

  // ============================================================
  // Patch 8: Battle UI Element Position at CHRONOCROSS.exe+1CF28
  // Original: movups [eax],xmm0 (3 bytes) + start of mov [eax+10],0 (we
  // overwrite 10 bytes). Scale position at [eax] and width at [eax+4] by aspect
  // ratio, then run overwritten instruction.
  // ============================================================
  {
    uintptr_t hookAddr8 = base + 0x1CF28;
    g_battleUIElementPosReturnAddr = base + 0x1CF32;

    if (!InstallJmpHook(hookAddr8, (void *)&BattleUIElementPositionHook, 10)) {
      std::cout << "Failed to apply battle UI/menu patch 8 (battle UI element "
                   "position)"
                << std::endl;
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

    if (!InstallJmpHook(hookAddr9, (void *)&BattleDialogTextPositionHook, 8)) {
      std::cout << "Failed to apply battle UI/menu patch 9 (battle dialog text "
                   "position)"
                << std::endl;
      success = false;
    }
  }

  // ============================================================
  // Patch 10: CHRONOCROSS.exe+1CAFC - Battle dialog cursor
  // Original: add [CHRONOCROSS.exe+186DCB0], eax (6 bytes). We add
  // 30*aspectRatio after it.
  // ============================================================
  {
    g_battleDialogCursorAddr = base + 0x186DCB0;
    g_battleDialogCursorReturnAddr = base + 0x1CB02;

    if (!InstallJmpHook(base + 0x1CAFC, (void *)&BattleDialogCursorHook, 6)) {
      std::cout
          << "Failed to apply battle UI/menu patch 10 (battle dialog cursor)"
          << std::endl;
      success = false;
    }
  }

  if (success) {
    std::cout << "Battle UI/Menu patch applied" << std::endl;
  }

  return success;
}

bool IsInBattle() { return ReadGameByte(g_inBattleAddr) == 1; }

void UpdateBattleUIAndMenuValues(float aspectRatio) {
  if (aspectRatio < WIDESCREEN_THRESHOLD) {
    // Reset to 4:3 default
    if (g_baseRes != 1280 || g_aspectRatioMultiplier != 1.0f) {
      g_baseRes = 1280;
      g_aspectRatioMultiplier = 1.0f;
      g_battleDialogCursorAddend = 30; // 30 * 1.0
#ifdef _DEBUG
      std::cout << "Battle UI/Menu baseRes restored to default (4:3): 1280, "
                   "aspect multiplier: 1.0"
                << std::endl;
#endif
    }
  } else {
    // Scale down the baseRes for wider aspect ratios
    // Base 1280 for 4:3, divide by aspect ratio increase
    // For 16:9 (1.777): 1280 / (1.777 / 1.333) = 1280 / 1.333 = 960
    uint32_t newBaseRes = (uint32_t)(1280.0f / (aspectRatio / BASE_ASPECT_RATIO));

    // Calculate aspect ratio multiplier
    // For 4:3: 1.333 / 1.333 = 1.0
    // For 16:9: 1.777 / 1.333 = 1.333
    float newAspectMultiplier = aspectRatio / BASE_ASPECT_RATIO;
    int32_t newCursorAddend = (int32_t)(30.0f * newAspectMultiplier + 0.5f);

    if (g_baseRes != newBaseRes ||
        g_aspectRatioMultiplier != newAspectMultiplier ||
        g_battleDialogCursorAddend != newCursorAddend) {
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
