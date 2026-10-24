// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace tgr
{

// Writes RGBA8 rows (top row first) as a 32-bit BMP.
bool writeBmp(const std::filesystem::path& path, const std::vector<std::uint8_t>& rgba, std::uint32_t width,
              std::uint32_t height);

} // namespace tgr
