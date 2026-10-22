// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace tgr::disc
{

// SYSTEM.CNF from the root of a PS2 disc, for example:
//   BOOT2 = cdrom0:\SCUS_971.33;1
//   VER = 1.00
//   VMODE = NTSC
struct SystemCnf
{
    std::string boot2;   // full BOOT2 value
    std::string bootElf; // file name only: "SCUS_971.33"
    std::string version;
    std::string videoMode;

    static std::optional<SystemCnf> parse(std::string_view text);
};

} // namespace tgr::disc
