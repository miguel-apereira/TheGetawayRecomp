// SPDX-License-Identifier: MIT
#include "hw/gs/GifParser.h"

#include "hw/gs/GsRegs.h"

#include <cstring>

namespace tgr::hw::gs
{

namespace
{

std::uint64_t load64(const std::uint8_t* p)
{
    std::uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

constexpr std::uint32_t kOneFloat = 0x3F800000u; // the GIF sets Q to 1.0 at each GIFtag

// One PACKED quadword for register descriptor `desc`.
void packed(std::uint8_t desc, std::uint64_t lo, std::uint64_t hi, std::uint32_t& q, GifVisitor& visitor)
{
    switch (desc)
    {
    case 0x0: // PRIM
        visitor.registerWrite(reg::PRIM, lo & 0x7FF);
        break;
    case 0x1: // RGBAQ: R, G, B, A in the low byte of each word; Q from the last ST
        visitor.registerWrite(reg::RGBAQ, (lo & 0xFF) | (bits(lo, 32, 8) << 8) | ((hi & 0xFF) << 16) |
                                              (static_cast<std::uint64_t>(bits(hi, 32, 8)) << 24) |
                                              (static_cast<std::uint64_t>(q) << 32));
        break;
    case 0x2: // ST, and Q for the following RGBAQ
        q = static_cast<std::uint32_t>(hi);
        visitor.registerWrite(reg::ST, lo);
        break;
    case 0x3: // UV
        visitor.registerWrite(reg::UV,
                              bits(lo, 0, 14) | (static_cast<std::uint64_t>(bits(lo, 32, 14)) << 16));
        break;
    case 0x4: // XYZF2, or XYZF3 when ADC is set
    case 0xC:
    {
        const bool noKick = desc == 0xC || bits(hi, 47, 1);
        visitor.registerWrite(noKick ? reg::XYZF3 : reg::XYZF2,
                              bits(lo, 0, 16) | (static_cast<std::uint64_t>(bits(lo, 32, 16)) << 16) |
                                  (static_cast<std::uint64_t>(bits(hi, 4, 24)) << 32) |
                                  (static_cast<std::uint64_t>(bits(hi, 36, 8)) << 56));
        break;
    }
    case 0x5: // XYZ2, or XYZ3 when ADC is set
    case 0xD:
    {
        const bool noKick = desc == 0xD || bits(hi, 47, 1);
        visitor.registerWrite(noKick ? reg::XYZ3 : reg::XYZ2,
                              bits(lo, 0, 16) | (static_cast<std::uint64_t>(bits(lo, 32, 16)) << 16) |
                                  (static_cast<std::uint64_t>(bits(hi, 0, 32)) << 32));
        break;
    }
    case 0xA: // FOG
        visitor.registerWrite(reg::FOG, static_cast<std::uint64_t>(bits(hi, 36, 8)) << 56);
        break;
    case 0xB: // reserved
    case 0xF: // NOP
        break;
    case 0xE: // A+D
        visitor.registerWrite(static_cast<std::uint8_t>(hi & 0xFF), lo);
        break;
    default: // TEX0_1/2, CLAMP_1/2: the low 64 bits as they are
        visitor.registerWrite(desc, lo);
        break;
    }
}

} // namespace

std::uint32_t parseGifPacket(const std::uint8_t* data, std::uint32_t bytes, GifVisitor& visitor)
{
    std::uint32_t at = 0;
    while (at + 16 <= bytes)
    {
        const std::uint64_t lo = load64(data + at);
        const std::uint64_t hi = load64(data + at + 8);
        GifTag tag;
        tag.nloop = bits(lo, 0, 15);
        tag.eop = bits(lo, 15, 1);
        tag.pre = bits(lo, 46, 1);
        tag.prim = bits(lo, 47, 11);
        tag.format = static_cast<GifFormat>(bits(lo, 58, 2));
        tag.nreg = bits(lo, 60, 4) ? bits(lo, 60, 4) : 16;
        tag.regs = tag.nreg == 16 ? hi : hi & ((1ull << (tag.nreg * 4)) - 1);
        at += 16;
        visitor.tag(tag);

        if (tag.pre && (tag.format == GifFormat::Packed || tag.format == GifFormat::Reglist))
            visitor.registerWrite(reg::PRIM, tag.prim);

        switch (tag.format)
        {
        case GifFormat::Packed:
        {
            std::uint32_t q = kOneFloat;
            for (std::uint32_t loop = 0; loop < tag.nloop; ++loop)
            {
                for (std::uint32_t r = 0; r < tag.nreg; ++r)
                {
                    if (at + 16 > bytes)
                        return at;
                    packed(static_cast<std::uint8_t>((tag.regs >> (r * 4)) & 0xF), load64(data + at),
                           load64(data + at + 8), q, visitor);
                    at += 16;
                }
            }
            break;
        }
        case GifFormat::Reglist:
        {
            const std::uint64_t words = static_cast<std::uint64_t>(tag.nloop) * tag.nreg;
            for (std::uint64_t i = 0; i < words; ++i)
            {
                if (at + 8 > bytes)
                    return at & ~15u;
                const std::uint8_t desc = static_cast<std::uint8_t>((tag.regs >> ((i % tag.nreg) * 4)) & 0xF);
                if (desc != 0xE && desc != 0xF) // A+D and NOP write nothing in REGLIST
                    visitor.registerWrite(desc, load64(data + at));
                at += 8;
            }
            if (words & 1)
                at += 8;
            break;
        }
        case GifFormat::Image:
        case GifFormat::Disabled:
        {
            std::uint32_t imageBytes = tag.nloop * 16;
            if (at + imageBytes > bytes)
                imageBytes = (bytes - at) & ~15u;
            if (imageBytes)
                visitor.imageData(data + at, imageBytes);
            at += imageBytes;
            break;
        }
        }
    }
    return at;
}

} // namespace tgr::hw::gs
