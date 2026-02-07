#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <cstdint>

struct ZipEntry {
    std::string filename;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t compressionMethod;
    uint32_t crc32;
    uint32_t localHeaderOffset;  // offset in real hd.dat
    uint16_t extraFieldLength;   // CD extra field length
    uint16_t fileCommentLength;
    uint16_t internalAttrs;
    uint32_t externalAttrs;
    uint16_t lastModTime;
    uint16_t lastModDate;
    uint16_t versionMadeBy;
    uint16_t versionNeeded;
    uint16_t generalPurposeFlag;
    uint16_t diskNumberStart;

    // LFH fields (may differ from CD)
    uint16_t lfhNameLength;
    uint16_t lfhExtraLength;

    // Offset in the real file where the file DATA starts
    uint32_t dataOffset;

    // Offset and size of this entry within the raw CD buffer
    uint32_t cdEntryOffset;
    uint32_t cdEntrySize;

    // Total size of this entry in the real file (LFH + name + extra + data)
    uint32_t realEntryTotalSize;

    // Virtual layout (computed by ComputeLayout)
    uint64_t virtualLocalHeaderOffset;
    uint64_t virtualEntryTotalSize;

    // Mod state
    bool isModded;
    std::string modFilePath;
    uint32_t moddedFileSize;
    uint32_t moddedCrc32;
};

class VirtualHd {
public:
    VirtualHd();
    ~VirtualHd();

    // Parse hd.dat and scan for mods. Call once with the real file handle.
    bool Build(HANDLE realHdDat, const std::string& modsDir);

    // Create the virtual ZIP in a VirtualAlloc'd buffer, reading unmodded data
    // from realMappedBase (the real hd.dat mapped into memory).
    // Caller must eventually VirtualFree the returned pointer.
    uint8_t* CreateVirtualView(const uint8_t* realMappedBase);

    uint64_t GetVirtualSize() const { return m_virtualSize; }
    bool IsBuilt() const { return m_built; }
    // True if any entry was overridden by a mod (so we need to serve the virtual view)
    bool HasMods() const;

private:
    bool ParseRealZip(HANDLE realHdDat);
    void ScanMods(const std::string& modsDir);
    void ComputeLayout();

    std::vector<ZipEntry> m_entries;
    std::vector<uint8_t> m_rawCd;  // raw CD bytes from real file
    uint32_t m_cdOffset;
    uint32_t m_cdSize;
    uint32_t m_eocdOffset;
    uint64_t m_virtualSize;
    uint64_t m_virtualCdOffset;
    bool m_built;
};
