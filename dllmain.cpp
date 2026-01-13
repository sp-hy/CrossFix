// dllmain.cpp : Defines the entry point for the DLL application.


// If using a proxy build config, include the necessary code to proxy calls to winmm.dll
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
#include "utils/settings.h"


// Get handle from the injected process
HANDLE crossfix_handle = GetCurrentProcess();

// Spawn thread to do work
DWORD WINAPI MainThread(LPVOID param) {
    // Get base address from the injected process
	uintptr_t base = (uintptr_t)GetModuleHandle(NULL);

	// Verify we're running in CHRONOCROSS.exe
	char exePath[MAX_PATH];
	if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) {
		// Could not get executable name, exit silently
		FreeLibraryAndExitThread((HMODULE)param, 0);
		return 0;
	}

	// Extract just the executable name from the full path
	char* exeName = strrchr(exePath, '\\');
	if (exeName == NULL) {
		exeName = exePath; // No path separator found, use the whole string
	} else {
		exeName++; // Skip the backslash
	}

	// Convert to lowercase for case-insensitive comparison
	std::string exeNameLower(exeName);
	std::transform(exeNameLower.begin(), exeNameLower.end(), exeNameLower.begin(), ::tolower);

	// Check if we're running in CHRONOCROSS.exe
	if (exeNameLower != "chronocross.exe") {
		// Not the target executable, exit silently without showing console
		FreeLibraryAndExitThread((HMODULE)param, 0);
		return 0;
	}

	// We're in CHRONOCROSS.exe - allocate a console and show status
	AllocConsole();
	FILE* f;
	freopen_s(&f, "CONOUT$", "w", stdout);

	// Log a message
	std::cout << "CrossFix - v0.3 (Minimal)" << std::endl;
	std::cout << std::endl;
	std::cout << "DLL loaded successfully! Base address of the injected executable is: 0x" << std::hex << base << std::dec << std::endl;
	std::cout << std::endl;

	// Load settings
	Settings settings;
	settings.Load("settings.ini");

	// Initialize D3D11 proxy
	InitD3D11Proxy();

	// Check if widescreen is enabled
	bool widescreenEnabled = settings.GetBool("widescreen_enabled", true);
	int widescreenModeInt = settings.GetInt("widescreen_mode", 0);
	bool widescreen2DEnabled = settings.GetBool("widescreen_2d_enabled", true);

	if (widescreenEnabled) {
		float ratio2D = 0.75f; 
		WidescreenMode mode = WIDESCREEN_16_9;
		
		if (widescreenModeInt >= 0 && widescreenModeInt <= 2) {
			mode = static_cast<WidescreenMode>(widescreenModeInt);
			switch (mode) {
				case 0: mode = WIDESCREEN_16_9; ratio2D = 0.75f; break;
				case 1: mode = WIDESCREEN_21_9; ratio2D = 0.571428f; break;
				case 2: mode = WIDESCREEN_32_9; ratio2D = 0.375f; break;
			}
		} else {
			std::cout << "Warning: Invalid widescreen_mode value (" << widescreenModeInt << "), defaulting to 16:9" << std::endl;
		}

		// Apply widescreen patch with selected mode
		if (!ApplyWidescreenPatch(base, mode)) {
			std::cout << "Failed to apply widescreen patch!" << std::endl;
		}
		
		// Configure 2D widescreen transformation (D3D11 proxy handles the hooking)
		SetWidescreen2DRatio(ratio2D);
		SetWidescreen2DEnabled(widescreen2DEnabled);
		if (widescreen2DEnabled) {
			std::cout << "2D widescreen transformation configured (waiting for device creation)" << std::endl;
		}
	} else {
		std::cout << "Widescreen patch disabled in settings" << std::endl;
	}

	// Apply other patches
	if (settings.GetBool("double_fps_mode", false)) {
		if (ApplyDoubleFpsPatch(base)) std::cout << "Double FPS patch applied successfully!" << std::endl;
	}

	if (settings.GetBool("disable_pause_on_focus_loss", true)) {
		if (ApplyDisablePausePatch(base)) std::cout << "Disable pause on focus loss patch applied successfully!" << std::endl;
	} else {
		std::cout << "Disable pause on focus loss disabled in settings" << std::endl;
	}

	// Run thread loop until END key is pressed
	while (!GetAsyncKeyState(VK_END)) {
		Sleep(1000);
	}

	std::cout << "Exiting..." << std::endl;
	
	// Cleanup D3D11 hooks
	CleanupD3D11Hooks();
	
	Sleep(1000);

	HWND consoleWindow = GetConsoleWindow();
	FreeConsole();
	if (consoleWindow != NULL) {
		PostMessage(consoleWindow, WM_CLOSE, 0, 0);
	}

	FreeLibraryAndExitThread((HMODULE)param, 0);
	return 0;
}


// Main DLL entry point
BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		// Create thread
		CreateThread(0, 0, MainThread, hModule, 0, 0);
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

