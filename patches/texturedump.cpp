#include "texturedump.h"
#include "../utils/memory.h"
#include "../utils/settings.h"
#include "texturereplace.h"
#include <Windows.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {
bool g_textureDumpEnabled = false;
bool g_settingsLoaded = false;
std::string g_dumpPath;
CRITICAL_SECTION g_dumpCS;
volatile LONG g_dumpCSInitialized = 0;
std::set<uint64_t> g_recentDumps;

void InitDumpCS() {
  if (InterlockedCompareExchange(&g_dumpCSInitialized, 1, 0) == 0) {
    InitializeCriticalSection(&g_dumpCS);
  }
}

// Simple FNV-1a hash function
uint64_t HashData(const void *data, size_t size) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  uint64_t hash = 14695981039346656037ULL; // FNV offset basis

  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL; // FNV prime
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
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
    return "BC1_DXT1";
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
    return "BC2_DXT3";
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
    return "BC3_DXT5";
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    return "BC7";
  default:
    return "other";
  }
}

// Check if texture is fully transparent (all alpha values are 0)
bool IsFullyTransparent(const void *pData, UINT width, UINT height,
                        UINT rowPitch, DXGI_FORMAT format) {
  if (!pData || width == 0 || height == 0)
    return false;

  const uint8_t *bytes = static_cast<const uint8_t *>(pData);

  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    // 32-bit RGBA/BGRA - alpha is in the 4th byte of each pixel
    for (UINT y = 0; y < height; ++y) {
      const uint8_t *row = bytes + y * rowPitch;
      for (UINT x = 0; x < width; ++x) {
        if (row[x * 4 + 3] != 0)
          return false; // Check alpha byte
      }
    }
    return true;

  case DXGI_FORMAT_B4G4R4A4_UNORM:
    // 16-bit BGRA4 - alpha is in the high 4 bits of the high byte
    for (UINT y = 0; y < height; ++y) {
      const uint8_t *row = bytes + y * rowPitch;
      for (UINT x = 0; x < width; ++x) {
        uint16_t pixel = *reinterpret_cast<const uint16_t *>(row + x * 2);
        uint8_t alpha = (pixel >> 12) & 0xF; // High 4 bits
        if (alpha != 0)
          return false;
      }
    }
    return true;

  default:
    return false; // Can't determine transparency for other formats
  }
}

// Initialize dump directory with subdirectories
void InitializeDumpPath() {
  if (!g_dumpPath.empty())
    return;

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
  CreateDirectoryA((g_dumpPath + "\\BC1_DXT1").c_str(), NULL);
  CreateDirectoryA((g_dumpPath + "\\BC2_DXT3").c_str(), NULL);
  CreateDirectoryA((g_dumpPath + "\\BC3_DXT5").c_str(), NULL);
  CreateDirectoryA((g_dumpPath + "\\BC7").c_str(), NULL);
  CreateDirectoryA((g_dumpPath + "\\transparent").c_str(), NULL);
  CreateDirectoryA((g_dumpPath + "\\rendertargets").c_str(), NULL);
  CreateDirectoryA((g_dumpPath + "\\rendertargets\\transparent").c_str(), NULL);
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
  // Compressed formats
  case DXGI_FORMAT_BC1_UNORM: // DXT1
  case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC2_UNORM: // DXT3
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_UNORM: // DXT5
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    return true;
  default:
    return false;
  }
}

// Save texture as DDS, returns transparency status via out parameter
bool SaveTextureAsDDS(ID3D11Texture2D *pTexture,
                      const D3D11_TEXTURE2D_DESC *pDesc,
                      const std::string &filepath,
                      bool *pIsTransparent = nullptr) {
  if (pIsTransparent)
    *pIsTransparent = false;
  if (!pTexture || !pDesc)
    return false;

  // Skip if format is not dumpable
  if (!IsDumpableFormat(pDesc->Format)) {
    std::cout << "Skipping texture " << pDesc->Width << "x" << pDesc->Height
              << " - unsupported format: " << pDesc->Format << std::endl;
    return false;
  }

  // Skip if texture is too large (safety check)
  if (pDesc->Width > 16384 || pDesc->Height > 16384) {
    std::cout << "Skipping texture " << pDesc->Width << "x" << pDesc->Height
              << " - too large" << std::endl;
    return false;
  }

  // Note: We now allow render targets and depth stencils to be dumped
  // They will be copied to a staging texture for reading
  // This works for all texture types: default, dynamic, render targets, etc.

  ComPtr<ID3D11Device> pDevice;
  pTexture->GetDevice(&pDevice);
  if (!pDevice)
    return false;

  ComPtr<ID3D11DeviceContext> pContext;
  pDevice->GetImmediateContext(&pContext);
  if (!pContext)
    return false;

  // Create staging texture to read data
  D3D11_TEXTURE2D_DESC stagingDesc = *pDesc;
  stagingDesc.Usage = D3D11_USAGE_STAGING;
  stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  stagingDesc.BindFlags = 0;
  stagingDesc.MiscFlags = 0;

  ComPtr<ID3D11Texture2D> pStaging;
  HRESULT hr = pDevice->CreateTexture2D(&stagingDesc, nullptr, &pStaging);
  if (FAILED(hr) || !pStaging) {
    std::cout << "Failed to create staging texture for " << pDesc->Width << "x"
              << pDesc->Height << std::endl;
    return false;
  }

  // Copy resource - this might fail if texture is in use, so handle gracefully
  pContext->CopyResource(pStaging.Get(), pTexture);

  // Flush to ensure copy completes
  pContext->Flush();

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = pContext->Map(pStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr) || !mapped.pData) {
    std::cout << "Failed to map staging texture for " << pDesc->Width << "x"
              << pDesc->Height << " HRESULT: 0x" << std::hex << hr << std::dec
              << std::endl;
    pStaging.Reset();
    return false;
  }

  // Check if texture is fully transparent
  if (pIsTransparent) {
    *pIsTransparent =
        IsFullyTransparent(mapped.pData, pDesc->Width, pDesc->Height,
                           mapped.RowPitch, pDesc->Format);
  }

  // Write simple DDS file
  std::ofstream file(filepath, std::ios::binary);
  if (!file.is_open()) {
    pContext->Unmap(pStaging.Get(), 0);
    return false;
  }

  // DDS header
  struct DDS_HEADER {
    DWORD dwMagic; // "DDS "
    DWORD dwSize;  // 124
    DWORD dwFlags; // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
    DWORD dwHeight;
    DWORD dwWidth;
    DWORD dwPitchOrLinearSize;
    DWORD dwDepth;
    DWORD dwMipMapCount;
    DWORD dwReserved1[11];
    struct {
      DWORD dwSize;  // 32
      DWORD dwFlags; // DDPF_FOURCC or DDPF_RGB
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
  ddsHeader.dwFlags =
      0x1 | 0x2 | 0x4 |
      0x1000; // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
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
    ddsHeader.ddspf.dwRBitMask =
        0x0F00; // BGRA4: B=0x000F, G=0x00F0, R=0x0F00, A=0xF000
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
  // Block-compressed formats use FourCC codes
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
    ddsHeader.ddspf.dwFlags = 0x4;         // DDPF_FOURCC
    ddsHeader.ddspf.dwFourCC = 0x31545844; // "DXT1"
    ddsHeader.dwFlags |= 0x80000;          // DDSD_LINEARSIZE
    ddsHeader.dwPitchOrLinearSize =
        ((pDesc->Width + 3) / 4) * ((pDesc->Height + 3) / 4) * 8;
    break;
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
    ddsHeader.ddspf.dwFlags = 0x4;         // DDPF_FOURCC
    ddsHeader.ddspf.dwFourCC = 0x33545844; // "DXT3"
    ddsHeader.dwFlags |= 0x80000;          // DDSD_LINEARSIZE
    ddsHeader.dwPitchOrLinearSize =
        ((pDesc->Width + 3) / 4) * ((pDesc->Height + 3) / 4) * 16;
    break;
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
    ddsHeader.ddspf.dwFlags = 0x4;         // DDPF_FOURCC
    ddsHeader.ddspf.dwFourCC = 0x35545844; // "DXT5"
    ddsHeader.dwFlags |= 0x80000;          // DDSD_LINEARSIZE
    ddsHeader.dwPitchOrLinearSize =
        ((pDesc->Width + 3) / 4) * ((pDesc->Height + 3) / 4) * 16;
    break;
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    ddsHeader.ddspf.dwFlags = 0x4; // DDPF_FOURCC
    ddsHeader.ddspf.dwFourCC =
        0x20495844; // "DX10" - BC7 requires DX10 extended header
    // Note: BC7 technically needs a DDS_HEADER_DXT10 extension, but many tools
    // accept this
    ddsHeader.dwFlags |= 0x80000; // DDSD_LINEARSIZE
    ddsHeader.dwPitchOrLinearSize =
        ((pDesc->Width + 3) / 4) * ((pDesc->Height + 3) / 4) * 16;
    break;
  default:
    pContext->Unmap(pStaging.Get(), 0);
    return false; // Should not reach here if IsDumpableFormat works correctly
  }

  ddsHeader.dwCaps = 0x1000; // DDSCAPS_TEXTURE

  file.write(reinterpret_cast<const char *>(&ddsHeader), sizeof(ddsHeader));

  // Write pixel data
  bool writeSuccess = true;
  try {
    const uint8_t *pData = static_cast<const uint8_t *>(mapped.pData);

    // Check if this is a block-compressed format
    bool isBlockCompressed = false;
    UINT bytesPerBlock = 0;

    switch (pDesc->Format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
      isBlockCompressed = true;
      bytesPerBlock = 8;
      break;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
      isBlockCompressed = true;
      bytesPerBlock = 16;
      break;
    }

    if (isBlockCompressed) {
      // For block-compressed formats, write in block rows
      UINT blocksWide = (pDesc->Width + 3) / 4;
      UINT blocksHigh = (pDesc->Height + 3) / 4;
      UINT blockRowSize = blocksWide * bytesPerBlock;

      for (UINT blockY = 0; blockY < blocksHigh; ++blockY) {
        file.write(
            reinterpret_cast<const char *>(pData + blockY * mapped.RowPitch),
            blockRowSize);
        if (file.fail()) {
          writeSuccess = false;
          break;
        }
      }
    } else {
      // For uncompressed formats, calculate bytes per pixel
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
      for (UINT y = 0; y < pDesc->Height; ++y) {
        file.write(reinterpret_cast<const char *>(pData + y * mapped.RowPitch),
                   rowSize);
        if (file.fail()) {
          writeSuccess = false;
          break;
        }
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
// Write DDS file directly from CPU-side pInitialData (no GPU staging needed)
bool SaveDDSFromInitialData(const D3D11_TEXTURE2D_DESC *pDesc,
                            const D3D11_SUBRESOURCE_DATA *pInitialData,
                            const std::string &filepath, bool *pIsTransparent) {
  if (pIsTransparent)
    *pIsTransparent = false;
  if (!pDesc || !pInitialData || !pInitialData->pSysMem)
    return false;
  if (!IsDumpableFormat(pDesc->Format))
    return false;
  if (pDesc->Width > 16384 || pDesc->Height > 16384)
    return false;

  UINT rowPitch = pInitialData->SysMemPitch;
  if (rowPitch == 0)
    return false;

  // Check transparency on CPU-side data
  if (pIsTransparent) {
    *pIsTransparent =
        IsFullyTransparent(pInitialData->pSysMem, pDesc->Width, pDesc->Height,
                           rowPitch, pDesc->Format);
  }

  std::ofstream file(filepath, std::ios::binary);
  if (!file.is_open())
    return false;

  // DDS header (same structure as SaveTextureAsDDS)
  struct DDS_HEADER {
    DWORD dwMagic;
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwHeight;
    DWORD dwWidth;
    DWORD dwPitchOrLinearSize;
    DWORD dwDepth;
    DWORD dwMipMapCount;
    DWORD dwReserved1[11];
    struct {
      DWORD dwSize;
      DWORD dwFlags;
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
  ddsHeader.dwFlags = 0x1 | 0x2 | 0x4 | 0x1000;
  ddsHeader.dwHeight = pDesc->Height;
  ddsHeader.dwWidth = pDesc->Width;
  ddsHeader.dwMipMapCount = pDesc->MipLevels > 1 ? pDesc->MipLevels : 0;
  ddsHeader.ddspf.dwSize = 32;

  // Calculate bytes per pixel / block info
  bool isBlockCompressed = false;
  UINT bytesPerPixel = 0;
  UINT bytesPerBlock = 0;

  switch (pDesc->Format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    ddsHeader.ddspf.dwFlags = 0x40 | 0x1;
    ddsHeader.ddspf.dwRGBBitCount = 32;
    ddsHeader.ddspf.dwRBitMask = 0x000000FF;
    ddsHeader.ddspf.dwGBitMask = 0x0000FF00;
    ddsHeader.ddspf.dwBBitMask = 0x00FF0000;
    ddsHeader.ddspf.dwABitMask = 0xFF000000;
    bytesPerPixel = 4;
    break;
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    ddsHeader.ddspf.dwFlags = 0x40 | 0x1;
    ddsHeader.ddspf.dwRGBBitCount = 32;
    ddsHeader.ddspf.dwRBitMask = 0x00FF0000;
    ddsHeader.ddspf.dwGBitMask = 0x0000FF00;
    ddsHeader.ddspf.dwBBitMask = 0x000000FF;
    ddsHeader.ddspf.dwABitMask = 0xFF000000;
    bytesPerPixel = 4;
    break;
  case DXGI_FORMAT_B4G4R4A4_UNORM:
    ddsHeader.ddspf.dwFlags = 0x40 | 0x1;
    ddsHeader.ddspf.dwRGBBitCount = 16;
    ddsHeader.ddspf.dwRBitMask = 0x0F00;
    ddsHeader.ddspf.dwGBitMask = 0x00F0;
    ddsHeader.ddspf.dwBBitMask = 0x000F;
    ddsHeader.ddspf.dwABitMask = 0xF000;
    bytesPerPixel = 2;
    break;
  case DXGI_FORMAT_R8_UNORM:
    ddsHeader.ddspf.dwFlags = 0x40;
    ddsHeader.ddspf.dwRGBBitCount = 8;
    ddsHeader.ddspf.dwRBitMask = 0xFF;
    bytesPerPixel = 1;
    break;
  case DXGI_FORMAT_R8G8_UNORM:
    ddsHeader.ddspf.dwFlags = 0x40;
    ddsHeader.ddspf.dwRGBBitCount = 16;
    ddsHeader.ddspf.dwRBitMask = 0x00FF;
    ddsHeader.ddspf.dwGBitMask = 0xFF00;
    bytesPerPixel = 2;
    break;
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
    ddsHeader.ddspf.dwFlags = 0x4;
    ddsHeader.ddspf.dwFourCC = 0x31545844; // "DXT1"
    ddsHeader.dwFlags |= 0x80000;
    isBlockCompressed = true;
    bytesPerBlock = 8;
    break;
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
    ddsHeader.ddspf.dwFlags = 0x4;
    ddsHeader.ddspf.dwFourCC = 0x33545844; // "DXT3"
    ddsHeader.dwFlags |= 0x80000;
    isBlockCompressed = true;
    bytesPerBlock = 16;
    break;
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
    ddsHeader.ddspf.dwFlags = 0x4;
    ddsHeader.ddspf.dwFourCC = 0x35545844; // "DXT5"
    ddsHeader.dwFlags |= 0x80000;
    isBlockCompressed = true;
    bytesPerBlock = 16;
    break;
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    ddsHeader.ddspf.dwFlags = 0x4;
    ddsHeader.ddspf.dwFourCC = 0x20495844; // "DX10"
    ddsHeader.dwFlags |= 0x80000;
    isBlockCompressed = true;
    bytesPerBlock = 16;
    break;
  default:
    return false;
  }

  // Set pitch/linear size in header
  if (isBlockCompressed) {
    UINT blocksWide = (pDesc->Width + 3) / 4;
    UINT blocksHigh = (pDesc->Height + 3) / 4;
    ddsHeader.dwPitchOrLinearSize = blocksWide * blocksHigh * bytesPerBlock;
  } else {
    ddsHeader.dwPitchOrLinearSize = pDesc->Width * bytesPerPixel;
  }

  ddsHeader.dwCaps = 0x1000; // DDSCAPS_TEXTURE

  file.write(reinterpret_cast<const char *>(&ddsHeader), sizeof(ddsHeader));

  // Write pixel data from pInitialData (CPU memory)
  bool writeSuccess = true;
  const uint8_t *pData = static_cast<const uint8_t *>(pInitialData->pSysMem);

  try {
    if (isBlockCompressed) {
      UINT blocksWide = (pDesc->Width + 3) / 4;
      UINT blocksHigh = (pDesc->Height + 3) / 4;
      UINT blockRowSize = blocksWide * bytesPerBlock;

      for (UINT blockY = 0; blockY < blocksHigh; ++blockY) {
        file.write(reinterpret_cast<const char *>(pData + blockY * rowPitch),
                   blockRowSize);
        if (file.fail()) {
          writeSuccess = false;
          break;
        }
      }
    } else {
      UINT rowSize = pDesc->Width * bytesPerPixel;
      for (UINT y = 0; y < pDesc->Height; ++y) {
        file.write(reinterpret_cast<const char *>(pData + y * rowPitch),
                   rowSize);
        if (file.fail()) {
          writeSuccess = false;
          break;
        }
      }
    }
  } catch (...) {
    writeSuccess = false;
  }

  file.close();
  return writeSuccess;
}

} // namespace

// Shared hash core - works with both pInitialData (CPU) and staging map (GPU)
// Hashes only content-identifying desc fields + all pixel data row-by-row
uint64_t HashTextureData(const D3D11_TEXTURE2D_DESC *pDesc, const void *pData,
                         UINT rowPitch) {
  if (!pDesc)
    return 0;

  uint64_t hash = 14695981039346656037ULL; // FNV offset basis

  auto mixField = [&hash](uint32_t val) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&val);
    for (int i = 0; i < 4; ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ULL;
    }
  };

  mixField(pDesc->Width);
  mixField(pDesc->Height);
  mixField(static_cast<uint32_t>(pDesc->Format));
  mixField(pDesc->MipLevels);
  mixField(pDesc->ArraySize);

  if (!pData || rowPitch == 0 || pDesc->Width == 0 || pDesc->Height == 0)
    return hash;

  UINT bytesPerPixel = 0;
  UINT bytesPerBlock = 0;
  bool isBlockCompressed = false;

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
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
    bytesPerBlock = 8;
    isBlockCompressed = true;
    break;
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    bytesPerBlock = 16;
    isBlockCompressed = true;
    break;
  default:
    bytesPerPixel = 4;
    break;
  }

  if (bytesPerPixel == 0 && bytesPerBlock == 0)
    return hash;

  UINT actualRowSize;
  UINT numRows;
  if (isBlockCompressed) {
    actualRowSize = ((pDesc->Width + 3) / 4) * bytesPerBlock;
    numRows = (pDesc->Height + 3) / 4;
  } else {
    actualRowSize = pDesc->Width * bytesPerPixel;
    numRows = pDesc->Height;
  }

  if (rowPitch < actualRowSize)
    return hash;

  size_t dataSize =
      (numRows <= 1)
          ? actualRowSize
          : static_cast<size_t>(rowPitch) * (numRows - 1) + actualRowSize;

  if (dataSize == 0 || dataSize >= 256 * 1024 * 1024)
    return hash;

  if (IsBadReadPtr(pData, dataSize))
    return hash;

  __try {
    const uint8_t *srcData = static_cast<const uint8_t *>(pData);
    for (UINT row = 0; row < numRows; ++row) {
      const uint8_t *rowData = srcData + static_cast<size_t>(row) * rowPitch;
      for (UINT i = 0; i < actualRowSize; ++i) {
        hash ^= rowData[i];
        hash *= 1099511628211ULL;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  return hash;
}

// Thin wrapper for CreateTexture2D path (uses pInitialData)
uint64_t HashTexture(const D3D11_TEXTURE2D_DESC *pDesc,
                     const D3D11_SUBRESOURCE_DATA *pInitialData) {
  if (!pDesc)
    return 0;
  if (pInitialData && pInitialData->pSysMem && pInitialData->SysMemPitch > 0)
    return HashTextureData(pDesc, pInitialData->pSysMem,
                           pInitialData->SysMemPitch);
  return HashTextureData(pDesc, nullptr, 0);
}

bool IsTextureDumpEnabled() {
  if (g_settingsLoaded)
    return g_textureDumpEnabled;

  g_settingsLoaded = true;

  Settings settings;
  settings.Load(Settings::GetSettingsPath());
  g_textureDumpEnabled = settings.GetBool("texture_dump_enabled", false);

  if (g_textureDumpEnabled) {
    InitializeDumpPath();
  }

  return g_textureDumpEnabled;
}

void DumpTexture2D(ID3D11Device *pDevice, ID3D11DeviceContext *pContext,
                   ID3D11Texture2D *pTexture, const D3D11_TEXTURE2D_DESC *pDesc,
                   const D3D11_SUBRESOURCE_DATA *pInitialData) {
  if (!IsTextureDumpEnabled() || !pDesc)
    return;

  // Skip very small textures
  if (pDesc->Width < 4 || pDesc->Height < 4)
    return;

  // Skip unsupported formats
  if (!IsDumpableFormat(pDesc->Format))
    return;

  bool isRenderTarget = (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
  bool hasInitialData =
      pInitialData && pInitialData->pSysMem && pInitialData->SysMemPitch > 0;

  // For non-render-targets, we need initial data to dump
  if (!isRenderTarget && !hasInitialData)
    return;

  // Calculate content-based hash
  uint64_t hash = HashTexture(pDesc, pInitialData);

  // Thread-safe duplicate check
  InitDumpCS();
  EnterCriticalSection(&g_dumpCS);
  bool alreadyDumped = g_recentDumps.find(hash) != g_recentDumps.end();
  if (!alreadyDumped) {
    if (g_recentDumps.size() > 10000)
      g_recentDumps.clear();
    g_recentDumps.insert(hash);
  }
  LeaveCriticalSection(&g_dumpCS);

  if (alreadyDumped)
    return;

  // Generate filename: WIDTHxHEIGHT_HASH.dds
  std::ostringstream filename;
  filename << std::dec << pDesc->Width << "x" << pDesc->Height << "_"
           << std::hex << std::setfill('0') << std::setw(16) << hash << ".dds";
  std::string filenameStr = filename.str();

  // Check if file already exists on disk (persisted from previous session)
  std::string foldersToCheck[] = {
      "BGRA4",    "BGRA8", "RGBA8",       "BC1_DXT1",      "BC2_DXT3",
      "BC3_DXT5", "BC7",   "transparent", "rendertargets", "other"};
  for (const auto &folder : foldersToCheck) {
    std::string checkPath = g_dumpPath + "\\" + folder + "\\" + filenameStr;
    DWORD attrs = GetFileAttributesA(checkPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
      return; // Already on disk
    }
  }

  try {
    if (isRenderTarget) {
      // Render targets go to rendertargets\ folder via GPU staging copy
      std::string rtPath = g_dumpPath + "\\rendertargets\\" + filenameStr;
      bool isTransparent = false;
      if (pTexture &&
          SaveTextureAsDDS(pTexture, pDesc, rtPath, &isTransparent)) {
        if (isTransparent) {
          std::string finalPath =
              g_dumpPath + "\\rendertargets\\transparent\\" + filenameStr;
          MoveFileA(rtPath.c_str(), finalPath.c_str());
          std::cout << "Dumped RT (transparent): " << pDesc->Width << "x"
                    << pDesc->Height << " " << filenameStr << std::endl;
        } else {
          std::cout << "Dumped RT: " << pDesc->Width << "x" << pDesc->Height
                    << " " << filenameStr << std::endl;
        }
      }
    } else {
      // Regular textures: write DDS from pInitialData (CPU-side, no staging)
      std::string formatFolder = GetFormatFolderName(pDesc->Format);
      std::string tempPath =
          g_dumpPath + "\\" + formatFolder + "\\" + filenameStr;
      bool isTransparent = false;
      if (SaveDDSFromInitialData(pDesc, pInitialData, tempPath,
                                 &isTransparent)) {
        if (isTransparent) {
          std::string finalPath = g_dumpPath + "\\transparent\\" + filenameStr;
          MoveFileA(tempPath.c_str(), finalPath.c_str());
          std::cout << "Dumped transparent: " << pDesc->Width << "x"
                    << pDesc->Height << " " << filenameStr << std::endl;
        } else {
          std::cout << "Dumped: " << pDesc->Width << "x" << pDesc->Height << " "
                    << filenameStr << std::endl;
        }
      }
    }
  } catch (...) {
    // Prevent crashes from texture dumping
  }
}

// ============================================================================
// PSSetShaderResources Hook - Dump textures at bind time
// ============================================================================
// This catches textures the emulator creates empty and fills later via
// UpdateSubresource/Map, which CreateTexture2D misses.

namespace {
typedef void(STDMETHODCALLTYPE *PSSetShaderResources_t)(
    ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);

volatile PSSetShaderResources_t Original_PSSetShaderResources = nullptr;
volatile LONG g_psSetSRHookReady = 0;
volatile LONG g_dumpHooksApplied = 0;

// Pointer-based dedup: skip staging for textures we've already processed
std::unordered_set<void *> g_seenTexturePointers;
CRITICAL_SECTION g_seenTexturesCS;
volatile LONG g_seenTexturesCSInitialized = 0;

void InitSeenTexturesCS() {
  if (InterlockedCompareExchange(&g_seenTexturesCSInitialized, 1, 0) == 0) {
    InitializeCriticalSection(&g_seenTexturesCS);
  }
}

// Replacement SRV cache: original texture ptr -> replacement SRV
std::unordered_map<void *, ComPtr<ID3D11ShaderResourceView>>
    g_replacementSRVCache;
std::unordered_set<void *> g_checkedNoReplacement;
CRITICAL_SECTION g_replacementCacheCS;
volatile LONG g_replacementCacheCSInitialized = 0;

void InitReplacementCacheCS() {
  if (InterlockedCompareExchange(&g_replacementCacheCSInitialized, 1, 0) == 0) {
    InitializeCriticalSection(&g_replacementCacheCS);
  }
}

// Dump a texture from GPU via staging copy + content hash
void DumpTextureFromGPU(ID3D11DeviceContext *pContext,
                        ID3D11Texture2D *pTexture,
                        const D3D11_TEXTURE2D_DESC *pDesc) {
  if (!pContext || !pTexture || !pDesc)
    return;

  ComPtr<ID3D11Device> pDevice;
  pTexture->GetDevice(&pDevice);
  if (!pDevice)
    return;

  // Create staging texture
  D3D11_TEXTURE2D_DESC stagingDesc = *pDesc;
  stagingDesc.Usage = D3D11_USAGE_STAGING;
  stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  stagingDesc.BindFlags = 0;
  stagingDesc.MiscFlags = 0;
  stagingDesc.MipLevels = 1;

  ComPtr<ID3D11Texture2D> pStaging;
  HRESULT hr = pDevice->CreateTexture2D(&stagingDesc, nullptr, &pStaging);
  if (FAILED(hr) || !pStaging)
    return;

  pContext->CopySubresourceRegion(pStaging.Get(), 0, 0, 0, 0, pTexture, 0,
                                  nullptr);
  pContext->Flush();

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = pContext->Map(pStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr) || !mapped.pData)
    return;

  // Hash from actual GPU content
  uint64_t hash = HashTextureData(pDesc, mapped.pData, mapped.RowPitch);

  // Check content-hash dedup
  InitDumpCS();
  EnterCriticalSection(&g_dumpCS);
  bool alreadyDumped = g_recentDumps.count(hash) > 0;
  if (!alreadyDumped) {
    if (g_recentDumps.size() > 10000)
      g_recentDumps.clear();
    g_recentDumps.insert(hash);
  }
  LeaveCriticalSection(&g_dumpCS);

  if (alreadyDumped) {
    pContext->Unmap(pStaging.Get(), 0);
    return;
  }

  // Generate filename
  std::ostringstream filename;
  filename << std::dec << pDesc->Width << "x" << pDesc->Height << "_"
           << std::hex << std::setfill('0') << std::setw(16) << hash << ".dds";
  std::string filenameStr = filename.str();

  // Check if already on disk
  bool isRT = (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
  std::string foldersToCheck[] = {
      "BGRA4",    "BGRA8", "RGBA8",       "BC1_DXT1",      "BC2_DXT3",
      "BC3_DXT5", "BC7",   "transparent", "rendertargets", "other"};
  for (const auto &folder : foldersToCheck) {
    std::string checkPath = g_dumpPath + "\\" + folder + "\\" + filenameStr;
    if (GetFileAttributesA(checkPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
      pContext->Unmap(pStaging.Get(), 0);
      return;
    }
  }

  // Check transparency
  bool isTransparent =
      IsFullyTransparent(mapped.pData, pDesc->Width, pDesc->Height,
                         mapped.RowPitch, pDesc->Format);

  // Determine folder
  std::string folder;
  if (isRT)
    folder = isTransparent ? "rendertargets\\transparent" : "rendertargets";
  else if (isTransparent)
    folder = "transparent";
  else
    folder = GetFormatFolderName(pDesc->Format);

  std::string filepath = g_dumpPath + "\\" + folder + "\\" + filenameStr;

  // Write DDS using the mapped data (mip 0 only)
  D3D11_TEXTURE2D_DESC writeDesc = *pDesc;
  writeDesc.MipLevels = 1;

  D3D11_SUBRESOURCE_DATA tempData = {};
  tempData.pSysMem = mapped.pData;
  tempData.SysMemPitch = mapped.RowPitch;

  bool isTransparentUnused = false;
  if (SaveDDSFromInitialData(&writeDesc, &tempData, filepath,
                             &isTransparentUnused)) {
    std::cout << "Dumped" << (isRT ? " RT" : "") << ": " << pDesc->Width << "x"
              << pDesc->Height << " " << filenameStr << std::endl;
  }

  pContext->Unmap(pStaging.Get(), 0);
}

void STDMETHODCALLTYPE Hooked_PSSetShaderResources(
    ID3D11DeviceContext *This, UINT StartSlot, UINT NumViews,
    ID3D11ShaderResourceView *const *ppShaderResourceViews) {
  MemoryBarrier();
  PSSetShaderResources_t pOriginal = Original_PSSetShaderResources;

  if (!pOriginal)
    return;
  if (!g_psSetSRHookReady || !ppShaderResourceViews || NumViews == 0) {
    pOriginal(This, StartSlot, NumViews, ppShaderResourceViews);
    return;
  }

  bool dumpEnabled = IsTextureDumpEnabled();
  bool replaceEnabled = IsTextureReplacementEnabled();

  if (!dumpEnabled && !replaceEnabled) {
    pOriginal(This, StartSlot, NumViews, ppShaderResourceViews);
    return;
  }

  // Prepare modified SRV array for replacement swaps (D3D11 max = 128)
  ID3D11ShaderResourceView *modSRVs[128];
  bool anyReplaced = false;
  UINT safeNumViews = (NumViews <= 128) ? NumViews : 128;

  if (replaceEnabled) {
    memcpy(modSRVs, ppShaderResourceViews,
           safeNumViews * sizeof(ID3D11ShaderResourceView *));
  }

  for (UINT i = 0; i < safeNumViews; ++i) {
    if (!ppShaderResourceViews[i])
      continue;

    ID3D11Resource *pResource = nullptr;
    ppShaderResourceViews[i]->GetResource(&pResource);
    if (!pResource)
      continue;

    ID3D11Texture2D *pTexture = nullptr;
    HRESULT hr = pResource->QueryInterface(__uuidof(ID3D11Texture2D),
                                           (void **)&pTexture);
    pResource->Release();
    if (FAILED(hr) || !pTexture)
      continue;

    bool needsProcessing = false;

    if (replaceEnabled) {
      // Fast path: check replacement cache
      InitReplacementCacheCS();
      EnterCriticalSection(&g_replacementCacheCS);
      auto cacheIt = g_replacementSRVCache.find(pTexture);
      if (cacheIt != g_replacementSRVCache.end()) {
        modSRVs[i] = cacheIt->second.Get();
        anyReplaced = true;
        LeaveCriticalSection(&g_replacementCacheCS);
        pTexture->Release();
        continue;
      }
      bool checked = g_checkedNoReplacement.count(pTexture) > 0;
      LeaveCriticalSection(&g_replacementCacheCS);

      if (!checked)
        needsProcessing = true;
    }

    if (!needsProcessing && dumpEnabled) {
      // Dump-only pointer dedup
      InitSeenTexturesCS();
      EnterCriticalSection(&g_seenTexturesCS);
      bool seen = g_seenTexturePointers.count(pTexture) > 0;
      if (!seen)
        g_seenTexturePointers.insert(pTexture);
      LeaveCriticalSection(&g_seenTexturesCS);

      if (!seen)
        needsProcessing = true;
    }

    if (!needsProcessing) {
      pTexture->Release();
      continue;
    }

    // First encounter - stage, hash, dump, and/or replace
    D3D11_TEXTURE2D_DESC desc;
    pTexture->GetDesc(&desc);

    if (desc.Width < 4 || desc.Height < 4 || !IsDumpableFormat(desc.Format)) {
      if (replaceEnabled) {
        InitReplacementCacheCS();
        EnterCriticalSection(&g_replacementCacheCS);
        g_checkedNoReplacement.insert(pTexture);
        LeaveCriticalSection(&g_replacementCacheCS);
      }
      pTexture->Release();
      continue;
    }

    try {
      ComPtr<ID3D11Device> pDevice;
      pTexture->GetDevice(&pDevice);
      if (!pDevice) {
        pTexture->Release();
        continue;
      }

      // Create staging texture (shared for dump + replace)
      D3D11_TEXTURE2D_DESC stagingDesc = desc;
      stagingDesc.Usage = D3D11_USAGE_STAGING;
      stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      stagingDesc.BindFlags = 0;
      stagingDesc.MiscFlags = 0;
      stagingDesc.MipLevels = 1;

      ComPtr<ID3D11Texture2D> pStaging;
      hr = pDevice->CreateTexture2D(&stagingDesc, nullptr, &pStaging);
      if (FAILED(hr) || !pStaging) {
        if (replaceEnabled) {
          InitReplacementCacheCS();
          EnterCriticalSection(&g_replacementCacheCS);
          g_checkedNoReplacement.insert(pTexture);
          LeaveCriticalSection(&g_replacementCacheCS);
        }
        pTexture->Release();
        continue;
      }

      This->CopySubresourceRegion(pStaging.Get(), 0, 0, 0, 0, pTexture, 0,
                                  nullptr);
      This->Flush();

      D3D11_MAPPED_SUBRESOURCE mapped = {};
      hr = This->Map(pStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
      if (FAILED(hr) || !mapped.pData) {
        if (replaceEnabled) {
          InitReplacementCacheCS();
          EnterCriticalSection(&g_replacementCacheCS);
          g_checkedNoReplacement.insert(pTexture);
          LeaveCriticalSection(&g_replacementCacheCS);
        }
        pTexture->Release();
        continue;
      }

      // Content hash from GPU data
      uint64_t hash = HashTextureData(&desc, mapped.pData, mapped.RowPitch);

      // Dump if enabled
      if (dumpEnabled) {
        InitDumpCS();
        EnterCriticalSection(&g_dumpCS);
        bool alreadyDumped = g_recentDumps.count(hash) > 0;
        if (!alreadyDumped) {
          if (g_recentDumps.size() > 10000)
            g_recentDumps.clear();
          g_recentDumps.insert(hash);
        }
        LeaveCriticalSection(&g_dumpCS);

        if (!alreadyDumped) {
          std::ostringstream filename;
          filename << std::dec << desc.Width << "x" << desc.Height << "_"
                   << std::hex << std::setfill('0') << std::setw(16) << hash
                   << ".dds";
          std::string filenameStr = filename.str();

          // Check if already on disk
          bool onDisk = false;
          std::string foldersToCheck[] = {
              "BGRA4",    "BGRA8", "RGBA8",       "BC1_DXT1",      "BC2_DXT3",
              "BC3_DXT5", "BC7",   "transparent", "rendertargets", "other"};
          for (const auto &folder : foldersToCheck) {
            std::string checkPath =
                g_dumpPath + "\\" + folder + "\\" + filenameStr;
            if (GetFileAttributesA(checkPath.c_str()) !=
                INVALID_FILE_ATTRIBUTES) {
              onDisk = true;
              break;
            }
          }

          if (!onDisk) {
            bool isRT = (desc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
            bool isTransparent =
                IsFullyTransparent(mapped.pData, desc.Width, desc.Height,
                                   mapped.RowPitch, desc.Format);

            std::string folder;
            if (isRT)
              folder = isTransparent ? "rendertargets\\transparent"
                                     : "rendertargets";
            else if (isTransparent)
              folder = "transparent";
            else
              folder = GetFormatFolderName(desc.Format);

            std::string filepath =
                g_dumpPath + "\\" + folder + "\\" + filenameStr;

            D3D11_TEXTURE2D_DESC writeDesc = desc;
            writeDesc.MipLevels = 1;

            D3D11_SUBRESOURCE_DATA tempData = {};
            tempData.pSysMem = mapped.pData;
            tempData.SysMemPitch = mapped.RowPitch;

            bool unused = false;
            if (SaveDDSFromInitialData(&writeDesc, &tempData, filepath,
                                       &unused)) {
              std::cout << "Dumped" << (isRT ? " RT" : "") << ": " << desc.Width
                        << "x" << desc.Height << " " << filenameStr
                        << std::endl;
            }
          }
        }
      }

      // Try replacement if enabled
      if (replaceEnabled) {
        ComPtr<ID3D11ShaderResourceView> pReplaceSRV;
        if (LoadReplacementSRV(pDevice.Get(), &desc, hash,
                               pReplaceSRV.GetAddressOf())) {
          InitReplacementCacheCS();
          EnterCriticalSection(&g_replacementCacheCS);
          g_replacementSRVCache[pTexture] = pReplaceSRV;
          LeaveCriticalSection(&g_replacementCacheCS);
          modSRVs[i] = pReplaceSRV.Get();
          anyReplaced = true;
        } else {
          InitReplacementCacheCS();
          EnterCriticalSection(&g_replacementCacheCS);
          g_checkedNoReplacement.insert(pTexture);
          LeaveCriticalSection(&g_replacementCacheCS);
        }
      }

      This->Unmap(pStaging.Get(), 0);
    } catch (...) {
      if (replaceEnabled) {
        InitReplacementCacheCS();
        EnterCriticalSection(&g_replacementCacheCS);
        g_checkedNoReplacement.insert(pTexture);
        LeaveCriticalSection(&g_replacementCacheCS);
      }
    }

    pTexture->Release();
  }

  if (replaceEnabled && anyReplaced) {
    pOriginal(This, StartSlot, safeNumViews, modSRVs);
  } else {
    pOriginal(This, StartSlot, NumViews, ppShaderResourceViews);
  }
}
} // namespace

void ApplyTextureDumpHooks(ID3D11Device *pDevice,
                           ID3D11DeviceContext *pContext) {
  if (!pDevice || !pContext)
    return;
  if (InterlockedCompareExchange(&g_dumpHooksApplied, 1, 0) != 0)
    return;

  Settings settings;
  settings.Load(Settings::GetSettingsPath());

  bool dumpEnabled = settings.GetBool("texture_dump_enabled", false);
  bool replaceEnabled = settings.GetBool("texture_replace_enabled", false);

  if (!dumpEnabled && !replaceEnabled)
    return;

  void **contextVtable = *(void ***)pContext;
  if (!contextVtable)
    return;

  // PSSetShaderResources = context vtable index 8
  InstallVtableHook(contextVtable, 50, 8, (void *)Hooked_PSSetShaderResources,
                    (volatile void **)&Original_PSSetShaderResources,
                    &g_psSetSRHookReady);
  Sleep(1);

  std::cout << "Texture dump/replace hooks installed (PSSetShaderResources)"
            << std::endl;
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
  if (g_resizeHashesInitialized)
    return;
  g_resizeHashesInitialized = true;

  // Example: texture_4f83cf6716f5f0c5_120x120_BGRA8.dds
  // This hash is from the filename - you can add more hashes here

  g_resizeHashes.insert(0x30c9ba509d8378f8ULL); // Main Logo (1280x960)
  // g_resizeSizes.insert({ 1024, 1024 });        // Key Items
  // g_resizeSizes.insert({ 512, 512 });        // Key Items
  // g_resizeSizes.insert({ 256, 1792 });        // Menu portraits (post battle)
  // g_resizeSizes.insert({ 1164, 1166 });        // Menu compass
  // g_resizeSizes.insert({ 405, 58 });        // Menu compass arrow
  // g_resizeSizes.insert({ 250, 45 });        // Battle hit chance anchor
  // g_resizeSizes.insert({ 128, 128 }); // Menu mini avatar
  // g_resizeSizes.insert({ 92, 44 });        // menu arrows

  // g_resizeSizes.insert({ 480, 64 }); // Battle element name container
  // g_resizeSizes.insert({ 736, 96 }); // Battle element name menu?
  // g_resizeSizes.insert({ 64, 32 }); // Battle element mini

  // g_resizeSizes.insert({ 480, 64 });
  // g_resizeSizes.insert({ 256, 192 });
  // g_resizeSizes.insert({ 208, 112 });
  // g_resizeSizes.insert({ 608, 224 });
  // g_resizeSizes.insert({ 384, 192 });
  // g_resizeSizes.insert({ 512, 64 });
  // g_resizeSizes.insert({ 800, 88 });

  std::cout << "Initialized texture resize list with " << g_resizeHashes.size()
            << " hashes and " << g_resizeSizes.size() << " sizes" << std::endl;
}
} // namespace

// Add a hash to the resize list
void AddResizeHash(uint64_t hash) {
  InitializeResizeHashes();
  g_resizeHashes.insert(hash);
  std::cout << "Added hash 0x" << std::hex << hash << std::dec
            << " to resize list" << std::endl;
}

// Clear all resize hashes
void ClearResizeHashes() {
  g_resizeHashes.clear();
  g_resizeHashesInitialized = false;
  std::cout << "Cleared texture resize hash list" << std::endl;
}

// Check if a texture should be resized based on its hash
bool ShouldResizeTexture(const D3D11_TEXTURE2D_DESC *pDesc,
                         const D3D11_SUBRESOURCE_DATA *pInitialData) {
  if (!pDesc)
    return false;

  // Validate initial data
  if (!pInitialData || !pInitialData->pSysMem ||
      pInitialData->SysMemPitch == 0) {
    return false;
  }

  // Validate texture dimensions
  if (pDesc->Width == 0 || pDesc->Height == 0 || pDesc->Width > 16384 ||
      pDesc->Height > 16384) {
    return false;
  }

  try {
    InitializeResizeHashes();

    // 1. Check if this size is explicitly whitelisted
    if (g_resizeSizes.find({pDesc->Width, pDesc->Height}) !=
        g_resizeSizes.end()) {
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
void ResizeTexture2D(const D3D11_TEXTURE2D_DESC *pDesc,
                     D3D11_TEXTURE2D_DESC *pNewDesc) {
  if (!pDesc || !pNewDesc)
    return;

  // Just copy the description - we're not changing dimensions
  *pNewDesc = *pDesc;

  // Note: Actual pixel manipulation would need to happen in the hook
  // This function just prepares the description
  // The real work is done in the CreateTexture2D hook by modifying pInitialData
}
