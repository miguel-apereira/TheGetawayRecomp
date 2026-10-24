// SPDX-License-Identifier: MIT
#pragma once

#include "hw/gs/GsRegs.h"

#include <cstdint>

// Everything that reaches the GS, in the order it arrives: from the game
// (through the runtime's GS host hook) or from a GS dump being replayed.
namespace tgr::hw::gs
{

struct ImageUploadRegs
{
    std::uint64_t bitbltbuf = 0;
    std::uint64_t trxpos = 0;
    std::uint64_t trxreg = 0;
    std::uint64_t trxdir = 0;
};

class GsSink
{
public:
    virtual ~GsSink() = default;
    // A GIF packet after PATH arbitration.
    virtual void gifPacket(const std::uint8_t* data, std::uint32_t bytes) = 0;
    // A register write that did not come through the GIF.
    virtual void registerWrite(std::uint8_t address, std::uint64_t value) = 0;
    // A host-to-local transfer started directly: the four registers, then the data.
    virtual void imageUpload(const ImageUploadRegs& regs, const std::uint8_t* data, std::uint32_t bytes) = 0;
    // The runtime's whole-frame-buffer fill for a context.
    virtual void clearFrame(std::uint32_t context, std::uint32_t rgba) = 0;
    // The game reads back up to `bytes` of a local-to-host transfer.
    virtual void localToHost(std::uint32_t bytes) = 0;
    // A vertical blank starts; `privileged` is what the game has set to display.
    virtual void vsync(std::uint64_t tick, const Privileged& privileged) = 0;
};

} // namespace tgr::hw::gs
