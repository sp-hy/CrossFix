#include "d3d11_proxy.h"
#include "../patches/viewportwidescreenfix.h"
#include "../patches/texturedump.h"
#include "../patches/textureresize.h"
#include "../patches/sampleroverride.h"
#include "../patches/upscale4k.h"
#include <Windows.h>
#include <iostream>

static HMODULE g_hD3D11 = nullptr;

typedef HRESULT(WINAPI *PFN_D3D11_CREATE_DEVICE)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
    UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
    UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_D3D11_CORE_CREATE_DEVICE)(void *, void *, void *);
typedef HRESULT(WINAPI *PFN_D3D11_CORE_CREATE_LAYERED_DEVICE)(void *, void *,
                                                              void *, void *,
                                                              void *);
typedef SIZE_T(WINAPI *PFN_D3D11_CORE_GET_LAYERED_DEVICE_SIZE)(void *, void *);
typedef HRESULT(WINAPI *PFN_D3D11_CORE_REGISTER_LAYERS)(void *, void *);

static PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice = nullptr;
static PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN pD3D11CreateDeviceAndSwapChain =
    nullptr;
static PFN_D3D11_CORE_CREATE_DEVICE pD3D11CoreCreateDevice = nullptr;
static PFN_D3D11_CORE_CREATE_LAYERED_DEVICE pD3D11CoreCreateLayeredDevice =
    nullptr;
static PFN_D3D11_CORE_GET_LAYERED_DEVICE_SIZE pD3D11CoreGetLayeredDeviceSize =
    nullptr;
static PFN_D3D11_CORE_REGISTER_LAYERS pD3D11CoreRegisterLayers = nullptr;

bool InitD3D11Proxy() {
  if (g_hD3D11)
    return true;

  char systemPath[MAX_PATH];
  GetSystemDirectoryA(systemPath, MAX_PATH);

  char d3d11Path[MAX_PATH];
  strcpy_s(d3d11Path, systemPath);
  strcat_s(d3d11Path, "\\d3d11.dll");

  g_hD3D11 = LoadLibraryA(d3d11Path);
  if (!g_hD3D11) {
    MessageBoxA(NULL, "Failed to load system d3d11.dll", "CrossFix D3D11 Proxy",
                MB_ICONERROR);
    return false;
  }

  pD3D11CreateDevice =
      (PFN_D3D11_CREATE_DEVICE)GetProcAddress(g_hD3D11, "D3D11CreateDevice");
  pD3D11CreateDeviceAndSwapChain =
      (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(
          g_hD3D11, "D3D11CreateDeviceAndSwapChain");
  pD3D11CoreCreateDevice = (PFN_D3D11_CORE_CREATE_DEVICE)GetProcAddress(
      g_hD3D11, "D3D11CoreCreateDevice");
  pD3D11CoreCreateLayeredDevice =
      (PFN_D3D11_CORE_CREATE_LAYERED_DEVICE)GetProcAddress(
          g_hD3D11, "D3D11CoreCreateLayeredDevice");
  pD3D11CoreGetLayeredDeviceSize =
      (PFN_D3D11_CORE_GET_LAYERED_DEVICE_SIZE)GetProcAddress(
          g_hD3D11, "D3D11CoreGetLayeredDeviceSize");
  pD3D11CoreRegisterLayers = (PFN_D3D11_CORE_REGISTER_LAYERS)GetProcAddress(
      g_hD3D11, "D3D11CoreRegisterLayers");

  return true;
}

// SEH-safe wrapper functions (no C++ objects that need unwinding)
static DWORD SafeApplyTextureResizeHooks(ID3D11Device *pDevice) {
  __try {
    ApplyTextureResizeHooks(pDevice);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return GetExceptionCode();
  }
}

static DWORD SafeApplySamplerOverridePatch(ID3D11Device *pDevice) {
  __try {
    ApplySamplerOverridePatch(pDevice);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return GetExceptionCode();
  }
}

static DWORD SafeApplyUpscale4KPatch(ID3D11Device *pDevice,
                                     ID3D11DeviceContext *pContext) {
  __try {
    ApplyUpscale4KPatch(pDevice, pContext);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return GetExceptionCode();
  }
}

static DWORD SafeApplyViewportWidescreenFixPatch(ID3D11Device *pDevice,
                                                 ID3D11DeviceContext *pContext) {
  __try {
    ApplyViewportWidescreenFixPatch(pDevice, pContext);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return GetExceptionCode();
  }
}

static DWORD SafeApplyTextureDumpHooks(ID3D11Device *pDevice,
                                       ID3D11DeviceContext *pContext) {
  __try {
    ApplyTextureDumpHooks(pDevice, pContext);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return GetExceptionCode();
  }
}

// Apply all hooks with SEH protection
static void ApplyHooksWithProtection(ID3D11Device *pDevice,
                                     ID3D11DeviceContext *pContext) {
  DWORD exCode;

  exCode = SafeApplyTextureResizeHooks(pDevice);
  if (exCode != 0) {
    std::cout << "Warning: Texture resize hooks failed (0x" << std::hex
              << exCode << std::dec << "), continuing without them"
              << std::endl;
  }

  // Try to apply viewport widescreen fix first (for when upscale is disabled)
  exCode = SafeApplyViewportWidescreenFixPatch(pDevice, pContext);
  if (exCode != 0) {
    std::cout << "Warning: Viewport widescreen fix hooks failed (0x"
              << std::hex << exCode << std::dec << "), continuing without them"
              << std::endl;
  }

  exCode = SafeApplySamplerOverridePatch(pDevice);
  if (exCode != 0) {
    std::cout << "Warning: Sampler override hooks failed (0x" << std::hex
              << exCode << std::dec << "), continuing without them"
              << std::endl;
  }

  // Apply upscale patch (includes viewport widescreen fix when enabled)
  // If upscale is enabled, this will overwrite the viewport fix hook but
  // includes the same logic
  exCode = SafeApplyUpscale4KPatch(pDevice, pContext);
  if (exCode != 0) {
    std::cout << "Warning: Upscale hooks failed (0x" << std::hex << exCode
              << std::dec << "), continuing without them" << std::endl;
  }

  // Apply texture dump/replace hooks (PSSetShaderResources)
  exCode = SafeApplyTextureDumpHooks(pDevice, pContext);
  if (exCode != 0) {
    std::cout << "Warning: Texture dump hooks failed (0x" << std::hex << exCode
              << std::dec << "), continuing without them" << std::endl;
  }
}

extern "C" {
HRESULT WINAPI D3D11CreateDevice_wrapper(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext) {
  if (!pD3D11CreateDevice)
    InitD3D11Proxy();
  if (!pD3D11CreateDevice)
    return E_FAIL;

  HRESULT hr = pD3D11CreateDevice(pAdapter, DriverType, Software, Flags,
                                  pFeatureLevels, FeatureLevels, SDKVersion,
                                  ppDevice, pFeatureLevel, ppImmediateContext);
  if (SUCCEEDED(hr) && ppDevice && *ppDevice && ppImmediateContext &&
      *ppImmediateContext) {
    if (g_versionCheckPassed) {
      ApplyHooksWithProtection(*ppDevice, *ppImmediateContext);
    }
  }
  return hr;
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain_wrapper(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext) {
  if (!pD3D11CreateDeviceAndSwapChain)
    InitD3D11Proxy();
  if (!pD3D11CreateDeviceAndSwapChain)
    return E_FAIL;

  HRESULT hr = pD3D11CreateDeviceAndSwapChain(
      pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
      SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
      ppImmediateContext);
  if (SUCCEEDED(hr) && ppDevice && *ppDevice && ppImmediateContext &&
      *ppImmediateContext) {
    if (g_versionCheckPassed) {
      ApplyHooksWithProtection(*ppDevice, *ppImmediateContext);
    }
  }
  return hr;
}

HRESULT WINAPI D3D11CoreCreateDevice_wrapper(void *a1, void *a2, void *a3) {
  if (!pD3D11CoreCreateDevice)
    InitD3D11Proxy();
  if (pD3D11CoreCreateDevice)
    return pD3D11CoreCreateDevice(a1, a2, a3);
  return E_FAIL;
}

HRESULT WINAPI D3D11CoreCreateLayeredDevice_wrapper(void *a1, void *a2,
                                                    void *a3, void *a4,
                                                    void *a5) {
  if (!pD3D11CoreCreateLayeredDevice)
    InitD3D11Proxy();
  if (pD3D11CoreCreateLayeredDevice)
    return pD3D11CoreCreateLayeredDevice(a1, a2, a3, a4, a5);
  return E_FAIL;
}

SIZE_T WINAPI D3D11CoreGetLayeredDeviceSize_wrapper(void *a1, void *a2) {
  if (!pD3D11CoreGetLayeredDeviceSize)
    InitD3D11Proxy();
  if (pD3D11CoreGetLayeredDeviceSize)
    return pD3D11CoreGetLayeredDeviceSize(a1, a2);
  return 0;
}

HRESULT WINAPI D3D11CoreRegisterLayers_wrapper(void *a1, void *a2) {
  if (!pD3D11CoreRegisterLayers)
    InitD3D11Proxy();
  if (pD3D11CoreRegisterLayers)
    return pD3D11CoreRegisterLayers(a1, a2);
  return E_FAIL;
}
}
