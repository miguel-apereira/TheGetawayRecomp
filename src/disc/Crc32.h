// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace tgr::disc
{

// Standard CRC-32 (zlib/PNG polynomial), the same checksum PS2Recomp's game
// overrides and PCSX2 use to identify an ELF.
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);

} // namespace tgr::disc
