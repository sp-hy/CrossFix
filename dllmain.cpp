#include <Windows.h>
#include <iostream>
#include <string>
#include <algorithm>
#include <cctype>
#include "patches/widescreen.h"
#include "patches/widescreen2d.h"
#include "d3d11/d3d11_proxy.h"
#include "patches/fps.h"
#include "patches/pausefix.h"
#include "patches/dialog.h"
#include "patches/battleuimenu.h"
#include "patches/misc.h"
#include "utils/settings.h"
#include "utils/version.h"

HANDLE crossfix_handle = GetCurrentProcess();

// Global flag to indicate if patches should be applied
bool g_versionCheckPassed = false;

DWORD WINAPI MainThread(LPVOID param) {
	uintptr_t base = (uintptr_t)GetModuleHandle(NULL);

	char exePath[MAX_PATH];
	if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) {
		return 0;
	}

	char* exeName = strrchr(exePath, '\\');
	if (exeName == NULL) {
		exeName = exePath;
	} else {
		exeName++;
	}

	std::string exeNameLower(exeName);
	std::transform(exeNameLower.begin(), exeNameLower.end(), exeNameLower.begin(), ::tolower);

	if (exeNameLower != "chronocross.exe") {
		return 0;
	}

	AllocConsole();
	FILE* fout;
	FILE* fin;
	freopen_s(&fout, "CONOUT$", "w", stdout);
	freopen_s(&fin, "CONIN$", "r", stdin);

	std::cout << "CrossFix - v0.6" << std::endl;
	std::cout << std::endl;
	std::cout << "https://github.com/sp-hy/CrossFix || https://www.nexusmods.com/chronocrosstheradicaldreamersedition/mods/77" << std::endl;
	std::cout << std::endl;

	// Check executable version
	const char* expectedVersion = "1.0.1.0";
	if (!CheckExecutableVersion(exePath, expectedVersion)) {
		std::cout << std::endl;
		std::cout << "ERROR: Incompatible game version detected!" << std::endl;
		std::cout << "Patching has been disabled." << std::endl;
		std::cout << std::endl;
		// Don't apply any patches - just exit gracefully
		return 0;
	}

	// Version check passed - enable patching
	g_versionCheckPassed = true;

	std::cout << std::endl;
#ifdef _DEBUG
	std::cout << "DLL loaded successfully! Base address of the injected executable is: 0x" << std::hex << base << std::dec << std::endl;
#endif
	std::cout << std::endl;

	std::string settingsPath = "settings.ini";
	std::string exePathStr(exePath);
	size_t lastBackslash = exePathStr.find_last_of("\\/");
	if (lastBackslash != std::string::npos) {
		settingsPath = exePathStr.substr(0, lastBackslash + 1) + "settings.ini";
	}

	Settings settings;
	settings.Load(settingsPath);


	InitD3D11Proxy();

	bool widescreenEnabled = settings.GetBool("widescreen_enabled", true);

	if (widescreenEnabled) {
		std::cout << "Initializing dynamic widescreen patch system..." << std::endl;
		
		// Try to apply initial widescreen patch (3D)
		if (!ApplyWidescreenPatchAuto(base)) {
			std::cout << "Initial aspect ratio is not widescreen or detection failed. Monitoring for changes..." << std::endl;
		} else {
			SetWidescreen2DEnabled(true);
		}

		// Always initialize 2D and dialog patches if widescreen features are enabled
		if (!InitWidescreen2DHook()) {
			std::cout << "Failed to initialize 2D widescreen ASM hook!" << std::endl;
		}
		
		ApplyDialogPatch(base);
		ApplyBattleUIAndMenuPatch(base);
		
		// Always start dynamic monitoring to handle resolution changes
		StartDynamicWidescreenMonitoring(base);
	} else {
		std::cout << "Widescreen patches disabled in settings" << std::endl;
	}

	if (settings.GetBool("double_fps_mode", false)) {
		ApplyDoubleFpsPatch(base);
	}

	if (settings.GetBool("disable_pause_on_focus_loss", true)) {
		ApplyDisablePausePatch(base);
	} else {
		std::cout << "Disable pause on focus loss disabled in settings" << std::endl;
	}

	ApplyVerticalBorderRemoverPatch(base);

	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
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
