#include "pgxpshaders.h"
#include "../utils/memory.h"
#include "../utils/settings.h"
#include "pgxp.h"
#include "shaders/pgxp_shaders.h"
#include <Windows.h>
#include <cstring>
#include <iostream>

namespace {
typedef HRESULT(STDMETHODCALLTYPE *CreateVertexShader_t)(
    ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *,
    ID3D11VertexShader **);
typedef HRESULT(STDMETHODCALLTYPE *CreatePixelShader_t)(
    ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *,
    ID3D11PixelShader **);

constexpr int kCreateVertexShaderIndex = 12;
constexpr int kCreatePixelShaderIndex = 15;

volatile CreateVertexShader_t Original_CreateVertexShader = nullptr;
volatile CreatePixelShader_t Original_CreatePixelShader = nullptr;
volatile LONG g_vertexShaderHookReady = 0;
volatile LONG g_pixelShaderHookReady = 0;

// An original shader, identified by its DXBC size and checksum (bytes 4..19)
struct OriginalShader {
  SIZE_T length;
  uint8_t checksum[16];
};

struct Bytecode {
  const BYTE *code;
  SIZE_T length;
};

// RDE's polygon vertex shader, its textured polygon pixel shader, and the
// pixel shader variant that truncates alpha
const OriginalShader kPolyVs = {0x2E0,
                                {0xa4, 0xed, 0xdd, 0xea, 0xf0, 0xb8, 0x0e, 0x69,
                                 0x8c, 0x15, 0x63, 0x5e, 0xea, 0x78, 0x86, 0xce}};
const OriginalShader kPolyPs = {0x410,
                                {0xab, 0xe2, 0xd7, 0x74, 0x9c, 0xab, 0xc8, 0xc9,
                                 0x17, 0xd2, 0x82, 0xc5, 0x15, 0xc8, 0xbb, 0x01}};
const OriginalShader kPolyPsRoundAlpha = {
    0x44C,
    {0x8e, 0xcb, 0xb9, 0xae, 0xdf, 0x3f, 0xb6, 0x55, 0x2b, 0x09, 0x18, 0x61,
     0x5f, 0xee, 0x1a, 0xe7}};

const Bytecode kPgxpPolyVs = {g_pgxpPolyVs, sizeof(g_pgxpPolyVs)};
const Bytecode kPgxpPolyPs = {g_pgxpPolyPs, sizeof(g_pgxpPolyPs)};
const Bytecode kPgxpPolyPsRoundAlpha = {g_pgxpPolyPsRoundAlpha,
                                        sizeof(g_pgxpPolyPsRoundAlpha)};

// The replacement pixel shaders read outputs only the replacement vertex
// shader writes; RDE creates the vertex shader first
volatile LONG g_vertexShaderReplaced = 0;

bool Matches(const void *bytecode, SIZE_T length,
             const OriginalShader &original) {
  return bytecode && length == original.length &&
         memcmp(static_cast<const uint8_t *>(bytecode) + 4, original.checksum,
                16) == 0;
}

void LogFallback(const char *shader) {
  std::cout << "[PGXP] Replacement " << shader
            << " shader rejected by the device, using the original"
            << std::endl;
}
} // namespace

HRESULT STDMETHODCALLTYPE Hooked_CreateVertexShader(
    ID3D11Device *This, const void *pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage *pClassLinkage, ID3D11VertexShader **ppVertexShader) {
  MemoryBarrier();
  CreateVertexShader_t pOriginal = Original_CreateVertexShader;
  if (!pOriginal)
    return E_FAIL;

  if (g_vertexShaderHookReady &&
      Matches(pShaderBytecode, BytecodeLength, kPolyVs)) {
    HRESULT hr = pOriginal(This, kPgxpPolyVs.code, kPgxpPolyVs.length,
                           pClassLinkage, ppVertexShader);
    if (SUCCEEDED(hr)) {
      InterlockedExchange(&g_vertexShaderReplaced, 1);
      return hr;
    }
    InterlockedExchange(&g_vertexShaderReplaced, 0);
    LogFallback("vertex");
  }
  return pOriginal(This, pShaderBytecode, BytecodeLength, pClassLinkage,
                   ppVertexShader);
}

HRESULT STDMETHODCALLTYPE Hooked_CreatePixelShader(
    ID3D11Device *This, const void *pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage *pClassLinkage, ID3D11PixelShader **ppPixelShader) {
  MemoryBarrier();
  CreatePixelShader_t pOriginal = Original_CreatePixelShader;
  if (!pOriginal)
    return E_FAIL;

  if (g_pixelShaderHookReady && g_vertexShaderReplaced) {
    const Bytecode *replacement = nullptr;
    if (Matches(pShaderBytecode, BytecodeLength, kPolyPs))
      replacement = &kPgxpPolyPs;
    else if (Matches(pShaderBytecode, BytecodeLength, kPolyPsRoundAlpha))
      replacement = &kPgxpPolyPsRoundAlpha;
    if (replacement) {
      HRESULT hr = pOriginal(This, replacement->code, replacement->length,
                             pClassLinkage, ppPixelShader);
      if (SUCCEEDED(hr))
        return hr;
      LogFallback("pixel");
    }
  }
  return pOriginal(This, pShaderBytecode, BytecodeLength, pClassLinkage,
                   ppPixelShader);
}

void ApplyPgxpShaderHooks(ID3D11Device *pDevice) {
  if (!pDevice)
    return;

  static volatile LONG applied = 0;
  if (InterlockedCompareExchange(&applied, 1, 0) != 0)
    return;

  Settings settings;
  settings.Load(Settings::GetSettingsPath());
  if (!PgxpEnabled(settings))
    return;

  void **deviceVtable = *(void ***)pDevice;
  if (!deviceVtable)
    return;

  InstallVtableHook(deviceVtable, 50, kCreateVertexShaderIndex,
                    (void *)Hooked_CreateVertexShader,
                    (volatile void **)&Original_CreateVertexShader,
                    &g_vertexShaderHookReady);
  InstallVtableHook(deviceVtable, 50, kCreatePixelShaderIndex,
                    (void *)Hooked_CreatePixelShader,
                    (volatile void **)&Original_CreatePixelShader,
                    &g_pixelShaderHookReady);
}
