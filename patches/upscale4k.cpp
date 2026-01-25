// 4K Upscale Patch - Based on SpecialK's implementation

#define NOMINMAX
#include "upscale4k.h"
#include "texturedump.h"
#include <Windows.h>
#include <algorithm>
#include <iostream>
#include "../utils/settings.h"


namespace {
    // Upscale configuration
    constexpr float ResMultiplier = 4.0f;
    constexpr float BaseWidth = 4096.0f;
    constexpr float BaseHeight = 2048.0f;
    constexpr float NewWidth = BaseWidth * ResMultiplier;
    constexpr float NewHeight = BaseHeight * ResMultiplier;

    // Function pointer typedefs
    typedef void (STDMETHODCALLTYPE *RSSetViewports_t)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
    typedef void (STDMETHODCALLTYPE *CopySubresourceRegion_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*, BOOL);
    typedef HRESULT (STDMETHODCALLTYPE *CreateTexture2D_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);

    // Original function pointers
    RSSetViewports_t Original_RSSetViewports = nullptr;
    CopySubresourceRegion_t Original_CopySubresourceRegion = nullptr;
    CreateTexture2D_t Original_CreateTexture2D = nullptr;
    
    // Ready flags to prevent race conditions during hook installation
    volatile bool g_viewportsHookReady = false;
    volatile bool g_copyHookReady = false;
    volatile bool g_createTextureHookReady = false;

    // Helper: Check if format is a color render target format
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
    if (!g_viewportsHookReady || !Original_RSSetViewports) {
        return; // Not ready yet
    }
    
    if (NumViewports == 1 && pViewports) {
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

                    if (texDesc.Width == (UINT)NewWidth && texDesc.Height == (UINT)NewHeight) {
                        D3D11_VIEWPORT vp = *pViewports;

                        float left_ndc = 2.0f * (vp.TopLeftX / BaseWidth) - 1.0f;
                        float top_ndc = 2.0f * (vp.TopLeftY / BaseHeight) - 1.0f;

                        vp.TopLeftX = (left_ndc * NewWidth + NewWidth) / 2.0f;
                        vp.TopLeftY = (top_ndc * NewHeight + NewHeight) / 2.0f;
                        vp.Width = ResMultiplier * vp.Width;
                        vp.Height = ResMultiplier * vp.Height;

                        vp.TopLeftX = std::min(vp.TopLeftX, 32767.0f);
                        vp.TopLeftY = std::min(vp.TopLeftY, 32767.0f);
                        vp.Width = std::min(vp.Width, 32767.0f);
                        vp.Height = std::min(vp.Height, 32767.0f);

                        Original_RSSetViewports(This, 1, &vp);

                        pTex->Release();
                        pRes->Release();
                        rtv->Release();
                        return;
                    }
                    pTex->Release();
                }
                pRes->Release();
            }
            rtv->Release();
        }
    }

    Original_RSSetViewports(This, NumViewports, pViewports);
}

void STDMETHODCALLTYPE Hooked_CopySubresourceRegion(ID3D11DeviceContext* This, ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox, BOOL bWrapped) {
    if (!g_copyHookReady || !Original_CopySubresourceRegion) {
        return; // Not ready yet
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

            if (srcDesc.Width == (UINT)NewWidth && srcDesc.Height == (UINT)NewHeight && pSrcBox &&
                dstDesc.Width == (UINT)NewWidth && dstDesc.Height == (UINT)NewHeight) {
                
                newBox = *pSrcBox;

                constexpr float HalfWidth = NewWidth / 2.0f;
                constexpr float HalfHeight = NewHeight / 2.0f;

                float left_ndc = 2.0f * (static_cast<float>(std::clamp((UINT)newBox.left, 0U, 4096U)) / 4096.0f) - 1.0f;
                float top_ndc = 2.0f * (static_cast<float>(std::clamp((UINT)newBox.top, 0U, 2048U)) / 2048.0f) - 1.0f;
                float right_ndc = 2.0f * (static_cast<float>(std::clamp((UINT)newBox.right, 0U, 4096U)) / 4096.0f) - 1.0f;
                float bottom_ndc = 2.0f * (static_cast<float>(std::clamp((UINT)newBox.bottom, 0U, 2048U)) / 2048.0f) - 1.0f;

                newBox.left = static_cast<UINT>(std::max(0.0f, left_ndc * HalfWidth + HalfWidth));
                newBox.top = static_cast<UINT>(std::max(0.0f, top_ndc * HalfHeight + HalfHeight));
                newBox.right = static_cast<UINT>(std::max(0.0f, right_ndc * HalfWidth + HalfWidth));
                newBox.bottom = static_cast<UINT>(std::max(0.0f, bottom_ndc * HalfHeight + HalfHeight));

                ActualDstX = DstX * (UINT)ResMultiplier;
                ActualDstY = DstY * (UINT)ResMultiplier;
                
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

    Original_CopySubresourceRegion(This, pDstResource, DstSubresource, ActualDstX, ActualDstY, DstZ, pSrcResource, SrcSubresource, pActualSrcBox, bWrapped);
}

HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D(ID3D11Device* This, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) {
    if (!g_createTextureHookReady || !Original_CreateTexture2D) {
        return E_FAIL; // Not ready yet
    }
    
    HRESULT hr;
    const D3D11_TEXTURE2D_DESC* actualDesc = pDesc;
    D3D11_TEXTURE2D_DESC newDesc;
    
    if (pDesc && pDesc->Width == (UINT)BaseWidth && pDesc->Height == (UINT)BaseHeight && 
        (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) && IsColorFormat(pDesc->Format) && !pInitialData) {
        
        newDesc = *pDesc;
        newDesc.Width = (UINT)NewWidth;
        newDesc.Height = (UINT)NewHeight;
        actualDesc = &newDesc;
        
        hr = Original_CreateTexture2D(This, &newDesc, pInitialData, ppTexture2D);
    } else {
        hr = Original_CreateTexture2D(This, pDesc, pInitialData, ppTexture2D);
    }
    
    // Dump texture if creation succeeded
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

    static bool applied = false;
    if (applied) return;
    applied = true;

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
    if (!settings.GetBool("upscale_4k", true)) {
        return;
    }


    void** contextVtable = *(void***)pContext;
    void** deviceVtable = *(void***)pDevice;
    DWORD oldProtect;

    // Hook ID3D11DeviceContext methods
    if (VirtualProtect(contextVtable, sizeof(void*) * 50, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (contextVtable[44] != (void*)Hooked_RSSetViewports) {
            Original_RSSetViewports = (RSSetViewports_t)contextVtable[44];
            contextVtable[44] = (void*)Hooked_RSSetViewports;
            FlushInstructionCache(GetCurrentProcess(), &contextVtable[44], sizeof(void*));
            g_viewportsHookReady = true;
        }

        if (contextVtable[46] != (void*)Hooked_CopySubresourceRegion) {
            Original_CopySubresourceRegion = (CopySubresourceRegion_t)contextVtable[46];
            contextVtable[46] = (void*)Hooked_CopySubresourceRegion;
            FlushInstructionCache(GetCurrentProcess(), &contextVtable[46], sizeof(void*));
            g_copyHookReady = true;
        }

        VirtualProtect(contextVtable, sizeof(void*) * 50, oldProtect, &oldProtect);
    }

    // Hook ID3D11Device methods
    if (VirtualProtect(deviceVtable, sizeof(void*) * 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (deviceVtable[5] != (void*)Hooked_CreateTexture2D) {
            Original_CreateTexture2D = (CreateTexture2D_t)deviceVtable[5];
            deviceVtable[5] = (void*)Hooked_CreateTexture2D;
            FlushInstructionCache(GetCurrentProcess(), &deviceVtable[5], sizeof(void*));
            g_createTextureHookReady = true;
        }
        
        VirtualProtect(deviceVtable, sizeof(void*) * 10, oldProtect, &oldProtect);
    }
    
#ifdef _DEBUG
    std::cout << "Upscale 4K Patch Applied (Multiplier: " << ResMultiplier << ")" << std::endl;
#else
    std::cout << "Upscale 4K Patch Applied" << std::endl;
#endif
}
