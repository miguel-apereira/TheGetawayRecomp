// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

class PS2Runtime;

// The C++ that ps2_recomp generated from SCUS_971.33 (recomp/generated),
// linked in when TGR_WITH_RECOMPILED_CODE is on.
namespace tgr::game
{

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
    // guest's cdrom0: root, and starts the guest thread.
    bool start(const std::filesystem::path& gameDataDir, std::string& error);

    // True once the guest thread has returned or died.
    bool finished() const;

    // Last guest PC the runtime recorded, for progress logs.
    std::uint32_t pc() const;

    // Asks the guest to stop and waits for its thread.
    void stop();

private:
    std::unique_ptr<PS2Runtime> runtime_;
    bool started_ = false;
};

} // namespace tgr::game
