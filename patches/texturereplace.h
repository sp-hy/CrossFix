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

// Look up replacement by content hash (for PSSetShaderResources path)
// Returns filepath or empty string if no replacement found
std::string FindReplacementPath(UINT width, UINT height, uint64_t contentHash);

// Load replacement texture and create SRV for bind-time replacement
// Used by PSSetShaderResources hook to swap textures at bind time
bool LoadReplacementSRV(ID3D11Device *pDevice,
                        const D3D11_TEXTURE2D_DESC *pDesc,
                        uint64_t contentHash,
                        ID3D11ShaderResourceView **ppSRV);

// Quick check: does any replacement file exist at these dimensions?
// Used to skip expensive staging for textures that can't possibly match.
bool HasReplacementAtDimensions(UINT width, UINT height);
