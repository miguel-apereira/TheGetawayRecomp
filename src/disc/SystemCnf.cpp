// SPDX-License-Identifier: MIT
#include "disc/SystemCnf.h"

namespace tgr::disc
{

namespace
{
std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\0'))
        s.remove_suffix(1);
    return s;
}
} // namespace

std::optional<SystemCnf> SystemCnf::parse(std::string_view text)
{
    SystemCnf cnf;
    while (!text.empty())
    {
        const std::size_t eol = text.find('\n');
        const std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));

        if (key == "BOOT2")
            cnf.boot2 = value;
        else if (key == "VER")
            cnf.version = value;
        else if (key == "VMODE")
            cnf.videoMode = value;
    }

    if (cnf.boot2.empty())
        return std::nullopt;

    // "cdrom0:\SCUS_971.33;1" -> "SCUS_971.33"
    std::string_view elf = cnf.boot2;
    if (const std::size_t sep = elf.find_last_of("\\/:"); sep != std::string_view::npos)
        elf = elf.substr(sep + 1);
    if (const std::size_t semicolon = elf.find(';'); semicolon != std::string_view::npos)
        elf = elf.substr(0, semicolon);
    cnf.bootElf = elf;
    return cnf;
}

} // namespace tgr::disc
