#pragma once
#include <d3d11.h>

// Sampler Override Patch - Fixes lines around overlay textures when upscaling
// by forcing POINT filtering on all sampler states.
void ApplySamplerOverridePatch(ID3D11Device *pDevice);
