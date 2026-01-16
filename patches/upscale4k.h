#pragma once
#include <d3d11.h>

// 4K Upscale Patch - Upscales 4096x2048 render targets to higher resolutions
void ApplyUpscale4KPatch(ID3D11Device* pDevice, ID3D11DeviceContext* pContext);
