#include "textureresize.h"
#include "../utils/settings.h"
#include "texturedump.h"
#include "widescreen.h"
#include <Windows.h>
#include <algorithm>
#include <vector>

namespace {
typedef HRESULT(STDMETHODCALLTYPE *CreateTexture2D_t)(
    ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
    const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);

// Win32 primitives for thread-safe access (avoids C++ runtime initialization
// issues)
volatile CreateTexture2D_t Original_CreateTexture2D_Resize = nullptr;
volatile LONG g_resizeHooksApplied = 0;
volatile LONG g_resizeHooksReady = 0;

CRITICAL_SECTION g_resizeHookInstallCS;
volatile LONG g_resizeCSInitialized = 0;

void InitResizeCS() {
  if (InterlockedCompareExchange(&g_resizeCSInitialized, 1, 0) == 0) {
    InitializeCriticalSection(&g_resizeHookInstallCS);
  }
}

// Static buffer pool
CRITICAL_SECTION g_bufferPoolCS;
volatile LONG g_bufferCSInitialized = 0;
std::vector<uint8_t> *g_bufferPool[4] = {nullptr, nullptr, nullptr, nullptr};
volatile LONG g_currentBufferIndex = 0;
const size_t MAX_BUFFERS = 4;

void InitBufferCS() {
  if (InterlockedCompareExchange(&g_bufferCSInitialized, 1, 0) == 0) {
    InitializeCriticalSection(&g_bufferPoolCS);
  }
}

std::vector<uint8_t> *GetBuffer() {
  InitBufferCS();
  EnterCriticalSection(&g_bufferPoolCS);

  if (g_bufferPool[0] == nullptr) {
    for (size_t i = 0; i < MAX_BUFFERS; ++i) {
      g_bufferPool[i] = new std::vector<uint8_t>();
    }
  }

  LONG idx = InterlockedIncrement(&g_currentBufferIndex) % MAX_BUFFERS;
  auto *buffer = g_bufferPool[idx];

  LeaveCriticalSection(&g_bufferPoolCS);
  return buffer;
}
} // namespace

bool IsBlockCompressedFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    return true;
  default:
    return false;
  }
}

UINT GetBytesPerPixel(DXGI_FORMAT format) {
  // Block-compressed formats can't be resized with pixel interpolation
  if (IsBlockCompressedFormat(format)) {
    return 0; // Signal that this format is not supported for resizing
  }

  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    return 4;
  case DXGI_FORMAT_B4G4R4A4_UNORM:
    return 2;
  default:
    return 4;
  }
}

void CreateCenterPaddedData(const D3D11_TEXTURE2D_DESC *pDesc,
                            const D3D11_SUBRESOURCE_DATA *pInitialData,
                            std::vector<uint8_t> &buffer) {
  if (!pDesc || !pInitialData || !pInitialData->pSysMem ||
      pInitialData->SysMemPitch == 0)
    return;
  if (pDesc->Width == 0 || pDesc->Height == 0)
    return;

  // Skip block-compressed formats - they can't be resized with pixel
  // interpolation
  UINT bytesPerPixel = GetBytesPerPixel(pDesc->Format);
  if (bytesPerPixel == 0)
    return;

  try {
    float widescreenRatio = GetCurrentWidescreenRatio();

    // Get number of pieces this texture is split into
    UINT numPieces = GetTextureSplitPieces(pDesc);
    if (numPieces == 0)
      numPieces = 2; // Default to 2 pieces for backward compatibility

    // Each piece width (e.g., 512/2=256 for 2 pieces, 1280/5=256 for 5 pieces)
    UINT pieceWidth = pDesc->Width / numPieces;

    // Calculate the scaled content width for each piece
    UINT pieceContentWidth = static_cast<UINT>(pieceWidth * widescreenRatio);
    if (pieceContentWidth < 1)
      pieceContentWidth = 1;
    if (pieceContentWidth > pieceWidth)
      pieceContentWidth = pieceWidth;

    // Padding for each piece (distributed evenly on both sides)
    UINT piecePadding = (pieceWidth - pieceContentWidth) / 2;

    UINT rowPitch = pDesc->Width * bytesPerPixel;

    buffer.resize(rowPitch * pDesc->Height);
    std::fill(buffer.begin(), buffer.end(), 0);

    if (pInitialData && pInitialData->pSysMem &&
        pInitialData->SysMemPitch > 0) {
      const uint8_t *srcData =
          static_cast<const uint8_t *>(pInitialData->pSysMem);
      size_t srcBufferSize = pInitialData->SysMemPitch * pDesc->Height;

      // Process each piece
      for (UINT piece = 0; piece < numPieces; ++piece) {
        UINT pieceStartX = piece * pieceWidth; // Source start position
        UINT dstStartX =
            pieceStartX +
            piecePadding; // Destination start position with padding

        for (UINT y = 0; y < pDesc->Height; ++y) {
          for (UINT x = 0; x < pieceContentWidth; ++x) {
            float srcXf = (float)x / widescreenRatio;
            UINT srcX0 = pieceStartX + static_cast<UINT>(srcXf);
            UINT srcX1 = srcX0 + 1;

            // Clamp to piece boundaries
            UINT pieceEndX = pieceStartX + pieceWidth;
            if (srcX0 >= pieceEndX)
              srcX0 = pieceEndX - 1;
            if (srcX1 >= pieceEndX)
              srcX1 = pieceEndX - 1;

            float fracX = srcXf - static_cast<UINT>(srcXf);

            // Position in output with padding offset
            UINT dstOffset = y * rowPitch + (dstStartX + x) * bytesPerPixel;
            if (dstOffset + bytesPerPixel > buffer.size())
              continue;

            if (pDesc->Format == DXGI_FORMAT_B4G4R4A4_UNORM) {
              UINT srcOffset0 =
                  y * pInitialData->SysMemPitch + srcX0 * bytesPerPixel;
              UINT srcOffset1 =
                  y * pInitialData->SysMemPitch + srcX1 * bytesPerPixel;

              if (srcOffset0 + 1 >= srcBufferSize ||
                  srcOffset1 + 1 >= srcBufferSize) {
                buffer[dstOffset] = 0;
                buffer[dstOffset + 1] = 0;
                continue;
              }

              uint16_t pixel0 =
                  *reinterpret_cast<const uint16_t *>(srcData + srcOffset0);
              uint16_t pixel1 =
                  *reinterpret_cast<const uint16_t *>(srcData + srcOffset1);

              uint8_t b0 = pixel0 & 0xF, b1 = pixel1 & 0xF;
              uint8_t g0 = (pixel0 >> 4) & 0xF, g1 = (pixel1 >> 4) & 0xF;
              uint8_t r0 = (pixel0 >> 8) & 0xF, r1 = (pixel1 >> 8) & 0xF;
              uint8_t a0 = (pixel0 >> 12) & 0xF, a1 = (pixel1 >> 12) & 0xF;

              uint8_t b =
                  static_cast<uint8_t>(b0 * (1.0f - fracX) + b1 * fracX + 0.5f);
              uint8_t g =
                  static_cast<uint8_t>(g0 * (1.0f - fracX) + g1 * fracX + 0.5f);
              uint8_t r =
                  static_cast<uint8_t>(r0 * (1.0f - fracX) + r1 * fracX + 0.5f);
              uint8_t a =
                  static_cast<uint8_t>(a0 * (1.0f - fracX) + a1 * fracX + 0.5f);

              uint16_t result = (b & 0xF) | ((g & 0xF) << 4) |
                                ((r & 0xF) << 8) | ((a & 0xF) << 12);
              *reinterpret_cast<uint16_t *>(buffer.data() + dstOffset) = result;
            } else {
              for (UINT b = 0; b < bytesPerPixel; ++b) {
                UINT srcOffset0 =
                    y * pInitialData->SysMemPitch + srcX0 * bytesPerPixel + b;
                UINT srcOffset1 =
                    y * pInitialData->SysMemPitch + srcX1 * bytesPerPixel + b;

                if (srcOffset0 >= srcBufferSize ||
                    srcOffset1 >= srcBufferSize) {
                  buffer[dstOffset + b] = 0;
                  continue;
                }

                float val0 = srcData[srcOffset0];
                float val1 = srcData[srcOffset1];
                float result = val0 * (1.0f - fracX) + val1 * fracX;
                buffer[dstOffset + b] = static_cast<uint8_t>(result + 0.5f);
              }
            }
          }
        }
      }
    }
  } catch (...) {
    buffer.clear();
  }
}

void CreatePillarboxedData(const D3D11_TEXTURE2D_DESC *pDesc,
                           const D3D11_SUBRESOURCE_DATA *pInitialData,
                           std::vector<uint8_t> &buffer) {
  if (!pDesc || !pInitialData || !pInitialData->pSysMem ||
      pInitialData->SysMemPitch == 0)
    return;
  if (pDesc->Width == 0 || pDesc->Height == 0)
    return;

  // Check if this texture needs center padding (for split left/right textures)
  if (ShouldCenterPadTexture(pDesc)) {
    CreateCenterPaddedData(pDesc, pInitialData, buffer);
    return;
  }

  // Skip block-compressed formats - they can't be resized with pixel
  // interpolation
  UINT bytesPerPixel = GetBytesPerPixel(pDesc->Format);
  if (bytesPerPixel == 0)
    return;

  try {
    float widescreenRatio = GetCurrentWidescreenRatio();
    UINT contentWidth = static_cast<UINT>(pDesc->Width * widescreenRatio);
    if (contentWidth < 1)
      contentWidth = 1;
    if (contentWidth > pDesc->Width)
      contentWidth = pDesc->Width;

    UINT padding = (pDesc->Width - contentWidth) / 2;
    UINT rowPitch = pDesc->Width * bytesPerPixel;

    buffer.resize(rowPitch * pDesc->Height);
    std::fill(buffer.begin(), buffer.end(), 0);

    if (pInitialData && pInitialData->pSysMem &&
        pInitialData->SysMemPitch > 0) {
      const uint8_t *srcData =
          static_cast<const uint8_t *>(pInitialData->pSysMem);
      size_t srcBufferSize = pInitialData->SysMemPitch * pDesc->Height;

      for (UINT y = 0; y < pDesc->Height; ++y) {
        for (UINT x = 0; x < contentWidth; ++x) {
          float srcXf = (float)x / widescreenRatio;
          UINT srcX0 = static_cast<UINT>(srcXf);
          if (srcX0 >= pDesc->Width)
            srcX0 = pDesc->Width - 1;
          UINT srcX1 =
              (srcX0 + 1 < pDesc->Width - 1) ? srcX0 + 1 : pDesc->Width - 1;
          float fracX = srcXf - srcX0;

          UINT dstOffset = y * rowPitch + (padding + x) * bytesPerPixel;
          if (dstOffset + bytesPerPixel > buffer.size())
            continue;

          if (pDesc->Format == DXGI_FORMAT_B4G4R4A4_UNORM) {
            UINT srcOffset0 =
                y * pInitialData->SysMemPitch + srcX0 * bytesPerPixel;
            UINT srcOffset1 =
                y * pInitialData->SysMemPitch + srcX1 * bytesPerPixel;

            if (srcOffset0 + 1 >= srcBufferSize ||
                srcOffset1 + 1 >= srcBufferSize) {
              buffer[dstOffset] = 0;
              buffer[dstOffset + 1] = 0;
              continue;
            }

            uint16_t pixel0 =
                *reinterpret_cast<const uint16_t *>(srcData + srcOffset0);
            uint16_t pixel1 =
                *reinterpret_cast<const uint16_t *>(srcData + srcOffset1);

            uint8_t b0 = pixel0 & 0xF, b1 = pixel1 & 0xF;
            uint8_t g0 = (pixel0 >> 4) & 0xF, g1 = (pixel1 >> 4) & 0xF;
            uint8_t r0 = (pixel0 >> 8) & 0xF, r1 = (pixel1 >> 8) & 0xF;
            uint8_t a0 = (pixel0 >> 12) & 0xF, a1 = (pixel1 >> 12) & 0xF;

            uint8_t b =
                static_cast<uint8_t>(b0 * (1.0f - fracX) + b1 * fracX + 0.5f);
            uint8_t g =
                static_cast<uint8_t>(g0 * (1.0f - fracX) + g1 * fracX + 0.5f);
            uint8_t r =
                static_cast<uint8_t>(r0 * (1.0f - fracX) + r1 * fracX + 0.5f);
            uint8_t a =
                static_cast<uint8_t>(a0 * (1.0f - fracX) + a1 * fracX + 0.5f);

            uint16_t result = (b & 0xF) | ((g & 0xF) << 4) | ((r & 0xF) << 8) |
                              ((a & 0xF) << 12);
            *reinterpret_cast<uint16_t *>(buffer.data() + dstOffset) = result;
          } else {
            for (UINT b = 0; b < bytesPerPixel; ++b) {
              UINT srcOffset0 =
                  y * pInitialData->SysMemPitch + srcX0 * bytesPerPixel + b;
              UINT srcOffset1 =
                  y * pInitialData->SysMemPitch + srcX1 * bytesPerPixel + b;

              if (srcOffset0 >= srcBufferSize || srcOffset1 >= srcBufferSize) {
                buffer[dstOffset + b] = 0;
                continue;
              }

              float val0 = srcData[srcOffset0];
              float val1 = srcData[srcOffset1];
              float result = val0 * (1.0f - fracX) + val1 * fracX;
              buffer[dstOffset + b] = static_cast<uint8_t>(result + 0.5f);
            }
          }
        }
      }
    }
  } catch (...) {
    buffer.clear();
  }
}

HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D_Resize(
    ID3D11Device *This, const D3D11_TEXTURE2D_DESC *pDesc,
    const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D) {
  MemoryBarrier();
  CreateTexture2D_t pOriginal = Original_CreateTexture2D_Resize;

  if (!pOriginal)
    return E_FAIL;
  if (!g_resizeHooksReady)
    return pOriginal(This, pDesc, pInitialData, ppTexture2D);

  HRESULT hr;

  if (pDesc && pInitialData && ShouldResizeTexture(pDesc, pInitialData)) {
    float widescreenRatio = GetCurrentWidescreenRatio();

    if (widescreenRatio >= 0.99f && widescreenRatio <= 1.01f) {
      hr = pOriginal(This, pDesc, pInitialData, ppTexture2D);
    } else if (pDesc->Usage != D3D11_USAGE_DEFAULT ||
               pDesc->CPUAccessFlags != 0) {
      hr = pOriginal(This, pDesc, pInitialData, ppTexture2D);
    } else {
      try {
        std::vector<uint8_t> *buffer = GetBuffer();
        CreatePillarboxedData(pDesc, pInitialData, *buffer);

        if (buffer->empty()) {
          hr = pOriginal(This, pDesc, pInitialData, ppTexture2D);
        } else {
          D3D11_SUBRESOURCE_DATA newInitialData;
          newInitialData.pSysMem = buffer->data();
          newInitialData.SysMemPitch =
              pDesc->Width * GetBytesPerPixel(pDesc->Format);
          newInitialData.SysMemSlicePitch = 0;

          hr = pOriginal(This, pDesc, &newInitialData, ppTexture2D);

          if (SUCCEEDED(hr)) {
            ID3D11DeviceContext *pContext = nullptr;
            This->GetImmediateContext(&pContext);
            if (pContext) {
              pContext->Flush();
              pContext->Release();
            }
          }

          if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D) {
            ID3D11DeviceContext *pContext = nullptr;
            This->GetImmediateContext(&pContext);
            if (pContext) {
              DumpTexture2D(This, pContext, *ppTexture2D, pDesc,
                            &newInitialData);
              pContext->Release();
            }
          }
        }
      } catch (...) {
        hr = pOriginal(This, pDesc, pInitialData, ppTexture2D);
      }
    }
  } else {
    hr = pOriginal(This, pDesc, pInitialData, ppTexture2D);

    if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D && pDesc) {
      ID3D11DeviceContext *pContext = nullptr;
      This->GetImmediateContext(&pContext);
      if (pContext) {
        DumpTexture2D(This, pContext, *ppTexture2D, pDesc, pInitialData);
        pContext->Release();
      }
    }
  }

  return hr;
}

void ApplyTextureResizeHooks(ID3D11Device *pDevice) {
  if (!pDevice)
    return;
  if (InterlockedCompareExchange(&g_resizeHooksApplied, 1, 0) != 0)
    return;

  InitResizeCS();
  EnterCriticalSection(&g_resizeHookInstallCS);

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

    bool widescreenEnabled = settings.GetBool("widescreen_enabled", true);
    bool textureDumpEnabled = settings.GetBool("texture_dump_enabled", false);
    bool enabled = widescreenEnabled && !textureDumpEnabled;

    if (!enabled) {
      LeaveCriticalSection(&g_resizeHookInstallCS);
      return;
    }

    void **deviceVtable = *(void ***)pDevice;
    if (!deviceVtable || !deviceVtable[5]) {
      LeaveCriticalSection(&g_resizeHookInstallCS);
      return;
    }

    DWORD oldProtect;
    if (VirtualProtect(deviceVtable, sizeof(void *) * 10,
                       PAGE_EXECUTE_READWRITE, &oldProtect)) {
      if (deviceVtable[5] != (void *)Hooked_CreateTexture2D_Resize) {
        Original_CreateTexture2D_Resize = (CreateTexture2D_t)deviceVtable[5];
        MemoryBarrier();
        deviceVtable[5] = (void *)Hooked_CreateTexture2D_Resize;
        FlushInstructionCache(GetCurrentProcess(), deviceVtable,
                              sizeof(void *) * 10);
        MemoryBarrier();
      }
      VirtualProtect(deviceVtable, sizeof(void *) * 10, oldProtect,
                     &oldProtect);
      InterlockedExchange(&g_resizeHooksReady, 1);
      Sleep(1);
    }
    LeaveCriticalSection(&g_resizeHookInstallCS);
  } catch (...) {
    LeaveCriticalSection(&g_resizeHookInstallCS);
  }
}
