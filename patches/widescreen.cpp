#include "widescreen.h"
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
static float g_widescreenRatio3D = 0.75f; // Default to 16:9

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
		mov eax, [esi+0x60]  // original: read OFX
		cdq                  // original: sign extend
		add edi, eax         // original: add OFX
		adc ecx, edx         // original: add with carry
		ret
	}
}

// Calculate widescreen ratio from aspect ratio
float CalculateWidescreenRatio(float aspectRatio) {
	// Formula: (4/3) / aspectRatio
	// This gives us the compression ratio needed to fit 4:3 content into widescreen
	const float original43 = 4.0f / 3.0f;
	return original43 / aspectRatio;
}

// Apply the widescreen patches with dynamic aspect ratio
bool ApplyWidescreenPatch(uintptr_t base, float aspectRatio) {
	// Calculate the widescreen ratio from aspect ratio
	g_widescreenRatio3D = CalculateWidescreenRatio(aspectRatio);
	
	// Also set the 2D ratio
	SetWidescreen2DRatio(g_widescreenRatio3D);
	
	// Calculate the addresses to patch
	uintptr_t addr1 = base + 0x18EE7B;
	uintptr_t addr2 = base + 0x18AD25;
	
	// Save original bytes if not already saved
	if (!g_originalBytesSaved) {
		memcpy(g_originalBytes1, (void*)addr1, 8);
		memcpy(g_originalBytes2, (void*)addr2, 8);
		g_originalBytesSaved = true;
	}
	
	// Use the universal hook function
	void* hookFunction = (void*)WidescreenHookUniversal;
	
	// Calculate relative call address for addr1
	int32_t relativeAddr1 = (int32_t)((uintptr_t)hookFunction - (addr1 + 5));
	// Calculate relative call address for addr2
	int32_t relativeAddr2 = (int32_t)((uintptr_t)hookFunction - (addr2 + 5));
	
	// Prepare the patch bytes for addr1: call + 3 nops
	unsigned char patch1[8] = {
		0xE8, // call
		(unsigned char)(relativeAddr1 & 0xFF),
		(unsigned char)((relativeAddr1 >> 8) & 0xFF),
		(unsigned char)((relativeAddr1 >> 16) & 0xFF),
		(unsigned char)((relativeAddr1 >> 24) & 0xFF),
		0x90, 0x90, 0x90 // nop nop nop
	};
	
	// Prepare the patch bytes for addr2: call + 3 nops
	unsigned char patch2[8] = {
		0xE8, // call
		(unsigned char)(relativeAddr2 & 0xFF),
		(unsigned char)((relativeAddr2 >> 8) & 0xFF),
		(unsigned char)((relativeAddr2 >> 16) & 0xFF),
		(unsigned char)((relativeAddr2 >> 24) & 0xFF),
		0x90, 0x90, 0x90 // nop nop nop
	};
	
	// Apply the patches
	if (!WriteMemory(addr1, patch1, 8)) {
		std::cout << "Failed to patch address 1 (0x" << std::hex << addr1 << ")" << std::dec << std::endl;
		return false;
	}
	
	if (!WriteMemory(addr2, patch2, 8)) {
		std::cout << "Failed to patch address 2 (0x" << std::hex << addr2 << ")" << std::dec << std::endl;
		return false;
	}
	
	std::cout << "Widescreen patch applied for aspect ratio " << aspectRatio << " (ratio: " << g_widescreenRatio3D << ")" << std::endl;
	g_wasWidescreen = true;
	return true;
}

// Auto-detect and optionally return the detected aspect ratio
bool ApplyWidescreenPatchAuto(uintptr_t base, float* outAspectRatio) {
	// Memory addresses for screen resolution
	// CHRONOCROSS.exe+E2F488 - screen res X
	// CHRONOCROSS.exe+E2F48C - screen res Y
	uintptr_t resXAddr = base + 0xE2F488;
	uintptr_t resYAddr = base + 0xE2F48C;
	
	// Read the resolution values from memory with retry logic
	int resX = 0;
	int resY = 0;
	const int maxRetries = 10;
	const int retryDelayMs = 100;
	
	for (int attempt = 0; attempt < maxRetries; attempt++) {
		try {
			resX = *(int*)resXAddr;
			resY = *(int*)resYAddr;
			
			if (resX > 0 && resY > 0) break;
			
			if (attempt < maxRetries - 1) Sleep(retryDelayMs);
		} catch (...) {
			std::cout << "Failed to read screen resolution from memory!" << std::endl;
			return false;
		}
	}
	
	if (resX <= 0 || resY <= 0) {
		std::cout << "Invalid screen resolution detected: " << resX << "x" << resY << std::endl;
		return false;
	}
	
	float aspectRatio = (float)resX / (float)resY;
	
#ifdef _DEBUG
	std::cout << "Detected screen resolution: " << resX << "x" << resY << " (aspect ratio: " << aspectRatio << ")" << std::endl;
#endif

	if (outAspectRatio != nullptr) {
		*outAspectRatio = aspectRatio;
	}
	
	// If it's not widescreen (e.g. 4:3), don't apply
	if (aspectRatio < 1.4f) {
		std::cout << "Aspect ratio is not widescreen. Widescreen patch not applied." << std::endl;
		return false;
	}
	
	return ApplyWidescreenPatch(base, aspectRatio);
}

// Restore default (non-widescreen) behavior
bool RestoreDefaultBehavior(uintptr_t base) {
	if (!g_originalBytesSaved) {
		std::cout << "Cannot restore default behavior: original bytes not saved" << std::endl;
		return false;
	}
	
	uintptr_t addr1 = base + 0x18EE7B;
	uintptr_t addr2 = base + 0x18AD25;
	
	// Restore original bytes
	if (!WriteMemory(addr1, g_originalBytes1, 8)) {
		std::cout << "Failed to restore address 1 (0x" << std::hex << addr1 << ")" << std::dec << std::endl;
		return false;
	}
	
	if (!WriteMemory(addr2, g_originalBytes2, 8)) {
		std::cout << "Failed to restore address 2 (0x" << std::hex << addr2 << ")" << std::dec << std::endl;
		return false;
	}
	
	std::cout << "Restored default (4:3) behavior" << std::endl;
	
	// Also disable 2D widescreen
	SetWidescreen2DEnabled(false);
	g_wasWidescreen = false;
	
	return true;
}

// Thread function for dynamic resolution monitoring
DWORD WINAPI ResolutionMonitorThread(LPVOID param) {
	uintptr_t base = (uintptr_t)param;
	uintptr_t resXAddr = base + 0xE2F488;
	uintptr_t resYAddr = base + 0xE2F48C;
	
	// Initialize with current resolution to avoid applying patch on first iteration
	int lastResX = 0;
	int lastResY = 0;
	float lastAspectRatio = 0.0f;
	
	// Read initial resolution
	try {
		lastResX = *(int*)resXAddr;
		lastResY = *(int*)resYAddr;
		if (lastResX > 0 && lastResY > 0) {
			lastAspectRatio = (float)lastResX / (float)lastResY;
			g_lastAspectRatio = lastAspectRatio;
		}
	} catch (...) {
		// If we can't read initial resolution, start with 0
	}
	
	while (g_monitoringActive) {
		try {
			int resX = *(int*)resXAddr;
			int resY = *(int*)resYAddr;
			
			// Check if resolution has changed
			if (resX > 0 && resY > 0 && (resX != lastResX || resY != lastResY)) {
				lastResX = resX;
				lastResY = resY;
				
				// Calculate new aspect ratio
				float aspectRatio = (float)resX / (float)resY;
				bool isWidescreen = aspectRatio >= 1.4f;
				
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
					// We just need to update the ratio, ApplyWidescreenPatch handles everything
					if (ApplyWidescreenPatch(base, aspectRatio)) {
						if (!g_wasWidescreen) {
							SetWidescreen2DEnabled(true);
							g_wasWidescreen = true;
						}
					}
				}
			}
		} catch (...) {
			// Ignore errors and continue monitoring
		}
		
		// Check every second
		Sleep(1000);
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
	
	g_monitorThread = CreateThread(nullptr, 0, ResolutionMonitorThread, (LPVOID)base, 0, nullptr);
	
	if (!g_monitorThread) {
		std::cout << "Failed to start dynamic widescreen monitoring thread!" << std::endl;
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
