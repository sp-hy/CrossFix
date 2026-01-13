#pragma once
#include <Windows.h>
#include <d3d11.h>

// Initialize the D3D11 proxy
bool InitD3D11Proxy();

// Wrapper functions
extern "C" {
	HRESULT WINAPI D3D11CreateDevice_wrapper(
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

	HRESULT WINAPI D3D11CreateDeviceAndSwapChain_wrapper(
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

	// Other forwarding wrappers
	HRESULT WINAPI D3D11CoreCreateDevice_wrapper(void* a1, void* a2, void* a3);
	HRESULT WINAPI D3D11CoreCreateLayeredDevice_wrapper(void* a1, void* a2, void* a3, void* a4, void* a5);
	SIZE_T  WINAPI D3D11CoreGetLayeredDeviceSize_wrapper(void* a1, void* a2);
	HRESULT WINAPI D3D11CoreRegisterLayers_wrapper(void* a1, void* a2);
}
