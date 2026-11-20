#pragma once
#include <d3d11.h>

// Viewport Widescreen Fix - Applies widescreen corrections to UI viewports
// (stamina bars, battle UI, loading bar, menu elements, etc.)
void ApplyViewportWidescreenFixPatch(ID3D11Device *pDevice,
                                    ID3D11DeviceContext *pContext);

// The RSSetViewports the widescreen fix hook forwards to, or nullptr when the
// hook is not installed. A later RSSetViewports hook that applies the fix
// itself calls this to skip a second pass.
typedef void(STDMETHODCALLTYPE *RSSetViewportsFn)(ID3D11DeviceContext *, UINT,
                                                  const D3D11_VIEWPORT *);
RSSetViewportsFn GetUnhookedRSSetViewports();
