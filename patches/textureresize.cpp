#include "textureresize.h"
#include "texturedump.h"
#include "widescreen.h"
#include <Windows.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <memory>
#include <mutex>

namespace {
    typedef HRESULT (STDMETHODCALLTYPE *CreateTexture2D_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
    CreateTexture2D_t Original_CreateTexture2D_Resize = nullptr;
    volatile bool g_resizeHooksApplied = false;
    volatile bool g_resizeHooksReady = false; // Separate flag for when hook is fully ready
    
    // Static buffer pool - buffers never get freed to avoid use-after-free
    std::mutex g_bufferPoolMutex;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> g_bufferPool;
    size_t g_currentBufferIndex = 0;
    const size_t MAX_BUFFERS = 4; // Rotate through 4 buffers
    
    std::vector<uint8_t>* GetBuffer() {
        std::lock_guard<std::mutex> lock(g_bufferPoolMutex);
        
        // Initialize pool if needed
        if (g_bufferPool.empty()) {
            for (size_t i = 0; i < MAX_BUFFERS; ++i) {
                g_bufferPool.push_back(std::make_unique<std::vector<uint8_t>>());
            }
        }
        
        // Get next buffer in rotation
        auto* buffer = g_bufferPool[g_currentBufferIndex].get();
        g_currentBufferIndex = (g_currentBufferIndex + 1) % MAX_BUFFERS;
        return buffer;
    }
}

// Get bytes per pixel for supported formats
UINT GetBytesPerPixel(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return 4;
        case DXGI_FORMAT_B4G4R4A4_UNORM:
            return 2;
        default:
            return 4; // Default assumption
    }
}

// Create pillarboxed texture data
void CreatePillarboxedData(const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, std::vector<uint8_t>& buffer) {
    // Validate inputs
    if (!pDesc || !pInitialData || !pInitialData->pSysMem || pInitialData->SysMemPitch == 0) {
        std::cout << "Warning: Invalid input to CreatePillarboxedData" << std::endl;
        return;
    }
    
    if (pDesc->Width == 0 || pDesc->Height == 0) {
        std::cout << "Warning: Invalid texture dimensions" << std::endl;
        return;
    }
    
    try {
        float widescreenRatio = GetCurrentWidescreenRatio();
        
        // Calculate new content width (scaled down)
        UINT contentWidth = static_cast<UINT>(pDesc->Width * widescreenRatio);
        if (contentWidth < 1) contentWidth = 1;
        if (contentWidth > pDesc->Width) contentWidth = pDesc->Width;
        
        UINT padding = (pDesc->Width - contentWidth) / 2;
        
        // Support multiple formats
        UINT bytesPerPixel = GetBytesPerPixel(pDesc->Format);
        UINT rowPitch = pDesc->Width * bytesPerPixel;
        
        buffer.resize(rowPitch * pDesc->Height);
        std::fill(buffer.begin(), buffer.end(), 0); // Fill with transparent black
        
        if (pInitialData && pInitialData->pSysMem && pInitialData->SysMemPitch > 0) {
            const uint8_t* srcData = static_cast<const uint8_t*>(pInitialData->pSysMem);
            
            // Calculate safe source buffer size
            size_t srcBufferSize = pInitialData->SysMemPitch * pDesc->Height;
            
            // Copy each row with bilinear interpolation
            for (UINT y = 0; y < pDesc->Height; ++y) {
                for (UINT x = 0; x < contentWidth; ++x) {
                    // Map from scaled position to original position (with sub-pixel precision)
                    float srcXf = (float)x / widescreenRatio;
                    UINT srcX0 = static_cast<UINT>(srcXf);
                    if (srcX0 >= pDesc->Width) srcX0 = pDesc->Width - 1;
                    UINT srcX1 = (srcX0 + 1 < pDesc->Width - 1) ? srcX0 + 1 : pDesc->Width - 1;
                    float fracX = srcXf - srcX0;
                    
                    UINT dstOffset = y * rowPitch + (padding + x) * bytesPerPixel;
                    
                    // Safety check on destination
                    if (dstOffset + bytesPerPixel > buffer.size()) continue;
                    
                    // Handle different formats
                    if (pDesc->Format == DXGI_FORMAT_B4G4R4A4_UNORM) {
                        // 16-bit BGRA4 format - interpolate as 16-bit values
                        UINT srcOffset0 = y * pInitialData->SysMemPitch + srcX0 * bytesPerPixel;
                        UINT srcOffset1 = y * pInitialData->SysMemPitch + srcX1 * bytesPerPixel;
                        
                        if (srcOffset0 + 1 >= srcBufferSize || srcOffset1 + 1 >= srcBufferSize) {
                            buffer[dstOffset] = 0;
                            buffer[dstOffset + 1] = 0;
                            continue;
                        }
                        
                        uint16_t pixel0 = *reinterpret_cast<const uint16_t*>(srcData + srcOffset0);
                        uint16_t pixel1 = *reinterpret_cast<const uint16_t*>(srcData + srcOffset1);
                        
                        // Extract and interpolate each 4-bit channel
                        uint8_t b0 = pixel0 & 0xF, b1 = pixel1 & 0xF;
                        uint8_t g0 = (pixel0 >> 4) & 0xF, g1 = (pixel1 >> 4) & 0xF;
                        uint8_t r0 = (pixel0 >> 8) & 0xF, r1 = (pixel1 >> 8) & 0xF;
                        uint8_t a0 = (pixel0 >> 12) & 0xF, a1 = (pixel1 >> 12) & 0xF;
                        
                        uint8_t b = static_cast<uint8_t>(b0 * (1.0f - fracX) + b1 * fracX + 0.5f);
                        uint8_t g = static_cast<uint8_t>(g0 * (1.0f - fracX) + g1 * fracX + 0.5f);
                        uint8_t r = static_cast<uint8_t>(r0 * (1.0f - fracX) + r1 * fracX + 0.5f);
                        uint8_t a = static_cast<uint8_t>(a0 * (1.0f - fracX) + a1 * fracX + 0.5f);
                        
                        uint16_t result = (b & 0xF) | ((g & 0xF) << 4) | ((r & 0xF) << 8) | ((a & 0xF) << 12);
                        *reinterpret_cast<uint16_t*>(buffer.data() + dstOffset) = result;
                    } else {
                        // 32-bit RGBA/BGRA formats - interpolate byte by byte
                        for (UINT b = 0; b < bytesPerPixel; ++b) {
                            UINT srcOffset0 = y * pInitialData->SysMemPitch + srcX0 * bytesPerPixel + b;
                            UINT srcOffset1 = y * pInitialData->SysMemPitch + srcX1 * bytesPerPixel + b;
                            
                            // CRITICAL: Bounds check on source buffer
                            if (srcOffset0 >= srcBufferSize || srcOffset1 >= srcBufferSize) {
                                buffer[dstOffset + b] = 0; // Use black if out of bounds
                                continue;
                            }
                            
                            float val0 = srcData[srcOffset0];
                            float val1 = srcData[srcOffset1];
                            
                            // Linear interpolation
                            float result = val0 * (1.0f - fracX) + val1 * fracX;
                            buffer[dstOffset + b] = static_cast<uint8_t>(result + 0.5f);
                        }
                    }
                }
            }
        }
        
        std::cout << "Created pillarboxed texture: " << pDesc->Width << "x" << pDesc->Height 
                  << " (content: " << contentWidth << "x" << pDesc->Height 
                  << ", padding: " << padding << " each side, ratio: " << widescreenRatio << ")" << std::endl;
    } catch (...) {
        std::cout << "Warning: Exception in CreatePillarboxedData" << std::endl;
        buffer.clear();
    }
}

HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D_Resize(ID3D11Device* This, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) {
    // Safety: If hook isn't fully ready, just pass through
    if (!g_resizeHooksReady || !Original_CreateTexture2D_Resize) {
        return E_FAIL; // Will cause upscale hook to use its original
    }
    
    HRESULT hr;
    
    // Check for texture resizing based on widescreen ratio
    if (pDesc && pInitialData && ShouldResizeTexture(pDesc, pInitialData)) {
        float widescreenRatio = GetCurrentWidescreenRatio();
        
        // Skip if not widescreen
        if (widescreenRatio >= 0.99f && widescreenRatio <= 1.01f) {
            hr = Original_CreateTexture2D_Resize(This, pDesc, pInitialData, ppTexture2D);
        } else {
            // Only resize if texture usage allows UpdateSubresource
            // Must be D3D11_USAGE_DEFAULT and not have CPU access flags
            if (pDesc->Usage != D3D11_USAGE_DEFAULT || pDesc->CPUAccessFlags != 0) {
                // Can't use UpdateSubresource on this texture, create normally
                hr = Original_CreateTexture2D_Resize(This, pDesc, pInitialData, ppTexture2D);
            } else {
                try {
                    // Get a persistent buffer from the pool
                    std::vector<uint8_t>* buffer = GetBuffer();
                    CreatePillarboxedData(pDesc, pInitialData, *buffer);
                    
                    if (buffer->empty()) {
                        // Failed to create buffer, use original data
                        hr = Original_CreateTexture2D_Resize(This, pDesc, pInitialData, ppTexture2D);
                    } else {
                        // Create new initial data structure
                        D3D11_SUBRESOURCE_DATA newInitialData;
                        newInitialData.pSysMem = buffer->data();
                        newInitialData.SysMemPitch = pDesc->Width * GetBytesPerPixel(pDesc->Format);
                        newInitialData.SysMemSlicePitch = 0;
                        
                        // Create texture with pillarboxed data
                        hr = Original_CreateTexture2D_Resize(This, pDesc, &newInitialData, ppTexture2D);
                        
                        // CRITICAL: Flush to ensure D3D11 has copied the buffer
                        // Buffer persists in pool, so no scope issues
                        if (SUCCEEDED(hr)) {
                            ID3D11DeviceContext* pContext = nullptr;
                            This->GetImmediateContext(&pContext);
                            if (pContext) {
                                pContext->Flush();
                                pContext->Release();
                            }
                        }
                        
                        // Dump texture if creation succeeded
                        if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D) {
                            ID3D11DeviceContext* pContext = nullptr;
                            This->GetImmediateContext(&pContext);
                            if (pContext) {
                                DumpTexture2D(This, pContext, *ppTexture2D, pDesc, &newInitialData);
                                pContext->Release();
                            }
                        }
                    }
                } catch (...) {
                    std::cout << "Warning: Exception in texture resize, using original data" << std::endl;
                    hr = Original_CreateTexture2D_Resize(This, pDesc, pInitialData, ppTexture2D);
                }
            }
        }
    } else {
        hr = Original_CreateTexture2D_Resize(This, pDesc, pInitialData, ppTexture2D);
        
        // Dump texture if creation succeeded
        if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D && pDesc) {
            ID3D11DeviceContext* pContext = nullptr;
            This->GetImmediateContext(&pContext);
            if (pContext) {
                DumpTexture2D(This, pContext, *ppTexture2D, pDesc, pInitialData);
                pContext->Release();
            }
        }
    }
    
    return hr;
}

void ApplyTextureResizeHooks(ID3D11Device* pDevice) {
    if (!pDevice || g_resizeHooksApplied) return;
    
    // Check if texture resizing is enabled in settings
    char exePath[MAX_PATH];
    std::string settingsPath = "settings.ini";
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) != 0) {
        std::string exePathStr(exePath);
        size_t lastBackslash = exePathStr.find_last_of("\\/");
        if (lastBackslash != std::string::npos) {
            settingsPath = exePathStr.substr(0, lastBackslash + 1) + "settings.ini";
        }
    }
    
    // Read settings (need to include settings.h)
    std::ifstream file(settingsPath);
    bool enabled = false;
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("texture_resize_enabled=1") != std::string::npos) {
                enabled = true;
                break;
            }
        }
        file.close();
    }
    
    if (!enabled) {
        std::cout << "Texture resize disabled in settings" << std::endl;
        g_resizeHooksApplied = true; // Mark as applied so we don't check again
        return;
    }
    
    g_resizeHooksApplied = true;
    
    void** deviceVtable = *(void***)pDevice;
    DWORD oldProtect;
    
    if (VirtualProtect(deviceVtable, sizeof(void*) * 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (deviceVtable[5] != (void*)Hooked_CreateTexture2D_Resize) {
            Original_CreateTexture2D_Resize = (CreateTexture2D_t)deviceVtable[5];
            deviceVtable[5] = (void*)Hooked_CreateTexture2D_Resize;
            
            // Memory barrier to ensure all cores see the updated VTable
            FlushInstructionCache(GetCurrentProcess(), deviceVtable, sizeof(void*) * 10);
        }
        
        VirtualProtect(deviceVtable, sizeof(void*) * 10, oldProtect, &oldProtect);
        
        // Mark as ready AFTER everything is set up
        g_resizeHooksReady = true;
        
        std::cout << "Texture resize hooks applied" << std::endl;
    }
}
