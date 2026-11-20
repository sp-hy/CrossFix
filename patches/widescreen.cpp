#include "widescreen.h"
#include "../utils/memory.h"
#include "../utils/rde_symbols.h"
#include "battleuimenu.h"
#include "dialog.h"
#include "widescreen2d.h"
#include <iostream>

// Dynamic monitoring state
static HANDLE g_monitorThread = nullptr;
static bool g_monitoringActive = false;
static uintptr_t g_monitorBase = 0;
static float g_lastAspectRatio = 0.0f;
static bool g_wasWidescreen = false;

// Original bytes storage for restoring default behavior
static unsigned char g_originalBytes1[8] = {0};
static unsigned char g_originalBytes2[8] = {0};
static bool g_originalBytesSaved = false;

// Dynamic widescreen ratio (calculated from aspect ratio)
static float g_widescreenRatio3D = 1.0f; // Default to 4:3 (no scaling)

// Universal widescreen hook - uses dynamic ratio
__declspec(naked) void WidescreenHookUniversal() {
  __asm {
    // Save registers
		push eax
		push edx

        // Convert edi to float
		cvtsi2ss xmm0, edi

        // Multiply by the dynamic ratio
		movss xmm1, dword ptr [g_widescreenRatio3D]
		mulss xmm0, xmm1

        // Convert back to integer
		cvttss2si eax, xmm0
		mov edi, eax

        // Restore registers
		pop edx
		pop eax

            // Execute the original instructions we replaced
		mov eax, [esi+0x60] // original: read OFX
		cdq // original: sign extend
		add edi, eax // original: add OFX
		adc ecx, edx // original: add with carry
		ret
  }
}

// Calculate widescreen ratio from aspect ratio
float CalculateWidescreenRatio(float aspectRatio) {
  // Formula: (4/3) / aspectRatio
  // This gives us the compression ratio needed to fit 4:3 content into
  // widescreen
  return BASE_ASPECT_RATIO / aspectRatio;
}

// Apply the widescreen patches with dynamic aspect ratio
bool ApplyWidescreenPatch(uintptr_t base, float aspectRatio) {
  // Calculate the widescreen ratio from aspect ratio
  g_widescreenRatio3D = CalculateWidescreenRatio(aspectRatio);

  // Also set the 2D ratio
  SetWidescreen2DRatio(g_widescreenRatio3D);

  // Calculate the addresses to patch
  uintptr_t addr1 = base + rde::gte_ofx_add_b;
  uintptr_t addr2 = base + rde::gte_ofx_add_a;

  // Save original bytes if not already saved
  if (!g_originalBytesSaved) {
    const unsigned char original[8] = {0x8B, 0x46, 0x60, 0x99,
                                       0x03, 0xF8, 0x13, 0xCA};
    if (!VerifyBytes(addr1, original, sizeof(original), "Widescreen hook 1") ||
        !VerifyBytes(addr2, original, sizeof(original), "Widescreen hook 2")) {
      return false;
    }
    memcpy(g_originalBytes1, (void *)addr1, 8);
    memcpy(g_originalBytes2, (void *)addr2, 8);
    g_originalBytesSaved = true;
  }

  // Apply the CALL hooks
  if (!InstallCallHook(addr1, (void *)WidescreenHookUniversal, 8)) {
    std::cout << "Failed to patch address 1 (0x" << std::hex << addr1 << ")"
              << std::dec << std::endl;
    return false;
  }

  if (!InstallCallHook(addr2, (void *)WidescreenHookUniversal, 8)) {
    std::cout << "Failed to patch address 2 (0x" << std::hex << addr2 << ")"
              << std::dec << std::endl;
    return false;
  }

  g_wasWidescreen = true;
  return true;
}

// Auto-detect and optionally return the detected aspect ratio
bool ApplyWidescreenPatchAuto(uintptr_t base, float *outAspectRatio) {
  // Memory addresses for screen resolution
  uintptr_t resXAddr = base + rde::render_screen_width;
  uintptr_t resYAddr = base + rde::render_screen_height;

  // Read the resolution values from memory with retry logic
  int resX = 0;
  int resY = 0;
  const int maxRetries = 10;
  const int retryDelayMs = 100;

  for (int attempt = 0; attempt < maxRetries; attempt++) {
    try {
      resX = *(int *)resXAddr;
      resY = *(int *)resYAddr;

      if (resX > 0 && resY > 0)
        break;

      if (attempt < maxRetries - 1)
        Sleep(retryDelayMs);
    } catch (...) {
      std::cout << "Failed to read screen resolution from memory!" << std::endl;
      return false;
    }
  }

  if (resX <= 0 || resY <= 0) {
    std::cout << "Invalid screen resolution detected: " << resX << "x" << resY
              << std::endl;
    return false;
  }

  float aspectRatio = (float)resX / (float)resY;

#ifdef _DEBUG
  std::cout << "Detected screen resolution: " << resX << "x" << resY
            << " (aspect ratio: " << aspectRatio << ")" << std::endl;
#endif

  if (outAspectRatio != nullptr) {
    *outAspectRatio = aspectRatio;
  }

  // If it's not widescreen (e.g. 4:3), don't apply
  if (aspectRatio < WIDESCREEN_THRESHOLD) {
    std::cout << "Aspect ratio is not widescreen. Widescreen patch not applied."
              << std::endl;
    return false;
  }

  return ApplyWidescreenPatch(base, aspectRatio);
}

// Restore default (non-widescreen) behavior
bool RestoreDefaultBehavior(uintptr_t base) {
  if (!g_originalBytesSaved) {
    std::cout << "Cannot restore default behavior: original bytes not saved"
              << std::endl;
    return false;
  }

  uintptr_t addr1 = base + rde::gte_ofx_add_b;
  uintptr_t addr2 = base + rde::gte_ofx_add_a;

  // Restore original bytes
  if (!WriteMemory(addr1, g_originalBytes1, 8)) {
    std::cout << "Failed to restore address 1 (0x" << std::hex << addr1 << ")"
              << std::dec << std::endl;
    return false;
  }

  if (!WriteMemory(addr2, g_originalBytes2, 8)) {
    std::cout << "Failed to restore address 2 (0x" << std::hex << addr2 << ")"
              << std::dec << std::endl;
    return false;
  }

  std::cout << "Restored default (4:3) behavior" << std::endl;

  // Reset widescreen ratio to 1.0 (no scaling)
  g_widescreenRatio3D = 1.0f;

  // Also disable 2D widescreen and reset its ratio
  SetWidescreen2DRatio(1.0f);
  SetWidescreen2DEnabled(false);
  g_wasWidescreen = false;

  return true;
}

// Thread function for dynamic resolution monitoring
DWORD WINAPI ResolutionMonitorThread(LPVOID param) {
  uintptr_t base = (uintptr_t)param;
  uintptr_t resXAddr = base + rde::render_screen_width;
  uintptr_t resYAddr = base + rde::render_screen_height;
  uintptr_t screenTypeAddr =
      base + rde::render_screen_type; // Screen type: 0 = normal, 1 = full

  // Initialize state
  int lastResX = 0;
  int lastResY = 0;
  float lastAspectRatio = 0.0f;

  // Read initial values
  try {
    lastResX = *(int *)resXAddr;
    lastResY = *(int *)resYAddr;
    if (lastResX > 0 && lastResY > 0) {
      lastAspectRatio = (float)lastResX / (float)lastResY;
      g_lastAspectRatio = lastAspectRatio;
    }
  } catch (...) {
    // If we can't read initial values, start with defaults
  }

  while (g_monitoringActive) {
    try {
      int resX = *(int *)resXAddr;
      int resY = *(int *)resYAddr;

      // Check if resolution has changed
      if (resX > 0 && resY > 0 && (resX != lastResX || resY != lastResY)) {
        lastResX = resX;
        lastResY = resY;

        // Calculate new aspect ratio
        float aspectRatio = (float)resX / (float)resY;
        bool isWidescreen = aspectRatio >= WIDESCREEN_THRESHOLD;

        // Only apply patch if aspect ratio has actually changed
        bool aspectRatioChanged = (aspectRatio != lastAspectRatio);
        lastAspectRatio = aspectRatio;
        g_lastAspectRatio = aspectRatio;

        if (!isWidescreen && g_wasWidescreen) {
          // Switch from widescreen to 4:3
          std::cout << "Resolution changed to " << resX << "x" << resY
                    << " (aspect ratio: " << aspectRatio << ")" << std::endl;
          std::cout << "Switching to default 4:3 behavior" << std::endl;

          RestoreDefaultBehavior(base);
        } else if (isWidescreen && aspectRatioChanged) {
          // We just need to update the ratio, ApplyWidescreenPatch handles
          // everything
          if (ApplyWidescreenPatch(base, aspectRatio)) {
            // Always enable 2D widescreen when applying widescreen patch
            SetWidescreen2DEnabled(true);
          }
        }
      }

      // Force screen type to full (1) when in widescreen mode
      if (g_wasWidescreen) {
        // Check if FMV is playing
        uintptr_t isFMVPlayingAddr = base + rde::fld_movie_active;
        int isFMVPlaying = *(int *)isFMVPlayingAddr;

        // Set screenType to 0 if FMV is playing, otherwise 1
        int targetScreenType = (isFMVPlaying == 1) ? 0 : 1;
        int currentScreenType = *(int *)screenTypeAddr;
        if (currentScreenType != targetScreenType) {
          WriteMemory(screenTypeAddr, &targetScreenType, sizeof(int));
        }
      }

      // Update dialog values based on aspect ratio and battle status
      UpdateDialogValues(g_lastAspectRatio, IsInBattle());

      // Update battle UI and menu values based on aspect ratio
      UpdateBattleUIAndMenuValues(g_lastAspectRatio, IsGameMenuOpen());
    } catch (...) {
      // Ignore errors and continue monitoring
    }

    // Check every 50ms
    Sleep(50);
  }

  return 0;
}

// Start dynamic resolution monitoring
void StartDynamicWidescreenMonitoring(uintptr_t base) {
  if (g_monitoringActive) {
    return; // Already monitoring
  }

  g_monitorBase = base;
  g_monitoringActive = true;

  g_monitorThread = CreateThread(nullptr, 0, ResolutionMonitorThread,
                                 (LPVOID)base, 0, nullptr);

  if (!g_monitorThread) {
    std::cout << "Failed to start dynamic widescreen monitoring thread!"
              << std::endl;
    g_monitoringActive = false;
  }
}

// Stop dynamic resolution monitoring
void StopDynamicWidescreenMonitoring() {
  if (!g_monitoringActive) {
    return; // Not monitoring
  }

  g_monitoringActive = false;

  if (g_monitorThread) {
    WaitForSingleObject(g_monitorThread, 2000); // Wait up to 2 seconds
    CloseHandle(g_monitorThread);
    g_monitorThread = nullptr;
  }

  std::cout << "Dynamic widescreen monitoring stopped" << std::endl;
}

// Get the current widescreen ratio
float GetCurrentWidescreenRatio() { return g_widescreenRatio3D; }
