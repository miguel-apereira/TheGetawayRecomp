// SPDX-License-Identifier: MIT
#include "hw/gs/GsRegs.h"

#include <format>

namespace tgr::hw::gs
{

std::string_view registerName(std::uint8_t address)
{
    switch (address)
    {
    case reg::PRIM:
        return "PRIM";
    case reg::RGBAQ:
        return "RGBAQ";
    case reg::ST:
        return "ST";
    case reg::UV:
        return "UV";
    case reg::XYZF2:
        return "XYZF2";
    case reg::XYZ2:
        return "XYZ2";
    case reg::TEX0_1:
        return "TEX0_1";
    case reg::TEX0_2:
        return "TEX0_2";
    case reg::CLAMP_1:
        return "CLAMP_1";
    case reg::CLAMP_2:
        return "CLAMP_2";
    case reg::FOG:
        return "FOG";
    case reg::XYZF3:
        return "XYZF3";
    case reg::XYZ3:
        return "XYZ3";
    case reg::TEX1_1:
        return "TEX1_1";
    case reg::TEX1_2:
        return "TEX1_2";
    case reg::TEX2_1:
        return "TEX2_1";
    case reg::TEX2_2:
        return "TEX2_2";
    case reg::XYOFFSET_1:
        return "XYOFFSET_1";
    case reg::XYOFFSET_2:
        return "XYOFFSET_2";
    case reg::PRMODECONT:
        return "PRMODECONT";
    case reg::PRMODE:
        return "PRMODE";
    case reg::TEXCLUT:
        return "TEXCLUT";
    case reg::SCANMSK:
        return "SCANMSK";
    case reg::MIPTBP1_1:
        return "MIPTBP1_1";
    case reg::MIPTBP1_2:
        return "MIPTBP1_2";
    case reg::MIPTBP2_1:
        return "MIPTBP2_1";
    case reg::MIPTBP2_2:
        return "MIPTBP2_2";
    case reg::TEXA:
        return "TEXA";
    case reg::FOGCOL:
        return "FOGCOL";
    case reg::TEXFLUSH:
        return "TEXFLUSH";
    case reg::SCISSOR_1:
        return "SCISSOR_1";
    case reg::SCISSOR_2:
        return "SCISSOR_2";
    case reg::ALPHA_1:
        return "ALPHA_1";
    case reg::ALPHA_2:
        return "ALPHA_2";
    case reg::DIMX:
        return "DIMX";
    case reg::DTHE:
        return "DTHE";
    case reg::COLCLAMP:
        return "COLCLAMP";
    case reg::TEST_1:
        return "TEST_1";
    case reg::TEST_2:
        return "TEST_2";
    case reg::PABE:
        return "PABE";
    case reg::FBA_1:
        return "FBA_1";
    case reg::FBA_2:
        return "FBA_2";
    case reg::FRAME_1:
        return "FRAME_1";
    case reg::FRAME_2:
        return "FRAME_2";
    case reg::ZBUF_1:
        return "ZBUF_1";
    case reg::ZBUF_2:
        return "ZBUF_2";
    case reg::BITBLTBUF:
        return "BITBLTBUF";
    case reg::TRXPOS:
        return "TRXPOS";
    case reg::TRXREG:
        return "TRXREG";
    case reg::TRXDIR:
        return "TRXDIR";
    case reg::HWREG:
        return "HWREG";
    case reg::SIGNAL:
        return "SIGNAL";
    case reg::FINISH:
        return "FINISH";
    case reg::LABEL:
        return "LABEL";
    default:
        return "";
    }
}

std::string psmName(std::uint64_t value)
{
    switch (value)
    {
    case psm::CT32:
        return "CT32";
    case psm::CT24:
        return "CT24";
    case psm::CT16:
        return "CT16";
    case psm::CT16S:
        return "CT16S";
    case psm::T8:
        return "T8";
    case psm::T4:
        return "T4";
    case psm::T8H:
        return "T8H";
    case psm::T4HL:
        return "T4HL";
    case psm::T4HH:
        return "T4HH";
    case psm::Z32:
        return "Z32";
    case psm::Z24:
        return "Z24";
    case psm::Z16:
        return "Z16";
    case psm::Z16S:
        return "Z16S";
    default:
        return std::format("PSM 0x{:02X}", value);
    }
}

std::uint32_t psmBits(std::uint64_t value)
{
    switch (value)
    {
    case psm::CT32:
    case psm::Z32:
    case psm::CT24: // stored in 32 bits in memory, 24 in a transfer
    case psm::Z24:
    case psm::T8H:
    case psm::T4HL:
    case psm::T4HH:
        return 32;
    case psm::CT16:
    case psm::CT16S:
    case psm::Z16:
    case psm::Z16S:
        return 16;
    case psm::T8:
        return 8;
    case psm::T4:
        return 4;
    default:
        return 0;
    }
}

std::uint32_t psmTransferBits(std::uint64_t value)
{
    switch (value)
    {
    case psm::CT24:
    case psm::Z24:
        return 24;
    case psm::T8H:
        return 8;
    case psm::T4HL:
    case psm::T4HH:
        return 4;
    default:
        return psmBits(value);
    }
}

PageSize psmPageSize(std::uint64_t value)
{
    switch (psmBits(value))
    {
    case 16:
        return {64, 64};
    case 8:
        return {128, 64};
    case 4:
        return {128, 128};
    default:
        return {64, 32};
    }
}

std::string_view primTypeName(PrimType type)
{
    switch (type)
    {
    case PrimType::Point:
        return "point";
    case PrimType::Line:
        return "line";
    case PrimType::LineStrip:
        return "line strip";
    case PrimType::Triangle:
        return "triangle";
    case PrimType::TriangleStrip:
        return "triangle strip";
    case PrimType::TriangleFan:
        return "triangle fan";
    case PrimType::Sprite:
        return "sprite";
    default:
        return "invalid (7)";
    }
}

} // namespace tgr::hw::gs
