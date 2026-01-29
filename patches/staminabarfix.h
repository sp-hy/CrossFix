#pragma once
#include <d3d11.h>

// Stamina Bar Fix Patch - Applies widescreen corrections to UI viewports (stamina bars, etc.)
void ApplyStaminaBarFixPatch(ID3D11Device* pDevice, ID3D11DeviceContext* pContext);
