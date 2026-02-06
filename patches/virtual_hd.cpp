#include "virtual_hd.h"
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ZIP structure signatures
static constexpr uint32_t EOCD_SIGNATURE = 0x06054b50;
static constexpr uint32_t CD_SIGNATURE   = 0x02014b50;
static constexpr uint32_t LFH_SIGNATURE  = 0x04034b50;

// ZIP structure sizes (fixed portions)
static constexpr uint32_t EOCD_FIXED_SIZE = 22;
static constexpr uint32_t CD_FIXED_SIZE   = 46;
static constexpr uint32_t LFH_FIXED_SIZE  = 30;

// CRC32 lookup table
static uint32_t s_crc32Table[256];
static bool s_crc32TableInit = false;

static void InitCrc32Table() {
    if (s_crc32TableInit) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1)));
        }
        s_crc32Table[i] = crc;
    }
    s_crc32TableInit = true;
}

static uint32_t UpdateCrc32(uint32_t crc, const void* data, size_t size) {
    const uint8_t* buf = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc = s_crc32Table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// Helper to read from a real file handle at a specific offset
static bool ReadAt(HANDLE hFile, uint64_t offset, void* buffer, uint32_t size, uint32_t* bytesRead) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) {
        return false;
    }
    DWORD dwRead = 0;
    if (!ReadFile(hFile, buffer, size, &dwRead, NULL)) {
        return false;
    }
    if (bytesRead) *bytesRead = dwRead;
    return true;
}

// Helper to read a little-endian uint16 from a byte buffer
static uint16_t ReadU16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Helper to read a little-endian uint32 from a byte buffer
static uint32_t ReadU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Helper to write a little-endian uint16 into a byte buffer
static void WriteU16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

// Helper to write a little-endian uint32 into a byte buffer
static void WriteU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

VirtualHd::VirtualHd() : m_virtualSize(0), m_built(false) {
    InitCrc32Table();
}

VirtualHd::~VirtualHd() {
}

bool VirtualHd::Build(HANDLE realHdDat, const std::string& modsDir) {
    m_modsDir = modsDir;

    if (!ParseRealZip(realHdDat)) {
        std::cout << "[ModLoader] Failed to parse real hd.dat ZIP structure" << std::endl;
        return false;
    }

    std::cout << "[ModLoader] Parsed " << m_entries.size() << " entries from hd.dat" << std::endl;

    ScanMods(modsDir);

    BuildLayout(realHdDat);

    m_built = true;
    std::cout << "[ModLoader] Virtual hd.dat built: " << m_virtualSize << " bytes ("
              << m_entries.size() << " entries)" << std::endl;

    return true;
}

uint64_t VirtualHd::GetVirtualSize() const {
    return m_virtualSize;
}

bool VirtualHd::ReadVirtual(HANDLE realHandle, uint64_t offset, void* buffer, uint32_t bytesToRead, uint32_t* bytesRead) {
    if (!m_built || offset >= m_virtualSize) {
        if (bytesRead) *bytesRead = 0;
        return true; // EOF
    }

    // Clamp to virtual size
    if (offset + bytesToRead > m_virtualSize) {
        bytesToRead = (uint32_t)(m_virtualSize - offset);
    }

    uint8_t* outBuf = (uint8_t*)buffer;
    uint32_t totalRead = 0;

    while (bytesToRead > 0) {
        // Binary search for the segment containing 'offset'
        // Segments are sorted by virtualOffset, find the last one with virtualOffset <= offset
        size_t lo = 0, hi = m_segments.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (m_segments[mid].virtualOffset <= offset) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }

        if (lo == 0) {
            // offset is before the first segment - shouldn't happen
            break;
        }

        const Segment& seg = m_segments[lo - 1];

        // Check we're actually within this segment
        if (offset >= seg.virtualOffset + seg.size) {
            // In a gap between segments or past end - shouldn't happen with correct layout
            break;
        }

        uint64_t offsetInSeg = offset - seg.virtualOffset;
        uint32_t canRead = (uint32_t)(std::min)((uint64_t)bytesToRead, seg.size - offsetInSeg);

        switch (seg.source) {
        case SegmentSource::Memory:
            memcpy(outBuf, seg.memData + offsetInSeg, canRead);
            break;

        case SegmentSource::RealFile: {
            uint32_t readCount = 0;
            if (!ReadRealFile(realHandle, seg.realOffset + offsetInSeg, outBuf, canRead, &readCount)) {
                if (bytesRead) *bytesRead = totalRead;
                return false;
            }
            if (readCount < canRead) {
                // Short read from real file
                canRead = readCount;
            }
            break;
        }

        case SegmentSource::ModFile: {
            // Open the mod file, read from it, close
            HANDLE hMod = CreateFileA(seg.modFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                      NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hMod == INVALID_HANDLE_VALUE) {
                std::cout << "[ModLoader] ERROR: Cannot open mod file: " << seg.modFilePath << std::endl;
                if (bytesRead) *bytesRead = totalRead;
                return false;
            }

            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)(seg.modFileOffset + offsetInSeg);
            SetFilePointerEx(hMod, li, NULL, FILE_BEGIN);

            DWORD dwRead = 0;
            ReadFile(hMod, outBuf, canRead, &dwRead, NULL);
            CloseHandle(hMod);

            canRead = (uint32_t)dwRead;
            break;
        }
        }

        outBuf += canRead;
        offset += canRead;
        totalRead += canRead;
        bytesToRead -= canRead;

        if (canRead == 0) break; // Prevent infinite loop on error
    }

    if (bytesRead) *bytesRead = totalRead;
    return true;
}

bool VirtualHd::ParseRealZip(HANDLE realHdDat) {
    // Get file size
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(realHdDat, &fileSize)) {
        return false;
    }
    uint64_t realSize = (uint64_t)fileSize.QuadPart;

    // Find EOCD: scan backwards from end of file
    // EOCD is at least 22 bytes, and comment can be up to 65535 bytes
    uint32_t searchSize = (uint32_t)(std::min)(realSize, (uint64_t)(EOCD_FIXED_SIZE + 65535));
    std::vector<uint8_t> tailBuf(searchSize);

    uint32_t readCount = 0;
    if (!ReadAt(realHdDat, realSize - searchSize, tailBuf.data(), searchSize, &readCount)) {
        return false;
    }

    // Search backwards for EOCD signature
    int64_t eocdPos = -1;
    for (int64_t i = (int64_t)searchSize - EOCD_FIXED_SIZE; i >= 0; i--) {
        if (ReadU32(&tailBuf[i]) == EOCD_SIGNATURE) {
            eocdPos = i;
            break;
        }
    }

    if (eocdPos < 0) {
        std::cout << "[ModLoader] EOCD signature not found in hd.dat" << std::endl;
        return false;
    }

    // Parse EOCD
    const uint8_t* eocd = &tailBuf[eocdPos];
    uint16_t numEntries = ReadU16(eocd + 10);       // total entries
    uint32_t cdSize     = ReadU32(eocd + 12);        // central directory size
    uint32_t cdOffset   = ReadU32(eocd + 16);        // central directory offset

    std::cout << "[ModLoader] EOCD: " << numEntries << " entries, CD at offset " << cdOffset
              << ", CD size " << cdSize << std::endl;

    // Read central directory
    std::vector<uint8_t> cdBuf(cdSize);
    if (!ReadAt(realHdDat, cdOffset, cdBuf.data(), cdSize, &readCount)) {
        return false;
    }

    // Parse each central directory entry
    m_entries.clear();
    m_entries.reserve(numEntries);
    uint32_t pos = 0;

    for (uint16_t i = 0; i < numEntries; i++) {
        if (pos + CD_FIXED_SIZE > cdSize) {
            std::cout << "[ModLoader] Truncated central directory at entry " << i << std::endl;
            return false;
        }

        const uint8_t* entry = &cdBuf[pos];
        if (ReadU32(entry) != CD_SIGNATURE) {
            std::cout << "[ModLoader] Invalid CD signature at entry " << i << std::endl;
            return false;
        }

        ZipEntry ze;
        ze.versionMadeBy    = ReadU16(entry + 4);
        ze.versionNeeded    = ReadU16(entry + 6);
        ze.generalPurposeFlag = ReadU16(entry + 8);
        ze.compressionMethod = ReadU16(entry + 10);
        ze.lastModTime      = ReadU16(entry + 12);
        ze.lastModDate      = ReadU16(entry + 14);
        ze.crc32            = ReadU32(entry + 16);
        ze.compressedSize   = ReadU32(entry + 20);
        ze.uncompressedSize = ReadU32(entry + 24);
        uint16_t nameLen    = ReadU16(entry + 28);
        ze.extraFieldLength = ReadU16(entry + 30);
        ze.fileCommentLength = ReadU16(entry + 32);
        ze.diskNumberStart  = ReadU16(entry + 34);
        ze.internalAttrs    = ReadU16(entry + 36);
        ze.externalAttrs    = ReadU32(entry + 38);
        ze.localHeaderOffset = ReadU32(entry + 42);

        if (pos + CD_FIXED_SIZE + nameLen > cdSize) {
            return false;
        }
        ze.filename = std::string((const char*)(entry + CD_FIXED_SIZE), nameLen);

        ze.isModded = false;
        ze.moddedFileSize = 0;
        ze.moddedCrc32 = 0;

        m_entries.push_back(std::move(ze));

        pos += CD_FIXED_SIZE + nameLen + ze.extraFieldLength + ze.fileCommentLength;
    }

    return true;
}

void VirtualHd::ScanMods(const std::string& modsDir) {
    if (!fs::exists(modsDir) || !fs::is_directory(modsDir)) {
        std::cout << "[ModLoader] Mods directory not found: " << modsDir << std::endl;
        return;
    }

    int modCount = 0;

    for (auto& dirEntry : fs::recursive_directory_iterator(modsDir)) {
        if (!dirEntry.is_regular_file()) continue;

        // Get relative path from mods dir, normalized with forward slashes
        fs::path relPath = fs::relative(dirEntry.path(), modsDir);
        std::string relStr = relPath.generic_string(); // uses forward slashes

        // Try to find matching ZIP entry
        for (auto& ze : m_entries) {
            if (ze.filename == relStr) {
                ze.isModded = true;
                ze.modFilePath = dirEntry.path().string();
                ze.moddedFileSize = (uint32_t)dirEntry.file_size();
                ze.moddedCrc32 = ComputeCrc32(ze.modFilePath);
                modCount++;
                std::cout << "[ModLoader] Mod override: " << relStr
                          << " (" << ze.moddedFileSize << " bytes)" << std::endl;
                break;
            }
        }
    }

    std::cout << "[ModLoader] Found " << modCount << " mod file(s)" << std::endl;
}

void VirtualHd::BuildLayout(HANDLE realHdDat) {
    m_segments.clear();
    m_memBuffers.clear();
    m_virtualSize = 0;

    uint64_t currentOffset = 0;

    // Phase 1: Emit local file headers + data for each entry
    // We also need to read real local file headers to get their extra field lengths
    std::vector<uint32_t> newLocalOffsets(m_entries.size());

    for (size_t i = 0; i < m_entries.size(); i++) {
        ZipEntry& ze = m_entries[i];
        newLocalOffsets[i] = (uint32_t)currentOffset;

        // Read the real local file header to get the actual extra field length
        uint8_t lfhBuf[LFH_FIXED_SIZE];
        uint32_t readCount = 0;
        ReadAt(realHdDat, ze.localHeaderOffset, lfhBuf, LFH_FIXED_SIZE, &readCount);

        uint16_t realLfhNameLen = ReadU16(lfhBuf + 26);
        uint16_t realLfhExtraLen = ReadU16(lfhBuf + 28);

        if (ze.isModded) {
            // Build a new local file header for the modded file (stored uncompressed)
            uint16_t nameLen = (uint16_t)ze.filename.size();
            uint32_t lfhSize = LFH_FIXED_SIZE + nameLen;

            m_memBuffers.emplace_back(lfhSize);
            auto& lfhData = m_memBuffers.back();

            WriteU32(&lfhData[0],  LFH_SIGNATURE);
            WriteU16(&lfhData[4],  20);                    // version needed (2.0)
            WriteU16(&lfhData[6],  0);                     // general purpose flag
            WriteU16(&lfhData[8],  0);                     // compression: stored
            WriteU16(&lfhData[10], ze.lastModTime);
            WriteU16(&lfhData[12], ze.lastModDate);
            WriteU32(&lfhData[14], ze.moddedCrc32);
            WriteU32(&lfhData[18], ze.moddedFileSize);     // compressed size (same as uncompressed for stored)
            WriteU32(&lfhData[22], ze.moddedFileSize);     // uncompressed size
            WriteU16(&lfhData[26], nameLen);
            WriteU16(&lfhData[28], 0);                     // extra field length
            memcpy(&lfhData[30], ze.filename.data(), nameLen);

            // Local file header segment (Memory)
            Segment lfhSeg;
            lfhSeg.virtualOffset = currentOffset;
            lfhSeg.size = lfhSize;
            lfhSeg.source = SegmentSource::Memory;
            lfhSeg.memData = lfhData.data();
            lfhSeg.realOffset = 0;
            lfhSeg.modFileOffset = 0;
            m_segments.push_back(lfhSeg);
            currentOffset += lfhSize;

            // File data segment (ModFile)
            Segment dataSeg;
            dataSeg.virtualOffset = currentOffset;
            dataSeg.size = ze.moddedFileSize;
            dataSeg.source = SegmentSource::ModFile;
            dataSeg.modFilePath = ze.modFilePath;
            dataSeg.modFileOffset = 0;
            dataSeg.realOffset = 0;
            dataSeg.memData = nullptr;
            m_segments.push_back(dataSeg);
            currentOffset += ze.moddedFileSize;
        } else {
            // Unmodded entry: emit from real file
            // Local file header + data as one RealFile segment
            uint32_t realDataSize = ze.compressedSize;
            uint32_t realLfhTotalSize = LFH_FIXED_SIZE + realLfhNameLen + realLfhExtraLen;
            uint32_t totalEntrySize = realLfhTotalSize + realDataSize;

            Segment entrySeg;
            entrySeg.virtualOffset = currentOffset;
            entrySeg.size = totalEntrySize;
            entrySeg.source = SegmentSource::RealFile;
            entrySeg.realOffset = ze.localHeaderOffset;
            entrySeg.memData = nullptr;
            entrySeg.modFileOffset = 0;
            m_segments.push_back(entrySeg);
            currentOffset += totalEntrySize;
        }
    }

    // Phase 2: Build central directory
    uint32_t cdStartOffset = (uint32_t)currentOffset;
    std::vector<uint8_t> cdData;

    for (size_t i = 0; i < m_entries.size(); i++) {
        const ZipEntry& ze = m_entries[i];
        uint16_t nameLen = (uint16_t)ze.filename.size();
        uint32_t cdEntrySize = CD_FIXED_SIZE + nameLen; // no extra field or comment for simplicity

        size_t oldSize = cdData.size();
        cdData.resize(oldSize + cdEntrySize);
        uint8_t* p = &cdData[oldSize];

        WriteU32(p + 0,  CD_SIGNATURE);
        WriteU16(p + 4,  ze.versionMadeBy);
        WriteU16(p + 6,  ze.isModded ? 20 : ze.versionNeeded);
        WriteU16(p + 8,  ze.isModded ? 0 : ze.generalPurposeFlag);
        WriteU16(p + 10, ze.isModded ? 0 : ze.compressionMethod);
        WriteU16(p + 12, ze.lastModTime);
        WriteU16(p + 14, ze.lastModDate);
        WriteU32(p + 16, ze.isModded ? ze.moddedCrc32 : ze.crc32);
        WriteU32(p + 20, ze.isModded ? ze.moddedFileSize : ze.compressedSize);
        WriteU32(p + 24, ze.isModded ? ze.moddedFileSize : ze.uncompressedSize);
        WriteU16(p + 28, nameLen);
        WriteU16(p + 30, 0);  // extra field length
        WriteU16(p + 32, 0);  // file comment length
        WriteU16(p + 34, 0);  // disk number start
        WriteU16(p + 36, ze.internalAttrs);
        WriteU32(p + 38, ze.externalAttrs);
        WriteU32(p + 42, newLocalOffsets[i]);
        memcpy(p + CD_FIXED_SIZE, ze.filename.data(), nameLen);
    }

    uint32_t cdTotalSize = (uint32_t)cdData.size();

    // Store CD in memory buffers
    m_memBuffers.emplace_back(std::move(cdData));
    const auto& cdBuf = m_memBuffers.back();

    Segment cdSeg;
    cdSeg.virtualOffset = currentOffset;
    cdSeg.size = cdTotalSize;
    cdSeg.source = SegmentSource::Memory;
    cdSeg.memData = cdBuf.data();
    cdSeg.realOffset = 0;
    cdSeg.modFileOffset = 0;
    m_segments.push_back(cdSeg);
    currentOffset += cdTotalSize;

    // Phase 3: Build EOCD
    m_memBuffers.emplace_back(EOCD_FIXED_SIZE);
    auto& eocdData = m_memBuffers.back();

    uint16_t numEntries = (uint16_t)m_entries.size();
    WriteU32(&eocdData[0],  EOCD_SIGNATURE);
    WriteU16(&eocdData[4],  0);                  // disk number
    WriteU16(&eocdData[6],  0);                  // disk with CD
    WriteU16(&eocdData[8],  numEntries);         // entries on this disk
    WriteU16(&eocdData[10], numEntries);         // total entries
    WriteU32(&eocdData[12], cdTotalSize);        // CD size
    WriteU32(&eocdData[16], cdStartOffset);      // CD offset
    WriteU16(&eocdData[20], 0);                  // comment length

    Segment eocdSeg;
    eocdSeg.virtualOffset = currentOffset;
    eocdSeg.size = EOCD_FIXED_SIZE;
    eocdSeg.source = SegmentSource::Memory;
    eocdSeg.memData = eocdData.data();
    eocdSeg.realOffset = 0;
    eocdSeg.modFileOffset = 0;
    m_segments.push_back(eocdSeg);
    currentOffset += EOCD_FIXED_SIZE;

    m_virtualSize = currentOffset;
}

uint32_t VirtualHd::ComputeCrc32(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return 0;

    uint32_t crc = 0;
    char buf[8192];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
        crc = UpdateCrc32(crc, buf, (size_t)file.gcount());
        if (file.eof()) break;
    }
    return crc;
}

bool VirtualHd::ReadRealFile(HANDLE realHandle, uint64_t offset, void* buffer, uint32_t size, uint32_t* bytesRead) {
    return ReadAt(realHandle, offset, buffer, size, bytesRead);
}
