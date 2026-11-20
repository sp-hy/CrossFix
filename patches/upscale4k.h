#pragma once
#include <d3d11.h>

// Interactive first-run setup prompt (call from MainThread before other output)
void RunFirstTimeSetup();

// Resolution scale: draws the 4096x2048 VRAM target at 1x-16x the PS1
// resolution (resolution_scale; the game's own is 4x)
void ApplyUpscale4KPatch(ID3D11Device* pDevice, ID3D11DeviceContext* pContext);

// Query upscale state (used by texture dump/replace to coordinate)
bool IsUpscaleActive();
float GetUpscaledWidth();
float GetUpscaledHeight();
