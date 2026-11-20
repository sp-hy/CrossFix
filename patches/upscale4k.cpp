// Resolution scale: draws RDE's VRAM render target at 1x to 16x the PS1
// resolution instead of the game's fixed 4x.
//
// RDE keeps VRAM on the GPU as two 4096x2048 surfaces, 4 texels per PS1 pixel
// each way: an RGBA8 colour target that polygons draw into (with a full-size
// copy or MSAA resolve texture beside it) and an R16_UINT mirror holding the
// raw 16-bit texels that texture pages are decoded from. Only the colour side
// is resized; the mirror stays at 4x, so texture decoding and the HD texture
// packs are untouched. Every place where the two meet works in 4x units and
// is converted here:
//   - textures: the colour target, its copy / resolve texture and the
//     temporaries of a VRAM move are created at the scaled size and tagged
//   - viewports set while a tagged target is bound are scaled; the state
//     restore that puts back a viewport read from the device passes it as is
//   - the copies of a VRAM move scale their boxes and destinations on tagged
//     textures. The D3D11 runtime rewrites the copy slots of its context
//     vtable after start-up, so RDE's call sites are patched rather than the
//     vtable
//   - the pixel shader that draws CPU uploads from the mirror into the colour
//     target reads the mirror at the scaled pixel's 4x position

#define NOMINMAX
#include "upscale4k.h"
#include "../utils/memory.h"
#include "../utils/rde_symbols.h"
#include "../utils/settings.h"
#include "../utils/viewport_utils.h"
#include "texturereplace.h"
#include "viewportwidescreenfix.h"
#include "widescreen.h"
#include <Windows.h>
#include <d3dcompiler.h>
#include <intrin.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

static bool g_upscaleActive = false;

// Signaled once RunFirstTimeSetup() completes (or finds setup already done).
// Initially unsignaled so ApplyUpscale4KPatch waits for MainThread to finish.
static HANDLE g_setupCompleteEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

namespace {
constexpr UINT kBaseWidth = 4096;
constexpr UINT kBaseHeight = 2048;
constexpr int kGameScale = 4;
constexpr int kMinScale = 1;
constexpr int kMaxScale = 16;

// Scale in multiples of the PS1 resolution (4 = the game's own)
int g_scale = kGameScale;

// {6E0C2B1A-3D4F-4C8B-9A61-5B7D2E8F0C13}: set on every scaled texture
const GUID kScaledTextureTag = {0x6e0c2b1a,
                                0x3d4f,
                                0x4c8b,
                                {0x9a, 0x61, 0x5b, 0x7d, 0x2e, 0x8f, 0x0c, 0x13}};

typedef void(STDMETHODCALLTYPE *RSSetViewports_t)(ID3D11DeviceContext *, UINT,
                                                  const D3D11_VIEWPORT *);
typedef HRESULT(STDMETHODCALLTYPE *CreateTexture2D_t)(
    ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
    const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
typedef HRESULT(STDMETHODCALLTYPE *CreatePixelShader_t)(
    ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *,
    ID3D11PixelShader **);

constexpr int kCreateTexture2DIndex = 5;
constexpr int kCreatePixelShaderIndex = 15;
constexpr int kRSSetViewportsIndex = 44;

// call dword ptr [eax|ecx + 0xB8]: CopySubresourceRegion in hd_vram_move_gpu
const uintptr_t kVramMoveCopySites[] = {
    rde::hd_vram_move_gpu_plus_1ec, rde::hd_vram_move_gpu_plus_23f,
    rde::hd_vram_move_gpu_plus_3fb, rde::hd_vram_move_gpu_plus_44d,
    rde::hd_vram_move_gpu_plus_4fb, rde::hd_vram_move_gpu_plus_55c};
const uint8_t kCopyCallEax[] = {0xFF, 0x90, 0xB8, 0x00, 0x00, 0x00};
const uint8_t kCopyCallEcx[] = {0xFF, 0x91, 0xB8, 0x00, 0x00, 0x00};
// call dword ptr [ecx + 0xB0]: RSSetViewports of the saved viewports
const uint8_t kRestoreViewportsCall[] = {0xFF, 0x91, 0xB0, 0x00, 0x00, 0x00};

volatile RSSetViewports_t Original_RSSetViewports = nullptr;
volatile CreateTexture2D_t Original_CreateTexture2D = nullptr;
volatile CreatePixelShader_t Original_CreatePixelShader = nullptr;

volatile LONG g_viewportsHookReady = 0;
volatile LONG g_createTextureHookReady = 0;
volatile LONG g_createPixelShaderHookReady = 0;

uintptr_t g_moduleBase = 0;

// Draws mirror texels into the colour target for CPU uploads. The original
// reads the mirror at the target pixel's own position, which is only right at
// 4x; this one reads the mirror texel under the scaled pixel's centre.
// SOURCE_PER_TARGET is 4 / scale.
const char kUploadShaderSource[] = R"(
Texture2D<uint> vram : register(t0);

struct PSIn
{
  float4 pos : SV_POSITION;
  float4 col : COLOR;
  float2 uv : TEXCOORD;
};

float4 main(PSIn i) : SV_Target
{
  uint texel = vram.Load(int3(int2(i.pos.xy * SOURCE_PER_TARGET), 0));
  uint packed = (texel & 0x7FFF) | (texel != 0 ? 0x8000 : 0);
  return float4(packed & 31, packed & 992, packed & 0x7C00, packed & 0x8000) *
         float4(1.0 / 31.0, 1.0 / 992.0, 1.0 / 31744.0, 1.0 / 32768.0);
}
)";

inline bool IsColorFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
         format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
         format == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

// RDE passes 4x coordinates that are whole multiples of 4, so the scaled
// value is exact for every scale
inline UINT ScaleUnits(UINT value) {
  return static_cast<UINT>(static_cast<uint64_t>(value) * g_scale /
                           kGameScale);
}

inline float ScaleUnits(float value) {
  return value * static_cast<float>(g_scale) / kGameScale;
}

bool IsScaledResource(ID3D11Resource *resource) {
  if (!resource)
    return false;
  BYTE tag = 0;
  UINT size = sizeof(tag);
  return SUCCEEDED(resource->GetPrivateData(kScaledTextureTag, &size, &tag)) &&
         tag != 0;
}

void TagScaledTexture(ID3D11Texture2D *texture) {
  const BYTE tag = 1;
  texture->SetPrivateData(kScaledTextureTag, sizeof(tag), &tag);
}

bool IsScaledTargetBound(ID3D11DeviceContext *context) {
  ID3D11RenderTargetView *rtv = nullptr;
  context->OMGetRenderTargets(1, &rtv, nullptr);
  if (!rtv)
    return false;
  ID3D11Resource *resource = nullptr;
  rtv->GetResource(&resource);
  const bool scaled = IsScaledResource(resource);
  if (resource)
    resource->Release();
  rtv->Release();
  return scaled;
}

// RDE allocates the temporaries of a VRAM move (rectangle-sized copies of the
// colour target and of the mirror) inside hd_vram_move_gpu
bool CalledFromVramMove(void *returnAddress) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(returnAddress);
  return address >= g_moduleBase + rde::hd_vram_move_gpu &&
         address < g_moduleBase + rde::hd_vram_move_gpu_end;
}

ID3D11PixelShader *CompileUploadShader(ID3D11Device *device,
                                       ID3D11ClassLinkage *linkage) {
  HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
  if (!compiler)
    return nullptr;
  auto compile = reinterpret_cast<pD3DCompile>(
      GetProcAddress(compiler, "D3DCompile"));
  if (!compile)
    return nullptr;

  const std::string ratio =
      std::to_string(static_cast<double>(kGameScale) / g_scale);
  const D3D_SHADER_MACRO defines[] = {{"SOURCE_PER_TARGET", ratio.c_str()},
                                      {nullptr, nullptr}};
  ID3DBlob *code = nullptr;
  ID3DBlob *errors = nullptr;
  HRESULT hr = compile(kUploadShaderSource, sizeof(kUploadShaderSource) - 1,
                       "upload_ps", defines, nullptr, "main", "ps_4_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (errors) {
    std::cout << "[Upscale] Upload shader: "
              << static_cast<const char *>(errors->GetBufferPointer())
              << std::endl;
    errors->Release();
  }
  if (FAILED(hr) || !code)
    return nullptr;

  ID3D11PixelShader *shader = nullptr;
  hr = Original_CreatePixelShader(device, code->GetBufferPointer(),
                                  code->GetBufferSize(), linkage, &shader);
  code->Release();
  return SUCCEEDED(hr) ? shader : nullptr;
}

// Largest scale up to the requested one whose colour target and copy both fit
int ProbeScale(ID3D11Device *device, int requested) {
  for (int scale = requested; scale > kGameScale; --scale) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = kBaseWidth * scale / kGameScale;
    desc.Height = kBaseHeight * scale / kGameScale;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D *target = nullptr;
    ID3D11Texture2D *copy = nullptr;
    const bool fits = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target)) &&
                      SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &copy));
    if (target)
      target->Release();
    if (copy)
      copy->Release();
    if (fits)
      return scale;
  }
  return std::min(requested, kGameScale);
}

int ReadScale(const Settings &settings) {
  if (settings.HasKey("resolution_scale"))
    return settings.GetInt("resolution_scale", kGameScale);
  const int legacy = settings.GetInt("upscale_scale", 1);
  return legacy > 1 ? legacy * kGameScale : kGameScale;
}
} // namespace

void STDMETHODCALLTYPE Hooked_RSSetViewports(ID3D11DeviceContext *This,
                                             UINT NumViewports,
                                             const D3D11_VIEWPORT *pViewports) {
  MemoryBarrier();
  RSSetViewports_t pOriginal = GetUnhookedRSSetViewports();
  if (!pOriginal)
    pOriginal = Original_RSSetViewports;

  if (!pOriginal)
    return;
  if (!pViewports || NumViewports == 0 || !g_viewportsHookReady) {
    pOriginal(This, NumViewports, pViewports);
    return;
  }

  D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  UINT count = ViewportUtils::CopyViewportsToBuffer(
      vps, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, pViewports,
      NumViewports);

  // The widescreen fix matches viewports in the game's 4x units, so it runs
  // before scaling
  float ratio = GetCurrentWidescreenRatio();
  const float WIDESCREEN_THRESHOLD = 0.99f;
  if (ratio < WIDESCREEN_THRESHOLD) {
    ViewportUtils::ApplyViewportWidescreenFix(vps, count, ratio);
  }

  if (IsScaledTargetBound(This)) {
    for (UINT i = 0; i < count; ++i) {
      vps[i].TopLeftX = ScaleUnits(vps[i].TopLeftX);
      vps[i].TopLeftY = ScaleUnits(vps[i].TopLeftY);
      vps[i].Width = std::min(ScaleUnits(vps[i].Width), 32767.0f);
      vps[i].Height = std::min(ScaleUnits(vps[i].Height), 32767.0f);
    }
  }

  pOriginal(This, count, vps);
}

namespace {
// Called in place of the context's CopySubresourceRegion by the copies of a
// VRAM move, with the same arguments (4x units)
void STDMETHODCALLTYPE VramMoveCopy(ID3D11DeviceContext *context,
                                   ID3D11Resource *dst, UINT dstSubresource,
                                   UINT dstX, UINT dstY, UINT dstZ,
                                   ID3D11Resource *src, UINT srcSubresource,
                                   const D3D11_BOX *srcBox) {
  D3D11_BOX box;
  if (srcBox && IsScaledResource(src)) {
    box = *srcBox;
    box.left = ScaleUnits(box.left);
    box.top = ScaleUnits(box.top);
    box.right = ScaleUnits(box.right);
    box.bottom = ScaleUnits(box.bottom);
    srcBox = &box;
  }
  if (IsScaledResource(dst)) {
    dstX = ScaleUnits(dstX);
    dstY = ScaleUnits(dstY);
  }
  context->CopySubresourceRegion(dst, dstSubresource, dstX, dstY, dstZ, src,
                                 srcSubresource, srcBox);
}

// Called in place of RSSetViewports by the state restore: the viewports were
// read back from the device, already fixed and scaled
void STDMETHODCALLTYPE RestoreViewports(ID3D11DeviceContext *context,
                                        UINT count,
                                        const D3D11_VIEWPORT *viewports) {
  RSSetViewports_t set = GetUnhookedRSSetViewports();
  if (!set)
    set = Original_RSSetViewports;
  if (set)
    set(context, count, viewports);
  else
    context->RSSetViewports(count, viewports);
}

bool PatchCallSites() {
  for (uintptr_t site : kVramMoveCopySites) {
    const uintptr_t address = g_moduleBase + site;
    if (!VerifyBytes(address, kCopyCallEax, sizeof(kCopyCallEax),
                     "hd_vram_move_gpu copy", kCopyCallEcx))
      return false;
  }
  const uintptr_t restore =
      g_moduleBase + rde::gpu_state_restore_plus_4b;
  if (!VerifyBytes(restore, kRestoreViewportsCall,
                   sizeof(kRestoreViewportsCall), "gpu_state_restore"))
    return false;
  for (uintptr_t site : kVramMoveCopySites)
    InstallCallHook(g_moduleBase + site, (void *)VramMoveCopy,
                    sizeof(kCopyCallEax));
  InstallCallHook(restore, (void *)RestoreViewports,
                  sizeof(kRestoreViewportsCall));
  return true;
}
} // namespace

// Re-entrancy guard: internal CreateTexture2D calls (replacement loading,
// staging texture creation) must bypass the hook to avoid recursive processing
static thread_local bool g_inCreateTexture2D = false;

HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D(
    ID3D11Device *This, const D3D11_TEXTURE2D_DESC *pDesc,
    const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D) {
  MemoryBarrier();
  CreateTexture2D_t pOriginal = Original_CreateTexture2D;
  void *returnAddress = _ReturnAddress();

  if (!pOriginal)
    return E_FAIL;
  if (!g_createTextureHookReady || g_inCreateTexture2D)
    return pOriginal(This, pDesc, pInitialData, ppTexture2D);

  g_inCreateTexture2D = true;

  HRESULT hr;
  const bool colour = pDesc && !pInitialData &&
                      pDesc->Usage == D3D11_USAGE_DEFAULT &&
                      IsColorFormat(pDesc->Format);
  const bool vramSized =
      colour && pDesc->Width == kBaseWidth && pDesc->Height == kBaseHeight;
  const bool scaled = vramSized || (colour && CalledFromVramMove(returnAddress));

  if (scaled) {
    D3D11_TEXTURE2D_DESC newDesc = *pDesc;
    newDesc.Width = std::max(1u, ScaleUnits(pDesc->Width));
    newDesc.Height = std::max(1u, ScaleUnits(pDesc->Height));
    hr = pOriginal(This, &newDesc, pInitialData, ppTexture2D);
    // An MSAA target too large for the video memory keeps fewer samples
    while (FAILED(hr) && newDesc.SampleDesc.Count > 2) {
      newDesc.SampleDesc.Count /= 2;
      newDesc.SampleDesc.Quality = 0;
      hr = pOriginal(This, &newDesc, pInitialData, ppTexture2D);
    }
    if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D)
      TagScaledTexture(*ppTexture2D);
    else if (vramSized)
      std::cout << "[Upscale] Could not create the " << newDesc.Width << "x"
                << newDesc.Height << " VRAM target (0x" << std::hex << hr
                << std::dec << ")" << std::endl;
  } else {
    if (pDesc && ppTexture2D &&
        TryLoadReplacementTexture(This, pDesc, pInitialData, ppTexture2D)) {
      g_inCreateTexture2D = false;
      return S_OK;
    }
    hr = pOriginal(This, pDesc, pInitialData, ppTexture2D);
  }

  g_inCreateTexture2D = false;
  return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_CreatePixelShader_Upscale(
    ID3D11Device *This, const void *pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage *pClassLinkage, ID3D11PixelShader **ppPixelShader) {
  MemoryBarrier();
  CreatePixelShader_t pOriginal = Original_CreatePixelShader;
  if (!pOriginal)
    return E_FAIL;

  const void *uploadShader = reinterpret_cast<const void *>(
      g_moduleBase + rde::shader_vram_decode_ps);
  if (g_createPixelShaderHookReady && ppPixelShader &&
      pShaderBytecode == uploadShader &&
      BytecodeLength == rde::shader_vram_decode_ps_size) {
    if (ID3D11PixelShader *shader = CompileUploadShader(This, pClassLinkage)) {
      *ppPixelShader = shader;
      return S_OK;
    }
    std::cout << "[Upscale] Upload shader unavailable, CPU uploads into the "
                 "VRAM target will be misplaced"
              << std::endl;
  }
  return pOriginal(This, pShaderBytecode, BytecodeLength, pClassLinkage,
                   ppPixelShader);
}

void RunFirstTimeSetup() {
  std::string settingsPath = Settings::GetSettingsPath();

  Settings settings;
  settings.Load(settingsPath);

  if (settings.GetBool("upscale_setup_completed", false)) {
    SetEvent(g_setupCompleteEvent);
    return;
  }

  // Under Wine the upscaler doesn't work; skip interactive setup and keep the
  // game's own scale
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll && GetProcAddress(ntdll, "wine_get_version")) {
    settings.UpdateFile(settingsPath, "resolution_scale",
                        std::to_string(kGameScale));
    settings.UpdateFile(settingsPath, "upscale_setup_completed", "1");
    SetEvent(g_setupCompleteEvent);
    return;
  }

  std::cout << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "       CrossFix - First Run Setup       " << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << std::endl;
  std::cout << "Resolution scale: 1-16 (4 is RDE's default)" << std::endl;
  std::cout << "You can change this later in settings.ini" << std::endl;
  std::cout << std::endl;
  std::cout << "Enter scale (1-16) [Enter = 4]: ";

  std::string line;
  std::getline(std::cin, line);
  int scaleChoice = 0;
  if (!line.empty()) {
    std::istringstream iss(line);
    iss >> scaleChoice;
  }
  int scale;
  if (scaleChoice >= kMinScale && scaleChoice <= kMaxScale) {
    scale = scaleChoice;
  } else {
    if (!line.empty())
      std::cout << "Invalid choice, using 4" << std::endl;
    scale = kGameScale;
  }

  settings.UpdateFile(settingsPath, "resolution_scale", std::to_string(scale));
  settings.UpdateFile(settingsPath, "upscale_setup_completed", "1");

  std::cout << std::endl;
  std::cout << "Settings saved to settings.ini" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << std::endl;

  SetEvent(g_setupCompleteEvent);
}

void ApplyUpscale4KPatch(ID3D11Device *pDevice, ID3D11DeviceContext *pContext) {
  if (!pContext || !pDevice)
    return;

  static volatile LONG applied = 0;
  if (InterlockedCompareExchange(&applied, 1, 0) != 0)
    return;

  // Wait for MainThread's RunFirstTimeSetup() to finish so we read the
  // user's chosen scale (and block the game from rendering until then).
  WaitForSingleObject(g_setupCompleteEvent, INFINITE);

  try {
    std::string settingsPath = Settings::GetSettingsPath();

    Settings settings;
    settings.Load(settingsPath);

    // Dump mode: skip upscale (dump needs unmodified textures)
    if (settings.GetBool("texture_dump_enabled", false))
      return;

    const int requested =
        std::clamp(ReadScale(settings), kMinScale, kMaxScale);
    if (requested == kGameScale)
      return;

    const int scale = ProbeScale(pDevice, requested);
    if (scale != requested)
      std::cout << "[Upscale] " << requested
                << "x does not fit the video memory, using " << scale << "x"
                << std::endl;
    if (scale == kGameScale)
      return;
    g_scale = scale;
    g_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));

    if (!PatchCallSites())
      return;

    void **contextVtable = *(void ***)pContext;
    void **deviceVtable = *(void ***)pDevice;

    if (!contextVtable || !deviceVtable)
      return;

    InstallVtableHook(contextVtable, 50, kRSSetViewportsIndex,
                      (void *)Hooked_RSSetViewports,
                      (volatile void **)&Original_RSSetViewports,
                      &g_viewportsHookReady);

    InstallVtableHook(deviceVtable, 50, kCreatePixelShaderIndex,
                      (void *)Hooked_CreatePixelShader_Upscale,
                      (volatile void **)&Original_CreatePixelShader,
                      &g_createPixelShaderHookReady);

    InstallVtableHook(deviceVtable, 10, kCreateTexture2DIndex,
                      (void *)Hooked_CreateTexture2D,
                      (volatile void **)&Original_CreateTexture2D,
                      &g_createTextureHookReady);

    g_upscaleActive = true;
    std::cout << "[Mod] Resolution scale " << scale << "x ("
              << kBaseWidth * scale / kGameScale << "x"
              << kBaseHeight * scale / kGameScale << " VRAM target)"
              << std::endl;

    Sleep(1);
  } catch (...) {
  }
}

bool IsUpscaleActive() { return g_upscaleActive; }
float GetUpscaledWidth() {
  return static_cast<float>(kBaseWidth * g_scale / kGameScale);
}
float GetUpscaledHeight() {
  return static_cast<float>(kBaseHeight * g_scale / kGameScale);
}
