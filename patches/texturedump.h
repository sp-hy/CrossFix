#pragma once
#include <d3d11.h>
#include <string>

// Texture dumper - dumps textures to /dump/ with hash-based filenames
void DumpTexture2D(ID3D11Device *pDevice, ID3D11DeviceContext *pContext,
                   ID3D11Texture2D *pTexture, const D3D11_TEXTURE2D_DESC *pDesc,
                   const D3D11_SUBRESOURCE_DATA *pInitialData);
bool IsTextureDumpEnabled();

// Hash texture for identification (used by both dumping and replacement)
uint64_t HashTexture(const D3D11_TEXTURE2D_DESC *pDesc,
                     const D3D11_SUBRESOURCE_DATA *pInitialData);

// Hash texture from raw data (works with both pInitialData and staging map)
uint64_t HashTextureData(const D3D11_TEXTURE2D_DESC *pDesc,
                         const void *pData, UINT rowPitch);

// Install PSSetShaderResources hook for runtime texture dumping
void ApplyTextureDumpHooks(ID3D11Device *pDevice,
                           ID3D11DeviceContext *pContext);
