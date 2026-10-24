// SPDX-License-Identifier: MIT
#pragma once

#include "game/GsTools.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class PS2Runtime;

namespace tgr::hw
{
class Ipu;
class Vif;
namespace vu
{
class CodeMemory;
struct State;
}
}

// The C++ that ps2_recomp generated from SCUS_971.33 (recomp/generated),
// linked in when TGR_WITH_RECOMPILED_CODE is on.
namespace tgr::game
{

class Pfs;
class Stream;

struct RecompiledCodeInfo
{
    std::uint32_t tableBase = 0;   // first guest PC in the function table
    std::uint32_t tableEnd = 0;    // one past the last guest PC
    std::size_t entryCount = 0;    // guest PCs with a recompiled function
};

// Reads the generated function table. Referencing it is also what makes the
// linker keep the generated code in the executable.
RecompiledCodeInfo recompiledCodeInfo();

// The recompiled game running on the PS2Recomp runtime (memory, R5900
// context, kernel, IOP). The guest runs on its own thread; the host keeps its
// own window and main loop and polls finished().
class Guest
{
public:
    Guest();
    ~Guest();
    Guest(const Guest&) = delete;
    Guest& operator=(const Guest&) = delete;

    // Loads the boot ELF from the game data folder, which also becomes the
    // guest's cdrom0: root, puts our native IOP modules in place of the
    // game's and our native IPU in the machine, and starts the guest thread.
    bool start(const std::filesystem::path& gameDataDir, std::string& error);

    // GS inventory and GS dumps (see GsTools). Set before start().
    void setGsTools(GsToolOptions options) { gsToolOptions_ = std::move(options); }
    // F9: starts a GS dump at the next vsync, or stops the one in progress.
    void toggleGsDump();

    // True once the guest thread has returned or died.
    bool finished() const;

    // Last guest PC the runtime recorded, for progress logs.
    std::uint32_t pc() const;

    // DMA, GIF, GS and VIF traffic so far, as one line for progress logs.
    std::string hardwareStats() const;

    // Every EE thread with its state, PC and what it waits on, one per line.
    std::string threadStates() const;

    // Vertical blanks the guest has seen since it started. The PS2 has 60 a
    // second, so their rate is the game's speed: 60 means full speed.
    std::uint64_t vsyncCount() const;

    // The graphics backend drawing the GS picture, for the window title.
    std::string_view rendererName() const;

    // A 32-bit word of EE memory, for progress logs (no synchronisation).
    std::uint32_t peek(std::uint32_t address) const;

    // Fills a 32-byte DualShock 2 report for a pad port; false when no pad
    // is plugged into it. Called on the guest thread.
    using PadReader = std::function<bool(int port, std::uint8_t* data, std::size_t size)>;
    void setPadReader(PadReader reader);

    // The picture the GS is displaying, as RGBA8 rows. False before the game
    // has set up a display.
    bool captureFrame(std::vector<std::uint8_t>& rgba, std::uint32_t& width, std::uint32_t& height);

    // Asks the guest to stop and waits for its thread.
    void stop();

private:
    std::unique_ptr<PS2Runtime> runtime_;
    std::unique_ptr<Pfs> pfs_;
    std::unique_ptr<Stream> stream_;
    std::unique_ptr<hw::Ipu> ipu_;
    std::unique_ptr<hw::vu::CodeMemory> vu0Code_;
    std::unique_ptr<hw::vu::CodeMemory> vu1Code_;
    std::unique_ptr<hw::vu::State> vu1_; // VU1's registers live between microprograms
    std::unique_ptr<hw::Vif> vif0_;
    std::unique_ptr<hw::Vif> vif1_;
    GsToolOptions gsToolOptions_;
    std::unique_ptr<GsTools> gsTools_;
    bool started_ = false;
};

} // namespace tgr::game
