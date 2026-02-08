#pragma once
#include <d3d11.h>
#include <string>

// Texture replacement system - loads modified textures from mods/textures folder
// Returns true if a replacement texture was loaded and applied
bool TryLoadReplacementTexture(ID3D11Device *pDevice, 
                               const D3D11_TEXTURE2D_DESC *pDesc,
                               const D3D11_SUBRESOURCE_DATA *pInitialData,
                               ID3D11Texture2D **ppTexture2D);

// Check if texture replacement is enabled
bool IsTextureReplacementEnabled();
