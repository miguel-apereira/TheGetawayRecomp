// SPDX-License-Identifier: MIT
#pragma once

#include "disc/IsoImage.h"
#include "disc/SystemCnf.h"

#include <cstdint>
#include <optional>
#include <string>

namespace tgr::disc
{

struct GameDiscInfo
{
    SystemCnf systemCnf;
    IsoEntry bootElf;
    std::uint32_t bootElfCrc32 = 0;
};

struct GameDiscCheck
{
    bool ok = false;
    std::string message; // why the disc was rejected, or a one-line summary when ok
    std::optional<GameDiscInfo> info;
};

// Reads SYSTEM.CNF and the boot ELF and checks them against game/GameBuild.h.
GameDiscCheck checkGameDisc(const IsoImage& iso);

} // namespace tgr::disc
