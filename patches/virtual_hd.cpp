#include "virtual_hd.h"
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static constexpr uint32_t EOCD_SIGNATURE = 0x06054b50;
static constexpr uint32_t CD_SIGNATURE   = 0x02014b50;
static constexpr uint32_t LFH_SIGNATURE  = 0x04034b50;
static constexpr uint32_t EOCD_FIXED_SIZE = 22;
static constexpr uint32_t CD_FIXED_SIZE   = 46;
static constexpr uint32_t LFH_FIXED_SIZE  = 30;

// CRC32
static uint32_t s_crc32Table[256];
static bool s_crc32TableInit = false;

static void InitCrc32Table() {
    if (s_crc32TableInit) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1)));
        s_crc32Table[i] = crc;
    }
    s_crc32TableInit = true;
}

static uint32_t UpdateCrc32(uint32_t crc, const void* data, size_t size) {
    const uint8_t* buf = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < size; i++)
        crc = s_crc32Table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static bool ReadAt(HANDLE hFile, uint64_t offset, void* buffer, uint32_t size, uint32_t* bytesRead) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) return false;
    DWORD dwRead = 0;
    if (!ReadFile(hFile, buffer, size, &dwRead, NULL)) return false;
    if (bytesRead) *bytesRead = dwRead;
    return true;
}

static uint16_t ReadU16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t ReadU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void WriteU16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
}
static void WriteU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

VirtualHd::VirtualHd()
    : m_cdOffset(0), m_cdSize(0), m_eocdOffset(0),
      m_virtualSize(0), m_virtualCdOffset(0), m_built(false) {
    InitCrc32Table();
}

VirtualHd::~VirtualHd() {}

bool VirtualHd::Build(HANDLE realHdDat, const std::string& modsDir) {
    if (!ParseRealZip(realHdDat)) {
        std::cout << "[ModLoader] Failed to parse real hd.dat ZIP structure" << std::endl;
        return false;
    }
    std::cout << "[ModLoader] Parsed " << m_entries.size() << " entries from hd.dat" << std::endl;

    ScanMods(modsDir);
    ComputeLayout();

    m_built = true;
    std::cout << "[ModLoader] Virtual layout computed: " << m_virtualSize << " bytes" << std::endl;
    return true;
}

bool VirtualHd::HasMods() const {
    for (const auto& ze : m_entries)
        if (ze.isModded) return true;
    return false;
}

void VirtualHd::ComputeLayout() {
    uint64_t offset = 0;

    for (auto& ze : m_entries) {
        ze.virtualLocalHeaderOffset = offset;

        if (ze.isModded) {
            // New LFH: 30 + filename length (no extra field) + mod file data
            uint32_t lfhSize = LFH_FIXED_SIZE + (uint32_t)ze.filename.size();
            ze.virtualEntryTotalSize = lfhSize + ze.moddedFileSize;
        } else {
            // Same as real file: LFH + name + extra + compressed data
            ze.virtualEntryTotalSize = ze.realEntryTotalSize;
        }

        offset += ze.virtualEntryTotalSize;
    }

    // Central Directory follows all entries
    m_virtualCdOffset = offset;

    // CD size: for unmodded entries use original raw size, for modded synthesize
    uint64_t cdSize = 0;
    for (const auto& ze : m_entries) {
        if (ze.isModded) {
            cdSize += CD_FIXED_SIZE + ze.filename.size();
        } else {
            cdSize += ze.cdEntrySize;
        }
    }

    offset += cdSize;
    offset += EOCD_FIXED_SIZE;

    m_virtualSize = offset;
}

// Maximum virtual size we support: 4GB - 1 (ZIP format uses 32-bit local header offset in CD; 32-bit process can't map larger anyway)
static constexpr uint64_t MAX_VIRTUAL_SIZE = 0xFFFFFFFFULL;

uint8_t* VirtualHd::CreateVirtualView(const uint8_t* realMappedBase) {
    if (!m_built) return nullptr;

    if (m_virtualSize > MAX_VIRTUAL_SIZE) {
        std::cout << "[ModLoader] Virtual archive size " << m_virtualSize << " exceeds 4GB limit (ZIP 32-bit offsets / 32-bit process limit). Mods disabled for this archive." << std::endl;
        return nullptr;
    }

    uint8_t* buf = nullptr;
    // Try VirtualAlloc first (fast path for small/medium sizes)
    if (m_virtualSize <= SIZE_T(-1)) {
        buf = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)m_virtualSize,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    // Fallback: page-file-backed mapping for large sizes (avoids contiguous reserve failure)
    if (!buf) {
        std::cout << "[ModLoader] VirtualAlloc failed for " << m_virtualSize << " bytes, trying file mapping..." << std::endl;
        HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            (DWORD)(m_virtualSize >> 32), (DWORD)(m_virtualSize & 0xFFFFFFFF), NULL);
        if (hMap) {
            buf = (uint8_t*)MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0);
            CloseHandle(hMap);  // view remains valid
            if (!buf)
                std::cout << "[ModLoader] MapViewOfFile failed: " << GetLastError() << std::endl;
        } else {
            std::cout << "[ModLoader] CreateFileMapping failed: " << GetLastError() << std::endl;
        }
    }
    if (!buf) {
        std::cout << "[ModLoader] Could not allocate virtual view for " << m_virtualSize << " bytes" << std::endl;
        return nullptr;
    }

    // Phase 1: Write local file headers + data
    for (auto& ze : m_entries) {
        uint8_t* dst = buf + ze.virtualLocalHeaderOffset;

        if (ze.isModded) {
            uint16_t nameLen = (uint16_t)ze.filename.size();
            uint8_t* dataStart = dst + LFH_FIXED_SIZE + nameLen;

            // Single read + CRC in memory (avoids double-read: ScanMods no longer calls ComputeCrc32)
            std::ifstream modFile(ze.modFilePath, std::ios::binary);
            if (modFile.is_open()) {
                modFile.read((char*)dataStart, ze.moddedFileSize);
                ze.moddedCrc32 = UpdateCrc32(0, dataStart, ze.moddedFileSize);
                modFile.close();
            } else {
                std::cout << "[ModLoader] ERROR: Cannot read mod file: " << ze.modFilePath << std::endl;
            }

            // Synthesize new LFH (stored/uncompressed)
            WriteU32(dst + 0,  LFH_SIGNATURE);
            WriteU16(dst + 4,  20);                    // version needed
            WriteU16(dst + 6,  0);                     // flags
            WriteU16(dst + 8,  0);                     // compression: stored
            WriteU16(dst + 10, ze.lastModTime);
            WriteU16(dst + 12, ze.lastModDate);
            WriteU32(dst + 14, ze.moddedCrc32);
            WriteU32(dst + 18, ze.moddedFileSize);     // compressed = uncompressed
            WriteU32(dst + 22, ze.moddedFileSize);
            WriteU16(dst + 26, nameLen);
            WriteU16(dst + 28, 0);                     // no extra field
            memcpy(dst + LFH_FIXED_SIZE, ze.filename.data(), nameLen);
        } else {
            // Copy entire entry (LFH + name + extra + data) from real mapped file
            memcpy(dst, realMappedBase + ze.localHeaderOffset, ze.realEntryTotalSize);
        }
    }

    // Phase 2: Write Central Directory
    uint8_t* cdStart = buf + m_virtualCdOffset;
    uint64_t cdPos = 0;

    for (const auto& ze : m_entries) {
        uint8_t* p = cdStart + cdPos;

        if (ze.isModded) {
            uint16_t nameLen = (uint16_t)ze.filename.size();
            uint32_t cdEntrySize = CD_FIXED_SIZE + nameLen;

            WriteU32(p + 0,  CD_SIGNATURE);
            WriteU16(p + 4,  ze.versionMadeBy);
            WriteU16(p + 6,  20);                       // version needed
            WriteU16(p + 8,  0);                         // flags
            WriteU16(p + 10, 0);                         // compression: stored
            WriteU16(p + 12, ze.lastModTime);
            WriteU16(p + 14, ze.lastModDate);
            WriteU32(p + 16, ze.moddedCrc32);
            WriteU32(p + 20, ze.moddedFileSize);
            WriteU32(p + 24, ze.moddedFileSize);
            WriteU16(p + 28, nameLen);
            WriteU16(p + 30, 0);                         // extra field length
            WriteU16(p + 32, 0);                         // comment length
            WriteU16(p + 34, 0);                         // disk number
            WriteU16(p + 36, ze.internalAttrs);
            WriteU32(p + 38, ze.externalAttrs);
            WriteU32(p + 42, (uint32_t)ze.virtualLocalHeaderOffset);
            memcpy(p + CD_FIXED_SIZE, ze.filename.data(), nameLen);

            cdPos += cdEntrySize;
        } else {
            // Copy original raw CD entry, patch the local header offset
            memcpy(p, m_rawCd.data() + ze.cdEntryOffset, ze.cdEntrySize);
            WriteU32(p + 42, (uint32_t)ze.virtualLocalHeaderOffset);
            cdPos += ze.cdEntrySize;
        }
    }

    uint32_t totalCdSize = (uint32_t)cdPos;

    // Phase 3: Write EOCD
    uint8_t* eocd = cdStart + cdPos;
    uint16_t numEntries = (uint16_t)m_entries.size();
    WriteU32(eocd + 0,  EOCD_SIGNATURE);
    WriteU16(eocd + 4,  0);                              // disk number
    WriteU16(eocd + 6,  0);                              // disk with CD
    WriteU16(eocd + 8,  numEntries);
    WriteU16(eocd + 10, numEntries);
    WriteU32(eocd + 12, totalCdSize);
    WriteU32(eocd + 16, (uint32_t)m_virtualCdOffset);
    WriteU16(eocd + 20, 0);                              // comment length

    std::cout << "[ModLoader] Virtual view built: " << m_virtualSize << " bytes" << std::endl;
    return buf;
}

bool VirtualHd::ParseRealZip(HANDLE realHdDat) {
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(realHdDat, &fileSize)) return false;
    uint64_t realSize = (uint64_t)fileSize.QuadPart;

    uint32_t searchSize = (uint32_t)(std::min)(realSize, (uint64_t)(EOCD_FIXED_SIZE + 65535));
    std::vector<uint8_t> tailBuf(searchSize);
    uint32_t readCount = 0;
    if (!ReadAt(realHdDat, realSize - searchSize, tailBuf.data(), searchSize, &readCount))
        return false;

    int64_t eocdPos = -1;
    for (int64_t i = (int64_t)searchSize - EOCD_FIXED_SIZE; i >= 0; i--) {
        if (ReadU32(&tailBuf[i]) == EOCD_SIGNATURE) { eocdPos = i; break; }
    }
    if (eocdPos < 0) {
        std::cout << "[ModLoader] EOCD signature not found" << std::endl;
        return false;
    }

    m_eocdOffset = (uint32_t)(realSize - searchSize + eocdPos);
    const uint8_t* eocd = &tailBuf[eocdPos];
    uint16_t numEntries = ReadU16(eocd + 10);
    m_cdSize   = ReadU32(eocd + 12);
    m_cdOffset = ReadU32(eocd + 16);

    std::cout << "[ModLoader] EOCD: " << numEntries << " entries, CD at " << m_cdOffset
              << ", size " << m_cdSize << std::endl;

    // Read and store the raw CD (we'll copy from it for unmodded entries)
    m_rawCd.resize(m_cdSize);
    if (!ReadAt(realHdDat, m_cdOffset, m_rawCd.data(), m_cdSize, &readCount))
        return false;

    m_entries.clear();
    m_entries.reserve(numEntries);
    uint32_t pos = 0;

    for (uint16_t i = 0; i < numEntries; i++) {
        if (pos + CD_FIXED_SIZE > m_cdSize) return false;
        const uint8_t* entry = &m_rawCd[pos];
        if (ReadU32(entry) != CD_SIGNATURE) return false;

        ZipEntry ze = {};
        ze.versionMadeBy     = ReadU16(entry + 4);
        ze.versionNeeded     = ReadU16(entry + 6);
        ze.generalPurposeFlag = ReadU16(entry + 8);
        ze.compressionMethod = ReadU16(entry + 10);
        ze.lastModTime       = ReadU16(entry + 12);
        ze.lastModDate       = ReadU16(entry + 14);
        ze.crc32             = ReadU32(entry + 16);
        ze.compressedSize    = ReadU32(entry + 20);
        ze.uncompressedSize  = ReadU32(entry + 24);
        uint16_t nameLen     = ReadU16(entry + 28);
        ze.extraFieldLength  = ReadU16(entry + 30);
        ze.fileCommentLength = ReadU16(entry + 32);
        ze.diskNumberStart   = ReadU16(entry + 34);
        ze.internalAttrs     = ReadU16(entry + 36);
        ze.externalAttrs     = ReadU32(entry + 38);
        ze.localHeaderOffset = ReadU32(entry + 42);

        if (pos + CD_FIXED_SIZE + nameLen > m_cdSize) return false;
        ze.filename = std::string((const char*)(entry + CD_FIXED_SIZE), nameLen);

        ze.cdEntryOffset = pos;
        ze.cdEntrySize = CD_FIXED_SIZE + nameLen + ze.extraFieldLength + ze.fileCommentLength;

        m_entries.push_back(std::move(ze));
        pos += m_entries.back().cdEntrySize;
    }

    // Read each LFH to get actual data offsets and entry sizes
    for (auto& ze : m_entries) {
        uint8_t lfhBuf[LFH_FIXED_SIZE];
        if (!ReadAt(realHdDat, ze.localHeaderOffset, lfhBuf, LFH_FIXED_SIZE, &readCount))
            return false;
        ze.lfhNameLength  = ReadU16(lfhBuf + 26);
        ze.lfhExtraLength = ReadU16(lfhBuf + 28);
        ze.dataOffset = ze.localHeaderOffset + LFH_FIXED_SIZE + ze.lfhNameLength + ze.lfhExtraLength;
        ze.realEntryTotalSize = LFH_FIXED_SIZE + ze.lfhNameLength + ze.lfhExtraLength + ze.compressedSize;
    }

    return true;
}

void VirtualHd::ScanMods(const std::string& modsDir) {
    if (!fs::exists(modsDir) || !fs::is_directory(modsDir)) return;

    int modCount = 0;
    for (auto& dirEntry : fs::recursive_directory_iterator(modsDir)) {
        if (!dirEntry.is_regular_file()) continue;

        std::string relStr = fs::relative(dirEntry.path(), modsDir).generic_string();

        for (auto& ze : m_entries) {
            if (ze.filename == relStr) {
                ze.isModded = true;
                ze.modFilePath = dirEntry.path().string();
                ze.moddedFileSize = (uint32_t)dirEntry.file_size();
                // CRC32 is computed lazily during CreateVirtualView to avoid double-reading
                ze.moddedCrc32 = 0;
                modCount++;
                std::cout << "[ModLoader] Mod: " << relStr << " ("
                          << ze.moddedFileSize << " bytes)" << std::endl;
                break;
            }
        }
    }
    std::cout << "[ModLoader] Found " << modCount << " mod file(s)" << std::endl;
}

