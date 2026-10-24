// SPDX-License-Identifier: MIT
#include "app/Bmp.h"

#include <algorithm>
#include <fstream>

namespace tgr
{

bool writeBmp(const std::filesystem::path& path, const std::vector<std::uint8_t>& rgba, std::uint32_t width,
              std::uint32_t height)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    const std::uint32_t imageBytes = width * height * 4;
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    out.write("BM", 2);
    u32(14 + 40 + imageBytes);
    u32(0);
    u32(14 + 40);
    u32(40);
    u32(width);
    u32(static_cast<std::uint32_t>(-static_cast<std::int32_t>(height))); // top-down rows
    u16(1);
    u16(32);
    u32(0); // BI_RGB
    u32(imageBytes);
    u32(2835);
    u32(2835);
    u32(0);
    u32(0);
    std::vector<std::uint8_t> bgra(rgba.begin(),
                                   rgba.begin() + std::min<std::size_t>(rgba.size(), imageBytes));
    for (std::size_t i = 0; i + 3 < bgra.size(); i += 4)
        std::swap(bgra[i], bgra[i + 2]);
    bgra.resize(imageBytes);
    out.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    return static_cast<bool>(out);
}

} // namespace tgr
