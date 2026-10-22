// SPDX-License-Identifier: MIT
#pragma once

#include "disc/GameDisc.h"
#include "disc/IsoImage.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace tgr::disc
{

// The user's disc is extracted once into a game data folder. The folder
// mirrors the disc's file tree and carries two files of our own:
//
//   disc_layout.tsv  one line per file: LBA <tab> size <tab> path, so the
//                    game's sector-based reads can be mapped back to files
//   install.txt      serial and boot ELF CRC32, written last; a folder
//                    without it is an interrupted extraction
inline constexpr const char* kLayoutFileName = "disc_layout.tsv";
inline constexpr const char* kInstallMarkerName = "install.txt";

// True if dir holds a complete extraction of the supported build.
bool isGameDataInstalled(const std::filesystem::path& dir);

// Total bytes installGameData() will write, for progress reporting.
std::uint64_t gameDataSize(const IsoImage& iso);

// Extracts every file on the disc into dir. bytesDone is updated as data is
// written, so another thread can show progress.
bool installGameData(const IsoImage& iso, const GameDiscInfo& disc, const std::filesystem::path& dir,
                     std::atomic<std::uint64_t>& bytesDone, std::string& error);

} // namespace tgr::disc
