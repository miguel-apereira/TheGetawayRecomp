// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tgr::disc
{

// The game keeps almost all of its data in two archives on the disc:
// PACK_EE.PAK (files the EE loads: models, textures, animation, cutscenes,
// FMV) and PACK_IOP.PAK (files the IOP streams: speech, music, sound banks).
// The format below was read from the game's own loader in SCUS_971.33
// (open at 0x2a07e8, name lookup at 0x2a0f18, completion and checksum at
// 0x2a1f60, decompression at 0x2a1d60).
//
// All offsets are little-endian and relative to the start of the archive.
//
//   0x000  char[8]   magic: "SceeWhPC" (EE, with checksums) or "SceeWhPk" (IOP)
//   0x008  char[20]  build date, "[dd/mm/yy,hh:mm:ss]"
//   0x01c  u32       size of the name list in bytes
//   0x020  u32       number of files
//   0x024  char[64]  path the archive was built from, stripped from lookups
//   0x064  char[64]  a second stripped prefix (empty on this disc)
//   0x0a4  u32[28]   offset into the name list of the first name starting
//                    with: < 'a', 'a'..'z', > 'z' (-1 when there is none)
//   0x114  char[29][4] extension table; a name stores its extension as one
//                    byte, index + 1
//   0x190  u32       number of numbered groups
//   0x194  u32       size of the group record tables in bytes
//   0x198  group[n]  0x74 bytes each, see below
//   ...    records   per-group tables of file records
//   ...    names     the name list
//
// A numbered group holds files named "<dir>\<number>.<ext>", such as
// "tegfiles\12.teg", looked up by number instead of by name:
//
//   char[4] ext, char[96] dir, u32 files used, u32 first number,
//   u32 last number, u32 offset of its record table (from the first table)
//
// Its table has one record per number from first to last, zero when unused:
// {u32 size, u32 sector, u32 checksum} in a "C" archive, {u32 size, u32 sector}
// otherwise.
//
// The name list is sorted and prefix-compressed. Each node is
//
//   u24 sector, u8 shared prefix length (bit 7: hidden), u8 suffix length,
//   u16 skip (bytes to the next node with a shorter prefix, for lookups),
//   u32 size, [u32 checksum in a "C" archive], suffix bytes
//
// The last node has an empty name and ends the list.
//
// Sizes carry flags in bits 30-31; bit 30 set means the file is compressed
// (see pakDecompress). The checksum is the size of the file's final
// contents XORed with every 32-bit word of them, zero-padded.

struct PakEntry
{
    std::string path; // "animation/cutscenes/101r/rootblock.bin", lowercase, '/'-separated
    std::uint32_t sector = 0;
    std::uint32_t storedSize = 0; // bytes in the archive
    std::uint32_t checksum = 0;
    bool compressed = false;
    bool hasChecksum = false;
};

class PakArchive
{
public:
    // Reads `count` 2048-byte sectors of the archive starting at `sector`.
    using SectorReader = std::function<bool(std::uint32_t sector, std::uint32_t count, void* out)>;

    static std::unique_ptr<PakArchive> open(SectorReader reader, std::uint64_t sizeBytes, std::string& error);
    static std::unique_ptr<PakArchive> openFile(const std::filesystem::path& path, std::string& error);

    const std::string& magic() const { return magic_; }
    const std::string& buildDate() const { return buildDate_; }
    const std::string& sourceRoot() const { return sourceRoot_; }
    bool hasChecksums() const { return hasChecksums_; }
    const std::vector<PakEntry>& entries() const { return entries_; }

    // Reads a file, decompresses it if needed and checks its checksum.
    std::optional<std::vector<std::uint8_t>> read(const PakEntry& entry, std::string& error) const;

private:
    PakArchive() = default;

    bool parse(const std::vector<std::uint8_t>& dir, std::string& error);

    SectorReader reader_;
    std::uint64_t sizeBytes_ = 0;
    std::string magic_;
    std::string buildDate_;
    std::string sourceRoot_;
    bool hasChecksums_ = false;
    std::vector<PakEntry> entries_;
};

// Expands a compressed PAK file. The data is 16-bit values in 8 interleaved
// lanes (16-byte rows), each lane coded as runs of literals, byte deltas,
// nibble deltas and repeats. Returns false on malformed input.
bool pakDecompress(const std::uint8_t* src, std::size_t srcSize, std::vector<std::uint8_t>& out);

std::uint32_t pakChecksum(const std::uint8_t* data, std::size_t size);

} // namespace tgr::disc
