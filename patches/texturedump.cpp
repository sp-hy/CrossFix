#include "texturedump.h"
#include "../utils/settings.h"
#include "widescreen.h"
#include <Windows.h>
#include <wrl/client.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <functional>
#include <algorithm>
#include <set>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace {
    bool g_textureDumpEnabled = false;
    bool g_settingsLoaded = false;
    std::string g_dumpPath;
    volatile long g_dumpInProgress = 0; // Simple flag to prevent concurrent dumps
    std::set<uint64_t> g_recentDumps; // Track recently dumped textures to avoid rapid re-dumps

    // Simple FNV-1a hash function
    uint64_t HashData(const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        uint64_t hash = 14695981039346656037ULL; // FNV offset basis
        
        for (size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL; // FNV prime
        }
        
        return hash;
    }

    // Hash texture description and initial data
    uint64_t HashTexture(const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData) {
        uint64_t hash = 0;
        
        // Hash texture description
        if (pDesc) {
            hash = HashData(pDesc, sizeof(D3D11_TEXTURE2D_DESC));
        }
        
        // Hash initial data if available - with comprehensive validation
        if (pInitialData && pInitialData->pSysMem && pDesc) {
            UINT rowPitch = pInitialData->SysMemPitch;
            
            // Validate dimensions and pitch
            if (pDesc->Height == 0 || pDesc->Width == 0 || rowPitch == 0) {
                return hash; // Invalid dimensions, skip data hashing
            }
            
            // Calculate bytes per pixel
            UINT bytesPerPixel = 0;
            switch (pDesc->Format) {
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                    bytesPerPixel = 4;
                    break;
                case DXGI_FORMAT_B4G4R4A4_UNORM:
                    bytesPerPixel = 2;
                    break;
                case DXGI_FORMAT_R16G16B16A16_FLOAT:
                    bytesPerPixel = 8;
                    break;
                case DXGI_FORMAT_R32G32B32A32_FLOAT:
                    bytesPerPixel = 16;
                    break;
                case DXGI_FORMAT_R8_UNORM:
                    bytesPerPixel = 1;
                    break;
                case DXGI_FORMAT_R8G8_UNORM:
                    bytesPerPixel = 2;
                    break;
                default:
                    bytesPerPixel = 4; // Default assumption
                    break;
            }
            
            if (bytesPerPixel == 0) {
                return hash; // Unknown format
            }
            
            // Calculate minimum required row size
            UINT actualRowSize = pDesc->Width * bytesPerPixel;
            
            // Validate that rowPitch is at least as large as actualRowSize
            if (rowPitch < actualRowSize) {
                return hash; // Invalid pitch, skip data hashing
            }
            
            // Calculate actual buffer size: rowPitch * (height - 1) + actual last row size
            // The last row doesn't need padding, so we can't assume full rowPitch for all rows
            size_t dataSize;
            if (pDesc->Height == 1) {
                dataSize = actualRowSize;
            } else {
                dataSize = rowPitch * (pDesc->Height - 1) + actualRowSize;
            }
            
            // Additional safety checks
            if (dataSize == 0 || dataSize >= 100 * 1024 * 1024) {
                return hash; // Size out of acceptable range
            }
            
            // For large textures, only sample a portion to reduce crash risk and improve performance
            // This is a trade-off: less unique hashes but much safer on problematic memory
            size_t sampleSize = dataSize;
            const size_t MAX_SAFE_HASH_SIZE = 64 * 1024; // Only hash first 64KB max
            if (sampleSize > MAX_SAFE_HASH_SIZE) {
                sampleSize = MAX_SAFE_HASH_SIZE;
            }
            
            // Use IsBadReadPtr to check if memory is readable (Windows-specific)
            if (IsBadReadPtr(pInitialData->pSysMem, sampleSize)) {
                return hash; // Memory not readable, skip
            }
            
            // Wrap in try-catch for additional safety
            __try {
                uint64_t dataHash = HashData(pInitialData->pSysMem, sampleSize);
                hash ^= dataHash;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                // Access violation or other exception - just skip data hashing
                // Hash will still be based on descriptor
            }
        }
        
        return hash;
    }

    // Get format folder name
    std::string GetFormatFolderName(DXGI_FORMAT format) {
        switch (format) {
            case DXGI_FORMAT_B4G4R4A4_UNORM:
                return "BGRA4";
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                return "BGRA8";
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                return "RGBA8";
            default:
                return "other";
        }
    }

    // Check if texture is fully transparent (all alpha values are 0)
    bool IsFullyTransparent(const void* pData, UINT width, UINT height, UINT rowPitch, DXGI_FORMAT format) {
        if (!pData || width == 0 || height == 0) return false;
        
        const uint8_t* bytes = static_cast<const uint8_t*>(pData);
        
        switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                // 32-bit RGBA/BGRA - alpha is in the 4th byte of each pixel
                for (UINT y = 0; y < height; ++y) {
                    const uint8_t* row = bytes + y * rowPitch;
                    for (UINT x = 0; x < width; ++x) {
                        if (row[x * 4 + 3] != 0) return false; // Check alpha byte
                    }
                }
                return true;
                
            case DXGI_FORMAT_B4G4R4A4_UNORM:
                // 16-bit BGRA4 - alpha is in the high 4 bits of the high byte
                for (UINT y = 0; y < height; ++y) {
                    const uint8_t* row = bytes + y * rowPitch;
                    for (UINT x = 0; x < width; ++x) {
                        uint16_t pixel = *reinterpret_cast<const uint16_t*>(row + x * 2);
                        uint8_t alpha = (pixel >> 12) & 0xF; // High 4 bits
                        if (alpha != 0) return false;
                    }
                }
                return true;
                
            default:
                return false; // Can't determine transparency for other formats
        }
    }

    // Initialize dump directory with subdirectories
    void InitializeDumpPath() {
        if (!g_dumpPath.empty()) return;
        
        char exePath[MAX_PATH];
        if (GetModuleFileNameA(NULL, exePath, MAX_PATH) != 0) {
            std::string exePathStr(exePath);
            size_t lastBackslash = exePathStr.find_last_of("\\/");
            if (lastBackslash != std::string::npos) {
                g_dumpPath = exePathStr.substr(0, lastBackslash + 1) + "dump";
            } else {
                g_dumpPath = "dump";
            }
        } else {
            g_dumpPath = "dump";
        }
        
        // Create main directory and subdirectories
        CreateDirectoryA(g_dumpPath.c_str(), NULL);
        CreateDirectoryA((g_dumpPath + "\\BGRA4").c_str(), NULL);
        CreateDirectoryA((g_dumpPath + "\\BGRA8").c_str(), NULL);
        CreateDirectoryA((g_dumpPath + "\\RGBA8").c_str(), NULL);
        CreateDirectoryA((g_dumpPath + "\\transparent").c_str(), NULL);
        CreateDirectoryA((g_dumpPath + "\\other").c_str(), NULL);
    }

    // Check if format is dumpable
    bool IsDumpableFormat(DXGI_FORMAT format) {
        switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B4G4R4A4_UNORM:
            case DXGI_FORMAT_R8_UNORM:
            case DXGI_FORMAT_R8G8_UNORM:
                return true;
            default:
                return false;
        }
    }

    // Save texture as DDS, returns transparency status via out parameter
    bool SaveTextureAsDDS(ID3D11Texture2D* pTexture, const D3D11_TEXTURE2D_DESC* pDesc, const std::string& filepath, bool* pIsTransparent = nullptr) {
        if (pIsTransparent) *pIsTransparent = false;
        if (!pTexture || !pDesc) return false;
        
        // Skip if format is not dumpable
        if (!IsDumpableFormat(pDesc->Format)) return false;
        
        // Skip if texture is too large (safety check)
        if (pDesc->Width > 16384 || pDesc->Height > 16384) return false;
        
        // Skip render targets and depth stencils - they're harder to copy and might be in use
        if (pDesc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) {
            return false;
        }
        
        // Skip if texture has no CPU access and isn't already staging
        if (pDesc->Usage != D3D11_USAGE_STAGING && !(pDesc->CPUAccessFlags & D3D11_CPU_ACCESS_READ)) {
            // We'll need to create a staging copy, but skip if it's a dynamic texture that might be in use
            if (pDesc->Usage == D3D11_USAGE_DYNAMIC) {
                return false;
            }
        }
        
        ComPtr<ID3D11Device> pDevice;
        pTexture->GetDevice(&pDevice);
        if (!pDevice) return false;
        
        ComPtr<ID3D11DeviceContext> pContext;
        pDevice->GetImmediateContext(&pContext);
        if (!pContext) return false;
        
        // Create staging texture to read data
        D3D11_TEXTURE2D_DESC stagingDesc = *pDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.BindFlags = 0;
        stagingDesc.MiscFlags = 0;
        
        ComPtr<ID3D11Texture2D> pStaging;
        HRESULT hr = pDevice->CreateTexture2D(&stagingDesc, nullptr, &pStaging);
        if (FAILED(hr) || !pStaging) return false;
        
        // Copy resource - this might fail if texture is in use, so handle gracefully
        pContext->CopyResource(pStaging.Get(), pTexture);
        
        // Flush to ensure copy completes
        pContext->Flush();
        
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = pContext->Map(pStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) {
            pStaging.Reset();
            return false;
        }
        
        // Check if texture is fully transparent
        if (pIsTransparent) {
            *pIsTransparent = IsFullyTransparent(mapped.pData, pDesc->Width, pDesc->Height, mapped.RowPitch, pDesc->Format);
        }
        
        // Write simple DDS file
        std::ofstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            pContext->Unmap(pStaging.Get(), 0);
            return false;
        }
        
        // DDS header
        struct DDS_HEADER {
            DWORD dwMagic;           // "DDS "
            DWORD dwSize;            // 124
            DWORD dwFlags;           // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
            DWORD dwHeight;
            DWORD dwWidth;
            DWORD dwPitchOrLinearSize;
            DWORD dwDepth;
            DWORD dwMipMapCount;
            DWORD dwReserved1[11];
            struct {
                DWORD dwSize;        // 32
                DWORD dwFlags;       // DDPF_FOURCC or DDPF_RGB
                DWORD dwFourCC;
                DWORD dwRGBBitCount;
                DWORD dwRBitMask;
                DWORD dwGBitMask;
                DWORD dwBBitMask;
                DWORD dwABitMask;
            } ddspf;
            DWORD dwCaps;
            DWORD dwCaps2;
            DWORD dwCaps3;
            DWORD dwCaps4;
            DWORD dwReserved2;
        } ddsHeader = {};
        
        ddsHeader.dwMagic = 0x20534444; // "DDS "
        ddsHeader.dwSize = 124;
        ddsHeader.dwFlags = 0x1 | 0x2 | 0x4 | 0x1000; // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
        ddsHeader.dwHeight = pDesc->Height;
        ddsHeader.dwWidth = pDesc->Width;
        ddsHeader.dwPitchOrLinearSize = mapped.RowPitch;
        ddsHeader.dwMipMapCount = pDesc->MipLevels > 1 ? pDesc->MipLevels : 0;
        ddsHeader.ddspf.dwSize = 32;
        
        // Set pixel format based on DXGI format
        switch (pDesc->Format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                ddsHeader.ddspf.dwFlags = 0x40 | 0x1; // DDPF_RGB | DDPF_ALPHAPIXELS
                ddsHeader.ddspf.dwRGBBitCount = 32;
                ddsHeader.ddspf.dwRBitMask = 0x000000FF;
                ddsHeader.ddspf.dwGBitMask = 0x0000FF00;
                ddsHeader.ddspf.dwBBitMask = 0x00FF0000;
                ddsHeader.ddspf.dwABitMask = 0xFF000000;
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                ddsHeader.ddspf.dwFlags = 0x40 | 0x1; // DDPF_RGB | DDPF_ALPHAPIXELS
                ddsHeader.ddspf.dwRGBBitCount = 32;
                ddsHeader.ddspf.dwRBitMask = 0x00FF0000; // B and R swapped for BGRA
                ddsHeader.ddspf.dwGBitMask = 0x0000FF00;
                ddsHeader.ddspf.dwBBitMask = 0x000000FF;
                ddsHeader.ddspf.dwABitMask = 0xFF000000;
                break;
            case DXGI_FORMAT_B4G4R4A4_UNORM:
                ddsHeader.ddspf.dwFlags = 0x40 | 0x1; // DDPF_RGB | DDPF_ALPHAPIXELS
                ddsHeader.ddspf.dwRGBBitCount = 16;
                ddsHeader.ddspf.dwRBitMask = 0x0F00; // BGRA4: B=0x000F, G=0x00F0, R=0x0F00, A=0xF000
                ddsHeader.ddspf.dwGBitMask = 0x00F0;
                ddsHeader.ddspf.dwBBitMask = 0x000F;
                ddsHeader.ddspf.dwABitMask = 0xF000;
                break;
            case DXGI_FORMAT_R8_UNORM:
                ddsHeader.ddspf.dwFlags = 0x40; // DDPF_RGB
                ddsHeader.ddspf.dwRGBBitCount = 8;
                ddsHeader.ddspf.dwRBitMask = 0xFF;
                ddsHeader.ddspf.dwGBitMask = 0;
                ddsHeader.ddspf.dwBBitMask = 0;
                ddsHeader.ddspf.dwABitMask = 0;
                break;
            case DXGI_FORMAT_R8G8_UNORM:
                ddsHeader.ddspf.dwFlags = 0x40; // DDPF_RGB
                ddsHeader.ddspf.dwRGBBitCount = 16;
                ddsHeader.ddspf.dwRBitMask = 0x00FF;
                ddsHeader.ddspf.dwGBitMask = 0xFF00;
                ddsHeader.ddspf.dwBBitMask = 0;
                ddsHeader.ddspf.dwABitMask = 0;
                break;
            default:
                pContext->Unmap(pStaging.Get(), 0);
                return false; // Should not reach here if IsDumpableFormat works correctly
        }
        
        ddsHeader.dwCaps = 0x1000; // DDSCAPS_TEXTURE
        
        file.write(reinterpret_cast<const char*>(&ddsHeader), sizeof(ddsHeader));
        
        // Calculate bytes per pixel
        UINT bytesPerPixel = 0;
        switch (pDesc->Format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                bytesPerPixel = 4;
                break;
            case DXGI_FORMAT_B4G4R4A4_UNORM:
            case DXGI_FORMAT_R8G8_UNORM:
                bytesPerPixel = 2;
                break;
            case DXGI_FORMAT_R8_UNORM:
                bytesPerPixel = 1;
                break;
        }
        
        UINT rowSize = pDesc->Width * bytesPerPixel;
        
        // Write pixel data (only write actual row size, not pitch)
        bool writeSuccess = true;
        try {
            const uint8_t* pData = static_cast<const uint8_t*>(mapped.pData);
            for (UINT y = 0; y < pDesc->Height; ++y) {
                file.write(reinterpret_cast<const char*>(pData + y * mapped.RowPitch), rowSize);
                if (file.fail()) {
                    writeSuccess = false;
                    break;
                }
            }
        } catch (...) {
            writeSuccess = false;
        }
        
        file.close();
        pContext->Unmap(pStaging.Get(), 0);
        pStaging.Reset();
        
        return writeSuccess;
    }
}

bool IsTextureDumpEnabled() {
    if (g_settingsLoaded) return g_textureDumpEnabled;
    
    g_settingsLoaded = true;
    
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
    g_textureDumpEnabled = settings.GetBool("texture_dump_enabled", false);
    
    if (g_textureDumpEnabled) {
        InitializeDumpPath();
    }
    
    return g_textureDumpEnabled;
}

void DumpTexture2D(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11Texture2D* pTexture, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData) {
    if (!IsTextureDumpEnabled() || !pTexture || !pDesc || !pDevice || !pContext) return;
    
    // Skip if another dump is in progress (simple rate limiting)
    if (InterlockedCompareExchange(&g_dumpInProgress, 1, 0) != 0) {
        return; // Another dump is in progress, skip this one
    }
    
    // Skip very small textures (likely noise or unimportant)
    if (pDesc->Width < 4 || pDesc->Height < 4) {
        InterlockedExchange(&g_dumpInProgress, 0);
        return;
    }
    
    // Calculate hash
    uint64_t hash = HashTexture(pDesc, pInitialData);
    
    // Check if we've recently dumped this texture (avoid rapid re-dumps)
    if (g_recentDumps.find(hash) != g_recentDumps.end()) {
        InterlockedExchange(&g_dumpInProgress, 0);
        return; // Recently dumped, skip
    }
    
    // Determine format folder
    std::string formatFolder = GetFormatFolderName(pDesc->Format);
    
    // Generate filename with decimal size first: WIDTHxHEIGHT_HASH.dds
    std::ostringstream filename;
    filename << std::dec << pDesc->Width << "x" << pDesc->Height 
             << "_" << std::hex << std::setfill('0') << std::setw(16) << hash
             << ".dds";
    
    std::string filenameStr = filename.str();
    
    // Build temp filepath (we'll move to final location after checking transparency)
    std::string tempPath = g_dumpPath + "\\" + formatFolder + "\\" + filenameStr;
    
    // Check if file already exists in any folder (cache hit)
    std::vector<std::string> foldersToCheck = {"BGRA4", "BGRA8", "RGBA8", "transparent", "other"};
    for (const auto& folder : foldersToCheck) {
        std::string checkPath = g_dumpPath + "\\" + folder + "\\" + filenameStr;
        try {
            std::ifstream testFile(checkPath, std::ios::binary);
            if (testFile.good()) {
                testFile.close();
                InterlockedExchange(&g_dumpInProgress, 0);
                return; // Already dumped
            }
            testFile.close();
        } catch (...) {
            // Continue checking
        }
    }
    
    // Save texture - wrap in try-catch to prevent crashes
    bool dumped = false;
    bool isTransparent = false;
    try {
        if (SaveTextureAsDDS(pTexture, pDesc, tempPath, &isTransparent)) {
            // If fully transparent, move to transparent folder
            if (isTransparent) {
                std::string finalPath = g_dumpPath + "\\transparent\\" + filenameStr;
                // Delete from temp location and write to transparent folder
                if (tempPath != finalPath) {
                    MoveFileA(tempPath.c_str(), finalPath.c_str());
                    std::cout << "Dumped transparent texture: " << finalPath << std::endl;
                }
            } else {
                std::cout << "Dumped texture: " << tempPath << std::endl;
            }
            dumped = true;
        }
    } catch (...) {
        // Silently fail to prevent crashes
    }
    
    // If successfully dumped, add to recent dumps cache (limit cache size)
    if (dumped) {
        if (g_recentDumps.size() > 1000) {
            g_recentDumps.clear(); // Clear cache if it gets too large
        }
        g_recentDumps.insert(hash);
    }
    
    // Release the lock
    InterlockedExchange(&g_dumpInProgress, 0);
}

// ============================================================================
// Texture Resizing System
// ============================================================================

namespace {
    // List of texture hashes that should be resized based on widescreen ratio
    std::unordered_set<uint64_t> g_resizeHashes;
    // List of texture sizes (Width, Height) that should be resized
    std::set<std::pair<UINT, UINT>> g_resizeSizes;
    bool g_resizeHashesInitialized = false;
    
    // Initialize the resize hash list with known textures
    void InitializeResizeHashes() {
        if (g_resizeHashesInitialized) return;
        g_resizeHashesInitialized = true;
        
        // Example: texture_4f83cf6716f5f0c5_120x120_BGRA8.dds
        // This hash is from the filename - you can add more hashes here

        g_resizeHashes.insert(0x30c9ba509d8378f8ULL); // Main Logo (1280x960)


        g_resizeSizes.insert({ 1024, 1024 });        // Key Items
        g_resizeSizes.insert({ 512, 512 });        // Key Items
        g_resizeSizes.insert({ 512, 896 });        // Menu portraits (menu)
        g_resizeSizes.insert({ 256, 1792 });        // Menu portraits (post battle)
        g_resizeSizes.insert({ 1164, 1166 });        // Menu compass
        g_resizeSizes.insert({ 405, 58 });        // Menu compass arrow
        g_resizeSizes.insert({ 250, 45 });        // Battle hit chance anchor
        g_resizeSizes.insert({ 128, 128 }); // Menu mini avatar
        g_resizeSizes.insert({ 92, 44 });        // menu arrows
        // g_resizeSizes.insert({ 480, 64 }); // Battle element name container
        // g_resizeSizes.insert({ 736, 96 }); // Battle element name menu?
        // g_resizeSizes.insert({ 64, 32 }); // Battle element mini



        
        std::cout << "Initialized texture resize list with " << g_resizeHashes.size() << " hashes and " << g_resizeSizes.size() << " sizes" << std::endl;
    }
}

// Add a hash to the resize list
void AddResizeHash(uint64_t hash) {
    InitializeResizeHashes();
    g_resizeHashes.insert(hash);
    std::cout << "Added hash 0x" << std::hex << hash << std::dec << " to resize list" << std::endl;
}

// Clear all resize hashes
void ClearResizeHashes() {
    g_resizeHashes.clear();
    g_resizeHashesInitialized = false;
    std::cout << "Cleared texture resize hash list" << std::endl;
}

// Check if a texture should be resized based on its hash
bool ShouldResizeTexture(const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData) {
    if (!pDesc) return false;
    
    // Validate initial data
    if (!pInitialData || !pInitialData->pSysMem || pInitialData->SysMemPitch == 0) {
        return false;
    }
    
    // Validate texture dimensions
    if (pDesc->Width == 0 || pDesc->Height == 0 || pDesc->Width > 16384 || pDesc->Height > 16384) {
        return false;
    }
    
    try {
        InitializeResizeHashes();
        
        // 1. Check if this size is explicitly whitelisted
        if (g_resizeSizes.find({pDesc->Width, pDesc->Height}) != g_resizeSizes.end()) {
            return true;
        }
        
        // 2. If size didn't match, check if this specific hash is whitelisted
        if (!g_resizeHashes.empty()) {
            uint64_t hash = HashTexture(pDesc, pInitialData);
            if (g_resizeHashes.find(hash) != g_resizeHashes.end()) {
                return true;
            }
        }
        
        return false;
    } catch (...) {
        // If anything fails, don't resize
        return false;
    }
}

// Resize texture content by adding pillarboxing (empty space on sides)
// Keeps texture dimensions the same but scales content horizontally
void ResizeTexture2D(const D3D11_TEXTURE2D_DESC* pDesc, D3D11_TEXTURE2D_DESC* pNewDesc) {
    if (!pDesc || !pNewDesc) return;
    
    // Just copy the description - we're not changing dimensions
    *pNewDesc = *pDesc;
    
    // Note: Actual pixel manipulation would need to happen in the hook
    // This function just prepares the description
    // The real work is done in the CreateTexture2D hook by modifying pInitialData
}
