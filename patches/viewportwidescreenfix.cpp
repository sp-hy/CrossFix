// Viewport Widescreen Fix - Widescreen corrections for UI viewports

#define NOMINMAX
#include "viewportwidescreenfix.h"
#include "../utils/memory.h"
#include "../utils/viewport_utils.h"
#include "widescreen.h"
#include <Windows.h>

namespace {
typedef void(STDMETHODCALLTYPE *RSSetViewports_t)(ID3D11DeviceContext *, UINT,
                                                  const D3D11_VIEWPORT *);

volatile RSSetViewports_t Original_RSSetViewports_ViewportWidescreenFix =
    nullptr;
volatile LONG g_viewportWidescreenFixHookReady = 0;
} // namespace

void STDMETHODCALLTYPE Hooked_RSSetViewports_ViewportWidescreenFix(
    ID3D11DeviceContext *This, UINT NumViewports,
    const D3D11_VIEWPORT *pViewports) {
  MemoryBarrier();
  RSSetViewports_t pOriginal = Original_RSSetViewports_ViewportWidescreenFix;

  if (!pOriginal)
    return;
  if (!pViewports || NumViewports == 0 || !g_viewportWidescreenFixHookReady) {
    pOriginal(This, NumViewports, pViewports);
    return;
  }

  // Allocate a small buffer on stack for modified viewports
  D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  UINT count = ViewportUtils::CopyViewportsToBuffer(
      vps, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, pViewports,
      NumViewports);

  float ratio = GetCurrentWidescreenRatio();

  // Only apply the fix if we're actually in widescreen mode
  // Use tolerance to account for floating-point precision
  // Ratio should be < 1.0 for widescreen (e.g., 0.75 for 16:9)
  const float WIDESCREEN_THRESHOLD = 0.99f;
  if (ratio < WIDESCREEN_THRESHOLD) {
    ViewportUtils::ApplyViewportWidescreenFix(vps, count, ratio);
  }

  pOriginal(This, count, vps);
}

void ApplyViewportWidescreenFixPatch(ID3D11Device *pDevice,
                                    ID3D11DeviceContext *pContext) {
  if (!pContext || !pDevice)
    return;

  static volatile LONG applied = 0;
  if (InterlockedCompareExchange(&applied, 1, 0) != 0)
    return;

  void **contextVtable = *(void ***)pContext;
  if (!contextVtable)
    return;

  InstallVtableHook(contextVtable, 50, 44,
                    (void *)Hooked_RSSetViewports_ViewportWidescreenFix,
                    (volatile void **)&Original_RSSetViewports_ViewportWidescreenFix,
                    &g_viewportWidescreenFixHookReady);
  Sleep(1);
}
