// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

class GS;
struct GSRegisters;

namespace tgr::game
{

// What to record about the GS while the game runs.
struct GsToolOptions
{
    // --gs-dump: where GS dumps go. A path ending in ".tgrgs" is the file
    // itself; anything else is a folder that gets one numbered file per dump.
    std::optional<std::filesystem::path> dumpPath;
    std::optional<std::uint64_t> dumpStart; // --gs-dump-start: vsync to start at (else F9 starts it)
    std::uint64_t dumpVsyncs = 600;         // --gs-dump-vsyncs: how many vsyncs to record
    std::optional<std::filesystem::path> inventoryPath; // --gs-inventory: report written when the game stops
};

// The GS inventory and the GS dump recorder, fed by the runtime's GS host
// hook: `gs` is the runtime's GS, `privileged` its privileged registers and
// `localMemory` its 4 MiB of local memory.
class GsTools
{
public:
    GsTools(GS& gs, const GSRegisters& privileged, const std::uint8_t* localMemory, GsToolOptions options);
    ~GsTools();
    GsTools(const GsTools&) = delete;
    GsTools& operator=(const GsTools&) = delete;

    // Starts a dump at the next vsync, or stops the one being recorded.
    void toggleDump();

    // Writes the inventory report (if asked for) and closes a dump in progress.
    void finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tgr::game
