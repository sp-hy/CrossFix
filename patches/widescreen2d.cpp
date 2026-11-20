/*
 * Widescreen 2D System
 *
 * In widescreen the field is drawn over the whole window and every projected
 * vertex is scaled horizontally by the widescreen ratio about the projection
 * centre x = 160 (widescreen.cpp). This file does the same for the HD
 * background layers and keeps the camera inside the room's picture:
 *
 * 1. Layers: when the room's chip2 layers are built, each layer quad's x and
 *    width are scaled by the ratio about x = 160, so backgrounds stay aligned
 *    with the actors in every room.
 *
 * 2. Scroll clamp: guest hooks catch every write of the room's pan limits (the
 *    room loader and the limit script op). The written values are kept as the
 *    room's own limits and replaced in guest RAM by the effective limits for
 *    the current ratio: the limits scaled by the ratio, then narrowed by the
 *    extra guest columns per side, e = 160 * (1 - ratio); both become the
 *    midpoint when they cross. y is untouched. A per-tick hook on the scroll
 *    update re-applies them when the ratio or the graphics mode changes.
 */

#include "widescreen2d.h"
#include "../utils/memory.h"
#include "../utils/rde_symbols.h"
#include <cmath>
#include <iostream>

// Global variables
bool g_widescreen2DEnabled = true;
float g_widescreenRatio = 0.75f;
static constexpr float g_widescreenThreshold = 0.99f;
static constexpr float g_projectionCentreX = 160.0f;

// The room's own x limits, as last written by the game (guest thread only)
static bool g_haveRoomLimits = false;
static int16_t g_roomXMax = 0;
static int16_t g_roomXMin = 0;

// The effective x limits last written into the clamp
static bool g_haveWritten = false;
static int16_t g_writtenXMax = 0;
static int16_t g_writtenXMin = 0;

static uintptr_t g_base = 0;

static uint8_t *GuestPtr(uint32_t guestAddr) {
  return (uint8_t *)(g_base + rde::guest_ram_base +
                     (guestAddr - rde::guest_origin));
}

static bool FieldOverlayResident() {
  return *(volatile uint32_t *)GuestPtr(rde::mode_overlay_id) ==
         rde::mode_overlay_field;
}

static bool NarrowingActive(float ratio) {
  return g_widescreen2DEnabled &&
         ratio < g_widescreenThreshold &&
         *(volatile uint8_t *)(g_base + rde::sys_graphics_hd) != 0;
}

// Writes the effective x limits for the current ratio into the guest clamp
static void ApplyScrollClamp() {
  volatile int16_t *clamp =
      (volatile int16_t *)GuestPtr(rde::fld_scroll_clamp);
  int16_t xMax = g_roomXMax;
  int16_t xMin = g_roomXMin;

  float ratio = g_widescreenRatio;
  if (NarrowingActive(ratio)) {
    float extra = g_projectionCentreX * (1.0f - ratio);
    int hi = (int)std::floor(g_roomXMax * ratio - extra);
    int lo = (int)std::ceil(g_roomXMin * ratio + extra);
    if (hi < lo) {
      hi = lo = (int)std::lround((g_roomXMax + g_roomXMin) * ratio * 0.5f);
    }
    xMax = (int16_t)hi;
    xMin = (int16_t)lo;
  }

  if (clamp[0] != xMax)
    clamp[0] = xMax;
  if (clamp[1] != xMin)
    clamp[1] = xMin;
  g_writtenXMax = xMax;
  g_writtenXMin = xMin;
  g_haveWritten = true;
}

// Guest hook: the room loader or the limit script op has just written the
// limits. The hook can also run while the clamp still holds the values this
// file wrote; those are not the room's own and are ignored.
static void __cdecl OnScrollLimitsWritten() {
  if (!FieldOverlayResident())
    return;
  volatile int16_t *clamp =
      (volatile int16_t *)GuestPtr(rde::fld_scroll_clamp);
  if (g_haveWritten && clamp[0] == g_writtenXMax &&
      clamp[1] == g_writtenXMin)
    return;
  g_roomXMax = clamp[0];
  g_roomXMin = clamp[1];
  g_haveRoomLimits = true;
  ApplyScrollClamp();
}

// Guest hook: start of the per-tick scroll update
static void __cdecl OnScrollApply() {
  if (!g_haveRoomLimits || !FieldOverlayResident())
    return;
  ApplyScrollClamp();
}

// Hooks are registered once the guest has loaded its first mode overlay: the
// game installs its own hooks before any guest code runs, so the page table is
// set up and no installer writes to it any more. The ID word is zero until then
// (guest RAM is cleared at boot and the main executable's image is zero there).
static DWORD WINAPI RegisterGuestHooksThread(LPVOID) {
  volatile uint32_t *overlayId =
      (volatile uint32_t *)GuestPtr(rde::mode_overlay_id);
  while (*overlayId == 0)
    Sleep(10);

  typedef void(__stdcall * RegisterHook_t)(uint32_t, void (*)());
  RegisterHook_t registerHook =
      (RegisterHook_t)(g_base + rde::register_hook);
  registerHook(rde::fld_room_load_limits_done, OnScrollLimitsWritten);
  registerHook(rde::fld_set_scroll_limits_done, OnScrollLimitsWritten);
  registerHook(rde::fld_camera_scroll_apply, OnScrollApply);

#ifdef _DEBUG
  std::cout << "[Widescreen2D] Guest scroll hooks registered" << std::endl;
#endif
  return 0;
}

// Hook-related globals
static LPVOID g_hookAddress = nullptr;

// Runs for each HD layer quad of the room's chip2 layers before it is stored:
// [ebp-3Ch] is the quad's x and [ebp-38h] its width, in field pixels
__declspec(naked) void HookCave() {
  __asm {
        push ecx
        pushfd

        mov cl, byte ptr [g_widescreen2DEnabled]
        test cl, cl
        jz skip_transform

        movsx ecx, word ptr [ebp-38h]
        test ecx, ecx
        jle skip_transform

        // width' = width * ratio
        cvtsi2ss xmm0, ecx
        mulss xmm0, dword ptr [g_widescreenRatio]
        cvttss2si ecx, xmm0
        test ecx, ecx
        jle skip_transform
        mov word ptr [ebp-38h], cx

        // x' = 160 + (x - 160) * ratio
        movsx ecx, word ptr [ebp-3Ch]
        sub ecx, 160
        cvtsi2ss xmm0, ecx
        mulss xmm0, dword ptr [g_widescreenRatio]
        cvttss2si ecx, xmm0
        add ecx, 160
        mov word ptr [ebp-3Ch], cx

    skip_transform:
        popfd
        pop ecx

        // The two original instructions the CALL replaced
        mov eax, [ebp-70h]
        mov edx, [ebp-38h]
        ret
  }
}

bool InitWidescreen2DHook() {
  HMODULE hModule = GetModuleHandleA("CHRONOCROSS.exe");
  if (!hModule) {
    return false;
  }
  g_base = (uintptr_t)hModule;

  // CHRONOCROSS.exe+1CCF51: mov eax,[ebp-70] (3 bytes)
  // +1CCF54: mov edx,[ebp-38] (3 bytes)
  // Total: 6 bytes - enough for our 5-byte CALL + 1 NOP
  g_hookAddress = (LPVOID)(g_base + 0x1CCF51);

#ifdef _DEBUG
  std::cout << "[Widescreen2D] Hook address: 0x" << std::hex
            << (uintptr_t)g_hookAddress << std::dec << std::endl;
#endif

  if (!InstallCallHook((uintptr_t)g_hookAddress, (void *)HookCave, 6)) {
    return false;
  }

  HANDLE thread =
      CreateThread(NULL, 0, RegisterGuestHooksThread, NULL, 0, NULL);
  if (!thread) {
    return false;
  }
  CloseHandle(thread);

#ifdef _DEBUG
  std::cout << "[Widescreen2D] ASM hook installed successfully" << std::endl;
#endif

  return true;
}

void SetWidescreen2DRatio(float ratio) { g_widescreenRatio = ratio; }

void SetWidescreen2DEnabled(bool enabled) {
  g_widescreen2DEnabled = enabled;
#ifdef _DEBUG
  std::cout << "[Widescreen2D] " << (enabled ? "Enabled" : "Disabled")
            << std::endl;
#endif
}
