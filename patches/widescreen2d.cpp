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

// The cave where our custom code will execute
__declspec(naked) void HookCave() {
    __asm {
        // Execute the original instruction first
        mov eax, [ebp-70h]
        
        // Save only the registers we'll modify (not eax - we need it!)
        push ebx
        push ecx
        push edx
        pushfd
        
        // Check if widescreen is enabled
        mov cl, byte ptr [g_widescreen2DEnabled]
        test cl, cl
        jz skip_transform
        
        // Check if width is large enough (skip invalid entries)
        // Only process if width > 0
        movsx ecx, word ptr [ebp-38h]  // Read as signed word
        test ecx, ecx
        jle skip_transform
        
        // Check layer index (now in eax)
        test eax, eax
        jnz process_overlay
        
        // === LAYER 0 (Background) Processing ===
process_background:
        // Read original width and X from ebp-relative locations
        movsx ecx, word ptr [ebp-38h]  // ecx = original width (sign-extended)
        movsx edx, word ptr [ebp-3Ch]  // edx = original X (sign-extended)
        
        // Sanity check width
        test ecx, ecx
        jle skip_transform
        cmp ecx, 10000
        jge skip_transform
        
        // Store for later use by overlays
        mov word ptr [g_originalBgWidth], cx
        mov word ptr [g_originalBgX], dx
        
        // Calculate new width
        cvtsi2ss xmm0, ecx
        movss xmm1, dword ptr [g_widescreenRatio]
        mulss xmm0, xmm1
        cvttss2si ebx, xmm0           // ebx = new width
        
        // Calculate centering offset
        sub ecx, ebx                  // ecx = original - new
        sar ecx, 1                    // ecx = padding
        
        add edx, ecx                  // edx = new X
        mov word ptr [g_newBgX], dx
        
        // Validate values before writing
        test ebx, ebx
        jle skip_transform            // Skip if new width <= 0
        cmp ebx, 10000
        jge skip_transform            // Skip if new width too large
        
        // Write back to memory (use words, not dwords)
        mov word ptr [ebp-38h], bx    // Store new width (16-bit)
        mov word ptr [ebp-3Ch], dx    // Store new X (16-bit)
        
        jmp skip_transform
        
        // === LAYER 1+ (Overlay) Processing ===
process_overlay:
        // Read values from ebp
        movsx edx, word ptr [ebp-38h]  // edx = original layer width
        movsx ebx, word ptr [g_originalBgWidth]
        
        // Check if same width as BG
        cmp edx, ebx
        je process_as_background
        
        // Resize width
        cvtsi2ss xmm0, edx
        movss xmm1, dword ptr [g_widescreenRatio]
        mulss xmm0, xmm1
        cvttss2si edx, xmm0           // edx = new width
        mov word ptr [ebp-38h], dx    // Store new width (16-bit)
        
        // Calculate new X using formula: x_new = bgXNew + (x_old - bgX) * ratio
        movsx ecx, word ptr [ebp-3Ch]  // ecx = x_old
        movsx ebx, word ptr [g_originalBgX]
        sub ecx, ebx                  // ecx = x_old - bgX
        
        cvtsi2ss xmm0, ecx
        movss xmm1, dword ptr [g_widescreenRatio]
        mulss xmm0, xmm1
        cvttss2si ecx, xmm0           // ecx = (x_old - bgX) * ratio
        
        movsx ebx, word ptr [g_newBgX]
        add ecx, ebx                  // ecx = bgXNew + scaled offset
        mov word ptr [ebp-3Ch], cx    // Store new X (16-bit)
        
        jmp skip_transform
        
process_as_background:
        // Same width as BG - treat like background
        mov ecx, edx                  // ecx = original width
        
        cvtsi2ss xmm0, ecx
        movss xmm1, dword ptr [g_widescreenRatio]
        mulss xmm0, xmm1
        cvttss2si edx, xmm0           // edx = new width
        mov word ptr [ebp-38h], dx    // Store new width (16-bit)
        
        sub ecx, edx                  // ecx = padding
        sar ecx, 1
        
        movsx edx, word ptr [ebp-3Ch]
        add edx, ecx
        mov word ptr [ebp-3Ch], dx    // Store new X (16-bit)
        
skip_transform:
        // Restore registers in reverse order (eax is already set correctly)
        popfd
        pop edx
        pop ecx
        pop ebx
        
        // Execute the second original instruction that we NOPed: mov edx,[ebp-38]
        // This loads the (now modified) width into edx
        mov edx, [ebp-38h]
        
        // eax contains layer index, edx contains width
        // Return
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
    // CHRONOCROSS.exe+1CCF51: mov eax,[ebp-70] (3 bytes)
    // +1CCF54: mov edx,[ebp-38] (3 bytes)
    // Total: 6 bytes - enough for our 5-byte CALL + 1 NOP
    g_hookAddress = (LPVOID)((uintptr_t)hModule + 0x1CCF51);
    
#ifdef _DEBUG
    std::cout << "[Widescreen2D] Hook address: 0x" << std::hex << (uintptr_t)g_hookAddress << std::dec << std::endl;
#endif
    
    // Save original bytes (6 bytes to be safe)
    const int actualHookSize = 6;
    memcpy(g_originalBytes, g_hookAddress, actualHookSize);
    
    // Create the CALL instruction to our cave + NOP padding
    BYTE callInstruction[6];
    callInstruction[0] = 0xE8; // CALL opcode (relative)
    DWORD relativeAddress = (DWORD)((uintptr_t)HookCave - (uintptr_t)g_hookAddress - 5);
    memcpy(&callInstruction[1], &relativeAddress, 4);
    callInstruction[5] = 0x90; // NOP to pad to 6 bytes
    
    // Write the CALL instruction
    DWORD oldProtect;
    if (!VirtualProtect(g_hookAddress, actualHookSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::cerr << "[Widescreen2D] Failed to change memory protection" << std::endl;
        return false;
    }
    
    memcpy(g_hookAddress, callInstruction, actualHookSize);
    VirtualProtect(g_hookAddress, actualHookSize, oldProtect, &oldProtect);
    
#ifdef _DEBUG
    std::cout << "[Widescreen2D] ASM hook installed successfully" << std::endl;
#endif
    
    return true;
}

void SetWidescreen2DRatio(float ratio) {
    g_widescreenRatio = ratio;
#ifdef _DEBUG
    std::cout << "[Widescreen2D] Ratio set to: " << ratio << std::endl;
#endif
}

void SetWidescreen2DEnabled(bool enabled) {
    g_widescreen2DEnabled = enabled;
#ifdef _DEBUG
    std::cout << "[Widescreen2D] " << (enabled ? "Enabled" : "Disabled") << std::endl;
#endif
}
