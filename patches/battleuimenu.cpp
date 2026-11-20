#include "battleuimenu.h"
#include "../utils/memory.h"
#include "../utils/rde_symbols.h"
#include <iostream>

static uintptr_t g_inBattleAddr = 0;

// Dynamic value for battle UI and menu aspect ratio scaling
// Base value is 1280 for 4:3, needs to be decreased with wider aspect ratios
static uint32_t g_baseRes = 1280; // Base 1280 for 4:3

// Aspect ratio multiplier for battle UI element positioning
// 1.0 for 4:3, 1.333 for 16:9, etc.
static float g_aspectRatioMultiplier = 1.0f;

// Menu container width in hd_draw_pause_menu – increased with aspect when main menu is open
static uint32_t g_gameMenuContainerRes = 1280;

// Main logo in hd_title_image_draw (movd xmm2,[render_size_w]) – base 1280,
// decrease with aspect ratio increase
static uint32_t g_mainLogoRes = 1280;

// Post-battle stat increase character name offset in pc_text_queue_draw
// (subss xmm3,[60.0]). Base 60.0; in battle we use 60*aspectoffset-10.
static float g_postBattleStatNameOffset = 60.0f;

// Menu UI underlines
static float g_battleMovss23 = 23.0f;
static float g_battleMovss735 = 735.0f;
// mulss xmm0 at hd_menu_underlines_draw_plus_3d8 – base 33.5, scaled by aspect
// (e.g. 16:9 = *0.75)
static float g_battleMulss33_5 = 33.5f;
// addss xmm0 at hd_menu_underlines_draw_plus_307 – base 31.5, scaled by aspect
// (e.g. 16:9 = *0.75)
static float g_battleAddss31_5 = 31.5f;
// addss xmm0 at hd_menu_underlines_draw_plus_428 – base 24.0, scaled by aspect
// (e.g. 16:9 = *0.75)
static float g_battleAddss24 = 24.0f;
// mulss xmm0 at hd_menu_underlines_draw_plus_66d – base 21.0, scaled by aspect
// (e.g. 16:9 = *0.75)
static float g_battleMulss21 = 21.0f;
static float g_battleAspectScale = 1.0f;
// Background X offset in hd_title_image_draw (movss xmm1,[-7.0]) – default
// -7.0f, add (1280 - (1280*aspect)) / 2 for proper offset (e.g. 16:9:
// -7+160=153)
static float g_backgroundXOffset = -7.0f;
static uintptr_t g_battleMovssEbx18ReturnAddr = 0;
static uintptr_t g_battleMovssEdi0CReturnAddr = 0;
static uintptr_t g_battleMovssEdi18ReturnAddr = 0;
static uintptr_t g_battleMovssEsp20ReturnAddr = 0;
// movss xmm2 at hd_menu_underlines_draw_plus_3b7: load from game address, scale by aspect (no hardcoded
// value)
static uintptr_t g_battleMovss29C07Addr = 0;
static uintptr_t g_battleMovss29C07ReturnAddr = 0;

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

// Battle dialog cursor: add 30*aspectRatio to font_text_pen_x after the
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

// hd_menu_underlines_draw_plus_193: movss xmm0,[ebx-18]. Scale value by aspect
// (e.g. 16:9 = *0.75).
__declspec(naked) static void BattleMovssEbx18Hook() {
  __asm {
		movss xmm0, [ebx-18h]
		mulss xmm0, dword ptr [g_battleAspectScale]
		mov eax, dword ptr [g_battleMovssEbx18ReturnAddr]
		jmp eax
  }
}

// hd_menu_underlines_draw_plus_4e4: movss xmm0,[edi-0C]. Scale value by aspect
// (e.g. 16:9 = *0.75).
__declspec(naked) static void BattleMovssEdi0CHook() {
  __asm {
		movss xmm0, [edi-0Ch]
		mulss xmm0, dword ptr [g_battleAspectScale]
		mov eax, dword ptr [g_battleMovssEdi0CReturnAddr]
		jmp eax
  }
}

// hd_menu_underlines_draw_plus_77f: movss xmm0,[edi-18]. Scale value by aspect
// (e.g. 16:9 = *0.75).
__declspec(naked) static void BattleMovssEdi18Hook() {
  __asm {
		movss xmm0, [edi-18h]
		mulss xmm0, dword ptr [g_battleAspectScale]
		mov eax, dword ptr [g_battleMovssEdi18ReturnAddr]
		jmp eax
  }
}

// hd_menu_underlines_draw_plus_581: movss xmm0,[esp+20]. Scale value by aspect
// (e.g. 16:9 = *0.75). Preserve eax (game uses it: lea at 29DC8, push at
// 29DE7).
__declspec(naked) static void BattleMovssEsp20Hook() {
  __asm {
		push eax
		mov edx, dword ptr [g_battleMovssEsp20ReturnAddr]
		movss xmm0, [esp+24h]
		mulss xmm0, dword ptr [g_battleAspectScale]
		pop eax
		jmp edx
  }
}

// hd_menu_underlines_draw_plus_3b7: movss xmm2,[addr]. Load from game memory,
// scale by aspect (e.g. 16:9 = *0.75).
__declspec(naked) static void BattleMovss29C07Hook() {
  __asm {
		push ecx
		mov ecx, dword ptr [g_battleMovss29C07Addr]
		movss xmm2, [ecx]
		pop ecx
		mulss xmm2, dword ptr [g_battleAspectScale]
		mov eax, dword ptr [g_battleMovss29C07ReturnAddr]
		jmp eax
  }
}

// Redirect the absolute operand at operandOffset to target after checking it
// still points at the original constant.
static bool PatchOperand(uintptr_t base, uintptr_t operandOffset,
                         uintptr_t originalOffset, const void *target,
                         const char *name) {
  if (!VerifyOperand(base + operandOffset, base + originalOffset, name) ||
      !RedirectOperand(base + operandOffset, target)) {
    std::cout << "Failed to apply battle UI/menu patch: " << name << std::endl;
    return false;
  }
  return true;
}

// Install a JMP hook at siteOffset after checking the first checkSize bytes
// are the original instruction bytes.
static bool PatchJmp(uintptr_t base, uintptr_t siteOffset,
                     const unsigned char *original, size_t checkSize,
                     void *hook, int totalSize, const char *name) {
  if (!VerifyBytes(base + siteOffset, original, checkSize, name) ||
      !InstallJmpHook(base + siteOffset, hook, totalSize)) {
    std::cout << "Failed to apply battle UI/menu patch: " << name << std::endl;
    return false;
  }
  return true;
}

bool ApplyBattleUIAndMenuPatch(uintptr_t base) {
  g_inBattleAddr = base + rde::name_game_mode;
  bool success = true;

  // ============================================================
  // Patches 1-7: Redirect operands to our dynamic g_baseRes
  // ============================================================
  struct {
    uintptr_t operand;
    const char *name;
  } operandPatches[] = {
      {rde::hd_world_name_draw_plus_255, "battle elements width (adc)"},
      {rde::pc_text_queue_layer_draw_plus_60,
       "battle text containers (and #1)"},
      {rde::hd_overlay_sprites_draw_plus_38,
       "battle elements container (movd mm1)"},
      {rde::pc_text_queue_draw_plus_4c, "menu / post battle UI (and #2)"},
      {rde::hd_menu_compass_hand_draw_plus_38,
       "menu compass hand (movd xmm1)"},
  };

  // Main logo (movd xmm2,[render_size_w]) – base 1280, decrease with aspect
  // ratio increase
  success &= PatchOperand(base, rde::hd_title_image_draw_plus_28,
                          rde::render_size_w, &g_mainLogoRes,
                          "main logo (movd xmm2)");

  // Menu container width (movd xmm0,[render_size_w]) – increased with aspect,
  // only effective when main menu is open
  success &= PatchOperand(base, rde::hd_draw_pause_menu_plus_27e,
                          rde::render_size_w, &g_gameMenuContainerRes,
                          "menu containers (movd xmm0)");

  // Post-battle stat increase character name offset (subss xmm3,[60.0]). We
  // supply 60*aspectoffset-10 in battle, else 60.
  success &= PatchOperand(base, rde::pc_text_queue_draw_plus_428,
                          rde::rdata_f32_60, &g_postBattleStatNameOffset,
                          "post-battle stat name offset (subss)");

  // movss xmm3,[23.0] – load from our float (base 23.0 * aspect)
  success &= PatchOperand(base, rde::hd_menu_underlines_draw_plus_8c,
                          rde::rdata_f32_23, &g_battleMovss23,
                          "movss xmm3 at hd_menu_underlines_draw_plus_8c");
  // movss xmm2,[735.0] – load from our float (base 735.0 * aspect)
  success &= PatchOperand(base, rde::hd_menu_underlines_draw_plus_5c,
                          rde::rdata_f32_735, &g_battleMovss735,
                          "movss xmm2 at hd_menu_underlines_draw_plus_5c");
  // mulss xmm0,[33.5] – load from our float (base 33.5 * aspect)
  success &= PatchOperand(base, rde::hd_menu_underlines_draw_plus_3d8,
                          rde::rdata_f32_33_5, &g_battleMulss33_5,
                          "mulss xmm0 at hd_menu_underlines_draw_plus_3d8");
  // addss xmm0,[31.5] – load from our float (base 31.5 * aspect)
  success &= PatchOperand(base, rde::hd_menu_underlines_draw_plus_307,
                          rde::rdata_f32_31_5, &g_battleAddss31_5,
                          "addss xmm0 at hd_menu_underlines_draw_plus_307");
  // addss xmm0,[24.0] – load from our float (base 24.0 * aspect)
  success &= PatchOperand(base, rde::hd_menu_underlines_draw_plus_428,
                          rde::rdata_f32_24, &g_battleAddss24,
                          "addss xmm0 at hd_menu_underlines_draw_plus_428");
  // movss xmm2,[735.0] – same 735.0 * aspect as above
  success &= PatchOperand(base, rde::hd_menu_underlines_draw_plus_65b,
                          rde::rdata_f32_735, &g_battleMovss735,
                          "movss xmm2 at hd_menu_underlines_draw_plus_65b");
  // mulss xmm0,[21.0] – load from our float (base 21.0 * aspect)
  success &= PatchOperand(base, rde::hd_menu_underlines_draw_plus_66d,
                          rde::rdata_f32_21, &g_battleMulss21,
                          "mulss xmm0 at hd_menu_underlines_draw_plus_66d");
  // addss xmm0,[31.5] – same 31.5 * aspect as above
  success &= PatchOperand(base, rde::hd_menu_underlines_draw_plus_7d0,
                          rde::rdata_f32_31_5, &g_battleAddss31_5,
                          "addss xmm0 at hd_menu_underlines_draw_plus_7d0");
  // Background X offset (movss xmm1,[-7.0]) – base -7.0f, add
  // (1280-(1280*aspect))/2 for proper offset
  success &= PatchOperand(base, rde::hd_title_image_draw_plus_a4,
                          rde::rdata_f32_minus_7, &g_backgroundXOffset,
                          "background X offset (movss xmm1)");
  // NOP the jne (75 0A) before it – always take the movss path
  {
    const unsigned char original[2] = {0x75, 0x0A};
    const unsigned char nops[2] = {0x90, 0x90};
    if (!VerifyBytes(base + rde::hd_title_image_draw_plus_9e, original, 2,
                     "NOP jne at hd_title_image_draw_plus_9e") ||
        !WriteMemory(base + rde::hd_title_image_draw_plus_9e, nops, 2)) {
      std::cout << "Failed to apply battle UI/menu patch: NOP jne at "
                   "hd_title_image_draw_plus_9e"
                << std::endl;
      success = false;
    }
  }

  for (const auto &p : operandPatches) {
    success &= PatchOperand(base, p.operand, rde::render_size_w, &g_baseRes,
                            p.name);
  }

  // ============================================================
  // Patch 8: Battle UI Element Position at
  // hook_hd_overlay_sprite_800c2f88_rep_plus_88
  // Original: movups [eax],xmm0 (3 bytes) + start of mov [eax+10],0 (we
  // overwrite 10 bytes). Scale position at [eax] and width at [eax+4] by aspect
  // ratio, then run overwritten instruction.
  // ============================================================
  {
    const unsigned char original[10] = {0x0F, 0x11, 0x00, 0xC7, 0x40,
                                        0x10, 0x00, 0x00, 0x00, 0x00};
    uintptr_t hookAddr8 = base + rde::hook_hd_overlay_sprite_800c2f88_rep_plus_88;
    g_battleUIElementPosReturnAddr = hookAddr8 + 10;

    success &= PatchJmp(base, rde::hook_hd_overlay_sprite_800c2f88_rep_plus_88,
                        original, sizeof(original),
                        (void *)&BattleUIElementPositionHook, 10,
                        "battle UI element position (patch 8)");
  }

  // ============================================================
  // Patch 9: pc_text_queue_layer_draw_plus_6ff - Battle dialog text position
  // Original: movss xmm1,[ebp-00000090]. Scale by aspect ratio, jump back.
  // ============================================================
  {
    const unsigned char original[8] = {0xF3, 0x0F, 0x10, 0x8D,
                                       0x70, 0xFF, 0xFF, 0xFF};
    uintptr_t hookAddr9 = base + rde::pc_text_queue_layer_draw_plus_6ff;
    g_battleDialogTextPosReturnAddr = hookAddr9 + 8;

    success &= PatchJmp(base, rde::pc_text_queue_layer_draw_plus_6ff, original,
                        sizeof(original),
                        (void *)&BattleDialogTextPositionHook, 8,
                        "battle dialog text position (patch 9)");
  }

  // ============================================================
  // Patch 10: hook_font_text_render_800c0228_obs_plus_2bc - Battle dialog
  // cursor
  // Original: add [font_text_pen_x], eax (6 bytes). We add 30*aspectRatio after
  // it.
  // ============================================================
  {
    const unsigned char original[2] = {0x01, 0x05};
    uintptr_t hookAddr10 =
        base + rde::hook_font_text_render_800c0228_obs_plus_2bc;
    g_battleDialogCursorAddr = base + rde::font_text_pen_x;
    g_battleDialogCursorReturnAddr = hookAddr10 + 6;

    if (!VerifyOperand(hookAddr10 + sizeof(original), g_battleDialogCursorAddr,
                       "battle dialog cursor operand (patch 10)")) {
      success = false;
    } else {
      success &= PatchJmp(base,
                          rde::hook_font_text_render_800c0228_obs_plus_2bc,
                          original, sizeof(original),
                          (void *)&BattleDialogCursorHook, 6,
                          "battle dialog cursor (patch 10)");
    }
  }

  // ============================================================
  // Patch 11: hd_menu_underlines_draw_plus_193 - movss xmm0,[ebx-18]
  // Scale loaded value by aspect (e.g. 16:9 = *0.75) before use.
  // ============================================================
  {
    const unsigned char original[5] = {0xF3, 0x0F, 0x10, 0x43, 0xE8};
    g_battleMovssEbx18ReturnAddr =
        base + rde::hd_menu_underlines_draw_plus_193 + 5;
    success &= PatchJmp(base, rde::hd_menu_underlines_draw_plus_193, original,
                        sizeof(original), (void *)&BattleMovssEbx18Hook, 5,
                        "movss [ebx-18] (patch 11)");
  }

  // ============================================================
  // Patch 12: hd_menu_underlines_draw_plus_4e4 - movss xmm0,[edi-0C]
  // Scale loaded value by aspect (e.g. 16:9 = *0.75) before use.
  // ============================================================
  {
    const unsigned char original[5] = {0xF3, 0x0F, 0x10, 0x47, 0xF4};
    g_battleMovssEdi0CReturnAddr =
        base + rde::hd_menu_underlines_draw_plus_4e4 + 5;
    success &= PatchJmp(base, rde::hd_menu_underlines_draw_plus_4e4, original,
                        sizeof(original), (void *)&BattleMovssEdi0CHook, 5,
                        "movss [edi-0C] (patch 12)");
  }

  // ============================================================
  // Patch 13: hd_menu_underlines_draw_plus_3b7 - movss xmm2,[addr]
  // Load from game memory, scale by aspect (e.g. 16:9 = *0.75).
  // ============================================================
  {
    const unsigned char original[4] = {0xF3, 0x0F, 0x10, 0x15};
    g_battleMovss29C07Addr = base + rde::rdata_f32_852;
    g_battleMovss29C07ReturnAddr =
        base + rde::hd_menu_underlines_draw_plus_3b7 + 8;
    if (!VerifyOperand(base + rde::hd_menu_underlines_draw_plus_3b7 +
                           sizeof(original),
                       g_battleMovss29C07Addr, "movss xmm2 operand (patch 13)")) {
      success = false;
    } else {
      success &= PatchJmp(base, rde::hd_menu_underlines_draw_plus_3b7, original,
                          sizeof(original), (void *)&BattleMovss29C07Hook, 8,
                          "movss xmm2 (patch 13)");
    }
  }

  // ============================================================
  // Patch 14: hd_menu_underlines_draw_plus_581 - movss xmm0,[esp+20]
  // Scale loaded value by aspect (e.g. 16:9 = *0.75) before use.
  // ============================================================
  {
    const unsigned char original[6] = {0xF3, 0x0F, 0x10, 0x44, 0x24, 0x20};
    g_battleMovssEsp20ReturnAddr =
        base + rde::hd_menu_underlines_draw_plus_581 + 6;
    success &= PatchJmp(base, rde::hd_menu_underlines_draw_plus_581, original,
                        sizeof(original), (void *)&BattleMovssEsp20Hook, 6,
                        "movss [esp+20] (patch 14)");
  }

  // ============================================================
  // Patch 15: hd_menu_underlines_draw_plus_77f - movss xmm0,[edi-18]
  // Scale loaded value by aspect (e.g. 16:9 = *0.75) before use.
  // ============================================================
  {
    const unsigned char original[5] = {0xF3, 0x0F, 0x10, 0x47, 0xE8};
    g_battleMovssEdi18ReturnAddr =
        base + rde::hd_menu_underlines_draw_plus_77f + 5;
    success &= PatchJmp(base, rde::hd_menu_underlines_draw_plus_77f, original,
                        sizeof(original), (void *)&BattleMovssEdi18Hook, 5,
                        "movss [edi-18] (patch 15)");
  }

  return success;
}

bool IsInBattle() { return ReadGameByte(g_inBattleAddr) == 1; }

void UpdateBattleUIAndMenuValues(float aspectRatio, bool isGameMenuOpen) {
  // Post-battle stat name offset: only scale in battle, else base 60
  if (aspectRatio < WIDESCREEN_THRESHOLD || !IsInBattle()) {
    g_postBattleStatNameOffset = 60.0f;
  } else {
    float aspectOffset = BASE_ASPECT_RATIO / aspectRatio; // e.g. 0.75 for 16:9
    g_postBattleStatNameOffset = 60.0f * aspectOffset - 10.0f;
  }

  // underline floats and [ebx-18] in hd_menu_underlines_draw: scale by aspect
  // (e.g. 16:9 = *0.75)
  float aspectOffset = (aspectRatio >= WIDESCREEN_THRESHOLD)
                           ? (BASE_ASPECT_RATIO / aspectRatio)
                           : 1.0f;
  g_battleMovss23 = 23.0f * aspectOffset;
  g_battleMovss735 = 735.0f * aspectOffset;
  g_battleMulss33_5 = 33.5f * aspectOffset;
  g_battleAddss31_5 = 31.5f * aspectOffset;
  g_battleAddss24 = 24.0f * aspectOffset;
  g_battleMulss21 = 21.0f * aspectOffset;
  g_battleAspectScale = aspectOffset;
  // Background X offset: -7 + (1280 - (1280*aspect)) / 2 (e.g. 16:9:
  // -7+160=153)
  g_backgroundXOffset = -7.0f + (1280.0f - (1280.0f * aspectOffset)) * 0.5f;

  if (aspectRatio < WIDESCREEN_THRESHOLD) {
    // Reset to 4:3 default
    if (g_baseRes != 1280 || g_aspectRatioMultiplier != 1.0f ||
        g_gameMenuContainerRes != 1280 || g_mainLogoRes != 1280) {
      g_baseRes = 1280;
      g_aspectRatioMultiplier = 1.0f;
      g_battleDialogCursorAddend = 30; // 30 * 1.0
      g_gameMenuContainerRes = 1280;
      g_mainLogoRes = 1280;
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
    uint32_t newBaseRes =
        (uint32_t)(1280.0f / (aspectRatio / BASE_ASPECT_RATIO));
    uint32_t newMainLogoRes = newBaseRes; // same formula for main logo

    // Calculate aspect ratio multiplier
    // For 4:3: 1.333 / 1.333 = 1.0
    // For 16:9: 1.777 / 1.333 = 1.333
    float newAspectMultiplier = aspectRatio / BASE_ASPECT_RATIO;
    int32_t newCursorAddend = (int32_t)(30.0f * newAspectMultiplier + 0.5f);

    // Menu container in hd_draw_pause_menu: increase with aspect, only when main menu open
    uint32_t newGameMenuContainerRes =
        isGameMenuOpen
            ? (uint32_t)(1280.0f * (aspectRatio / BASE_ASPECT_RATIO) + 0.5f)
            : 1280;

    if (g_baseRes != newBaseRes ||
        g_aspectRatioMultiplier != newAspectMultiplier ||
        g_battleDialogCursorAddend != newCursorAddend ||
        g_gameMenuContainerRes != newGameMenuContainerRes ||
        g_mainLogoRes != newMainLogoRes) {
      g_baseRes = newBaseRes;
      g_aspectRatioMultiplier = newAspectMultiplier;
      g_battleDialogCursorAddend = newCursorAddend;
      g_gameMenuContainerRes = newGameMenuContainerRes;
      g_mainLogoRes = newMainLogoRes;
#ifdef _DEBUG
      std::cout << "Battle UI/Menu baseRes updated: " << g_baseRes
                << ", aspect multiplier: " << g_aspectRatioMultiplier
                << " for aspect ratio " << aspectRatio << std::endl;
#endif
    }
  }
}

float *GetAspectRatioMultiplierPtr() { return &g_aspectRatioMultiplier; }
