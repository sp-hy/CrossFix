#pragma once
#include <d3d11.h>

// Replaces RDE's polygon vertex shader and textured polygon pixel shaders
// (matched by DXBC size and checksum) with versions that take the PGXP depth
// from position z: perspective-correct texture coordinates and affine colour.
// Polygons without PGXP data draw exactly as before.
void ApplyPgxpShaderHooks(ID3D11Device *pDevice);
