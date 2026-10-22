// SPDX-License-Identifier: MIT
#include "disc/IsoImage.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace tgr::disc
{

namespace
{
constexpr std::uint32_t kPrimaryVolumeDescriptorLba = 16;
constexpr int kMaxDirectoryDepth = 32;

std::uint32_t readLe32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string trimRight(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
        s.pop_back();
    return s;
}

bool equalsNoCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
           });
}

// Parses one directory record. Returns false for the "." and ".." entries.
bool parseRecord(const std::uint8_t* rec, std::string_view parentPath, IsoEntry& out)
{
    const std::uint8_t nameLen = rec[32];
    const char* name = reinterpret_cast<const char*>(rec + 33);
    if (nameLen == 1 && (name[0] == '\0' || name[0] == '\1'))
        return false;

    std::string fileName(name, nameLen);
    if (auto semicolon = fileName.find(';'); semicolon != std::string::npos)
        fileName.resize(semicolon);
    if (!fileName.empty() && fileName.back() == '.')
        fileName.pop_back();

    out.lba = readLe32(rec + 2);
    out.size = readLe32(rec + 10);
    out.isDirectory = (rec[25] & 0x02) != 0;
    out.path = parentPath.empty() ? fileName : std::string(parentPath) + "/" + fileName;
    return true;
}
} // namespace

std::unique_ptr<IsoImage> IsoImage::open(const std::filesystem::path& path, std::string& error)
{
    std::unique_ptr<IsoImage> iso(new IsoImage());

    std::error_code ec;
    iso->sizeBytes_ = std::filesystem::file_size(path, ec);
    if (ec)
    {
        error = "cannot read " + path.string() + ": " + ec.message();
        return nullptr;
    }

    iso->file_.open(path, std::ios::binary);
    if (!iso->file_)
    {
        error = "cannot open " + path.string();
        return nullptr;
    }

    std::array<std::uint8_t, kSectorSize> pvd{};
    if (!iso->readSectors(kPrimaryVolumeDescriptorLba, 1, pvd.data()) || pvd[0] != 1 ||
        std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
    {
        error = path.string() + " is not an ISO9660 image (raw .bin/.cue dumps are not supported, use .iso)";
        return nullptr;
    }

    iso->volumeId_ = trimRight(std::string(reinterpret_cast<const char*>(pvd.data() + 40), 32));
    // The root directory record sits at offset 156 of the primary volume descriptor.
    const std::uint8_t* rootRecord = pvd.data() + 156;
    iso->root_.lba = readLe32(rootRecord + 2);
    iso->root_.size = readLe32(rootRecord + 10);
    iso->root_.isDirectory = true;
    return iso;
}

bool IsoImage::readSectors(std::uint32_t lba, std::uint32_t count, void* out) const
{
    const std::uint64_t offset = static_cast<std::uint64_t>(lba) * kSectorSize;
    const std::uint64_t bytes = static_cast<std::uint64_t>(count) * kSectorSize;
    if (offset + bytes > sizeBytes_)
        return false;

    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(static_cast<char*>(out), static_cast<std::streamsize>(bytes));
    return static_cast<std::uint64_t>(file_.gcount()) == bytes;
}

std::optional<std::vector<std::uint8_t>> IsoImage::readFile(const IsoEntry& entry) const
{
    if (entry.isDirectory)
        return std::nullopt;

    const std::uint32_t sectors = (entry.size + kSectorSize - 1) / kSectorSize;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(sectors) * kSectorSize);
    if (sectors != 0 && !readSectors(entry.lba, sectors, data.data()))
        return std::nullopt;
    data.resize(entry.size);
    return data;
}

std::vector<IsoEntry> IsoImage::readDirectory(const IsoEntry& dir) const
{
    std::vector<IsoEntry> entries;
    const std::uint32_t sectors = (dir.size + kSectorSize - 1) / kSectorSize;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(sectors) * kSectorSize);
    if (sectors == 0 || !readSectors(dir.lba, sectors, data.data()))
        return entries;

    // Records never cross a sector boundary; a zero length byte means "skip to the next sector".
    std::size_t pos = 0;
    while (pos < dir.size)
    {
        const std::uint8_t len = data[pos];
        if (len == 0)
        {
            pos = (pos / kSectorSize + 1) * kSectorSize;
            continue;
        }
        if (len < 34 || pos + len > data.size())
            break;

        IsoEntry entry;
        if (parseRecord(&data[pos], dir.path, entry))
            entries.push_back(std::move(entry));
        pos += len;
    }
    return entries;
}

void IsoImage::walk(const IsoEntry& dir, std::vector<IsoEntry>& out, int depth) const
{
    if (depth > kMaxDirectoryDepth)
        return;
    for (IsoEntry& entry : readDirectory(dir))
    {
        out.push_back(entry);
        if (entry.isDirectory)
            walk(entry, out, depth + 1);
    }
}

std::vector<IsoEntry> IsoImage::listAll() const
{
    std::vector<IsoEntry> out;
    walk(root_, out, 0);
    return out;
}

std::optional<IsoEntry> IsoImage::find(std::string_view path) const
{
    IsoEntry current = root_;
    while (!path.empty())
    {
        while (!path.empty() && (path.front() == '/' || path.front() == '\\'))
            path.remove_prefix(1);
        const std::size_t sep = path.find_first_of("/\\");
        std::string_view component = path.substr(0, sep);
        path = sep == std::string_view::npos ? std::string_view{} : path.substr(sep);
        if (auto semicolon = component.find(';'); semicolon != std::string_view::npos)
            component = component.substr(0, semicolon);
        if (component.empty())
            continue;

        bool found = false;
        for (IsoEntry& entry : readDirectory(current))
        {
            const std::string_view name = std::string_view(entry.path).substr(entry.path.rfind('/') + 1);
            if (equalsNoCase(name, component))
            {
                current = std::move(entry);
                found = true;
                break;
            }
        }
        if (!found)
            return std::nullopt;
    }
    return current;
}

} // namespace tgr::disc
