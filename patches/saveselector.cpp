#include "saveselector.h"
#include "../utils/memory.h"
#include "dialog.h"
#include <iostream>

// Set at patch time: 24 target addresses (esi is compared against these)
static uint32_t g_saveSelectorTargets[24];
static uintptr_t g_saveSelectorReturnAddr = 0;
static const float *g_saveSelectorAspectRatioMultiplierPtr = nullptr;

__declspec(naked) static void SaveSelectorHook() {
  __asm {
    push eax
    push ecx
    sub esp, 10h
    movdqu [esp], xmm0

        // Original: mov edx, [esi]
    mov edx, [esi]

    // Compare esi with each target; jump to the correct value-check for that
    // group. Group 1: valid 73,107,141,175,209,243,277,311. Group 2: valid
    // 106,140,174,208,242,276,310,344.
    cmp esi, dword ptr [g_saveSelectorTargets]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+4]
    je check_group2
    cmp esi, dword ptr [g_saveSelectorTargets+8]
    je check_group2
    cmp esi, dword ptr [g_saveSelectorTargets+0Ch]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+10h]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+14h]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+18h]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+1Ch]
    je check_group2
    cmp esi, dword ptr [g_saveSelectorTargets+20h]
    je check_group2
    cmp esi, dword ptr [g_saveSelectorTargets+24h]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+28h]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+2Ch]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+30h]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+34h]
    je check_group2
    cmp esi, dword ptr [g_saveSelectorTargets+38h]
    je check_group2
    cmp esi, dword ptr [g_saveSelectorTargets+3Ch]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+40h]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+44h]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+48h]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+4Ch]
    je check_group2
    cmp esi, dword ptr [g_saveSelectorTargets+50h]
    je check_group2
    cmp esi, dword ptr [g_saveSelectorTargets+54h]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+58h]
    je check_group1
    cmp esi, dword ptr [g_saveSelectorTargets+5Ch]
    je check_group1
    jmp cleanup

  check_group1:
    cmp dx, 73
    je do_division
    cmp dx, 107
    je do_division
    cmp dx, 141
    je do_division
    cmp dx, 175
    je do_division
    cmp dx, 209
    je do_division
    cmp dx, 243
    je do_division
    cmp dx, 277
    je do_division
    cmp dx, 311
    je do_division
    jmp cleanup

  check_group2:
    cmp dx, 106
    je do_division
    cmp dx, 140
    je do_division
    cmp dx, 174
    je do_division
    cmp dx, 208
    je do_division
    cmp dx, 242
    je do_division
    cmp dx, 276
    je do_division
    cmp dx, 310
    je do_division
    cmp dx, 344
    je do_division
    jmp cleanup

  do_division:
    push edx
    call IsGameMenuOpen
    test al, al
    pop edx
    jz cleanup
    movzx eax, dx
    cvtsi2ss xmm0, eax
    mov eax, dword ptr [g_saveSelectorAspectRatioMultiplierPtr]
    divss xmm0, dword ptr [eax]
    cvttss2si eax, xmm0
    mov dx, ax

  cleanup:
    movdqu xmm0, [esp]
    add esp, 10h
    pop ecx
    pop eax
         // Overwritten instructions: mov [edi],edx; add edi,4
    mov [edi], edx
    add edi, 4
     // Return without clobbering eax (game may use it at 2602DE)
    push dword ptr [g_saveSelectorReturnAddr]
    ret
  }
}

bool ApplySaveSelectorPatch(uintptr_t base,
                            const float *aspectRatioMultiplier) {
  // CHRONOCROSS.exe+2602D7 - Save Selector
  // Original: mov edx,[esi] (2) + mov [edi],edx (2) + add edi,04 (3) = 7 bytes
  // When esi matches one of the target addresses, we divide the 2-byte value
  // by the aspect ratio multiplier before storing.
  // Original + new addresses grouped. Group 1: 73,107,141,175,209,243,277,311.
  // Group 2: 106,140,174,208,242,276,310,344
  const uintptr_t targetOffsets[] = {
      0xC1E55C, 0xC1E560, 0xC1E564, 0xC1E568, 0xC1E578, 0xC1E57C, // original
      0xC223DC, 0xC223E0, 0xC223E4, 0xC223E8, 0xC223F8, 0xC223FC, // original
      0xC1EE1C, 0xC1EE20, 0xC1EE24, 0xC1EE28, 0xC1EE38, 0xC1EE3C, // new
      0xC22C9C, 0xC22CA0, 0xC22CA4, 0xC22CA8, 0xC22CB8, 0xC22CBC  // new
  };

  for (int i = 0; i < 24; i++) {
    g_saveSelectorTargets[i] = (uint32_t)(base + targetOffsets[i]);
  }
  g_saveSelectorReturnAddr = base + 0x2602DE;
  g_saveSelectorAspectRatioMultiplierPtr = aspectRatioMultiplier;

  uintptr_t hookAddr = base + 0x2602D7;
  if (!InstallJmpHook(hookAddr, (void *)&SaveSelectorHook, 7)) {
    std::cout << "Failed to apply save selector patch" << std::endl;
    return false;
  }

  std::cout << "Save selector patch applied" << std::endl;
  return true;
}
