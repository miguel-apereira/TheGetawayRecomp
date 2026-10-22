// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

// The one retail build this project supports. Every address in
// recomp/config/ and every game override belongs to this exact executable.
namespace tgr::game
{

inline constexpr std::string_view kTitle = "The Getaway";
inline constexpr std::string_view kSerial = "SCUS-97133";
inline constexpr std::string_view kRegion = "NTSC-U";
inline constexpr std::string_view kBootElf = "SCUS_971.33";

// CRC-32 of the boot ELF, as printed by `tgr-disc info` (3,101,036 bytes,
// LBA 279, VER 1.00). Pinned from a verified dump on 2026-10-22.
inline constexpr std::uint32_t kBootElfCrc32 = 0x82DA90E7;

} // namespace tgr::game
