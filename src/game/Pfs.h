// SPDX-License-Identifier: MIT
#pragma once

#include "disc/PakArchive.h"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class PS2Runtime;

// Native replacement for PFS.IRX, The Getaway's own IOP file server (Team
// Soho's, not Sony's PFS). The game talks to it over SIF RPC server 0x791:
// it queues file reads by disc sector or by name, PFS reads them and copies
// the data into EE memory, then calls the game's own EE-side RPC server
// 0x791 back to say each read is done.
//
// Here the reads come from the game data folder the disc was extracted to,
// using disc_layout.tsv to turn sectors back into files. Queued reads finish
// on the next vertical blank, like the original's vblank-driven main loop.
//
// PFS also loads sound data from PACK_IOP.PAK into IOP memory for STREAM.IRX:
// an XML sound pack and two stream slots. Those are kept here, by name, for
// the native STREAM, and the EE is told when each one is in.
namespace tgr::game
{

class Pfs
{
public:
    static constexpr std::uint32_t kRpcId = 0x791;

    explicit Pfs(std::filesystem::path gameDataDir);

    // Replaces PFS.IRX in the runtime: its module load, RPC server and main loop.
    void install(PS2Runtime& runtime);

private:
    // One queued read, as the EE sends it (0x70 bytes).
    struct Request
    {
        std::string name;               // file name, used when sector <= 0
        std::uint8_t priority = 0;
        std::uint32_t statusAddress = 0; // EE: 0x80-byte status slot
        std::int32_t sector = 0;         // disc or PAK sector
        std::uint32_t size = 0;
        std::uint32_t destination = 0;   // EE address of the data
    };

    struct DiscFile
    {
        std::uint32_t sector = 0;
        std::uint64_t size = 0;
        std::string path; // relative to the game data folder
    };

    // A call PFS makes on the game's EE server once a load is done.
    struct EeCall
    {
        std::uint32_t function = 0;
        std::array<std::uint32_t, 4> data{}; // the IRX always sends 16 bytes
        bool hasData = false;
    };

    void serve(std::uint8_t* rdram, std::uint32_t function, std::uint32_t send, std::uint32_t sendSize);
    void onVBlank(std::uint8_t* rdram);

    void queueRead(std::uint8_t* rdram, std::uint32_t send);
    void openEePack(const std::string& name);
    void openIopPack(const std::string& name);
    bool readIopFile(const std::string& name, std::vector<std::uint8_t>& data);
    void loadStreamSlot(std::uint32_t slot, const std::string& name);
    void loadXmlPack(const std::string& name);
    void finishRead(std::uint8_t* rdram, const Request& request);
    bool readRequest(const Request& request, std::vector<std::uint8_t>& data);
    bool readFile(const std::filesystem::path& path, std::uint64_t offset, std::uint32_t size,
                  std::vector<std::uint8_t>& data);

    void loadLayout();
    const DiscFile* fileAtSector(std::uint32_t sector) const;
    const DiscFile* fileNamed(std::string_view name) const;

    std::filesystem::path dataDir_;
    std::vector<DiscFile> layout_;
    PS2Runtime* runtime_ = nullptr;

    bool eeIsCd_ = false;
    bool iopIsCd_ = false;
    bool eeIsPak_ = false;
    std::uint32_t eeBaseSector_ = 0;
    std::filesystem::path eePack_;
    std::string eeLocation_; // directory prefixes from the pack header, for host reads
    std::string eeNetLocation_;
    std::uint32_t eeStreamAddress_ = 0;
    std::uint32_t eeStreamSize_ = 0;

    std::unique_ptr<disc::PakArchive> iopPack_;
    std::vector<std::uint8_t> xmlPack_;
    std::array<std::vector<std::uint8_t>, 2> streamSlots_;

    std::deque<Request> requests_;
    std::deque<EeCall> eeCalls_;
    bool announced_ = false;
};

} // namespace tgr::game
