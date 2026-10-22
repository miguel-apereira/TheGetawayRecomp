// SPDX-License-Identifier: MIT
#include "disc/Crc32.h"

#include <array>

namespace tgr::disc
{

namespace
{
constexpr std::array<std::uint32_t, 256> makeTable()
{
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i)
    {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}

constexpr auto kTable = makeTable();
} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc)
{
    crc = ~crc;
    for (std::uint8_t byte : data)
        crc = kTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

} // namespace tgr::disc
