#include "virtual_hd.h"
#include <iostream>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;

static constexpr uint32_t EOCD_SIGNATURE     = 0x06054b50;
static constexpr uint32_t ZIP64_EOCD_SIGNATURE = 0x06064b50;
static constexpr uint32_t ZIP64_LOCATOR_SIGNATURE = 0x07064b50;
static constexpr uint32_t CD_SIGNATURE       = 0x02014b50;
static constexpr uint32_t LFH_SIGNATURE      = 0x04034b50;
static constexpr uint32_t EOCD_FIXED_SIZE    = 22;
static constexpr uint32_t ZIP64_LOCATOR_SIZE = 20;
static constexpr uint32_t ZIP64_EOCD_FIXED_SIZE = 56;  // up to and including CD offset
static constexpr uint32_t CD_FIXED_SIZE      = 46;
static constexpr uint32_t LFH_FIXED_SIZE     = 30;
static constexpr uint16_t ZIP64_EXTRA_ID     = 0x0001;

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

static bool ReadAt(HANDLE hFile, uint64_t offset, void* buffer, size_t size, size_t* bytesRead) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) return false;
    size_t remaining = size;
    uint8_t* dst = (uint8_t*)buffer;
    while (remaining > 0) {
        DWORD toRead = (DWORD)(remaining > 0x7FFFFFFFu ? 0x7FFFFFFFu : remaining);
        DWORD dwRead = 0;
        if (!ReadFile(hFile, dst, toRead, &dwRead, NULL)) return false;
        if (dwRead == 0) break;
        dst += dwRead;
        remaining -= dwRead;
        if (bytesRead) *bytesRead = size - remaining;
    }
    if (bytesRead) *bytesRead = size - remaining;
    return remaining == 0;
}

static uint16_t ReadU16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t ReadU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t ReadU64(const uint8_t* p) {
    return (uint64_t)ReadU32(p) | ((uint64_t)ReadU32(p + 4) << 32);
}
static void WriteU16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
}
static void WriteU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void WriteU64(uint8_t* p, uint64_t v) {
    WriteU32(p, (uint32_t)(v)); WriteU32(p + 4, (uint32_t)(v >> 32));
}

VirtualHd::VirtualHd()
    : m_cdOffset(0), m_cdSize(0), m_eocdOffset(0), m_virtualSize(0),
      m_virtualCdOffset(0), m_built(false) {
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
    // When Zip64: add 12 bytes per entry with virtualLocalHeaderOffset > 4GB
    static constexpr uint64_t MAX_32BIT = 0xFFFFFFFFULL;
    uint64_t cdSize = 0;
    for (const auto& ze : m_entries) {
        if (ze.isModded) {
            cdSize += CD_FIXED_SIZE + ze.filename.size();
            if (ze.virtualLocalHeaderOffset > MAX_32BIT)
                cdSize += 12;  // Zip64 extra (ID 0x0001, size 8, offset)
        } else {
            cdSize += ze.cdEntrySize;
            if (ze.virtualLocalHeaderOffset > MAX_32BIT)
                cdSize += 12;  // Zip64 extra for local header offset
        }
    }

    bool useZip64 = (m_virtualCdOffset > MAX_32BIT || cdSize > MAX_32BIT);
    offset += cdSize;
    if (useZip64)
        offset += ZIP64_EOCD_FIXED_SIZE + ZIP64_LOCATOR_SIZE;
    offset += EOCD_FIXED_SIZE;

    m_virtualSize = offset;
}

void VirtualHd::BuildSyntheticCDAndEOCD() {
    if (!m_syntheticCD.empty()) return;
    static constexpr uint64_t MAX_32BIT = 0xFFFFFFFFULL;

    uint64_t cdSize = 0;
    for (const auto& ze : m_entries) {
        if (ze.isModded)
            cdSize += CD_FIXED_SIZE + ze.filename.size() + (ze.virtualLocalHeaderOffset > MAX_32BIT ? 12u : 0u);
        else
            cdSize += ze.cdEntrySize + (ze.virtualLocalHeaderOffset > MAX_32BIT ? 12u : 0u);
    }
    bool useZip64 = (m_virtualCdOffset > MAX_32BIT || cdSize > MAX_32BIT);
    cdSize += useZip64 ? (ZIP64_EOCD_FIXED_SIZE + ZIP64_LOCATOR_SIZE) : 0;
    cdSize += EOCD_FIXED_SIZE;

    m_syntheticCD.resize((size_t)cdSize);
    uint8_t* cdStart = m_syntheticCD.data();
    uint64_t cdPos = 0;

    for (const auto& ze : m_entries) {
        uint8_t* p = cdStart + cdPos;
        if (ze.isModded) {
            uint16_t nameLen = (uint16_t)ze.filename.size();
            bool needsZip64Off = (ze.virtualLocalHeaderOffset > MAX_32BIT);
            uint16_t extraLen = needsZip64Off ? 12 : 0;
            WriteU32(p + 0,  CD_SIGNATURE);
            WriteU16(p + 4,  ze.versionMadeBy);
            WriteU16(p + 6,  45);  // version 4.5 for Zip64
            WriteU16(p + 8,  0);
            WriteU16(p + 10, 0);
            WriteU16(p + 12, ze.lastModTime);
            WriteU16(p + 14, ze.lastModDate);
            WriteU32(p + 16, ze.moddedCrc32);
            WriteU32(p + 20, (uint32_t)(ze.moddedFileSize > MAX_32BIT ? MAX_32BIT : ze.moddedFileSize));
            WriteU32(p + 24, (uint32_t)(ze.moddedFileSize > MAX_32BIT ? MAX_32BIT : ze.moddedFileSize));
            WriteU16(p + 28, nameLen);
            WriteU16(p + 30, extraLen);
            WriteU16(p + 32, 0);
            WriteU16(p + 34, 0);
            WriteU16(p + 36, ze.internalAttrs);
            WriteU32(p + 38, ze.externalAttrs);
            WriteU32(p + 42, (uint32_t)(needsZip64Off ? MAX_32BIT : ze.virtualLocalHeaderOffset));
            memcpy(p + CD_FIXED_SIZE, ze.filename.data(), nameLen);
            size_t entryPos = CD_FIXED_SIZE + nameLen;
            if (needsZip64Off) {
                WriteU16(p + entryPos, ZIP64_EXTRA_ID);
                WriteU16(p + entryPos + 2, 8);
                WriteU64(p + entryPos + 4, ze.virtualLocalHeaderOffset);
                entryPos += 12;
            }
            cdPos += entryPos;
        } else {
            const uint8_t* src = m_rawCd.data() + ze.cdEntryOffset;
            uint16_t nameLen = ReadU16(src + 28);
            uint16_t extraLen = ReadU16(src + 30);
            uint16_t commentLen = ReadU16(src + 32);
            if (ze.virtualLocalHeaderOffset > MAX_32BIT) {
                // Build entry with Zip64 extra appended to original extra field
                size_t beforeExtra = CD_FIXED_SIZE + nameLen;
                size_t extraTotal = extraLen + 12;
                memcpy(p, src, beforeExtra);
                WriteU32(p + 42, (uint32_t)MAX_32BIT);
                WriteU16(p + 30, (uint16_t)extraTotal);
                memcpy(p + beforeExtra, src + beforeExtra, extraLen);
                WriteU16(p + beforeExtra + extraLen, ZIP64_EXTRA_ID);
                WriteU16(p + beforeExtra + extraLen + 2, 8);
                WriteU64(p + beforeExtra + extraLen + 4, ze.virtualLocalHeaderOffset);
                memcpy(p + beforeExtra + extraTotal, src + beforeExtra + extraLen, commentLen);
                cdPos += beforeExtra + extraTotal + commentLen;
            } else {
                memcpy(p, src, ze.cdEntrySize);
                WriteU32(p + 42, (uint32_t)ze.virtualLocalHeaderOffset);
                cdPos += ze.cdEntrySize;
            }
        }
    }

    uint64_t totalCdSize = cdPos;
    if (useZip64) {
        uint8_t* z64 = cdStart + cdPos;
        WriteU32(z64 + 0,  ZIP64_EOCD_SIGNATURE);
        WriteU64(z64 + 4,  44);  // size of Zip64 EOCD record (56 - 12)
        WriteU16(z64 + 12, 45);
        WriteU16(z64 + 14, 45);
        WriteU32(z64 + 16, 0);
        WriteU32(z64 + 20, 0);
        WriteU64(z64 + 24, (uint64_t)m_entries.size());
        WriteU64(z64 + 32, (uint64_t)m_entries.size());
        WriteU64(z64 + 40, totalCdSize);
        WriteU64(z64 + 48, m_virtualCdOffset);
        cdPos += ZIP64_EOCD_FIXED_SIZE;

        uint8_t* loc = cdStart + cdPos;
        WriteU32(loc + 0,  ZIP64_LOCATOR_SIGNATURE);
        WriteU32(loc + 4,  0);
        WriteU64(loc + 8,  m_virtualCdOffset + totalCdSize);  // file offset of Zip64 EOCD (from start)
        WriteU32(loc + 16, 1);
        cdPos += ZIP64_LOCATOR_SIZE;
    }

    uint8_t* eocd = cdStart + cdPos;
    WriteU32(eocd + 0,  EOCD_SIGNATURE);
    WriteU16(eocd + 4,  0);
    WriteU16(eocd + 6,  0);
    WriteU16(eocd + 8,  (uint16_t)(m_entries.size() > 0xFFFF ? 0xFFFF : m_entries.size()));
    WriteU16(eocd + 10, (uint16_t)(m_entries.size() > 0xFFFF ? 0xFFFF : m_entries.size()));
    WriteU32(eocd + 12, (uint32_t)(useZip64 ? MAX_32BIT : totalCdSize));
    WriteU32(eocd + 16, (uint32_t)(useZip64 ? MAX_32BIT : m_virtualCdOffset));
    WriteU16(eocd + 20, 0);
}

size_t VirtualHd::ReadAtVirtualOffset(HANDLE realFile, uint64_t virtualOffset, void* buffer, size_t size,
    BOOL(WINAPI* readFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED),
    BOOL(WINAPI* setFilePointerEx)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD))
{
    if (virtualOffset >= m_virtualSize || size == 0) return 0;
    size = (size_t)(std::min)((uint64_t)size, m_virtualSize - virtualOffset);

    if (virtualOffset >= m_virtualCdOffset) {
        BuildSyntheticCDAndEOCD();
        uint64_t offInCD = virtualOffset - m_virtualCdOffset;
        size_t toCopy = (size_t)(std::min)((uint64_t)size, (uint64_t)m_syntheticCD.size() - offInCD);
        memcpy(buffer, m_syntheticCD.data() + offInCD, toCopy);
        return toCopy;
    }

    uint8_t* dst = (uint8_t*)buffer;
    size_t totalRead = 0;
    uint64_t pos = virtualOffset;

    while (totalRead < size && pos < m_virtualCdOffset) {
        // Binary search: find entry containing pos (entries sorted by virtualLocalHeaderOffset)
        size_t lo = 0, hi = m_entries.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (m_entries[mid].virtualLocalHeaderOffset <= pos)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == 0) break;
        size_t entryIdx = lo - 1;
        const auto& ze = m_entries[entryIdx];
        if (pos >= ze.virtualLocalHeaderOffset + ze.virtualEntryTotalSize) break;
        uint64_t offsetInEntry = pos - ze.virtualLocalHeaderOffset;
        uint64_t bytesInEntry = ze.virtualEntryTotalSize - offsetInEntry;
        size_t toRead = (size_t)(std::min)((uint64_t)(size - totalRead), bytesInEntry);

        if (ze.isModded) {
            uint32_t headerSize = LFH_FIXED_SIZE + (uint32_t)ze.filename.size();
            if (offsetInEntry < headerSize) {
                uint8_t lfhBuf[1024];
                uint32_t copyFromHeader = (uint32_t)(std::min)((uint64_t)toRead, (uint64_t)(headerSize - offsetInEntry));
                if (ze.moddedCrc32 == 0) {
                    std::ifstream mf(ze.modFilePath, std::ios::binary);
                    if (mf.is_open()) {
                        char tmp[65536];
                        uint32_t crc = 0;
                        while (mf.read(tmp, sizeof(tmp)) || mf.gcount() > 0)
                            crc = UpdateCrc32(crc, tmp, (size_t)mf.gcount());
                        m_entries[entryIdx].moddedCrc32 = crc;
                    }
                }
                WriteU32(lfhBuf + 0,  LFH_SIGNATURE);
                WriteU16(lfhBuf + 4,  20);
                WriteU16(lfhBuf + 6,  0);
                WriteU16(lfhBuf + 8,  0);
                WriteU16(lfhBuf + 10, ze.lastModTime);
                WriteU16(lfhBuf + 12, ze.lastModDate);
                WriteU32(lfhBuf + 14, ze.moddedCrc32);
                WriteU32(lfhBuf + 18, ze.moddedFileSize);
                WriteU32(lfhBuf + 22, ze.moddedFileSize);
                WriteU16(lfhBuf + 26, (uint16_t)ze.filename.size());
                WriteU16(lfhBuf + 28, 0);
                memcpy(lfhBuf + LFH_FIXED_SIZE, ze.filename.data(), ze.filename.size());
                memcpy(dst, lfhBuf + offsetInEntry, copyFromHeader);
                totalRead += copyFromHeader;
                dst += copyFromHeader;
                pos += copyFromHeader;
                toRead -= copyFromHeader;
            }
            if (toRead > 0) {
                uint64_t dataOffset = (offsetInEntry >= headerSize) ? (offsetInEntry - headerSize) : 0;
                std::ifstream mf(ze.modFilePath, std::ios::binary);
                if (mf.is_open()) {
                    mf.seekg((std::streamoff)dataOffset);
                    mf.read((char*)dst, toRead);
                    size_t got = (size_t)mf.gcount();
                    totalRead += got;
                    dst += got;
                    pos += got;
                }
            }
        } else {
            uint64_t realOffset = ze.localHeaderOffset + offsetInEntry;
            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)realOffset;
            if (setFilePointerEx(realFile, li, nullptr, FILE_BEGIN)) {
                DWORD got = 0;
                if (readFile(realFile, dst, (DWORD)toRead, &got, nullptr))
                    totalRead += got, dst += got, pos += got;
            }
        }
    }
    return totalRead;
}

// Parse Zip64 extended info extra field (ID 0x0001) for 64-bit sizes/offset
static void ParseZip64Extra(ZipEntry& ze, const uint8_t* extra, uint16_t extraLen) {
    const uint8_t* end = extra + extraLen;
    while (extra + 4 <= end) {
        uint16_t id = ReadU16(extra);
        uint16_t size = ReadU16(extra + 2);
        extra += 4;
        if (extra + size > end) break;
        if (id == ZIP64_EXTRA_ID && size >= 8) {
            const uint8_t* p = extra;
            if (ze.uncompressedSize == 0xFFFFFFFF && p + 8 <= extra + size) {
                ze.uncompressedSize = (uint32_t)ReadU64(p);
                p += 8;
            }
            if (ze.compressedSize == 0xFFFFFFFF && p + 8 <= extra + size) {
                ze.compressedSize = (uint32_t)ReadU64(p);
                p += 8;
            }
            if (ze.localHeaderOffset == 0xFFFFFFFFULL && p + 8 <= extra + size) {
                ze.localHeaderOffset = ReadU64(p);
                p += 8;
            }
            break;
        }
        extra += size;
    }
}

bool VirtualHd::ParseRealZip(HANDLE realHdDat) {
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(realHdDat, &fileSize)) return false;
    uint64_t realSize = (uint64_t)fileSize.QuadPart;

    uint32_t searchSize = (uint32_t)(std::min)(realSize, (uint64_t)(EOCD_FIXED_SIZE + 65535 + ZIP64_LOCATOR_SIZE + 64));
    std::vector<uint8_t> tailBuf(searchSize);
    if (!ReadAt(realHdDat, realSize - searchSize, tailBuf.data(), searchSize, nullptr))
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

    // Zip64: EOCD fields 0xFFFFFFFF/0xFFFF mean value is in Zip64 EOCD
    if (m_cdOffset == 0xFFFFFFFFULL || m_cdSize == 0xFFFFFFFFULL || numEntries == 0xFFFF) {
        int64_t locatorPos = -1;
        for (int64_t i = eocdPos - (int64_t)ZIP64_LOCATOR_SIZE; i >= 0; i--) {
            if (ReadU32(&tailBuf[i]) == ZIP64_LOCATOR_SIGNATURE) {
                locatorPos = i;
                break;
            }
        }
        if (locatorPos < 0) {
            std::cout << "[ModLoader] Zip64 EOCD locator not found" << std::endl;
            return false;
        }
        const uint8_t* loc = &tailBuf[locatorPos];
        uint64_t zip64EocdOffset = ReadU64(loc + 8);

        std::vector<uint8_t> zip64Buf(ZIP64_EOCD_FIXED_SIZE);
        if (!ReadAt(realHdDat, zip64EocdOffset, zip64Buf.data(), zip64Buf.size(), nullptr))
            return false;
        if (ReadU32(zip64Buf.data()) != ZIP64_EOCD_SIGNATURE) {
            std::cout << "[ModLoader] Zip64 EOCD signature not found" << std::endl;
            return false;
        }
        if (numEntries == 0xFFFF)
            numEntries = (uint16_t)(ReadU64(zip64Buf.data() + 32) & 0xFFFF);
        if (m_cdSize == 0xFFFFFFFF)
            m_cdSize = ReadU64(zip64Buf.data() + 40);
        if (m_cdOffset == 0xFFFFFFFF)
            m_cdOffset = ReadU64(zip64Buf.data() + 48);
        std::cout << "[ModLoader] Zip64: " << numEntries << " entries, CD at " << m_cdOffset
                  << ", size " << m_cdSize << std::endl;
    } else {
        std::cout << "[ModLoader] EOCD: " << numEntries << " entries, CD at " << m_cdOffset
                  << ", size " << m_cdSize << std::endl;
    }

    m_rawCd.resize((size_t)m_cdSize);
    if (!ReadAt(realHdDat, m_cdOffset, m_rawCd.data(), m_cdSize, nullptr))
        return false;

    m_entries.clear();
    m_entries.reserve(numEntries);
    uint32_t pos = 0;

    for (uint16_t i = 0; i < numEntries; i++) {
        if (pos + CD_FIXED_SIZE > (uint32_t)m_cdSize) return false;
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

        if (pos + CD_FIXED_SIZE + nameLen > (uint32_t)m_cdSize) return false;
        ze.filename = std::string((const char*)(entry + CD_FIXED_SIZE), nameLen);

        if (ze.extraFieldLength > 0 &&
            (ze.localHeaderOffset == 0xFFFFFFFFULL || ze.compressedSize == 0xFFFFFFFF ||
             ze.uncompressedSize == 0xFFFFFFFF)) {
            ParseZip64Extra(ze, entry + CD_FIXED_SIZE + nameLen, ze.extraFieldLength);
        }

        ze.cdEntryOffset = pos;
        ze.cdEntrySize = CD_FIXED_SIZE + nameLen + ze.extraFieldLength + ze.fileCommentLength;

        m_entries.push_back(std::move(ze));
        pos += m_entries.back().cdEntrySize;
    }

    for (auto& ze : m_entries) {
        uint8_t lfhBuf[LFH_FIXED_SIZE];
        if (!ReadAt(realHdDat, ze.localHeaderOffset, lfhBuf, LFH_FIXED_SIZE, nullptr))
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
                // CRC32 is computed lazily in ReadAtVirtualOffset when serving modded data
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

