// Upscale Patch - Based on SpecialK's implementation

#define NOMINMAX
#include "upscale4k.h"
#include "texturedump.h"
#include "widescreen.h"
#include "../utils/viewport_utils.h"
#include <Windows.h>
#include <algorithm>
#include <iostream>
#include "../utils/settings.h"

namespace {
    constexpr float BaseWidth = 4096.0f;
    constexpr float BaseHeight = 2048.0f;
    
    static float g_ResMultiplier = 4.0f;
    static float g_NewWidth = BaseWidth * 4.0f;
    static float g_NewHeight = BaseHeight * 4.0f;

    typedef void (STDMETHODCALLTYPE *RSSetViewports_t)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
    typedef void (STDMETHODCALLTYPE *CopySubresourceRegion_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*, BOOL);
    typedef HRESULT (STDMETHODCALLTYPE *CreateTexture2D_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);

    // Win32 primitives for thread-safe access (avoids C++ runtime initialization issues)
    volatile RSSetViewports_t Original_RSSetViewports = nullptr;
    volatile CopySubresourceRegion_t Original_CopySubresourceRegion = nullptr;
    volatile CreateTexture2D_t Original_CreateTexture2D = nullptr;
    
    volatile LONG g_viewportsHookReady = 0;
    volatile LONG g_copyHookReady = 0;
    volatile LONG g_createTextureHookReady = 0;
    
    CRITICAL_SECTION g_hookInstallCS;
    volatile LONG g_hookCSInitialized = 0;
    
    void InitHookCS() {
        if (InterlockedCompareExchange(&g_hookCSInitialized, 1, 0) == 0) {
            InitializeCriticalSection(&g_hookInstallCS);
        }
    }

    inline bool IsColorFormat(DXGI_FORMAT format) {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
               format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
               format == DXGI_FORMAT_B8G8R8A8_UNORM ||
               format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
               format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
               format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
}

void STDMETHODCALLTYPE Hooked_RSSetViewports(ID3D11DeviceContext* This, UINT NumViewports, const D3D11_VIEWPORT* pViewports) {
    MemoryBarrier();
    RSSetViewports_t pOriginal = Original_RSSetViewports;
    
    if (!pOriginal) return;
    if (!pViewports || NumViewports == 0 || !g_viewportsHookReady) {
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
    
    // Apply stamina bar widescreen fix FIRST (before upscaling)
    // Only apply if we're actually in widescreen mode
    // Use tolerance to account for floating-point precision
    // Ratio should be < 1.0 for widescreen (e.g., 0.75 for 16:9)
    float ratio = GetCurrentWidescreenRatio();
    const float WIDESCREEN_THRESHOLD = 0.99f;
    if (ratio < WIDESCREEN_THRESHOLD) {
        ViewportUtils::ApplyStaminaBarWidescreenFix(vps, count, ratio);
    }
    
    // Check render target once per call
    bool isUpscaledTarget = false;
    ID3D11RenderTargetView* rtv = nullptr;
    This->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
        ID3D11Resource* pRes = nullptr;
        rtv->GetResource(&pRes);
        if (pRes) {
            ID3D11Texture2D* pTex = nullptr;
            pRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTex);
            if (pTex) {
                D3D11_TEXTURE2D_DESC texDesc;
                pTex->GetDesc(&texDesc);
                if (texDesc.Width == (UINT)g_NewWidth && texDesc.Height == (UINT)g_NewHeight) {
                    isUpscaledTarget = true;
                }
                pTex->Release();
            }
            pRes->Release();
        }
        rtv->Release();
    }

    for (UINT i = 0; i < count; ++i) {
        // Apply 4K Upscale logic if rendering to upscaled target
        if (isUpscaledTarget) {
            float left_ndc = 2.0f * (vps[i].TopLeftX / BaseWidth) - 1.0f;
            float top_ndc = 2.0f * (vps[i].TopLeftY / BaseHeight) - 1.0f;

            vps[i].TopLeftX = (left_ndc * g_NewWidth + g_NewWidth) / 2.0f;
            vps[i].TopLeftY = (top_ndc * g_NewHeight + g_NewHeight) / 2.0f;
            vps[i].Width *= g_ResMultiplier;
            vps[i].Height *= g_ResMultiplier;

            vps[i].TopLeftX = std::min(vps[i].TopLeftX, 32767.0f);
            vps[i].TopLeftY = std::min(vps[i].TopLeftY, 32767.0f);
            vps[i].Width = std::min(vps[i].Width, 32767.0f);
            vps[i].Height = std::min(vps[i].Height, 32767.0f);
        }
    }

    pOriginal(This, count, vps);
}

void STDMETHODCALLTYPE Hooked_CopySubresourceRegion(ID3D11DeviceContext* This, ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox, BOOL bWrapped) {
    MemoryBarrier();
    CopySubresourceRegion_t pOriginal = Original_CopySubresourceRegion;
    
    if (!pOriginal) return;
    if (!g_copyHookReady) {
        pOriginal(This, pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox, bWrapped);
        return;
    }
    
    D3D11_BOX newBox = {};
    const D3D11_BOX* pActualSrcBox = pSrcBox;
    UINT ActualDstX = DstX;
    UINT ActualDstY = DstY;

    if (pSrcResource && pDstResource) {
        ID3D11Texture2D *pSrcTex = nullptr;
        ID3D11Texture2D *pDstTex = nullptr;

        pSrcResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pSrcTex);
        pDstResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pDstTex);

        if (pSrcTex && pDstTex) {
            D3D11_TEXTURE2D_DESC srcDesc = {};
            D3D11_TEXTURE2D_DESC dstDesc = {};
            pSrcTex->GetDesc(&srcDesc);
            pDstTex->GetDesc(&dstDesc);

            if (srcDesc.Width == (UINT)g_NewWidth && srcDesc.Height == (UINT)g_NewHeight && pSrcBox &&
                dstDesc.Width == (UINT)g_NewWidth && dstDesc.Height == (UINT)g_NewHeight) {
                
                newBox = *pSrcBox;

                float HalfWidth = g_NewWidth / 2.0f;
                float HalfHeight = g_NewHeight / 2.0f;

                float left_ndc = 2.0f * (static_cast<float>(std::clamp((UINT)newBox.left, 0U, 4096U)) / 4096.0f) - 1.0f;
                float top_ndc = 2.0f * (static_cast<float>(std::clamp((UINT)newBox.top, 0U, 2048U)) / 2048.0f) - 1.0f;
                float right_ndc = 2.0f * (static_cast<float>(std::clamp((UINT)newBox.right, 0U, 4096U)) / 4096.0f) - 1.0f;
                float bottom_ndc = 2.0f * (static_cast<float>(std::clamp((UINT)newBox.bottom, 0U, 2048U)) / 2048.0f) - 1.0f;

                newBox.left = static_cast<UINT>(std::max(0.0f, left_ndc * HalfWidth + HalfWidth));
                newBox.top = static_cast<UINT>(std::max(0.0f, top_ndc * HalfHeight + HalfHeight));
                newBox.right = static_cast<UINT>(std::max(0.0f, right_ndc * HalfWidth + HalfWidth));
                newBox.bottom = static_cast<UINT>(std::max(0.0f, bottom_ndc * HalfHeight + HalfHeight));

                ActualDstX = DstX * (UINT)g_ResMultiplier;
                ActualDstY = DstY * (UINT)g_ResMultiplier;
                
                pActualSrcBox = &newBox;
            } 
            else if (pSrcBox && (pSrcBox->right > srcDesc.Width || pSrcBox->bottom > srcDesc.Height)) {
                newBox = *pSrcBox;
                newBox.right = std::min(srcDesc.Width, newBox.right);
                newBox.bottom = std::min(srcDesc.Height, newBox.bottom);
                pActualSrcBox = &newBox;
            }
        }
        
        if (pSrcTex) pSrcTex->Release();
        if (pDstTex) pDstTex->Release();
    }

    pOriginal(This, pDstResource, DstSubresource, ActualDstX, ActualDstY, DstZ, pSrcResource, SrcSubresource, pActualSrcBox, bWrapped);
}

HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D(ID3D11Device* This, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) {
    MemoryBarrier();
    CreateTexture2D_t pOriginal = Original_CreateTexture2D;
    
    if (!pOriginal) return E_FAIL;
    if (!g_createTextureHookReady) return pOriginal(This, pDesc, pInitialData, ppTexture2D);
    
    HRESULT hr;
    const D3D11_TEXTURE2D_DESC* actualDesc = pDesc;
    D3D11_TEXTURE2D_DESC newDesc;
    
    if (pDesc && pDesc->Width == (UINT)BaseWidth && pDesc->Height == (UINT)BaseHeight && 
        (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) && IsColorFormat(pDesc->Format) && !pInitialData) {
        
        newDesc = *pDesc;
        newDesc.Width = (UINT)g_NewWidth;
        newDesc.Height = (UINT)g_NewHeight;
        actualDesc = &newDesc;
        
        hr = pOriginal(This, &newDesc, pInitialData, ppTexture2D);
    } else {
        hr = pOriginal(This, pDesc, pInitialData, ppTexture2D);
    }
    
    if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D && actualDesc) {
        ID3D11DeviceContext* pContext = nullptr;
        This->GetImmediateContext(&pContext);
        if (pContext) {
            DumpTexture2D(This, pContext, *ppTexture2D, actualDesc, pInitialData);
            pContext->Release();
        }
    }
    
    return hr;
}

void ApplyUpscale4KPatch(ID3D11Device* pDevice, ID3D11DeviceContext* pContext) {
    if (!pContext || !pDevice) return;

    static volatile LONG applied = 0;
    if (InterlockedCompareExchange(&applied, 1, 0) != 0) return;

    try {
        char exePath[MAX_PATH];
        std::string settingsPath = "settings.ini";
        if (GetModuleFileNameA(NULL, exePath, MAX_PATH) != 0) {
            std::string exePathStr(exePath);
            size_t lastBackslash = exePathStr.find_last_of("\\/");
            if (lastBackslash != std::string::npos) {
                settingsPath = exePathStr.substr(0, lastBackslash + 1) + "settings.ini";
            }
        }

        Settings settings;
        settings.Load(settingsPath);
        
        bool upscaleEnabled = settings.GetBool("upscale_enabled", false);
        int scale = settings.GetInt("upscale_scale", 4);
        bool setupCompleted = settings.GetBool("upscale_setup_completed", false);
        
        if (!setupCompleted) {
            std::cout << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << "       CrossFix - First Run Setup       " << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << std::endl;
            std::cout << "Would you like to enable texture upscaling?" << std::endl;
            std::cout << std::endl;
            std::cout << "WARNING: This feature is EXPERIMENTAL and may cause:" << std::endl;
            std::cout << "  - Game crashes" << std::endl;
            std::cout << "  - Visual glitches" << std::endl;
            std::cout << "  - Performance issues" << std::endl;
            std::cout << std::endl;
            std::cout << "You can change this later in settings.ini" << std::endl;
            std::cout << std::endl;
            std::cout << "Enable upscaling? (Y/N): ";
            
            char choice;
            std::cin >> choice;
            upscaleEnabled = (choice == 'Y' || choice == 'y');
            
            if (upscaleEnabled) {
                std::cout << std::endl;
                std::cout << "Select upscale multiplier:" << std::endl;
                std::cout << "  2 - 2x - Fastest, lower quality" << std::endl;
                std::cout << "  3 - 3x - Balanced" << std::endl;
                std::cout << "  4 - 4x - Best quality, most demanding, may crash" << std::endl;
                std::cout << std::endl;
                std::cout << "Enter scale (2/3/4): ";
                
                int scaleChoice;
                std::cin >> scaleChoice;
                
                if (scaleChoice >= 2 && scaleChoice <= 4) {
                    scale = scaleChoice;
                } else {
                    std::cout << "Invalid choice, using default (4x)" << std::endl;
                    scale = 4;
                }
            }
            
            settings.UpdateFile(settingsPath, "upscale_enabled", upscaleEnabled ? "1" : "0");
            settings.UpdateFile(settingsPath, "upscale_scale", std::to_string(scale));
            settings.UpdateFile(settingsPath, "upscale_setup_completed", "1");
            
            std::cout << std::endl;
            std::cout << "Settings saved to settings.ini" << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << std::endl;
        }
        
        if (!upscaleEnabled) return;
        
        if (scale < 2) scale = 2;
        if (scale > 4) scale = 4;
        
        g_ResMultiplier = static_cast<float>(scale);
        g_NewWidth = BaseWidth * g_ResMultiplier;
        g_NewHeight = BaseHeight * g_ResMultiplier;

        void** contextVtable = *(void***)pContext;
        void** deviceVtable = *(void***)pDevice;
        
        if (!contextVtable || !deviceVtable) return;
        if (!contextVtable[44] || !contextVtable[46] || !deviceVtable[5]) return;

        InitHookCS();
        EnterCriticalSection(&g_hookInstallCS);
        
        DWORD oldProtect;

        if (VirtualProtect(contextVtable, sizeof(void*) * 50, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            if (contextVtable[44] != (void*)Hooked_RSSetViewports) {
                Original_RSSetViewports = (RSSetViewports_t)contextVtable[44];
                MemoryBarrier();
                contextVtable[44] = (void*)Hooked_RSSetViewports;
                FlushInstructionCache(GetCurrentProcess(), contextVtable, sizeof(void*) * 50);
                MemoryBarrier();
                InterlockedExchange(&g_viewportsHookReady, 1);
            }

            if (contextVtable[46] != (void*)Hooked_CopySubresourceRegion) {
                Original_CopySubresourceRegion = (CopySubresourceRegion_t)contextVtable[46];
                MemoryBarrier();
                contextVtable[46] = (void*)Hooked_CopySubresourceRegion;
                FlushInstructionCache(GetCurrentProcess(), contextVtable, sizeof(void*) * 50);
                MemoryBarrier();
                InterlockedExchange(&g_copyHookReady, 1);
            }

            VirtualProtect(contextVtable, sizeof(void*) * 50, oldProtect, &oldProtect);
        }

        if (VirtualProtect(deviceVtable, sizeof(void*) * 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            if (deviceVtable[5] != (void*)Hooked_CreateTexture2D) {
                Original_CreateTexture2D = (CreateTexture2D_t)deviceVtable[5];
                MemoryBarrier();
                deviceVtable[5] = (void*)Hooked_CreateTexture2D;
                FlushInstructionCache(GetCurrentProcess(), deviceVtable, sizeof(void*) * 10);
                MemoryBarrier();
                InterlockedExchange(&g_createTextureHookReady, 1);
            }
            VirtualProtect(deviceVtable, sizeof(void*) * 10, oldProtect, &oldProtect);
        }
        
        LeaveCriticalSection(&g_hookInstallCS);
        Sleep(1);
    } catch (...) {
        LeaveCriticalSection(&g_hookInstallCS);
    }
}
