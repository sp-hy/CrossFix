#include "sampleroverride.h"
#include "../utils/memory.h"
#include "../utils/settings.h"
#include <Windows.h>
#include <iostream>

namespace {
typedef HRESULT(STDMETHODCALLTYPE *CreateSamplerState_t)(
    ID3D11Device *, const D3D11_SAMPLER_DESC *, ID3D11SamplerState **);

volatile CreateSamplerState_t Original_CreateSamplerState = nullptr;
volatile LONG g_samplerHookReady = 0;
} // namespace

HRESULT STDMETHODCALLTYPE
Hooked_CreateSamplerState(ID3D11Device *This, const D3D11_SAMPLER_DESC *pDesc,
                          ID3D11SamplerState **ppSamplerState) {
  MemoryBarrier();
  CreateSamplerState_t pOriginal = Original_CreateSamplerState;

  if (!pOriginal)
    return E_FAIL;
  if (!g_samplerHookReady || !pDesc)
    return pOriginal(This, pDesc, ppSamplerState);

  D3D11_SAMPLER_DESC newDesc = *pDesc;
  newDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

  return pOriginal(This, &newDesc, ppSamplerState);
}

void ApplySamplerOverridePatch(ID3D11Device *pDevice) {
  if (!pDevice)
    return;

  static volatile LONG applied = 0;
  if (InterlockedCompareExchange(&applied, 1, 0) != 0)
    return;

  try {
    std::string settingsPath = Settings::GetSettingsPath();

    Settings settings;
    settings.Load(settingsPath);

    bool forcePoint = settings.GetBool("sampler_force_point", false);
    if (!forcePoint)
      return;

    void **deviceVtable = *(void ***)pDevice;
    if (!deviceVtable)
      return;

    InstallVtableHook(deviceVtable, 50, 23,
                      (void *)Hooked_CreateSamplerState,
                      (volatile void **)&Original_CreateSamplerState,
                      &g_samplerHookReady);

    Sleep(1);
  } catch (...) {
  }
}
