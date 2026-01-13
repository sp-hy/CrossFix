#include "widescreen2d.h"
#include <iostream>
#include <vector>
#include <mutex>
#include <d3d11.h>

// Global state
static bool g_widescreen2DEnabled = false;
static float g_widescreenRatio = 0.75f; // Default 16:9 (3/4)
static std::mutex g_hookMutex;

typedef void (STDMETHODCALLTYPE* UpdateSubresource_t)(
    ID3D11DeviceContext* This,
    ID3D11Resource* pDstResource,
    UINT DstSubresource,
    const D3D11_BOX* pDstBox,
    const void* pSrcData,
    UINT SrcRowPitch,
    UINT SrcDepthPitch
);

static UpdateSubresource_t g_originalUpdateSubresource = nullptr;
static ID3D11DeviceContext* g_hookedContext = nullptr;
static void** g_originalVTable = nullptr;

// Helper to get texture dimensions
bool GetTextureDimensions(ID3D11Resource* pResource, UINT* pWidth, UINT* pHeight) {
    ID3D11Texture2D* pTexture2D = nullptr;
    HRESULT hr = pResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTexture2D);
    if (SUCCEEDED(hr) && pTexture2D) {
        D3D11_TEXTURE2D_DESC desc;
        pTexture2D->GetDesc(&desc);
        *pWidth = desc.Width;
        *pHeight = desc.Height;
        pTexture2D->Release();
        return true;
    }
    return false;
}

// Transform image data (horizontal compression + black bars)
std::vector<BYTE> TransformImageForWidescreen(
    const void* pSrcData,
    UINT width,
    UINT height,
    UINT srcRowPitch,
    UINT bytesPerPixel,
    float compressionRatio,
    UINT* pNewRowPitch
) {
    UINT newWidth = (UINT)(width * compressionRatio);
    if (newWidth % 2 != 0) newWidth++; // Ensure even width
    
    UINT paddingLeft = (width - newWidth) / 2;
    if (paddingLeft % 2 != 0) paddingLeft++;
    
    *pNewRowPitch = srcRowPitch;
    std::vector<BYTE> newData(height * srcRowPitch, 0); // Initialize with black
    
    BYTE* pDst = newData.data();
    const BYTE* pSrc = (const BYTE*)pSrcData;
    
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < newWidth; ++x) {
            // Bilinear or point sampling
            float srcX = (float)x / compressionRatio;
            UINT iSrcX = (UINT)srcX;
            if (iSrcX >= width) iSrcX = width - 1;
            
            UINT dstIdx = y * srcRowPitch + (paddingLeft + x) * bytesPerPixel;
            UINT srcIdx = y * srcRowPitch + iSrcX * bytesPerPixel;
            
            for (UINT b = 0; b < bytesPerPixel; ++b) {
                pDst[dstIdx + b] = pSrc[srcIdx + b];
            }
        }
    }
    
    return newData;
}

// Hooked UpdateSubresource
void STDMETHODCALLTYPE HookedUpdateSubresource(
    ID3D11DeviceContext* This,
    ID3D11Resource* pDstResource,
    UINT DstSubresource,
    const D3D11_BOX* pDstBox,
    const void* pSrcData,
    UINT SrcRowPitch,
    UINT SrcDepthPitch
) {
    UINT width = 0, height = 0;
    if (g_widescreen2DEnabled && pDstResource && pSrcData && !pDstBox) {
        if (GetTextureDimensions(pDstResource, &width, &height)) {
            if (width > 800) { // Large background texture
                UINT bytesPerPixel = SrcRowPitch / width;
                if (bytesPerPixel > 0 && bytesPerPixel <= 8) {
                    UINT newRowPitch = 0;
                    std::vector<BYTE> transformed = TransformImageForWidescreen(
                        pSrcData, width, height, SrcRowPitch, bytesPerPixel, g_widescreenRatio, &newRowPitch
                    );

                    g_originalUpdateSubresource(This, pDstResource, DstSubresource, pDstBox, transformed.data(), newRowPitch, SrcDepthPitch);
                    return;
                }
            }
        }
    }

    g_originalUpdateSubresource(This, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

bool HookD3D11Context(ID3D11DeviceContext* pContext) {
    if (!pContext) return false;
    
    std::lock_guard<std::mutex> lock(g_hookMutex);
    
    void** vtable = *(void***)pContext;
    const int UPDATE_SUBRESOURCE_INDEX = 48;
    
    if (vtable[UPDATE_SUBRESOURCE_INDEX] == (void*)HookedUpdateSubresource) {
        return true; // Already hooked
    }

    g_originalUpdateSubresource = (UpdateSubresource_t)vtable[UPDATE_SUBRESOURCE_INDEX];
    g_hookedContext = pContext;
    g_originalVTable = vtable;
    
    DWORD oldProtect;
    if (VirtualProtect(&vtable[UPDATE_SUBRESOURCE_INDEX], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        vtable[UPDATE_SUBRESOURCE_INDEX] = (void*)HookedUpdateSubresource;
        VirtualProtect(&vtable[UPDATE_SUBRESOURCE_INDEX], sizeof(void*), oldProtect, &oldProtect);
        std::cout << "[D3D11 Hook] Successfully hooked UpdateSubresource!" << std::endl;
        return true;
    }
    
    return false;
}

void CleanupD3D11Hooks() {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (g_originalVTable && g_originalUpdateSubresource) {
        DWORD oldProtect;
        if (VirtualProtect(&g_originalVTable[48], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            g_originalVTable[48] = (void*)g_originalUpdateSubresource;
            VirtualProtect(&g_originalVTable[48], sizeof(void*), oldProtect, &oldProtect);
        }
    }
}

void SetWidescreen2DRatio(float ratio) { g_widescreenRatio = ratio; }
void SetWidescreen2DEnabled(bool enabled) { g_widescreen2DEnabled = enabled; }
