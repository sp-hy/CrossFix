#include "widescreen2d.h"
#include <iostream>
#include <cmath>

static bool g_widescreen2DEnabled = false;
static float g_widescreenRatio = 0.75f; // 16:9 by default
static LPVOID g_hookAddress = nullptr;
static BYTE g_originalBytes[16] = {0};
static const int g_hookSize = 5; // Size of JMP instruction

// Storage for layer 0's original BG width and X position
static short g_originalBgWidth = 0;
static short g_originalBgX = 0;
static short g_newBgX = 0;
static bool g_bgDataValid = false;  // Flag to ensure we have valid BG data for this scene

// The cave where our custom code will execute
__declspec(naked) void HookCave() {
    __asm {
        // Save all registers we'll use
        pushad
        pushfd
        
        // Original instruction that we replaced: mov [eax+10h],edx
        mov dword ptr [eax+10h], edx
        
        // Check if widescreen is enabled
        mov cl, byte ptr [g_widescreen2DEnabled]
        test cl, cl
        je skip_transform
        
        // Get layer index from [ebp-70h]
        // We need to use explicit hex notation and be careful with addressing
        mov ecx, dword ptr [ebp-70h]
        
        // Check if this is layer 0 (starting new loop)
        test ecx, ecx
        jnz process_overlay
        
        // === LAYER 0 (Background) Processing ===
process_background:
        // Read and save original BG width and X position BEFORE we modify them
        movsx ecx, word ptr [eax+10h]  // ecx = original width
        movsx edx, word ptr [eax+0Ch]  // edx = original X position
        
        // Sanity check: only store if width is reasonable (> 0 and < 10000)
        test ecx, ecx
        jle skip_bg_store              // Skip if <= 0
        cmp ecx, 10000
        jge skip_bg_store              // Skip if >= 10000
        
        mov word ptr [g_originalBgWidth], cx
        mov word ptr [g_originalBgX], dx
        
skip_bg_store:
        // Calculate new width
        cvtsi2ss xmm0, ecx            // xmm0 = original width (float)
        movss xmm1, dword ptr [g_widescreenRatio]  // xmm1 = ratio
        mulss xmm0, xmm1              // xmm0 = new width
        cvttss2si ebx, xmm0           // ebx = new width (int)
        mov word ptr [eax+10h], bx    // Store new width
        
        // Calculate new X position: bgXNew = bgX + (bgWidth - bgWidthNew) / 2
        sub ecx, ebx                  // ecx = original - new
        sar ecx, 1                    // ecx = (original - new) / 2
        
        add edx, ecx                  // edx = original X + padding = new X
        mov word ptr [g_newBgX], dx   // Store new BG X for layers
        mov word ptr [eax+0Ch], dx    // Store new X offset
        
        jmp skip_transform
        
        // === LAYER 1+ (Overlay) Processing ===
process_overlay:
        // Get original width before transformation
        movsx edx, word ptr [eax+10h] // edx = original layer width
        push ebx                      // Save ebx, we'll use it temporarily
        movsx ebx, word ptr [g_originalBgWidth] // ebx = original BG width
        
        // Check if layer width matches BG width (treat as BG)
        cmp edx, ebx
        je process_as_background
        
        // Different width - this is an overlay element
        // Resize the width
        cvtsi2ss xmm0, edx            // xmm0 = original layer width
        movss xmm1, dword ptr [g_widescreenRatio]
        mulss xmm0, xmm1              // xmm0 = new layer width
        cvttss2si edx, xmm0           // edx = new layer width
        mov word ptr [eax+10h], dx    // Store new width
        
        // Calculate new X offset using formula:
        // layerXNew = bgXNew + (layerX - bgX) * scale
        
        // Check if we have valid BG data (width should be non-zero)
        movsx ecx, word ptr [g_originalBgWidth]
        test ecx, ecx
        jz skip_overlay_transform  // If BG width is 0, skip transformation
        
        movsx ebx, word ptr [eax+0Ch]      // ebx = layerX (original)
        movsx ecx, word ptr [g_originalBgX] // ecx = bgX (original)
        sub ebx, ecx                       // ebx = layerX - bgX
        
        // Scale the relative position
        cvtsi2ss xmm0, ebx                 // xmm0 = (layerX - bgX)
        movss xmm1, dword ptr [g_widescreenRatio]
        mulss xmm0, xmm1                   // xmm0 = (layerX - bgX) * scale
        cvttss2si ebx, xmm0                // ebx = scaled offset
        
        // Add to new BG position
        movsx ecx, word ptr [g_newBgX]     // ecx = bgXNew
        add ebx, ecx                       // ebx = bgXNew + scaled offset
        mov word ptr [eax+0Ch], bx         // Store new X offset
        
skip_overlay_transform:
        pop ebx                            // Restore ebx
        jmp skip_transform
        
process_as_background:
        // Layer has same width as BG - treat it exactly like layer 0
        // edx contains the original width, ebx contains original BG width (same value)
        
        // Save original width for padding calculation
        push edx                      // Save original width
        
        // Resize the width
        cvtsi2ss xmm0, edx            // xmm0 = original width
        movss xmm1, dword ptr [g_widescreenRatio]
        mulss xmm0, xmm1              // xmm0 = new width
        cvttss2si edx, xmm0           // edx = new width
        mov word ptr [eax+10h], dx    // Store new width
        
        // Calculate centering offset (same as layer 0)
        // offset_adjustment = (original_width - new_width) / 2
        pop ebx                       // ebx = original width
        sub ebx, edx                  // ebx = original - new
        sar ebx, 1                    // ebx = padding
        
        movsx edx, word ptr [eax+0Ch] // Get current X offset
        add edx, ebx                  // Add padding
        mov word ptr [eax+0Ch], dx    // Store new X offset
        pop ebx                       // Restore ebx
        
skip_transform:
        // Restore registers
        popfd
        popad
        
        // Execute the next instruction that was partially overwritten: add dword ptr [ecx+04],14h
        add dword ptr [ecx+04h], 14h
        
        // Return to caller (the CALL instruction will have pushed the return address)
        ret
    }
}

bool InitWidescreen2DHook() {
    // Get the base address of CHRONOCROSS.exe
    HMODULE hModule = GetModuleHandleA("CHRONOCROSS.exe");
    if (!hModule) {
        std::cerr << "[Widescreen2D] Failed to get CHRONOCROSS.exe module handle" << std::endl;
        return false;
    }
    
    // Calculate the absolute address of the hook point
    // CHRONOCROSS.exe+1CCF93 is where "mov [eax+10],edx" is located
    g_hookAddress = (LPVOID)((uintptr_t)hModule + 0x1CCF93);
    
    std::cout << "[Widescreen2D] Hook address: 0x" << std::hex << (uintptr_t)g_hookAddress << std::dec << std::endl;
    
    // Save original bytes
    memcpy(g_originalBytes, g_hookAddress, g_hookSize);
    
    // Create the CALL instruction to our cave
    // CALL will push the return address automatically
    BYTE callInstruction[5];
    callInstruction[0] = 0xE8; // CALL opcode (relative)
    DWORD relativeAddress = (DWORD)((uintptr_t)HookCave - (uintptr_t)g_hookAddress - 5);
    memcpy(&callInstruction[1], &relativeAddress, 4);
    
    // Write the CALL instruction
    DWORD oldProtect;
    if (!VirtualProtect(g_hookAddress, g_hookSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::cerr << "[Widescreen2D] Failed to change memory protection" << std::endl;
        return false;
    }
    
    memcpy(g_hookAddress, callInstruction, g_hookSize);
    VirtualProtect(g_hookAddress, g_hookSize, oldProtect, &oldProtect);
    
    std::cout << "[Widescreen2D] ASM hook installed successfully" << std::endl;
    return true;
}

void CleanupWidescreen2DHook() {
    if (g_hookAddress) {
        DWORD oldProtect;
        VirtualProtect(g_hookAddress, g_hookSize, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(g_hookAddress, g_originalBytes, g_hookSize);
        VirtualProtect(g_hookAddress, g_hookSize, oldProtect, &oldProtect);
        
        std::cout << "[Widescreen2D] ASM hook removed" << std::endl;
    }
}

void SetWidescreen2DRatio(float ratio) {
    g_widescreenRatio = ratio;
    std::cout << "[Widescreen2D] Ratio set to: " << ratio << std::endl;
}

void SetWidescreen2DEnabled(bool enabled) {
    g_widescreen2DEnabled = enabled;
    std::cout << "[Widescreen2D] " << (enabled ? "Enabled" : "Disabled") << std::endl;
}
