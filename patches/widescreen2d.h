#pragma once
#include <Windows.h>
#include <d3d11.h>

// D3D11 function signatures
typedef HRESULT(WINAPI* D3D11CreateDevice_t)(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext
);

typedef HRESULT(WINAPI* D3D11CreateDeviceAndSwapChain_t)(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext
);

// Hook the D3D11 device methods
bool HookD3D11Device(ID3D11Device* pDevice);

// Hook the D3D11 device context methods
bool HookD3D11Context(ID3D11DeviceContext* pContext);

// Cleanup DirectX 11 hooks
void CleanupD3D11Hooks();

// Set the widescreen ratio for 2D background transformation
// ratio: the horizontal compression ratio (e.g., 0.75 for 16:9, 0.571 for 21:9)
void SetWidescreen2DRatio(float ratio);

// Enable or disable 2D widescreen transformation
void SetWidescreen2DEnabled(bool enabled);

// Set the widescreen ratio for 2D background transformation
// ratio: the horizontal compression ratio (e.g., 0.75 for 16:9, 0.571 for 21:9)
void SetWidescreen2DRatio(float ratio);

// Enable or disable 2D widescreen transformation
void SetWidescreen2DEnabled(bool enabled);
