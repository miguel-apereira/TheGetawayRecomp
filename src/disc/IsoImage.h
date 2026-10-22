// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tgr::disc
{

inline constexpr std::uint32_t kSectorSize = 2048;

struct IsoEntry
{
    std::string path; // "MODULES/SIO2MAN.IRX", '/'-separated, version suffix (";1") removed
    std::uint32_t lba = 0;
    std::uint32_t size = 0;
    bool isDirectory = false;
};

// Read-only ISO9660 reader for a user-supplied PS2 DVD image (2048-byte
// sectors). Used to check the disc and extract it once (see GameData.h).
class IsoImage
{
public:
    static std::unique_ptr<IsoImage> open(const std::filesystem::path& path, std::string& error);

    const std::string& volumeId() const { return volumeId_; }
    std::uint64_t sizeBytes() const { return sizeBytes_; }
    std::uint32_t sectorCount() const { return static_cast<std::uint32_t>(sizeBytes_ / kSectorSize); }

    // Case-insensitive lookup, e.g. find("SYSTEM.CNF") or find("modules/foo.irx").
    std::optional<IsoEntry> find(std::string_view path) const;

    // Every file and directory on the disc, depth-first.
    std::vector<IsoEntry> listAll() const;

    bool readSectors(std::uint32_t lba, std::uint32_t count, void* out) const;
    std::optional<std::vector<std::uint8_t>> readFile(const IsoEntry& entry) const;

private:
    IsoImage() = default;

    std::vector<IsoEntry> readDirectory(const IsoEntry& dir) const;
    void walk(const IsoEntry& dir, std::vector<IsoEntry>& out, int depth) const;

    mutable std::ifstream file_;
    std::uint64_t sizeBytes_ = 0;
    std::string volumeId_;
    IsoEntry root_;
};

} // namespace tgr::disc
