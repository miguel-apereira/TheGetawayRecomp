// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

// Walks a GIF packet the way the GS reads it: GIFtags, then their data in
// PACKED, REGLIST or IMAGE form. PACKED data is turned into the register
// writes it stands for (A+D form), so a visitor only sees register writes
// and image data.
namespace tgr::hw::gs
{

enum class GifFormat : std::uint8_t
{
    Packed = 0,
    Reglist = 1,
    Image = 2,
    Disabled = 3, // behaves like IMAGE
};

struct GifTag
{
    std::uint32_t nloop = 0;
    bool eop = false;
    bool pre = false;
    std::uint32_t prim = 0;
    GifFormat format = GifFormat::Packed;
    std::uint32_t nreg = 0; // 1-16
    std::uint64_t regs = 0; // register descriptors, 4 bits each, nreg of them
};

class GifVisitor
{
public:
    virtual ~GifVisitor() = default;
    virtual void tag(const GifTag&) {}
    // A register write as A+D would carry it.
    virtual void registerWrite(std::uint8_t address, std::uint64_t value) = 0;
    // IMAGE (or DISABLED) data for a transfer.
    virtual void imageData(const std::uint8_t* data, std::uint32_t bytes) = 0;
};

// Returns the bytes used (a whole number of quadwords). A packet cut short
// stops at the last complete item.
std::uint32_t parseGifPacket(const std::uint8_t* data, std::uint32_t bytes, GifVisitor& visitor);

} // namespace tgr::hw::gs
