// Upscale Patch - Based on SpecialK's implementation

#define NOMINMAX
#include "upscale4k.h"
#include "../utils/memory.h"
#include "../utils/settings.h"
#include "../utils/viewport_utils.h"
#include "texturereplace.h"
#include "widescreen.h"
#include <Windows.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

// Upscale state (accessible via getters for cross-module coordination)
static bool g_upscaleActive = false;

// Signaled once RunFirstTimeSetup() completes (or finds setup already done).
// Initially unsignaled so ApplyUpscale4KPatch waits for MainThread to finish.
static HANDLE g_setupCompleteEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

namespace {
constexpr float BaseWidth = 4096.0f;
constexpr float BaseHeight = 2048.0f;

static float g_ResMultiplier = 4.0f;
static float g_NewWidth = BaseWidth * 4.0f;
static float g_NewHeight = BaseHeight * 4.0f;

typedef void(STDMETHODCALLTYPE *RSSetViewports_t)(ID3D11DeviceContext *, UINT,
                                                  const D3D11_VIEWPORT *);
typedef void(STDMETHODCALLTYPE *CopySubresourceRegion_t)(
    ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT,
    ID3D11Resource *, UINT, const D3D11_BOX *, BOOL);
typedef HRESULT(STDMETHODCALLTYPE *CreateTexture2D_t)(
    ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
    const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);

// Win32 primitives for thread-safe access (avoids C++ runtime initialization
// issues)
volatile RSSetViewports_t Original_RSSetViewports = nullptr;
volatile CopySubresourceRegion_t Original_CopySubresourceRegion = nullptr;
volatile CreateTexture2D_t Original_CreateTexture2D = nullptr;

volatile LONG g_viewportsHookReady = 0;
volatile LONG g_copyHookReady = 0;
volatile LONG g_createTextureHookReady = 0;

inline bool IsColorFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
         format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
         format == DXGI_FORMAT_R32G32B32A32_FLOAT;
}
} // namespace

void STDMETHODCALLTYPE Hooked_RSSetViewports(ID3D11DeviceContext *This,
                                             UINT NumViewports,
                                             const D3D11_VIEWPORT *pViewports) {
  MemoryBarrier();
  RSSetViewports_t pOriginal = Original_RSSetViewports;

  if (!pOriginal)
    return;
  if (!pViewports || NumViewports == 0 || !g_viewportsHookReady) {
    pOriginal(This, NumViewports, pViewports);
    return;
  }

  // Allocate a small buffer on stack for modified viewports
  D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  UINT count = ViewportUtils::CopyViewportsToBuffer(
      vps, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, pViewports,
      NumViewports);

  // Apply viewport widescreen fix FIRST (before upscaling)
  // Only apply if we're actually in widescreen mode
  // Use tolerance to account for floating-point precision
  // Ratio should be < 1.0 for widescreen (e.g., 0.75 for 16:9)
  float ratio = GetCurrentWidescreenRatio();
  const float WIDESCREEN_THRESHOLD = 0.99f;
  if (ratio < WIDESCREEN_THRESHOLD) {
    ViewportUtils::ApplyViewportWidescreenFix(vps, count, ratio);
  }

  // Check render target once per call
  bool isUpscaledTarget = false;
  ID3D11RenderTargetView *rtv = nullptr;
  This->OMGetRenderTargets(1, &rtv, nullptr);
  if (rtv) {
    ID3D11Resource *pRes = nullptr;
    rtv->GetResource(&pRes);
    if (pRes) {
      ID3D11Texture2D *pTex = nullptr;
      pRes->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&pTex);
      if (pTex) {
        D3D11_TEXTURE2D_DESC texDesc;
        pTex->GetDesc(&texDesc);
        if (texDesc.Width == (UINT)g_NewWidth &&
            texDesc.Height == (UINT)g_NewHeight) {
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

void STDMETHODCALLTYPE Hooked_CopySubresourceRegion(
    ID3D11DeviceContext *This, ID3D11Resource *pDstResource,
    UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ,
    ID3D11Resource *pSrcResource, UINT SrcSubresource, const D3D11_BOX *pSrcBox,
    BOOL bWrapped) {
  MemoryBarrier();
  CopySubresourceRegion_t pOriginal = Original_CopySubresourceRegion;

  if (!pOriginal)
    return;
  if (!g_copyHookReady) {
    pOriginal(This, pDstResource, DstSubresource, DstX, DstY, DstZ,
              pSrcResource, SrcSubresource, pSrcBox, bWrapped);
    return;
  }

  D3D11_BOX newBox = {};
  const D3D11_BOX *pActualSrcBox = pSrcBox;
  UINT ActualDstX = DstX;
  UINT ActualDstY = DstY;

  if (pSrcResource && pDstResource) {
    ID3D11Texture2D *pSrcTex = nullptr;
    ID3D11Texture2D *pDstTex = nullptr;

    pSrcResource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&pSrcTex);
    pDstResource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&pDstTex);

    if (pSrcTex && pDstTex) {
      D3D11_TEXTURE2D_DESC srcDesc = {};
      D3D11_TEXTURE2D_DESC dstDesc = {};
      pSrcTex->GetDesc(&srcDesc);
      pDstTex->GetDesc(&dstDesc);

      if (srcDesc.Width == (UINT)g_NewWidth &&
          srcDesc.Height == (UINT)g_NewHeight && pSrcBox &&
          dstDesc.Width == (UINT)g_NewWidth &&
          dstDesc.Height == (UINT)g_NewHeight) {

        newBox = *pSrcBox;

        float HalfWidth = g_NewWidth / 2.0f;
        float HalfHeight = g_NewHeight / 2.0f;

        float left_ndc =
            2.0f *
                (static_cast<float>(std::clamp((UINT)newBox.left, 0U, 4096U)) /
                 4096.0f) -
            1.0f;
        float top_ndc =
            2.0f *
                (static_cast<float>(std::clamp((UINT)newBox.top, 0U, 2048U)) /
                 2048.0f) -
            1.0f;
        float right_ndc =
            2.0f *
                (static_cast<float>(std::clamp((UINT)newBox.right, 0U, 4096U)) /
                 4096.0f) -
            1.0f;
        float bottom_ndc = 2.0f * (static_cast<float>(std::clamp(
                                       (UINT)newBox.bottom, 0U, 2048U)) /
                                   2048.0f) -
                           1.0f;

        newBox.left =
            static_cast<UINT>(std::max(0.0f, left_ndc * HalfWidth + HalfWidth));
        newBox.top = static_cast<UINT>(
            std::max(0.0f, top_ndc * HalfHeight + HalfHeight));
        newBox.right = static_cast<UINT>(
            std::max(0.0f, right_ndc * HalfWidth + HalfWidth));
        newBox.bottom = static_cast<UINT>(
            std::max(0.0f, bottom_ndc * HalfHeight + HalfHeight));

        ActualDstX = DstX * (UINT)g_ResMultiplier;
        ActualDstY = DstY * (UINT)g_ResMultiplier;

        pActualSrcBox = &newBox;
      } else if (pSrcBox && (pSrcBox->right > srcDesc.Width ||
                             pSrcBox->bottom > srcDesc.Height)) {
        newBox = *pSrcBox;
        newBox.right = std::min(srcDesc.Width, newBox.right);
        newBox.bottom = std::min(srcDesc.Height, newBox.bottom);
        pActualSrcBox = &newBox;
      }
    }

    if (pSrcTex)
      pSrcTex->Release();
    if (pDstTex)
      pDstTex->Release();
  }

  pOriginal(This, pDstResource, DstSubresource, ActualDstX, ActualDstY, DstZ,
            pSrcResource, SrcSubresource, pActualSrcBox, bWrapped);
}

// Re-entrancy guard: internal CreateTexture2D calls (replacement loading,
// staging texture creation) must bypass the hook to avoid recursive processing
static thread_local bool g_inCreateTexture2D = false;

HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D(
    ID3D11Device *This, const D3D11_TEXTURE2D_DESC *pDesc,
    const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D) {
  MemoryBarrier();
  CreateTexture2D_t pOriginal = Original_CreateTexture2D;

  if (!pOriginal)
    return E_FAIL;
  if (!g_createTextureHookReady || g_inCreateTexture2D)
    return pOriginal(This, pDesc, pInitialData, ppTexture2D);

  g_inCreateTexture2D = true;

  HRESULT hr;
  bool isUpscaleTarget = pDesc && pDesc->Width == (UINT)BaseWidth &&
                         pDesc->Height == (UINT)BaseHeight &&
                         (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) &&
                         IsColorFormat(pDesc->Format) && !pInitialData;

  if (isUpscaleTarget) {
    // Render target at base resolution: upscale it. No replacement for RTs
    // (they are blank framebuffers at creation time).
    D3D11_TEXTURE2D_DESC newDesc = *pDesc;
    newDesc.Width = (UINT)g_NewWidth;
    newDesc.Height = (UINT)g_NewHeight;
    hr = pOriginal(This, &newDesc, pInitialData, ppTexture2D);
    // Don't dump upscaled RTs - they have no content yet
  } else {
    // Non-render-target: try replacement first, then original creation
    if (pDesc && ppTexture2D &&
        TryLoadReplacementTexture(This, pDesc, pInitialData, ppTexture2D)) {
      g_inCreateTexture2D = false;
      return S_OK;
    }

    hr = pOriginal(This, pDesc, pInitialData, ppTexture2D);
    // No dump here: this hook is only active when upscale is enabled, and
    // dumping is suppressed when upscale or replacement is active.
  }

  g_inCreateTexture2D = false;
  return hr;
}

void RunFirstTimeSetup() {
  std::string settingsPath = Settings::GetSettingsPath();

  Settings settings;
  settings.Load(settingsPath);

  if (settings.GetBool("upscale_setup_completed", false)) {
    SetEvent(g_setupCompleteEvent);
    return;
  }

  // Under Wine the upscaler doesn't work; skip interactive setup and set scale
  // 1
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll && GetProcAddress(ntdll, "wine_get_version")) {
    settings.UpdateFile(settingsPath, "upscale_scale", "1");
    settings.UpdateFile(settingsPath, "upscale_setup_completed", "1");
    SetEvent(g_setupCompleteEvent);
    return;
  }

  std::cout << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "       CrossFix - First Run Setup       " << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << std::endl;
  std::cout << "Texture upscale (EXPERIMENTAL - may cause crashes/glitches):"
            << std::endl;
  std::cout << "  1 - Off (no upscaling)" << std::endl;
  std::cout << "  2 - 2x - Fastest, lower quality" << std::endl;
  std::cout << "  3 - 3x - Balanced" << std::endl;
  std::cout << "  4 - 4x - Best quality, most demanding" << std::endl;
  std::cout << std::endl;
  std::cout << "You can change this later in settings.ini" << std::endl;
  std::cout << std::endl;
  std::cout << "Enter scale (1/2/3/4) [Enter = off]: ";

  std::string line;
  std::getline(std::cin, line);
  int scaleChoice = 0;
  if (!line.empty()) {
    std::istringstream iss(line);
    iss >> scaleChoice;
  }
  int scale;
  if (scaleChoice >= 1 && scaleChoice <= 4) {
    scale = scaleChoice;
  } else {
    if (!line.empty())
      std::cout << "Invalid choice, using 1 (off)" << std::endl;
    scale = 1;
  }

  settings.UpdateFile(settingsPath, "upscale_scale", std::to_string(scale));
  settings.UpdateFile(settingsPath, "upscale_setup_completed", "1");

  std::cout << std::endl;
  std::cout << "Settings saved to settings.ini" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << std::endl;

  SetEvent(g_setupCompleteEvent);
}

void ApplyUpscale4KPatch(ID3D11Device *pDevice, ID3D11DeviceContext *pContext) {
  if (!pContext || !pDevice)
    return;

  static volatile LONG applied = 0;
  if (InterlockedCompareExchange(&applied, 1, 0) != 0)
    return;

  // Wait for MainThread's RunFirstTimeSetup() to finish so we read the
  // user's chosen scale (and block the game from rendering until then).
  WaitForSingleObject(g_setupCompleteEvent, INFINITE);

  try {
    std::string settingsPath = Settings::GetSettingsPath();

    Settings settings;
    settings.Load(settingsPath);

    // Dump mode: skip upscale (dump needs unmodified textures)
    if (settings.GetBool("texture_dump_enabled", false))
      return;

    int scale = settings.GetInt("upscale_scale", 1);

    if (scale < 1)
      scale = 1;
    if (scale > 4)
      scale = 4;

    if (scale == 1)
      return;

    g_ResMultiplier = static_cast<float>(scale);
    g_NewWidth = BaseWidth * g_ResMultiplier;
    g_NewHeight = BaseHeight * g_ResMultiplier;

    void **contextVtable = *(void ***)pContext;
    void **deviceVtable = *(void ***)pDevice;

    if (!contextVtable || !deviceVtable)
      return;

    InstallVtableHook(contextVtable, 50, 44, (void *)Hooked_RSSetViewports,
                      (volatile void **)&Original_RSSetViewports,
                      &g_viewportsHookReady);

    InstallVtableHook(
        contextVtable, 50, 46, (void *)Hooked_CopySubresourceRegion,
        (volatile void **)&Original_CopySubresourceRegion, &g_copyHookReady);

    InstallVtableHook(deviceVtable, 10, 5, (void *)Hooked_CreateTexture2D,
                      (volatile void **)&Original_CreateTexture2D,
                      &g_createTextureHookReady);

    g_upscaleActive = true;
    std::cout << "[Mod] " << scale << "x Upscale enabled" << std::endl;

    Sleep(1);
  } catch (...) {
  }
}

bool IsUpscaleActive() { return g_upscaleActive; }
float GetUpscaledWidth() { return g_NewWidth; }
float GetUpscaledHeight() { return g_NewHeight; }
