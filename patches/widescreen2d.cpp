#include "widescreen2d.h"
#include <iostream>
#include <vector>
#include <mutex>
#include <d3d11.h>

static bool g_widescreen2DEnabled = false;
static float g_widescreenRatio = 0.75f; 
static std::mutex g_hookMutex;

// x86 surgical VTable Indices
const int DEVICE_CREATE_TEXTURE2D = 5;
const int CONTEXT_UPDATE_SUBRESOURCE = 42;

typedef HRESULT (STDMETHODCALLTYPE* CreateTexture2D_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
typedef void (STDMETHODCALLTYPE* UpdateSubresource_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);

static CreateTexture2D_t g_originalCreateTexture2D = nullptr;
static UpdateSubresource_t g_originalUpdateSubresource = nullptr;

bool GetTextureDimensions(ID3D11Resource* pResource, UINT* pWidth, UINT* pHeight, UINT* pBindFlags) {
    if (!pResource) return false;
    D3D11_RESOURCE_DIMENSION dim;
    pResource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    ID3D11Texture2D* pTex = nullptr;
    if (SUCCEEDED(pResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTex))) {
        D3D11_TEXTURE2D_DESC desc;
        pTex->GetDesc(&desc);
        *pWidth = desc.Width;
        *pHeight = desc.Height;
        if (pBindFlags) *pBindFlags = desc.BindFlags;
        pTex->Release();
        return true;
    }
    return false;
}

std::vector<BYTE> TransformImageForWidescreen(const void* pSrcData, UINT width, UINT height, UINT srcRowPitch, float compressionRatio, UINT* pNewRowPitch) {
    if (!pSrcData) return std::vector<BYTE>();
    UINT newWidth = (UINT)(width * compressionRatio);
    if (newWidth % 2 != 0) newWidth++; 
    UINT paddingLeft = (width - newWidth) / 2;
    if (paddingLeft % 2 != 0) paddingLeft++;
    *pNewRowPitch = srcRowPitch;
    std::vector<BYTE> newData(height * srcRowPitch, 0); 
    BYTE* pDst = newData.data();
    const BYTE* pSrc = (const BYTE*)pSrcData;
    for (UINT y = 0; y < height; ++y) {
        const BYTE* pSrcLine = pSrc + (y * srcRowPitch);
        BYTE* pDstLine = pDst + (y * srcRowPitch);
        for (UINT x = 0; x < newWidth; ++x) {
            float srcX = (float)x / compressionRatio;
            UINT iSrcX = (UINT)srcX;
            if (iSrcX >= width) iSrcX = width - 1;
            UINT dstXPos = paddingLeft + x;
            if (dstXPos < width) memcpy(pDstLine + (dstXPos * 4), pSrcLine + (iSrcX * 4), 4);
        }
    }
    return newData;
}

HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(ID3D11Device* This, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) {
    if (g_widescreen2DEnabled && pDesc && pInitialData && pInitialData->pSysMem && pDesc->Width >= 800 && pDesc->Width > pDesc->Height) {
        if (!(pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) && (pInitialData->SysMemPitch / pDesc->Width) == 4) {
             std::cout << "[D3D11] Transforming backdrop: " << pDesc->Width << "x" << pDesc->Height << std::endl;
             UINT newPitch;
             std::vector<BYTE> transformed = TransformImageForWidescreen(pInitialData->pSysMem, pDesc->Width, pDesc->Height, pInitialData->SysMemPitch, g_widescreenRatio, &newPitch);
             D3D11_SUBRESOURCE_DATA newData = *pInitialData;
             newData.pSysMem = transformed.data();
             newData.SysMemPitch = newPitch;
             return g_originalCreateTexture2D(This, pDesc, &newData, ppTexture2D);
        }
    }
    return g_originalCreateTexture2D(This, pDesc, pInitialData, ppTexture2D);
}

void STDMETHODCALLTYPE HookedUpdateSubresource(ID3D11DeviceContext* This, ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) {
    UINT w, h, b;
    if (g_widescreen2DEnabled && pDstResource && pSrcData && !pDstBox && GetTextureDimensions(pDstResource, &w, &h, &b) && w >= 800 && w > h) {
        if (!(b & D3D11_BIND_RENDER_TARGET) && (SrcRowPitch / w) == 4) {
            UINT newPitch;
            std::vector<BYTE> transformed = TransformImageForWidescreen(pSrcData, w, h, SrcRowPitch, g_widescreenRatio, &newPitch);
            g_originalUpdateSubresource(This, pDstResource, DstSubresource, pDstBox, transformed.data(), newPitch, SrcDepthPitch);
            return;
        }
    }
    g_originalUpdateSubresource(This, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

bool HookD3D11Device(ID3D11Device* pDevice) {
    if (!pDevice) return false;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    void** vtable = *(void***)pDevice;
    if (vtable[DEVICE_CREATE_TEXTURE2D] == (void*)HookedCreateTexture2D) return true;
    g_originalCreateTexture2D = (CreateTexture2D_t)vtable[DEVICE_CREATE_TEXTURE2D];
    DWORD old;
    VirtualProtect(&vtable[DEVICE_CREATE_TEXTURE2D], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtable[DEVICE_CREATE_TEXTURE2D] = (void*)HookedCreateTexture2D;
    VirtualProtect(&vtable[DEVICE_CREATE_TEXTURE2D], sizeof(void*), old, &old);
    std::cout << "[D3D11 Hook] ID3D11Device hooked" << std::endl;
    return true;
}

bool HookD3D11Context(ID3D11DeviceContext* pContext) {
    if (!pContext) return false;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    void** vtable = *(void***)pContext;
    if (vtable[CONTEXT_UPDATE_SUBRESOURCE] == (void*)HookedUpdateSubresource) return true;
    g_originalUpdateSubresource = (UpdateSubresource_t)vtable[CONTEXT_UPDATE_SUBRESOURCE];
    DWORD old;
    VirtualProtect(&vtable[CONTEXT_UPDATE_SUBRESOURCE], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtable[CONTEXT_UPDATE_SUBRESOURCE] = (void*)HookedUpdateSubresource;
    VirtualProtect(&vtable[CONTEXT_UPDATE_SUBRESOURCE], sizeof(void*), old, &old);
    std::cout << "[D3D11 Hook] ID3D11DeviceContext hooked" << std::endl;
    return true;
}

void CleanupD3D11Hooks() { }
void SetWidescreen2DRatio(float ratio) { g_widescreenRatio = ratio; }
void SetWidescreen2DEnabled(bool enabled) { g_widescreen2DEnabled = enabled; }
