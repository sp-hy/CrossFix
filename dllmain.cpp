// dllmain.cpp : Defines the entry point for the DLL application.


// If using a proxy build config, include the necessary code to proxy calls to version.dll
#ifdef PROXY
    #include "version/version.h"
#else
    #include <Windows.h>
#endif

#include <iostream>


// Get handle from the injected process
HANDLE aoe_handle = GetCurrentProcess();


// Spawn thread to do work
DWORD WINAPI MainThread(LPVOID param) {
    // Get base address from the injected process
	uintptr_t base = (uintptr_t)GetModuleHandle(NULL);

	// Allocate a console
	AllocConsole();
	FILE* f;
	freopen_s(&f, "CONOUT$", "w", stdout);

	// Log a message
	std::cout << "DLL loaded successfully! Base address of the injected executable is: 0x" << std::hex << base << std::dec << std::endl;

	// Run thread loop until END key is pressed
	while (!GetAsyncKeyState(VK_END)) {
		// Main thread loop

		Sleep(1000);
	}

	std::cout << "Exiting..." << std::endl;
	Sleep(1000);

	HWND consoleWindow = GetConsoleWindow();
	FreeConsole();
	if (consoleWindow != NULL) {
		PostMessage(consoleWindow, WM_CLOSE, 0, 0);
	}

	FreeConsole();
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
		// Handle proxy calls to version.dll, if using a proxy build config
		#if PROXY
			setupWrappers();
		#endif

		// Create thread
		CreateThread(0, 0, MainThread, hModule, 0, 0);
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		// Close the console when the DLL is detached
		if (ul_reason_for_call == DLL_PROCESS_DETACH) {
			FreeConsole();
		}
		break;
	}
	return TRUE;
}

