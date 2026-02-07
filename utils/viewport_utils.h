// Viewport Utilities - Shared viewport manipulation logic
#pragma once

#include <d3d11.h>

namespace ViewportUtils {
// Copy viewports to a stack-allocated buffer with bounds checking
// Returns the actual count of viewports copied
UINT CopyViewportsToBuffer(D3D11_VIEWPORT *outBuffer, UINT bufferSize,
                           const D3D11_VIEWPORT *pViewports, UINT numViewports);

// Apply stamina bar widescreen fix to viewports
// Modifies viewports in-place based on stamina bar detection
void ApplyStaminaBarWidescreenFix(D3D11_VIEWPORT *viewports, UINT count,
                                  float widescreenRatio);
} // namespace ViewportUtils
