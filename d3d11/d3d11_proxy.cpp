#include "d3d11_proxy.h"
#include <string>
#include <iostream>
#include "../patches/widescreen2d.h"

static HMODULE g_hD3D11 = nullptr;

typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI *PFN_D3D11_CORE_CREATE_DEVICE)(void*, void*, void*);
typedef HRESULT (WINAPI *PFN_D3D11_CORE_CREATE_LAYERED_DEVICE)(void*, void*, void*, void*, void*);
typedef SIZE_T  (WINAPI *PFN_D3D11_CORE_GET_LAYERED_DEVICE_SIZE)(void*, void*);
typedef HRESULT (WINAPI *PFN_D3D11_CORE_REGISTER_LAYERS)(void*, void*);

static PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice = nullptr;
static PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN pD3D11CreateDeviceAndSwapChain = nullptr;
static PFN_D3D11_CORE_CREATE_DEVICE pD3D11CoreCreateDevice = nullptr;
static PFN_D3D11_CORE_CREATE_LAYERED_DEVICE pD3D11CoreCreateLayeredDevice = nullptr;
static PFN_D3D11_CORE_GET_LAYERED_DEVICE_SIZE pD3D11CoreGetLayeredDeviceSize = nullptr;
static PFN_D3D11_CORE_REGISTER_LAYERS pD3D11CoreRegisterLayers = nullptr;

bool InitD3D11Proxy() {
    if (g_hD3D11) return true;
    
    char systemPath[MAX_PATH];
    GetSystemDirectoryA(systemPath, MAX_PATH);
    std::string d3d11Path = std::string(systemPath) + "\\d3d11.dll";
    
    g_hD3D11 = LoadLibraryA(d3d11Path.c_str());
    if (!g_hD3D11) {
        MessageBoxA(NULL, "Failed to load system d3d11.dll", "CrossFix D3D11 Proxy", MB_ICONERROR);
        return false;
    }
    
    pD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(g_hD3D11, "D3D11CreateDevice");
    pD3D11CreateDeviceAndSwapChain = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(g_hD3D11, "D3D11CreateDeviceAndSwapChain");
    pD3D11CoreCreateDevice = (PFN_D3D11_CORE_CREATE_DEVICE)GetProcAddress(g_hD3D11, "D3D11CoreCreateDevice");
    pD3D11CoreCreateLayeredDevice = (PFN_D3D11_CORE_CREATE_LAYERED_DEVICE)GetProcAddress(g_hD3D11, "D3D11CoreCreateLayeredDevice");
    pD3D11CoreGetLayeredDeviceSize = (PFN_D3D11_CORE_GET_LAYERED_DEVICE_SIZE)GetProcAddress(g_hD3D11, "D3D11CoreGetLayeredDeviceSize");
    pD3D11CoreRegisterLayers = (PFN_D3D11_CORE_REGISTER_LAYERS)GetProcAddress(g_hD3D11, "D3D11CoreRegisterLayers");
    
    return true;
}

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
    ) {
        if (!pD3D11CreateDevice) InitD3D11Proxy();
        if (!pD3D11CreateDevice) return E_FAIL;
        
        HRESULT hr = pD3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
        
        if (SUCCEEDED(hr)) {
            static bool hooksApplied = false;
            if (ppDevice && *ppDevice) HookD3D11Device(*ppDevice);
            if (ppImmediateContext && *ppImmediateContext) HookD3D11Context(*ppImmediateContext);
            if (!hooksApplied) {
                std::cout << "[PROXY] D3D11 hooks applied." << std::endl;
                hooksApplied = true;
            }
        }
        return hr;
    }

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
    ) {
        if (!pD3D11CreateDeviceAndSwapChain) InitD3D11Proxy();
        if (!pD3D11CreateDeviceAndSwapChain) return E_FAIL;
        
        HRESULT hr = pD3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
        
        if (SUCCEEDED(hr)) {
            static bool hooksApplied = false;
            if (ppDevice && *ppDevice) HookD3D11Device(*ppDevice);
            if (ppImmediateContext && *ppImmediateContext) HookD3D11Context(*ppImmediateContext);
            if (!hooksApplied) {
                std::cout << "[PROXY] D3D11 hooks applied." << std::endl;
                hooksApplied = true;
            }
        }
        return hr;
    }
    
    HRESULT WINAPI D3D11CoreCreateDevice_wrapper(void* a1, void* a2, void* a3) {
        if (!pD3D11CoreCreateDevice) InitD3D11Proxy();
        if (pD3D11CoreCreateDevice) return pD3D11CoreCreateDevice(a1, a2, a3);
        return E_FAIL;
    }

    HRESULT WINAPI D3D11CoreCreateLayeredDevice_wrapper(void* a1, void* a2, void* a3, void* a4, void* a5) {
        if (!pD3D11CoreCreateLayeredDevice) InitD3D11Proxy();
        if (pD3D11CoreCreateLayeredDevice) return pD3D11CoreCreateLayeredDevice(a1, a2, a3, a4, a5);
        return E_FAIL;
    }

    SIZE_T  WINAPI D3D11CoreGetLayeredDeviceSize_wrapper(void* a1, void* a2) {
        if (!pD3D11CoreGetLayeredDeviceSize) InitD3D11Proxy();
        if (pD3D11CoreGetLayeredDeviceSize) return pD3D11CoreGetLayeredDeviceSize(a1, a2);
        return 0;
    }

    HRESULT WINAPI D3D11CoreRegisterLayers_wrapper(void* a1, void* a2) {
        if (!pD3D11CoreRegisterLayers) InitD3D11Proxy();
        if (pD3D11CoreRegisterLayers) return pD3D11CoreRegisterLayers(a1, a2);
        return E_FAIL;
    }
}
