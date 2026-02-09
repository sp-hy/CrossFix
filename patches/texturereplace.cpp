#include "texturereplace.h"
#include "../utils/settings.h"
#include "texturedump.h" // For HashTexture
#include <Windows.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

// Files that failed to load this session - skip so we don't retry every texture
static std::set<std::string> g_failedReplacementFiles;

namespace {
bool g_textureReplaceEnabled = false;
bool g_settingsLoaded = false;
std::string g_modsPath;
bool g_cacheBuilt = false;
// Exact lookup: (width, height, hash) -> full path
std::map<std::tuple<UINT, UINT, uint64_t>, std::string> g_replacementByHash;
// Fallback for render targets: (width, height) -> list of paths (WxH_*.dds)
std::map<std::pair<UINT, UINT>, std::vector<std::string>> g_replacementBySize;

// Initialize mods path
void InitializeModsPath() {
  if (!g_modsPath.empty())
    return;

  char exePath[MAX_PATH];
  if (GetModuleFileNameA(NULL, exePath, MAX_PATH) != 0) {
    std::string exePathStr(exePath);
    size_t lastBackslash = exePathStr.find_last_of("\\/");
    if (lastBackslash != std::string::npos) {
      g_modsPath = exePathStr.substr(0, lastBackslash + 1) + "mods\\textures";
    } else {
      g_modsPath = "mods\\textures";
    }
  } else {
    g_modsPath = "mods\\textures";
  }

  // Create directory if it doesn't exist
  CreateDirectoryA((g_modsPath.substr(0, g_modsPath.find_last_of("\\"))).c_str(), NULL);
  CreateDirectoryA(g_modsPath.c_str(), NULL);
}

// Parse "WxH_<16hex>.dds" or "WxH_<16hex>.png" filename.
bool ParseReplacementFilename(const std::string &filename, UINT *outW,
                              UINT *outH, uint64_t *outHash) {
  unsigned int w = 0, h = 0;
  if (sscanf_s(filename.c_str(), "%ux%u_", &w, &h) != 2)
    return false;
  if (w == 0 || h == 0)
    return false;
  size_t pos = filename.find('_');
  if (pos == std::string::npos || filename.size() < pos + 17)
    return false;
  // Accept .dds or .png extension
  bool isDds = filename.size() >= 4 &&
               filename.compare(filename.size() - 4, 4, ".dds") == 0;
  bool isPng = filename.size() >= 4 &&
               filename.compare(filename.size() - 4, 4, ".png") == 0;
  if (!isDds && !isPng)
    return false;
  const char *hex = filename.c_str() + pos + 1;
  uint64_t hash = 0;
  for (int i = 0; i < 16 && hex[i]; i++) {
    int nibble = 0;
    if (hex[i] >= '0' && hex[i] <= '9')
      nibble = hex[i] - '0';
    else if (hex[i] >= 'a' && hex[i] <= 'f')
      nibble = hex[i] - 'a' + 10;
    else if (hex[i] >= 'A' && hex[i] <= 'F')
      nibble = hex[i] - 'A' + 10;
    else
      return false;
    hash = (hash << 4) | nibble;
  }
  *outW = w;
  *outH = h;
  *outHash = hash;
  return true;
}

void ScanReplacementFiles(const std::string &pattern) {
  WIN32_FIND_DATAA fd;
  HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE)
    return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    UINT w, h;
    uint64_t hash;
    if (ParseReplacementFilename(fd.cFileName, &w, &h, &hash)) {
      std::string fullPath = g_modsPath + "\\" + fd.cFileName;
      g_replacementByHash[{w, h, hash}] = fullPath;
    }
  } while (FindNextFileA(hFind, &fd));
  FindClose(hFind);
}

void BuildReplacementCache() {
  if (g_cacheBuilt || g_modsPath.empty())
    return;
  g_cacheBuilt = true;
  ScanReplacementFiles(g_modsPath + "\\*.dds");
  ScanReplacementFiles(g_modsPath + "\\*.png");
  std::cout << "Texture replacement cache: " << g_replacementByHash.size()
            << " file(s) in mods/textures" << std::endl;
}

// Simple DDS header structure (minimal for reading)
#pragma pack(push, 1)
struct DDSHeader {
  DWORD dwMagic;      // "DDS "
  DWORD dwSize;       // 124
  DWORD dwFlags;
  DWORD dwHeight;
  DWORD dwWidth;
  DWORD dwPitchOrLinearSize;
  DWORD dwDepth;
  DWORD dwMipMapCount;
  DWORD dwReserved1[11];
  struct {
    DWORD dwSize;     // 32
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwABitMask;
  } ddspf;
  DWORD dwCaps;
  DWORD dwCaps2;
  DWORD dwCaps3;
  DWORD dwCaps4;
  DWORD dwReserved2;
};
#pragma pack(pop)

// Load DDS file and create texture
bool LoadDDSTexture(ID3D11Device *pDevice, const std::string &filepath,
                    const D3D11_TEXTURE2D_DESC *pOriginalDesc,
                    ID3D11Texture2D **ppTexture2D) {
  std::ifstream file(filepath, std::ios::binary);
  if (!file.is_open())
    return false;

  DDSHeader header;
  file.read(reinterpret_cast<char *>(&header), sizeof(header));

  if (header.dwMagic != 0x20534444) { // "DDS "
    file.close();
    return false;
  }

  // Verify dimensions match
  if (header.dwWidth != pOriginalDesc->Width ||
      header.dwHeight != pOriginalDesc->Height) {
    std::cout << "Replacement texture size mismatch: expected "
              << pOriginalDesc->Width << "x" << pOriginalDesc->Height
              << ", got " << header.dwWidth << "x" << header.dwHeight
              << std::endl;
    file.close();
    return false;
  }

  // Determine format and bytes per pixel
  DXGI_FORMAT format = pOriginalDesc->Format;
  UINT bytesPerPixel = 0;
  bool isBlockCompressed = false;
  UINT bytesPerBlock = 0;

  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    bytesPerPixel = 4;
    break;
  case DXGI_FORMAT_B4G4R4A4_UNORM:
    bytesPerPixel = 2;
    break;
  case DXGI_FORMAT_R8_UNORM:
    bytesPerPixel = 1;
    break;
  case DXGI_FORMAT_R8G8_UNORM:
    bytesPerPixel = 2;
    break;
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
    isBlockCompressed = true;
    bytesPerBlock = 8;
    break;
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    isBlockCompressed = true;
    bytesPerBlock = 16;
    break;
  default:
    std::cout << "Replacement skipped - unsupported texture format: "
              << static_cast<unsigned>(format) << " ("
              << pOriginalDesc->Width << "x" << pOriginalDesc->Height << ")"
              << std::endl;
    file.close();
    return false;
  }

  // Calculate data size (first mip only - we create texture with one level)
  size_t dataSize;
  UINT rowPitch;

  if (isBlockCompressed) {
    UINT blocksWide = (header.dwWidth + 3) / 4;
    UINT blocksHigh = (header.dwHeight + 3) / 4;
    rowPitch = blocksWide * bytesPerBlock;
    dataSize = rowPitch * blocksHigh;
  } else {
    rowPitch = header.dwWidth * bytesPerPixel;
    dataSize = rowPitch * header.dwHeight;
  }

  // Check file has enough bytes (avoids "Failed to read complete" when file is
  // different format, has only mips, or is truncated)
  const size_t headerSize = sizeof(header);
  file.seekg(0, std::ios::end);
  const size_t totalSize = static_cast<size_t>(file.tellg());
  file.seekg(static_cast<std::streamoff>(headerSize), std::ios::beg);
  if (totalSize <= headerSize || (totalSize - headerSize) < dataSize) {
    file.close();
    return false; // File too small for this format (e.g. different DDS format)
  }

  // Read pixel data
  std::vector<uint8_t> pixelData(dataSize);
  file.read(reinterpret_cast<char *>(pixelData.data()), dataSize);
  file.close();

  if (file.gcount() != static_cast<std::streamsize>(dataSize)) {
    return false; // Truncated read - skip (caller may blacklist)
  }

  // Create texture with replacement data
  D3D11_SUBRESOURCE_DATA initData;
  initData.pSysMem = pixelData.data();
  initData.SysMemPitch = rowPitch;
  initData.SysMemSlicePitch = 0;

  HRESULT hr = pDevice->CreateTexture2D(pOriginalDesc, &initData, ppTexture2D);
  if (FAILED(hr)) {
    std::cout << "Failed to create replacement texture from " << filepath
              << std::endl;
    return false;
  }

  return true;
}

// Load PNG via Windows Imaging Component (WIC)
// Supports BGRA8 and RGBA8 original formats
bool LoadPNGTexture(ID3D11Device *pDevice, const std::string &filepath,
                    const D3D11_TEXTURE2D_DESC *pOriginalDesc,
                    ID3D11Texture2D **ppTexture2D) {
  DXGI_FORMAT format = pOriginalDesc->Format;
  bool isBGRA = (format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                 format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
  bool isRGBA = (format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                 format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

  if (!isBGRA && !isRGBA) {
    std::cout << "PNG replacement skipped - unsupported format "
              << static_cast<unsigned>(format) << " for " << filepath
              << std::endl;
    return false;
  }

  // Convert path to wide string for WIC
  int wlen = MultiByteToWideChar(CP_ACP, 0, filepath.c_str(), -1, nullptr, 0);
  if (wlen <= 0)
    return false;
  std::vector<wchar_t> wpath(wlen);
  MultiByteToWideChar(CP_ACP, 0, filepath.c_str(), -1, wpath.data(), wlen);

  HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  ComPtr<IWICImagingFactory> pFactory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
  if (FAILED(hr)) {
    if (SUCCEEDED(hrCo) || hrCo == S_FALSE)
      CoUninitialize();
    return false;
  }

  ComPtr<IWICBitmapDecoder> pDecoder;
  hr = pFactory->CreateDecoderFromFilename(
      wpath.data(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
      &pDecoder);
  if (FAILED(hr)) {
    if (SUCCEEDED(hrCo) || hrCo == S_FALSE)
      CoUninitialize();
    return false;
  }

  ComPtr<IWICBitmapFrameDecode> pFrame;
  hr = pDecoder->GetFrame(0, &pFrame);
  if (FAILED(hr)) {
    if (SUCCEEDED(hrCo) || hrCo == S_FALSE)
      CoUninitialize();
    return false;
  }

  UINT width, height;
  pFrame->GetSize(&width, &height);

  if (width != pOriginalDesc->Width || height != pOriginalDesc->Height) {
    std::cout << "PNG replacement size mismatch: expected "
              << pOriginalDesc->Width << "x" << pOriginalDesc->Height
              << ", got " << width << "x" << height << std::endl;
    if (SUCCEEDED(hrCo) || hrCo == S_FALSE)
      CoUninitialize();
    return false;
  }

  // Convert to matching pixel format
  GUID targetWicFormat =
      isBGRA ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat32bppRGBA;

  ComPtr<IWICFormatConverter> pConverter;
  hr = pFactory->CreateFormatConverter(&pConverter);
  if (FAILED(hr)) {
    if (SUCCEEDED(hrCo) || hrCo == S_FALSE)
      CoUninitialize();
    return false;
  }

  hr = pConverter->Initialize(pFrame.Get(), targetWicFormat,
                              WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    if (SUCCEEDED(hrCo) || hrCo == S_FALSE)
      CoUninitialize();
    return false;
  }

  UINT rowPitch = width * 4;
  UINT imageSize = rowPitch * height;
  std::vector<uint8_t> pixels(imageSize);

  hr = pConverter->CopyPixels(nullptr, rowPitch, imageSize, pixels.data());
  if (FAILED(hr)) {
    if (SUCCEEDED(hrCo) || hrCo == S_FALSE)
      CoUninitialize();
    return false;
  }

  if (SUCCEEDED(hrCo) || hrCo == S_FALSE)
    CoUninitialize();

  // Create texture
  D3D11_TEXTURE2D_DESC createDesc = *pOriginalDesc;
  createDesc.MipLevels = 1;

  D3D11_SUBRESOURCE_DATA initData = {};
  initData.pSysMem = pixels.data();
  initData.SysMemPitch = rowPitch;

  hr = pDevice->CreateTexture2D(&createDesc, &initData, ppTexture2D);
  if (FAILED(hr)) {
    std::cout << "Failed to create PNG replacement texture from " << filepath
              << std::endl;
    return false;
  }

  return true;
}

bool IsReplacementFormatSupported(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B4G4R4A4_UNORM:
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8G8_UNORM:
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    return true;
  default:
    return false;
  }
}

} // namespace

bool IsTextureReplacementEnabled() {
  if (g_settingsLoaded)
    return g_textureReplaceEnabled;

  g_settingsLoaded = true;

  Settings settings;
  settings.Load(Settings::GetSettingsPath());

  // Dump mode: disable replacement (dump needs unmodified textures)
  if (settings.GetBool("texture_dump_enabled", false)) {
    g_textureReplaceEnabled = false;
    return false;
  }

  g_textureReplaceEnabled =
      settings.GetBool("texture_replace_enabled", false);

  if (g_textureReplaceEnabled) {
    InitializeModsPath();
    BuildReplacementCache();
    std::cout << "Texture replacement enabled. Mods path: " << g_modsPath
              << std::endl;
  }

  return g_textureReplaceEnabled;
}

bool TryLoadReplacementTexture(ID3D11Device *pDevice,
                                const D3D11_TEXTURE2D_DESC *pDesc,
                                const D3D11_SUBRESOURCE_DATA *pInitialData,
                                ID3D11Texture2D **ppTexture2D) {
  if (!IsTextureReplacementEnabled() || !pDevice || !pDesc || !ppTexture2D)
    return false;
  if (!IsReplacementFormatSupported(pDesc->Format))
    return false; // Avoid trying files we can't load for this format

  UINT w = pDesc->Width, h = pDesc->Height;
  uint64_t hash = HashTexture(pDesc, pInitialData);

  // Exact lookup from preloaded cache
  auto it = g_replacementByHash.find({w, h, hash});
  if (it != g_replacementByHash.end()) {
    const std::string &filepath = it->second;
    if (g_failedReplacementFiles.count(filepath))
      return false;

    bool loaded = false;
    // Choose loader by file extension
    if (filepath.size() >= 4 &&
        filepath.compare(filepath.size() - 4, 4, ".png") == 0) {
      loaded = LoadPNGTexture(pDevice, filepath, pDesc, ppTexture2D);
    } else {
      loaded = LoadDDSTexture(pDevice, filepath, pDesc, ppTexture2D);
    }

    if (loaded) {
      size_t nameStart = filepath.find_last_of("\\/");
      std::string name = (nameStart != std::string::npos)
                             ? filepath.substr(nameStart + 1)
                             : filepath;
      std::cout << "Replaced texture: " << name << std::endl;
      return true;
    }
    g_failedReplacementFiles.insert(filepath);
  }

  return false;
}

std::string FindReplacementPath(UINT width, UINT height,
                                uint64_t contentHash) {
  if (!IsTextureReplacementEnabled())
    return "";
  auto it = g_replacementByHash.find({width, height, contentHash});
  if (it != g_replacementByHash.end()) {
    if (!g_failedReplacementFiles.count(it->second))
      return it->second;
  }
  return "";
}

bool LoadReplacementSRV(ID3D11Device *pDevice,
                        const D3D11_TEXTURE2D_DESC *pDesc,
                        uint64_t contentHash,
                        ID3D11ShaderResourceView **ppSRV) {
  if (!pDevice || !pDesc || !ppSRV)
    return false;
  *ppSRV = nullptr;

  std::string path =
      FindReplacementPath(pDesc->Width, pDesc->Height, contentHash);
  if (path.empty())
    return false;

  D3D11_TEXTURE2D_DESC createDesc = *pDesc;
  createDesc.MipLevels = 1;

  ComPtr<ID3D11Texture2D> pReplaceTex;
  bool loaded = false;

  if (path.size() >= 4 &&
      path.compare(path.size() - 4, 4, ".png") == 0) {
    loaded = LoadPNGTexture(pDevice, path, &createDesc,
                            pReplaceTex.GetAddressOf());
  } else {
    loaded = LoadDDSTexture(pDevice, path, &createDesc,
                            pReplaceTex.GetAddressOf());
  }

  if (!loaded || !pReplaceTex) {
    g_failedReplacementFiles.insert(path);
    return false;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format = pDesc->Format;
  srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Texture2D.MostDetailedMip = 0;
  srvDesc.Texture2D.MipLevels = 1;

  HRESULT hr =
      pDevice->CreateShaderResourceView(pReplaceTex.Get(), &srvDesc, ppSRV);
  if (FAILED(hr)) {
    g_failedReplacementFiles.insert(path);
    return false;
  }

  size_t nameStart = path.find_last_of("\\/");
  std::string name = (nameStart != std::string::npos)
                         ? path.substr(nameStart + 1)
                         : path;
  std::cout << "Replaced texture (bind-time): " << name << std::endl;
  return true;
}
