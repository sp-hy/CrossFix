// Stamina Bar Fix Patch - Widescreen corrections for UI elements

#define NOMINMAX
#include "staminabarfix.h"
#include "widescreen.h"
#include "../utils/viewport_utils.h"
#include <Windows.h>
#include <algorithm>
#include <iostream>

namespace {
    typedef void (STDMETHODCALLTYPE *RSSetViewports_t)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
    
    volatile RSSetViewports_t Original_RSSetViewports_StaminaBarFix = nullptr;
    volatile LONG g_staminaBarFixHookReady = 0;
    
    CRITICAL_SECTION g_staminaBarFixCS;
    volatile LONG g_staminaBarFixCSInitialized = 0;
    
    void InitStaminaBarFixCS() {
        if (InterlockedCompareExchange(&g_staminaBarFixCSInitialized, 1, 0) == 0) {
            InitializeCriticalSection(&g_staminaBarFixCS);
        }
    }
}

void STDMETHODCALLTYPE Hooked_RSSetViewports_StaminaBarFix(ID3D11DeviceContext* This, UINT NumViewports, const D3D11_VIEWPORT* pViewports) {
    MemoryBarrier();
    RSSetViewports_t pOriginal = Original_RSSetViewports_StaminaBarFix;
    
    if (!pOriginal) return;
    if (!pViewports || NumViewports == 0 || !g_staminaBarFixHookReady) {
        pOriginal(This, NumViewports, pViewports);
        return;
    }

    // Allocate a small buffer on stack for modified viewports
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT count = ViewportUtils::CopyViewportsToBuffer(
        vps,
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE,
        pViewports,
        NumViewports
    );

    float ratio = GetCurrentWidescreenRatio();
    ViewportUtils::ApplyStaminaBarWidescreenFix(vps, count, ratio);

    pOriginal(This, count, vps);
}

void ApplyStaminaBarFixPatch(ID3D11Device* pDevice, ID3D11DeviceContext* pContext) {
    if (!pContext || !pDevice) return;

    static volatile LONG applied = 0;
    if (InterlockedCompareExchange(&applied, 1, 0) != 0) return;

    try {
        void** contextVtable = *(void***)pContext;
        
        if (!contextVtable) return;
        if (!contextVtable[44]) return;

        InitStaminaBarFixCS();
        EnterCriticalSection(&g_staminaBarFixCS);
        
        DWORD oldProtect;

        if (VirtualProtect(contextVtable, sizeof(void*) * 50, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            if (contextVtable[44] != (void*)Hooked_RSSetViewports_StaminaBarFix) {
                Original_RSSetViewports_StaminaBarFix = (RSSetViewports_t)contextVtable[44];
                MemoryBarrier();
                contextVtable[44] = (void*)Hooked_RSSetViewports_StaminaBarFix;
                FlushInstructionCache(GetCurrentProcess(), contextVtable, sizeof(void*) * 50);
                MemoryBarrier();
                InterlockedExchange(&g_staminaBarFixHookReady, 1);
            }

            VirtualProtect(contextVtable, sizeof(void*) * 50, oldProtect, &oldProtect);
        }
        
        LeaveCriticalSection(&g_staminaBarFixCS);
        Sleep(1);
    } catch (...) {
        LeaveCriticalSection(&g_staminaBarFixCS);
    }
}
