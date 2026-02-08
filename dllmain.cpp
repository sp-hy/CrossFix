#include "d3d11/d3d11_proxy.h"
#include "patches/battleuimenu.h"
#include "patches/dialog.h"
#include "patches/fps.h"
#include "patches/misc.h"
#include "patches/modloader.h"
#include "patches/pausefix.h"
#include "patches/saveselector.h"
#include "patches/widescreen.h"
#include "patches/widescreen2d.h"
#include "utils/settings.h"
#include "utils/version.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

HANDLE crossfix_handle = GetCurrentProcess();

// Global flag to indicate if patches should be applied
bool g_versionCheckPassed = false;

DWORD WINAPI MainThread(LPVOID param) {
  uintptr_t base = (uintptr_t)GetModuleHandle(NULL);

  char exePath[MAX_PATH];
  if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) {
    return 0;
  }

  char *exeName = strrchr(exePath, '\\');
  if (exeName == NULL) {
    exeName = exePath;
  } else {
    exeName++;
  }

  std::string exeNameLower(exeName);
  std::transform(exeNameLower.begin(), exeNameLower.end(), exeNameLower.begin(),
                 ::tolower);

  if (exeNameLower != "chronocross.exe") {
    return 0;
  }

  AllocConsole();
  FILE *fout;
  FILE *fin;
  freopen_s(&fout, "CONOUT$", "w", stdout);
  freopen_s(&fin, "CONIN$", "r", stdin);

  std::cout << "CrossFix - v0.8" << std::endl;
  std::cout << std::endl;
  std::cout << "https://github.com/sp-hy/CrossFix || "
               "https://www.nexusmods.com/chronocrosstheradicaldreamersedition/"
               "mods/77"
            << std::endl;
  std::cout << std::endl;

  // Load settings early so mod loader can check its toggle
  std::string settingsPath = Settings::GetSettingsPath();
  std::string exePathStr(exePath);

  Settings settings;
  settings.Load(settingsPath);

  // Check executable version first — must pass before any patches (including
  // mod loader)
  const char *expectedVersion = "1.0.1.0";
  if (!CheckExecutableVersion(exePath, expectedVersion)) {
    std::cout << std::endl;
    std::cout << "ERROR: Incompatible game version detected!" << std::endl;
    std::cout << "Patching has been disabled." << std::endl;
    std::cout << std::endl;
    // Don't apply any patches - just exit gracefully
    return 0;
  }

  // Version check passed - enable patching (set BEFORE InitModLoader to avoid
  // race: if the game creates D3D11 device while mod loader init runs, hooks
  // would be skipped)
  g_versionCheckPassed = true;

  // Initialize mod loader — hooks must be active before game opens hd.dat
  bool modLoaderEnabled = false;
  if (settings.GetBool("mod_loader_enabled", true)) {
    modLoaderEnabled = InitModLoader(exePathStr);
  }

  if (modLoaderEnabled) {
    std::cout << std::endl;
    std::cout << "[ModLoader] Mod loader enabled" << std::endl;
  }

  std::cout << std::endl;
#ifdef _DEBUG
  std::cout << "DLL loaded successfully! Base address of the injected "
               "executable is: 0x"
            << std::hex << base << std::dec << std::endl;
#endif
  std::cout << std::endl;

  InitD3D11Proxy();

  bool widescreenEnabled = settings.GetBool("widescreen_enabled", true);

  if (widescreenEnabled) {
    std::cout << "Initializing dynamic widescreen patch system..." << std::endl;

    // Try to apply initial widescreen patch (3D)
    if (!ApplyWidescreenPatchAuto(base)) {
      std::cout << "Initial aspect ratio is not widescreen or detection "
                   "failed. Monitoring for changes..."
                << std::endl;
    } else {
      SetWidescreen2DEnabled(true);
    }

    // Always initialize 2D and dialog patches if widescreen features are
    // enabled
    if (!InitWidescreen2DHook()) {
      std::cout << "Failed to initialize 2D widescreen ASM hook!" << std::endl;
    }

    ApplyDialogPatch(base);
    ApplyBattleUIAndMenuPatch(base);
    ApplySaveSelectorPatch(base, GetAspectRatioMultiplierPtr());

    // Always start dynamic monitoring to handle resolution changes
    StartDynamicWidescreenMonitoring(base);

    // Apply boundary overrides if enabled in settings
    if (settings.GetBool("boundary_overrides", true)) {
      SetBoundaryOverridesEnabled(true);
    }
  } else {
    std::cout << "Widescreen patches disabled in settings" << std::endl;
  }

  if (settings.GetBool("double_fps_mode", false)) {
    ApplyDoubleFpsPatch(base);
  }

  if (settings.GetBool("hide_slow_icon", true)) {
    ApplyHideSlowIconPatch(base);
  }

  if (settings.GetBool("disable_pause_on_focus_loss", true)) {
    ApplyDisablePausePatch(base);
  } else {
    std::cout << "Disable pause on focus loss disabled in settings"
              << std::endl;
  }

  ApplyVerticalBorderRemoverPatch(base);

  return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  switch (ul_reason_for_call) {
  case DLL_PROCESS_ATTACH:
    CreateThread(0, 0, MainThread, hModule, 0, 0);
    break;
  case DLL_THREAD_ATTACH:
  case DLL_THREAD_DETACH:
  case DLL_PROCESS_DETACH:
    break;
  }
  return TRUE;
}
