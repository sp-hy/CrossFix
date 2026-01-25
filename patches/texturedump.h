#pragma once
#include <d3d11.h>
#include <string>

// Texture dumper - dumps textures to /dump/ with hash-based filenames
void DumpTexture2D(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11Texture2D* pTexture, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData);
bool IsTextureDumpEnabled();

// Texture resizing - resizes specific textures based on widescreen ratio
void ResizeTexture2D(const D3D11_TEXTURE2D_DESC* pDesc, D3D11_TEXTURE2D_DESC* pNewDesc);
bool ShouldResizeTexture(const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData);
void AddResizeHash(uint64_t hash);
void ClearResizeHashes();
