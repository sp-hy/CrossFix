#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <cstdint>

enum class SegmentSource { RealFile, ModFile, Memory };

struct Segment {
    uint64_t virtualOffset;
    uint64_t size;
    SegmentSource source;

    // For RealFile: offset into real hd.dat
    uint64_t realOffset;

    // For ModFile: path to mod file on disk
    std::string modFilePath;
    uint64_t modFileOffset;

    // For Memory: pointer to synthesized data (local headers, central dir, eocd)
    const uint8_t* memData;
};

struct ZipEntry {
    std::string filename;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t compressionMethod;
    uint32_t crc32;
    uint32_t localHeaderOffset;  // offset in real hd.dat
    uint16_t extraFieldLength;
    uint16_t fileCommentLength;
    uint16_t internalAttrs;
    uint32_t externalAttrs;
    uint16_t lastModTime;
    uint16_t lastModDate;
    uint16_t versionMadeBy;
    uint16_t versionNeeded;
    uint16_t generalPurposeFlag;
    uint16_t diskNumberStart;

    // Computed during layout
    bool isModded;
    std::string modFilePath;
    uint32_t moddedFileSize;   // size of the mod file on disk
    uint32_t moddedCrc32;
};

class VirtualHd {
public:
    VirtualHd();
    ~VirtualHd();

    // Build the virtual layout from the real hd.dat handle and mods directory
    bool Build(HANDLE realHdDat, const std::string& modsDir);

    // Get total virtual file size
    uint64_t GetVirtualSize() const;

    // Read bytes from the virtual layout into buffer
    // realHandle is used when reading from real hd.dat segments
    bool ReadVirtual(HANDLE realHandle, uint64_t offset, void* buffer, uint32_t bytesToRead, uint32_t* bytesRead);

    // Check if the layout has been built
    bool IsBuilt() const { return m_built; }

private:
    bool ParseRealZip(HANDLE realHdDat);
    void ScanMods(const std::string& modsDir);
    void BuildLayout(HANDLE realHdDat);
    uint32_t ComputeCrc32(const std::string& filePath);
    bool ReadRealFile(HANDLE realHandle, uint64_t offset, void* buffer, uint32_t size, uint32_t* bytesRead);

    std::vector<ZipEntry> m_entries;
    std::vector<Segment> m_segments;
    std::vector<std::vector<uint8_t>> m_memBuffers; // owns memory for Memory segments
    uint64_t m_virtualSize;
    bool m_built;
    std::string m_modsDir;
};
