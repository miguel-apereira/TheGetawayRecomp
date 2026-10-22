// SPDX-License-Identifier: MIT
#include "disc/PakArchive.h"

#include "disc/IsoImage.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

namespace tgr::disc
{

namespace
{
constexpr std::size_t kHeaderSize = 0x198;
constexpr std::size_t kGroupSize = 0x74;
constexpr std::size_t kExtTableOffset = 0x114;
constexpr std::size_t kExtTableSlots = 29;
constexpr std::uint32_t kCompressedFlag = 0x40000000;
constexpr std::uint32_t kSizeMask = 0x3fffffff;
// Sanity limit for the directory the game loads in one piece (about 0.6 MB on this disc).
constexpr std::uint32_t kMaxDirectorySize = 64u << 20;

std::uint32_t readLe32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string readString(const std::uint8_t* p, std::size_t maxLen)
{
    const auto* end = static_cast<const std::uint8_t*>(std::memchr(p, 0, maxLen));
    return std::string(reinterpret_cast<const char*>(p), end ? static_cast<std::size_t>(end - p) : maxLen);
}

std::string toPath(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}
} // namespace

std::unique_ptr<PakArchive> PakArchive::open(SectorReader reader, std::uint64_t sizeBytes, std::string& error)
{
    std::unique_ptr<PakArchive> pak(new PakArchive());
    pak->reader_ = std::move(reader);
    pak->sizeBytes_ = sizeBytes;

    std::vector<std::uint8_t> head(kSectorSize);
    if (sizeBytes < kSectorSize || !pak->reader_(0, 1, head.data()))
    {
        error = "cannot read the archive header";
        return nullptr;
    }
    if (std::memcmp(head.data(), "SceeWhP", 7) != 0)
    {
        error = "not a PAK archive (no SceeWhP magic)";
        return nullptr;
    }

    const std::uint64_t dirSize = kHeaderSize + std::uint64_t{readLe32(&head[0x190])} * kGroupSize +
                                  readLe32(&head[0x194]) + readLe32(&head[0x1c]);
    if (dirSize > kMaxDirectorySize || dirSize > sizeBytes)
    {
        error = "archive directory is larger than the archive";
        return nullptr;
    }

    const auto dirSectors = static_cast<std::uint32_t>((dirSize + kSectorSize - 1) / kSectorSize);
    std::vector<std::uint8_t> dir(static_cast<std::size_t>(dirSectors) * kSectorSize);
    if (!pak->reader_(0, dirSectors, dir.data()))
    {
        error = "cannot read the archive directory";
        return nullptr;
    }
    dir.resize(static_cast<std::size_t>(dirSize));

    if (!pak->parse(dir, error))
        return nullptr;
    return pak;
}

std::unique_ptr<PakArchive> PakArchive::openFile(const std::filesystem::path& path, std::string& error)
{
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (ec || !*file)
    {
        error = "cannot open " + path.string();
        return nullptr;
    }

    auto reader = [file, size](std::uint32_t sector, std::uint32_t count, void* out) {
        const std::uint64_t offset = std::uint64_t{sector} * kSectorSize;
        if (offset >= size)
            return false;
        // The archive's last file may end mid-sector; pad the final read with zeros.
        const std::uint64_t bytes = std::min<std::uint64_t>(std::uint64_t{count} * kSectorSize, size - offset);
        std::memset(out, 0, static_cast<std::size_t>(count) * kSectorSize);
        file->clear();
        file->seekg(static_cast<std::streamoff>(offset));
        file->read(static_cast<char*>(out), static_cast<std::streamsize>(bytes));
        return static_cast<std::uint64_t>(file->gcount()) == bytes;
    };
    return open(reader, size, error);
}

bool PakArchive::parse(const std::vector<std::uint8_t>& dir, std::string& error)
{
    const std::uint8_t* d = dir.data();
    magic_ = readString(d, 8);
    buildDate_ = readString(d + 0x08, 20);
    sourceRoot_ = readString(d + 0x24, 64);
    hasChecksums_ = d[7] == 'C';

    const std::uint32_t namesSize = readLe32(d + 0x1c);
    const std::uint32_t fileCount = readLe32(d + 0x20);
    const std::uint32_t groupCount = readLe32(d + 0x190);
    const std::uint32_t tablesSize = readLe32(d + 0x194);
    const std::size_t recordSize = hasChecksums_ ? 12 : 8;
    const std::size_t tablesOffset = kHeaderSize + std::size_t{groupCount} * kGroupSize;
    const std::size_t namesOffset = tablesOffset + tablesSize;

    std::vector<std::string> extensions;
    for (std::size_t i = 0; i < kExtTableSlots; ++i)
        extensions.push_back(readString(d + kExtTableOffset + i * 4, 4));

    auto addEntry = [&](std::string path, std::uint32_t sector, std::uint32_t size, std::uint32_t checksum) {
        PakEntry entry;
        entry.path = toPath(std::move(path));
        entry.sector = sector;
        entry.storedSize = size & kSizeMask;
        entry.compressed = (size & kCompressedFlag) != 0;
        entry.checksum = checksum;
        entry.hasChecksum = hasChecksums_;
        entries_.push_back(std::move(entry));
    };

    // Numbered groups.
    for (std::uint32_t g = 0; g < groupCount; ++g)
    {
        const std::uint8_t* group = d + kHeaderSize + std::size_t{g} * kGroupSize;
        const std::string ext = readString(group, 4);
        const std::string dirName = readString(group + 4, 96);
        const std::uint32_t first = readLe32(group + 0x68);
        const std::uint32_t last = readLe32(group + 0x6c);
        const std::uint32_t table = readLe32(group + 0x70);
        if (last < first || table + (std::uint64_t{last - first} + 1) * recordSize > tablesSize)
        {
            error = std::format("group {} ({}) has a record table outside the directory", g, dirName);
            return false;
        }
        for (std::uint32_t n = first; n <= last; ++n)
        {
            const std::uint8_t* rec = d + tablesOffset + table + std::size_t{n - first} * recordSize;
            const std::uint32_t size = readLe32(rec);
            const std::uint32_t sector = readLe32(rec + 4);
            if (size == 0 && sector == 0)
                continue;
            std::string name = dirName.empty() ? std::string() : dirName + "\\";
            addEntry(name + std::to_string(n) + "." + ext, sector, size, hasChecksums_ ? readLe32(rec + 8) : 0);
        }
    }

    // Prefix-compressed name list.
    const std::size_t nodeHeader = hasChecksums_ ? 15 : 11;
    const std::size_t namesEnd = namesOffset + namesSize;
    std::string name;
    for (std::size_t pos = namesOffset; pos < namesEnd;)
    {
        if (pos + nodeHeader > namesEnd)
        {
            error = "name list is truncated";
            return false;
        }
        const std::uint8_t* node = d + pos;
        const std::uint32_t sector = node[0] | (node[1] << 8) | (node[2] << 16);
        const std::size_t prefix = node[3] & 0x7f;
        const std::size_t suffix = node[4];
        if (prefix > name.size() || pos + nodeHeader + suffix > namesEnd)
        {
            error = std::format("name list node at offset {:#x} is malformed", pos);
            return false;
        }
        name.resize(prefix);
        name.append(reinterpret_cast<const char*>(node + nodeHeader), suffix);
        pos += nodeHeader + suffix;

        if (name.empty())
            continue; // end of list
        std::string fullName = name;
        const auto last = static_cast<std::uint8_t>(fullName.back());
        if (last >= 1 && last <= kExtTableSlots && !extensions[last - 1].empty())
        {
            fullName.pop_back();
            fullName += "." + extensions[last - 1];
        }
        addEntry(std::move(fullName), sector, readLe32(node + 7), hasChecksums_ ? readLe32(node + 11) : 0);
    }

    if (entries_.size() != fileCount)
    {
        error = std::format("found {} files but the header says {}", entries_.size(), fileCount);
        return false;
    }
    for (const PakEntry& entry : entries_)
    {
        if (std::uint64_t{entry.sector} * kSectorSize + entry.storedSize > sizeBytes_)
        {
            error = entry.path + " lies outside the archive";
            return false;
        }
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> PakArchive::read(const PakEntry& entry, std::string& error) const
{
    const std::uint32_t sectors = (entry.storedSize + kSectorSize - 1) / kSectorSize;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(sectors) * kSectorSize);
    if (sectors != 0 && !reader_(entry.sector, sectors, data.data()))
    {
        error = "read failed";
        return std::nullopt;
    }
    data.resize(entry.storedSize);

    if (entry.compressed)
    {
        std::vector<std::uint8_t> expanded;
        if (!pakDecompress(data.data(), data.size(), expanded))
        {
            error = "decompression failed";
            return std::nullopt;
        }
        data = std::move(expanded);
    }

    if (entry.hasChecksum && pakChecksum(data.data(), data.size()) != entry.checksum)
    {
        error = "checksum mismatch";
        return std::nullopt;
    }
    return data;
}

bool pakDecompress(const std::uint8_t* src, std::size_t srcSize, std::vector<std::uint8_t>& out)
{
    if (srcSize < 4)
        return false;
    const std::uint32_t outSize = readLe32(src);
    if (outSize > kSizeMask)
        return false;
    out.assign(outSize, 0);

    // A port of the game's decoder (0x2a1d60). The running value carries over
    // from one lane to the next, as in the original.
    const std::size_t count = outSize / 2;
    std::size_t s = 4;
    std::uint16_t value = 0;
    auto put = [&](std::size_t index) {
        out[index * 2] = static_cast<std::uint8_t>(value);
        out[index * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    auto nibble = [](std::uint8_t n) { return static_cast<std::int16_t>(n >= 8 ? n - 16 : n); };

    for (std::size_t lane = 0; lane < 8; ++lane)
    {
        std::size_t p = lane;
        while (p < count)
        {
            if (s >= srcSize)
                return false;
            const std::uint8_t op = src[s++];
            if (op < 0x20) // op + 1 literal values
            {
                for (int i = 0; i <= op && p < count; ++i, p += 8)
                {
                    if (s + 2 > srcSize)
                        return false;
                    value = static_cast<std::uint16_t>(src[s] | (src[s + 1] << 8));
                    s += 2;
                    put(p);
                }
            }
            else if (op < 0x60) // op - 0x1f signed byte deltas
            {
                for (int i = 0; i <= op - 0x20 && p < count; ++i, p += 8)
                {
                    if (s >= srcSize)
                        return false;
                    value = static_cast<std::uint16_t>(value + static_cast<std::int8_t>(src[s++]));
                    put(p);
                }
            }
            else if (op < 0x80) // 2 * (op - 0x60) + 2 signed nibble deltas, high nibble first
            {
                if (s >= srcSize)
                    return false;
                value = static_cast<std::uint16_t>(value + nibble(src[s] >> 4));
                put(p);
                p += 8;
                for (int i = 0; i <= op - 0x60 && p < count; ++i)
                {
                    if (s >= srcSize)
                        return false;
                    const std::uint8_t b = src[s++];
                    value = static_cast<std::uint16_t>(value + nibble(b & 0x0f));
                    put(p);
                    p += 8;
                    if (i == op - 0x60 || p >= count)
                        break;
                    if (s >= srcSize)
                        return false;
                    value = static_cast<std::uint16_t>(value + nibble(src[s] >> 4));
                    put(p);
                    p += 8;
                }
            }
            else // op - 0x7f repeats of the current value
            {
                for (int i = 0; i <= op - 0x80 && p < count; ++i, p += 8)
                    put(p);
            }
        }
    }
    return true;
}

std::uint32_t pakChecksum(const std::uint8_t* data, std::size_t size)
{
    auto sum = static_cast<std::uint32_t>(size);
    std::size_t i = 0;
    for (; i + 4 <= size; i += 4)
        sum ^= readLe32(data + i);
    if (i < size)
    {
        std::uint8_t tail[4] = {};
        std::memcpy(tail, data + i, size - i);
        sum ^= readLe32(tail);
    }
    return sum;
}

} // namespace tgr::disc
