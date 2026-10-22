// SPDX-License-Identifier: MIT
//
// tgr-disc: inspect the user's own ISO and extract the files the
// recompilation pipeline needs. Output stays in iso/, which git ignores.
//
//   tgr-disc info    <image.iso>
//   tgr-disc ls      <image.iso>
//   tgr-disc extract <image.iso> <out-dir> [--all]
//
// "extract" writes SYSTEM.CNF, the boot ELF, every IRX module and the IOPRP
// image, which is all the recompilation pipeline needs. With --all it does the same full install
// the game does on first run: every file plus disc_layout.tsv and install.txt.

#include "disc/GameData.h"
#include "disc/GameDisc.h"
#include "disc/IsoImage.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace tgr::disc;

namespace
{

int usage()
{
    std::fputs("usage:\n"
               "  tgr-disc info    <image.iso>\n"
               "  tgr-disc ls      <image.iso>\n"
               "  tgr-disc extract <image.iso> <out-dir> [--all]\n",
               stderr);
    return 2;
}

bool endsWithNoCase(std::string_view s, std::string_view suffix)
{
    if (s.size() < suffix.size())
        return false;
    return std::equal(suffix.begin(), suffix.end(), s.end() - static_cast<std::ptrdiff_t>(suffix.size()),
                      [](char a, char b) {
                          return std::toupper(static_cast<unsigned char>(a)) ==
                                 std::toupper(static_cast<unsigned char>(b));
                      });
}

int cmdInfo(const IsoImage& iso)
{
    std::printf("Volume ID : %s\n", iso.volumeId().c_str());
    std::printf("Size      : %llu bytes (%u sectors)\n", static_cast<unsigned long long>(iso.sizeBytes()),
                iso.sectorCount());

    const GameDiscCheck check = checkGameDisc(iso);
    if (check.info)
    {
        const GameDiscInfo& info = *check.info;
        std::printf("BOOT2     : %s\n", info.systemCnf.boot2.c_str());
        std::printf("VER       : %s\n", info.systemCnf.version.c_str());
        std::printf("VMODE     : %s\n", info.systemCnf.videoMode.c_str());
        std::printf("Boot ELF  : %s, %u bytes at LBA %u\n", info.bootElf.path.c_str(), info.bootElf.size,
                    info.bootElf.lba);
        std::printf("ELF CRC32 : %08X\n", info.bootElfCrc32);
    }
    std::printf("Supported : %s (%s)\n", check.ok ? "yes" : "no", check.message.c_str());
    return check.ok ? 0 : 1;
}

int cmdList(const IsoImage& iso)
{
    std::printf("%10s %12s  %s\n", "LBA", "SIZE", "PATH");
    for (const IsoEntry& entry : iso.listAll())
        std::printf("%10u %12u  %s%s\n", entry.lba, entry.size, entry.path.c_str(), entry.isDirectory ? "/" : "");
    return 0;
}

int cmdInstall(const IsoImage& iso, const fs::path& outDir)
{
    const GameDiscCheck check = checkGameDisc(iso);
    if (!check.ok || !check.info)
    {
        std::fprintf(stderr, "error: %s\n", check.message.c_str());
        return 1;
    }

    std::atomic<std::uint64_t> done{0};
    std::string error;
    if (!installGameData(iso, *check.info, outDir, done, error))
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("%llu bytes extracted to %s\n", static_cast<unsigned long long>(done.load()), outDir.string().c_str());
    return 0;
}

int cmdExtract(const IsoImage& iso, const fs::path& outDir)
{
    const GameDiscCheck check = checkGameDisc(iso);
    const std::string bootElf = check.info ? check.info->bootElf.path : std::string();

    int written = 0;
    int failed = 0;
    for (const IsoEntry& entry : iso.listAll())
    {
        if (entry.isDirectory)
            continue;
        // IOPRP*.IMG is the IOP replacement image: more IOP modules, loaded at boot.
        const bool wanted = entry.path == "SYSTEM.CNF" || entry.path == bootElf ||
                            endsWithNoCase(entry.path, ".IRX") || endsWithNoCase(entry.path, ".IMG");
        if (!wanted)
            continue;

        const fs::path target = outDir / fs::path(entry.path);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);

        const auto data = iso.readFile(entry);
        std::ofstream out(target, std::ios::binary);
        if (!data || !out.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size())))
        {
            std::fprintf(stderr, "failed: %s\n", entry.path.c_str());
            ++failed;
            continue;
        }
        std::printf("%s\n", entry.path.c_str());
        ++written;
    }

    std::printf("%d file(s) written to %s\n", written, outDir.string().c_str());
    if (!check.ok)
        std::fprintf(stderr, "warning: %s\n", check.message.c_str());
    return failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
        return usage();

    const std::string_view command = argv[1];
    std::string error;
    const auto iso = IsoImage::open(fs::path(argv[2]), error);
    if (!iso)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    if (command == "info")
        return cmdInfo(*iso);
    if (command == "ls")
        return cmdList(*iso);
    if (command == "extract" && argc >= 4)
    {
        const fs::path outDir(argv[3]);
        if (argc >= 5 && std::string_view(argv[4]) == "--all")
            return cmdInstall(*iso, outDir);
        return cmdExtract(*iso, outDir);
    }
    return usage();
}
