// SPDX-License-Identifier: MIT
#include "disc/GameData.h"

#include "game/GameBuild.h"

#include <format>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace tgr::disc
{

namespace
{
constexpr std::uint32_t kChunkSectors = 512; // 1 MiB per read

bool copyFile(const IsoImage& iso, const IsoEntry& entry, const fs::path& target,
              std::atomic<std::uint64_t>& bytesDone, std::vector<std::uint8_t>& buffer)
{
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    std::uint32_t lba = entry.lba;
    std::uint32_t remaining = entry.size;
    while (remaining > 0)
    {
        const std::uint32_t sectorsLeft = (remaining + kSectorSize - 1) / kSectorSize;
        const std::uint32_t sectors = sectorsLeft < kChunkSectors ? sectorsLeft : kChunkSectors;
        if (!iso.readSectors(lba, sectors, buffer.data()))
            return false;

        const std::uint32_t bytes = remaining < sectors * kSectorSize ? remaining : sectors * kSectorSize;
        if (!out.write(reinterpret_cast<const char*>(buffer.data()), bytes))
            return false;

        lba += sectors;
        remaining -= bytes;
        bytesDone += bytes;
    }
    return true;
}
} // namespace

bool isGameDataInstalled(const fs::path& dir)
{
    std::ifstream marker(dir / kInstallMarkerName);
    if (!marker)
        return false;

    std::string serial;
    std::string crcText;
    std::getline(marker, serial);
    std::getline(marker, crcText);
    if (serial != game::kSerial || !fs::exists(dir / kLayoutFileName))
        return false;

    if constexpr (game::kBootElfCrc32 != 0)
    {
        std::uint32_t crc = 0;
        std::istringstream(crcText) >> std::hex >> crc;
        return crc == game::kBootElfCrc32;
    }
    return true;
}

std::uint64_t gameDataSize(const IsoImage& iso)
{
    std::uint64_t total = 0;
    for (const IsoEntry& entry : iso.listAll())
        if (!entry.isDirectory)
            total += entry.size;
    return total;
}

bool installGameData(const IsoImage& iso, const GameDiscInfo& disc, const fs::path& dir,
                     std::atomic<std::uint64_t>& bytesDone, std::string& error)
{
    std::error_code ec;
    fs::remove(dir / kInstallMarkerName, ec);
    fs::create_directories(dir, ec);
    if (ec)
    {
        error = std::format("cannot create {}: {}", dir.string(), ec.message());
        return false;
    }

    std::ofstream layout(dir / kLayoutFileName, std::ios::trunc);
    if (!layout)
    {
        error = std::format("cannot write {}", (dir / kLayoutFileName).string());
        return false;
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(kChunkSectors) * kSectorSize);
    for (const IsoEntry& entry : iso.listAll())
    {
        const fs::path target = dir / fs::path(entry.path);
        if (entry.isDirectory)
        {
            fs::create_directories(target, ec);
            continue;
        }

        if (!copyFile(iso, entry, target, bytesDone, buffer))
        {
            error = std::format("failed to extract {} (is the disk full?)", entry.path);
            return false;
        }
        layout << entry.lba << '\t' << entry.size << '\t' << entry.path << '\n';
    }
    layout.close();

    // Written last: its presence means the extraction completed.
    std::ofstream marker(dir / kInstallMarkerName, std::ios::trunc);
    marker << game::kSerial << '\n' << std::format("{:08X}", disc.bootElfCrc32) << '\n';
    if (!marker)
    {
        error = std::format("cannot write {}", (dir / kInstallMarkerName).string());
        return false;
    }
    return true;
}

} // namespace tgr::disc
