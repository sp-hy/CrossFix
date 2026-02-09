#pragma once
#include <d3d11.h>

// Viewport Widescreen Fix - Applies widescreen corrections to UI viewports
// (stamina bars, battle UI, loading bar, menu elements, etc.)
void ApplyViewportWidescreenFixPatch(ID3D11Device *pDevice,
                                    ID3D11DeviceContext *pContext);
