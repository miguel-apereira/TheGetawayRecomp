// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// GS register addresses and field decoding, from the public GS register
// documentation. Values are the 64-bit register contents as an A+D write
// carries them.
namespace tgr::hw::gs
{

namespace reg
{
inline constexpr std::uint8_t PRIM = 0x00;
inline constexpr std::uint8_t RGBAQ = 0x01;
inline constexpr std::uint8_t ST = 0x02;
inline constexpr std::uint8_t UV = 0x03;
inline constexpr std::uint8_t XYZF2 = 0x04;
inline constexpr std::uint8_t XYZ2 = 0x05;
inline constexpr std::uint8_t TEX0_1 = 0x06;
inline constexpr std::uint8_t TEX0_2 = 0x07;
inline constexpr std::uint8_t CLAMP_1 = 0x08;
inline constexpr std::uint8_t CLAMP_2 = 0x09;
inline constexpr std::uint8_t FOG = 0x0A;
inline constexpr std::uint8_t XYZF3 = 0x0C;
inline constexpr std::uint8_t XYZ3 = 0x0D;
inline constexpr std::uint8_t TEX1_1 = 0x14;
inline constexpr std::uint8_t TEX1_2 = 0x15;
inline constexpr std::uint8_t TEX2_1 = 0x16;
inline constexpr std::uint8_t TEX2_2 = 0x17;
inline constexpr std::uint8_t XYOFFSET_1 = 0x18;
inline constexpr std::uint8_t XYOFFSET_2 = 0x19;
inline constexpr std::uint8_t PRMODECONT = 0x1A;
inline constexpr std::uint8_t PRMODE = 0x1B;
inline constexpr std::uint8_t TEXCLUT = 0x1C;
inline constexpr std::uint8_t SCANMSK = 0x22;
inline constexpr std::uint8_t MIPTBP1_1 = 0x34;
inline constexpr std::uint8_t MIPTBP1_2 = 0x35;
inline constexpr std::uint8_t MIPTBP2_1 = 0x36;
inline constexpr std::uint8_t MIPTBP2_2 = 0x37;
inline constexpr std::uint8_t TEXA = 0x3B;
inline constexpr std::uint8_t FOGCOL = 0x3D;
inline constexpr std::uint8_t TEXFLUSH = 0x3F;
inline constexpr std::uint8_t SCISSOR_1 = 0x40;
inline constexpr std::uint8_t SCISSOR_2 = 0x41;
inline constexpr std::uint8_t ALPHA_1 = 0x42;
inline constexpr std::uint8_t ALPHA_2 = 0x43;
inline constexpr std::uint8_t DIMX = 0x44;
inline constexpr std::uint8_t DTHE = 0x45;
inline constexpr std::uint8_t COLCLAMP = 0x46;
inline constexpr std::uint8_t TEST_1 = 0x47;
inline constexpr std::uint8_t TEST_2 = 0x48;
inline constexpr std::uint8_t PABE = 0x49;
inline constexpr std::uint8_t FBA_1 = 0x4A;
inline constexpr std::uint8_t FBA_2 = 0x4B;
inline constexpr std::uint8_t FRAME_1 = 0x4C;
inline constexpr std::uint8_t FRAME_2 = 0x4D;
inline constexpr std::uint8_t ZBUF_1 = 0x4E;
inline constexpr std::uint8_t ZBUF_2 = 0x4F;
inline constexpr std::uint8_t BITBLTBUF = 0x50;
inline constexpr std::uint8_t TRXPOS = 0x51;
inline constexpr std::uint8_t TRXREG = 0x52;
inline constexpr std::uint8_t TRXDIR = 0x53;
inline constexpr std::uint8_t HWREG = 0x54;
inline constexpr std::uint8_t SIGNAL = 0x60;
inline constexpr std::uint8_t FINISH = 0x61;
inline constexpr std::uint8_t LABEL = 0x62;

inline constexpr std::uint32_t kCount = 0x80; // addresses 0x00-0x7F
} // namespace reg

// Name of a register address ("TEX0_1"), or "" for an unused one.
std::string_view registerName(std::uint8_t address);

// Pixel storage formats (PSM).
namespace psm
{
inline constexpr std::uint32_t CT32 = 0x00;
inline constexpr std::uint32_t CT24 = 0x01;
inline constexpr std::uint32_t CT16 = 0x02;
inline constexpr std::uint32_t CT16S = 0x0A;
inline constexpr std::uint32_t T8 = 0x13;
inline constexpr std::uint32_t T4 = 0x14;
inline constexpr std::uint32_t T8H = 0x1B;
inline constexpr std::uint32_t T4HL = 0x24;
inline constexpr std::uint32_t T4HH = 0x2C;
inline constexpr std::uint32_t Z32 = 0x30;
inline constexpr std::uint32_t Z24 = 0x31;
inline constexpr std::uint32_t Z16 = 0x32;
inline constexpr std::uint32_t Z16S = 0x3A;
} // namespace psm

// "CT32", "T8", ... or "PSM 0x.." for an invalid value.
std::string psmName(std::uint64_t value);

// Bits per pixel of a format as stored in local memory (0 for an invalid one).
std::uint32_t psmBits(std::uint64_t value);

// Bits per pixel of a format in a host-local transfer (CT24 sends 24, T8H 8).
std::uint32_t psmTransferBits(std::uint64_t value);

// Size of a page (8 KiB) in pixels for a format, used to work out how much
// of local memory a buffer covers.
struct PageSize
{
    std::uint32_t width = 64;
    std::uint32_t height = 32;
};
PageSize psmPageSize(std::uint64_t value);

constexpr std::uint32_t bits(std::uint64_t value, unsigned first, unsigned count)
{
    return static_cast<std::uint32_t>((value >> first) & ((count >= 64 ? ~0ull : (1ull << count) - 1)));
}

// Primitive types (PRIM bits 0-2).
enum class PrimType : std::uint8_t
{
    Point = 0,
    Line = 1,
    LineStrip = 2,
    Triangle = 3,
    TriangleStrip = 4,
    TriangleFan = 5,
    Sprite = 6,
    Invalid = 7,
};
std::string_view primTypeName(PrimType type);

struct Prim
{
    explicit Prim(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    PrimType type() const { return static_cast<PrimType>(bits(value, 0, 3)); }
    bool iip() const { return bits(value, 3, 1); } // Gouraud shading
    bool tme() const { return bits(value, 4, 1); } // texture mapping
    bool fge() const { return bits(value, 5, 1); } // fog
    bool abe() const { return bits(value, 6, 1); } // alpha blending
    bool aa1() const { return bits(value, 7, 1); } // antialiasing
    bool fst() const { return bits(value, 8, 1); } // UV (fixed point) instead of STQ
    std::uint32_t ctxt() const { return bits(value, 9, 1); }
    bool fix() const { return bits(value, 10, 1); }
};

struct Tex0
{
    explicit Tex0(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    std::uint32_t tbp0() const { return bits(value, 0, 14); } // in 256-byte blocks
    std::uint32_t tbw() const { return bits(value, 14, 6); }  // in 64 pixels
    std::uint32_t psm() const { return bits(value, 20, 6); }
    std::uint32_t tw() const { return bits(value, 26, 4); } // log2 width
    std::uint32_t th() const { return bits(value, 30, 4); } // log2 height
    bool tcc() const { return bits(value, 34, 1); }
    std::uint32_t tfx() const { return bits(value, 35, 2); }
    std::uint32_t cbp() const { return bits(value, 37, 14); }
    std::uint32_t cpsm() const { return bits(value, 51, 4); }
    std::uint32_t csm() const { return bits(value, 55, 1); }
    std::uint32_t csa() const { return bits(value, 56, 5); }
    std::uint32_t cld() const { return bits(value, 61, 3); }
};

struct Tex1
{
    explicit Tex1(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    std::uint32_t lcm() const { return bits(value, 0, 1); }
    std::uint32_t mxl() const { return bits(value, 2, 3); }
    std::uint32_t mmag() const { return bits(value, 5, 1); }
    std::uint32_t mmin() const { return bits(value, 6, 3); }
    std::uint32_t mtba() const { return bits(value, 9, 1); }
    std::uint32_t l() const { return bits(value, 19, 2); }
    std::uint32_t k() const { return bits(value, 32, 12); }
};

struct Clamp
{
    explicit Clamp(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    std::uint32_t wms() const { return bits(value, 0, 2); }
    std::uint32_t wmt() const { return bits(value, 2, 2); }
};

struct Alpha
{
    explicit Alpha(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    std::uint32_t a() const { return bits(value, 0, 2); }
    std::uint32_t b() const { return bits(value, 2, 2); }
    std::uint32_t c() const { return bits(value, 4, 2); }
    std::uint32_t d() const { return bits(value, 6, 2); }
    std::uint32_t fix() const { return bits(value, 32, 8); }
};

struct Test
{
    explicit Test(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    bool ate() const { return bits(value, 0, 1); }
    std::uint32_t atst() const { return bits(value, 1, 3); }
    std::uint32_t aref() const { return bits(value, 4, 8); }
    std::uint32_t afail() const { return bits(value, 12, 2); }
    bool date() const { return bits(value, 14, 1); }
    std::uint32_t datm() const { return bits(value, 15, 1); }
    bool zte() const { return bits(value, 16, 1); }
    std::uint32_t ztst() const { return bits(value, 17, 2); }
};

struct Frame
{
    explicit Frame(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    std::uint32_t fbp() const { return bits(value, 0, 9); }  // in 8 KiB pages
    std::uint32_t fbw() const { return bits(value, 16, 6); } // in 64 pixels
    std::uint32_t psm() const { return bits(value, 24, 6); }
    std::uint32_t fbmsk() const { return bits(value, 32, 32); }
};

struct Zbuf
{
    explicit Zbuf(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    std::uint32_t zbp() const { return bits(value, 0, 9); } // in 8 KiB pages
    std::uint32_t psm() const { return bits(value, 24, 4) | 0x30; }
    bool zmsk() const { return bits(value, 32, 1); }
};

struct Scissor
{
    explicit Scissor(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    std::uint32_t x0() const { return bits(value, 0, 11); }
    std::uint32_t x1() const { return bits(value, 16, 11); }
    std::uint32_t y0() const { return bits(value, 32, 11); }
    std::uint32_t y1() const { return bits(value, 48, 11); }
};

struct BitBltBuf
{
    explicit BitBltBuf(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    std::uint32_t sbp() const { return bits(value, 0, 14); }
    std::uint32_t sbw() const { return bits(value, 16, 6); }
    std::uint32_t spsm() const { return bits(value, 24, 6); }
    std::uint32_t dbp() const { return bits(value, 32, 14); }
    std::uint32_t dbw() const { return bits(value, 48, 6); }
    std::uint32_t dpsm() const { return bits(value, 56, 6); }
};

struct TrxReg
{
    explicit TrxReg(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    std::uint32_t rrw() const { return bits(value, 0, 12); }
    std::uint32_t rrh() const { return bits(value, 32, 12); }
};

// Privileged registers (0x12000000), as the game last wrote them.
struct Privileged
{
    std::uint64_t pmode = 0;
    std::uint64_t smode1 = 0;
    std::uint64_t smode2 = 0;
    std::uint64_t srfsh = 0;
    std::uint64_t synch1 = 0;
    std::uint64_t synch2 = 0;
    std::uint64_t syncv = 0;
    std::uint64_t dispfb1 = 0;
    std::uint64_t display1 = 0;
    std::uint64_t dispfb2 = 0;
    std::uint64_t display2 = 0;
    std::uint64_t extbuf = 0;
    std::uint64_t extdata = 0;
    std::uint64_t extwrite = 0;
    std::uint64_t bgcolor = 0;
    std::uint64_t csr = 0;
    std::uint64_t imr = 0;
    std::uint64_t busdir = 0;
    std::uint64_t siglblid = 0;
};
inline constexpr std::uint32_t kPrivilegedWords = sizeof(Privileged) / sizeof(std::uint64_t);
static_assert(sizeof(Privileged) == 19 * sizeof(std::uint64_t));

struct DispFb
{
    explicit DispFb(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    std::uint32_t fbp() const { return bits(value, 0, 9); }
    std::uint32_t fbw() const { return bits(value, 9, 6); }
    std::uint32_t psm() const { return bits(value, 15, 5); }
    std::uint32_t dbx() const { return bits(value, 32, 11); }
    std::uint32_t dby() const { return bits(value, 43, 11); }
};

struct Display
{
    explicit Display(std::uint64_t v) : value(v) {}
    std::uint64_t value;
    std::uint32_t dx() const { return bits(value, 0, 12); }
    std::uint32_t dy() const { return bits(value, 12, 11); }
    std::uint32_t magh() const { return bits(value, 23, 4); }
    std::uint32_t magv() const { return bits(value, 27, 2); }
    std::uint32_t dw() const { return bits(value, 32, 12); }
    std::uint32_t dh() const { return bits(value, 44, 11); }
};

} // namespace tgr::hw::gs
