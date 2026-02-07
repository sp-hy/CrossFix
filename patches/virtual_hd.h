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
    uint64_t localHeaderOffset;  // offset in real file (64-bit for Zip64)
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

    // Offset in the real file where the file DATA starts (64-bit for Zip64)
    uint64_t dataOffset;

    // Offset and size of this entry within the raw CD buffer
    uint32_t cdEntryOffset;
    uint32_t cdEntrySize;

    // Total size of this entry in the real file (LFH + name + extra + data)
    uint64_t realEntryTotalSize;

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

    uint64_t GetVirtualSize() const { return m_virtualSize; }
    uint64_t GetVirtualCdOffset() const { return m_virtualCdOffset; }
    bool IsBuilt() const { return m_built; }
    size_t GetEntryCount() const { return m_entries.size(); }
    const ZipEntry& GetEntry(size_t i) const { return m_entries[i]; }
    // True if any entry was overridden by a mod (so we need to serve the virtual view)
    bool HasMods() const;

    // Read from virtual layout at given offset - no buffer needed. Uses realFile for unmodded,
    // reads mod files for modded. Returns bytes read. Use when view allocation fails.
    size_t ReadAtVirtualOffset(HANDLE realFile, uint64_t virtualOffset, void* buffer, size_t size,
        BOOL(WINAPI* readFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED),
        BOOL(WINAPI* setFilePointerEx)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD));

private:
    bool ParseRealZip(HANDLE realHdDat);
    void ScanMods(const std::string& modsDir);
    void ComputeLayout();
    void BuildSyntheticCDAndEOCD();  // fills m_syntheticCD for ReadAtVirtualOffset

    std::vector<ZipEntry> m_entries;
    std::vector<uint8_t> m_rawCd;  // raw CD bytes from real file
    uint64_t m_cdOffset;   // 64-bit for Zip64
    uint64_t m_cdSize;
    uint32_t m_eocdOffset;
    uint64_t m_virtualSize;
    uint64_t m_virtualCdOffset;
    bool m_built;
    std::vector<uint8_t> m_syntheticCD;  // precomputed CD+EOCD for ReadAtVirtualOffset
};
