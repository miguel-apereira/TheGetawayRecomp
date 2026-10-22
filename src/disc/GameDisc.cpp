// SPDX-License-Identifier: MIT
#include "disc/GameDisc.h"

#include "disc/Crc32.h"
#include "game/GameBuild.h"

#include <format>

namespace tgr::disc
{

GameDiscCheck checkGameDisc(const IsoImage& iso)
{
    GameDiscCheck result;

    const auto cnfEntry = iso.find("SYSTEM.CNF");
    const auto cnfData = cnfEntry ? iso.readFile(*cnfEntry) : std::nullopt;
    if (!cnfData)
    {
        result.message = "SYSTEM.CNF not found: this does not look like a PS2 game disc";
        return result;
    }

    const std::string_view cnfText(reinterpret_cast<const char*>(cnfData->data()), cnfData->size());
    auto cnf = SystemCnf::parse(cnfText);
    if (!cnf)
    {
        result.message = "SYSTEM.CNF has no BOOT2 line";
        return result;
    }

    GameDiscInfo info;
    info.systemCnf = *cnf;

    const auto elfEntry = iso.find(cnf->bootElf);
    const auto elfData = elfEntry ? iso.readFile(*elfEntry) : std::nullopt;
    if (!elfData)
    {
        result.message = std::format("boot ELF {} listed in SYSTEM.CNF is missing from the disc", cnf->bootElf);
        return result;
    }
    info.bootElf = *elfEntry;
    info.bootElfCrc32 = crc32(*elfData);
    result.info = info;

    if (cnf->bootElf != game::kBootElf)
    {
        result.message = std::format("this disc boots {}, but only {} {} ({}) is supported", cnf->bootElf,
                                     game::kTitle, game::kSerial, game::kBootElf);
        return result;
    }

    if constexpr (game::kBootElfCrc32 != 0)
    {
        if (info.bootElfCrc32 != game::kBootElfCrc32)
        {
            result.message = std::format("{} has CRC32 {:08X}, expected {:08X}: a different revision or a bad dump",
                                         cnf->bootElf, info.bootElfCrc32, game::kBootElfCrc32);
            return result;
        }
    }

    result.ok = true;
    result.message = std::format("{} {} ({}, ELF CRC32 {:08X}{})", game::kTitle, game::kSerial, cnf->videoMode,
                                 info.bootElfCrc32, game::kBootElfCrc32 == 0 ? ", not pinned yet" : "");
    return result;
}

} // namespace tgr::disc
