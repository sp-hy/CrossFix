#include "widescreen2d.h"
#include <iostream>
#include <vector>
#include <mutex>
#include <d3d11.h>

static bool g_widescreen2DEnabled = false;
static float g_widescreenRatio = 0.75f; 
static std::mutex g_hookMutex;

// Custom GUID for tagging background textures
static const GUID GUID_CrossFix_Tag = { 0x7a5c2b1e, 0x9f0e, 0x4c3d, { 0x8a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x6a, 0x7c } };

struct TexMetadata {
    UINT width;
    UINT height;
    void* lastMappedData;
    UINT lastMappedPitch;
};

const int DEVICE_CREATE_TEXTURE2D = 5;
const int CONTEXT_MAP = 14;
const int CONTEXT_UNMAP = 15;
const int CONTEXT_UPDATE_SUBRESOURCE = 42;

typedef HRESULT (STDMETHODCALLTYPE* CreateTexture2D_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
typedef void (STDMETHODCALLTYPE* UpdateSubresource_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* Map_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void (STDMETHODCALLTYPE* Unmap_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT);

static CreateTexture2D_t g_originalCreateTexture2D = nullptr;
static UpdateSubresource_t g_originalUpdateSubresource = nullptr;
static Map_t g_originalMap = nullptr;
static Unmap_t g_originalUnmap = nullptr;

static thread_local std::vector<BYTE> tl_rowBuffer;
static thread_local std::vector<BYTE> tl_fullBuffer;

void TransformInPlace(void* pData, UINT width, UINT height, UINT rowPitch, float ratio) {
    if (!pData || rowPitch < width * 4) return;
    if (tl_rowBuffer.size() < rowPitch) tl_rowBuffer.resize(rowPitch);
    UINT newWidth = (UINT)(width * ratio);
    if (newWidth % 2 != 0) newWidth++; 
    UINT pad = (width - newWidth) / 2;
    if (pad % 2 != 0) pad++;
    BYTE* pPixels = (BYTE*)pData;
    for (UINT y = 0; y < height; ++y) {
        BYTE* pLine = pPixels + (y * rowPitch);
        memcpy(tl_rowBuffer.data(), pLine, rowPitch);
        memset(pLine, 0, rowPitch);
        for (UINT x = 0; x < newWidth; ++x) {
            UINT iSX = (UINT)((float)x / ratio);
            if (iSX < width && (pad + x) < width) memcpy(pLine + (pad + x) * 4, tl_rowBuffer.data() + iSX * 4, 4);
        }
    }
}

HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(ID3D11Device* This, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) {
    const D3D11_SUBRESOURCE_DATA* pDataToUse = pInitialData;
    std::vector<BYTE> transformed;

    if (g_widescreen2DEnabled && pDesc && pInitialData && pInitialData->pSysMem && pDesc->Width >= 800) {
        // Any aspect ratio, but only Shader Resources (No Render Targets/Depth)
        if (!(pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) && (pInitialData->SysMemPitch / pDesc->Width) == 4) {
             size_t total = (size_t)pDesc->Height * pInitialData->SysMemPitch;
             transformed.assign(total, 0);
             memcpy(transformed.data(), pInitialData->pSysMem, total);
             TransformInPlace(transformed.data(), pDesc->Width, pDesc->Height, pInitialData->SysMemPitch, g_widescreenRatio);
             
             static D3D11_SUBRESOURCE_DATA newData;
             newData = *pInitialData;
             newData.pSysMem = transformed.data();
             pDataToUse = &newData;
        }
    }

    HRESULT hr = g_originalCreateTexture2D(This, pDesc, pDataToUse, ppTexture2D);
    if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D && pDesc && pDesc->Width >= 800) {
        TexMetadata meta = { pDesc->Width, pDesc->Height, nullptr, 0 };
        (*ppTexture2D)->SetPrivateData(GUID_CrossFix_Tag, sizeof(TexMetadata), &meta);
    }
    return hr;
}

void STDMETHODCALLTYPE HookedUpdateSubresource(ID3D11DeviceContext* This, ID3D11Resource* pDst, UINT Sub, const D3D11_BOX* pBox, const void* pSrc, UINT Pitch, UINT Depth) {
    if (g_widescreen2DEnabled && pSrc && !pBox) {
        D3D11_RESOURCE_DIMENSION dim;
        pDst->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            ID3D11Texture2D* pTex = (ID3D11Texture2D*)pDst;
            D3D11_TEXTURE2D_DESC desc;
            pTex->GetDesc(&desc);
            // Process anything over 800w, regardless of aspect
            if (desc.Width >= 800 && !(desc.BindFlags & D3D11_BIND_RENDER_TARGET) && (Pitch / desc.Width) == 4) {
                if (tl_fullBuffer.size() < (size_t)desc.Height * Pitch) tl_fullBuffer.resize((size_t)desc.Height * Pitch);
                memcpy(tl_fullBuffer.data(), pSrc, (size_t)desc.Height * Pitch);
                TransformInPlace(tl_fullBuffer.data(), desc.Width, desc.Height, Pitch, g_widescreenRatio);
                g_originalUpdateSubresource(This, pDst, Sub, pBox, tl_fullBuffer.data(), Pitch, Depth);
                return;
            }
        }
    }
    g_originalUpdateSubresource(This, pDst, Sub, pBox, pSrc, Pitch, Depth);
}

HRESULT STDMETHODCALLTYPE HookedMap(ID3D11DeviceContext* This, ID3D11Resource* pResource, UINT Sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* pMapped) {
    HRESULT hr = g_originalMap(This, pResource, Sub, type, flags, pMapped);
    if (SUCCEEDED(hr) && pMapped && pMapped->pData && type == D3D11_MAP_WRITE_DISCARD) {
        UINT size = sizeof(TexMetadata);
        TexMetadata meta;
        if (SUCCEEDED(pResource->GetPrivateData(GUID_CrossFix_Tag, &size, &meta))) {
            meta.lastMappedData = pMapped->pData;
            meta.lastMappedPitch = pMapped->RowPitch;
            pResource->SetPrivateData(GUID_CrossFix_Tag, sizeof(TexMetadata), &meta);
        }
    }
    return hr;
}

void STDMETHODCALLTYPE HookedUnmap(ID3D11DeviceContext* This, ID3D11Resource* pResource, UINT Sub) {
    UINT size = sizeof(TexMetadata);
    TexMetadata meta;
    if (g_widescreen2DEnabled && SUCCEEDED(pResource->GetPrivateData(GUID_CrossFix_Tag, &size, &meta)) && meta.lastMappedData) {
        TransformInPlace(meta.lastMappedData, meta.width, meta.height, meta.lastMappedPitch, g_widescreenRatio);
        meta.lastMappedData = nullptr;
        pResource->SetPrivateData(GUID_CrossFix_Tag, sizeof(TexMetadata), &meta);
    }
    g_originalUnmap(This, pResource, Sub);
}

bool HookD3D11Device(ID3D11Device* pDevice) {
    if (!pDevice) return false;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    void** vtable = *(void***)pDevice;
    if (vtable[DEVICE_CREATE_TEXTURE2D] == (void*)HookedCreateTexture2D) return true;
    g_originalCreateTexture2D = (CreateTexture2D_t)vtable[DEVICE_CREATE_TEXTURE2D];
    DWORD old;
    VirtualProtect(&vtable[DEVICE_CREATE_TEXTURE2D], 4, PAGE_EXECUTE_READWRITE, &old);
    vtable[DEVICE_CREATE_TEXTURE2D] = (void*)HookedCreateTexture2D;
    VirtualProtect(&vtable[DEVICE_CREATE_TEXTURE2D], 4, old, &old);
    std::cout << "[D3D11 Hook] Device Hooked" << std::endl;
    return true;
}

bool HookD3D11Context(ID3D11DeviceContext* pContext) {
    if (!pContext) return false;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    void** vtable = *(void***)pContext;
    if (vtable[CONTEXT_MAP] == (void*)HookedMap) return true;
    g_originalMap = (Map_t)vtable[CONTEXT_MAP];
    g_originalUnmap = (Unmap_t)vtable[CONTEXT_UNMAP];
    g_originalUpdateSubresource = (UpdateSubresource_t)vtable[CONTEXT_UPDATE_SUBRESOURCE];
    DWORD old;
    VirtualProtect(&vtable[CONTEXT_MAP], 8, PAGE_EXECUTE_READWRITE, &old);
    vtable[CONTEXT_MAP] = (void*)HookedMap;
    vtable[CONTEXT_UNMAP] = (void*)HookedUnmap;
    VirtualProtect(&vtable[CONTEXT_MAP], 8, old, &old);
    VirtualProtect(&vtable[CONTEXT_UPDATE_SUBRESOURCE], 4, PAGE_EXECUTE_READWRITE, &old);
    vtable[CONTEXT_UPDATE_SUBRESOURCE] = (void*)HookedUpdateSubresource;
    VirtualProtect(&vtable[CONTEXT_UPDATE_SUBRESOURCE], 4, old, &old);
    std::cout << "[D3D11 Hook] Context Hooked" << std::endl;
    return true;
}

void CleanupD3D11Hooks() { }
void SetWidescreen2DRatio(float ratio) { g_widescreenRatio = ratio; }
void SetWidescreen2DEnabled(bool enabled) { g_widescreen2DEnabled = enabled; }
