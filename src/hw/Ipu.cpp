// SPDX-License-Identifier: MIT
#include "hw/Ipu.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tgr::hw
{

namespace
{

// IPU_CMD command codes (bits 28-31).
enum Command : std::uint32_t
{
    BCLR = 0,
    IDEC = 1,
    BDEC = 2,
    VDEC = 3,
    FDEC = 4,
    SETIQ = 5,
    SETVQ = 6,
    CSC = 7,
    PACK = 8,
    SETTH = 9,
};

// IPU_CTRL fields written by the game.
constexpr std::uint32_t kCtrlIdpShift = 16; // intra DC precision, 2 bits
constexpr std::uint32_t kCtrlAs = 1u << 20; // alternate scan
constexpr std::uint32_t kCtrlIvf = 1u << 21; // intra VLC format (table B-15)
constexpr std::uint32_t kCtrlQst = 1u << 22; // non-linear quantiser scale
constexpr std::uint32_t kCtrlMp1 = 1u << 23; // MPEG-1 bitstream
constexpr std::uint32_t kCtrlFieldsMask = 0x07FF0000u;
constexpr std::uint32_t kCtrlReset = 1u << 30;

// IDEC and PACK command fields.
constexpr std::uint32_t kCmdDtd = 1u << 24; // decode dct_type
constexpr std::uint32_t kCmdSgn = 1u << 25; // signed output
constexpr std::uint32_t kCmdOfm = 1u << 27; // RGB16 (or INDX4 for PACK) instead of RGB32

constexpr int kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                             12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                             35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                             58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
constexpr int kAlternate[64] = {0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49,
                                41, 33, 26, 18, 3,  11, 4,  12, 19, 27, 34, 42, 50, 58, 35, 43,
                                51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44, 52, 60, 37, 45,
                                53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};

// MPEG-2 default intra quantiser matrix, raster order.
constexpr std::uint8_t kDefaultIntraMatrix[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
    34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
    35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

constexpr int kNonLinearScale[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18,  20,  22,
                                     24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

// ---- Variable length codes ---------------------------------------------------

struct Vlc
{
    const char* code;
    int value;   // or run
    int level;   // DCT tables only
};

// A decoder for one VLC table: a lookup indexed by the next maxLength() bits.
class VlcTable
{
public:
    VlcTable(const Vlc* codes, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
            maxLength_ = std::max(maxLength_, static_cast<int>(std::strlen(codes[i].code)));
        lookup_.resize(std::size_t{1} << maxLength_);
        for (std::size_t i = 0; i < count; ++i)
        {
            const int length = static_cast<int>(std::strlen(codes[i].code));
            std::uint32_t bits = 0;
            for (int b = 0; b < length; ++b)
                bits = bits << 1 | (codes[i].code[b] == '1');
            const std::uint32_t first = bits << (maxLength_ - length);
            const std::uint32_t last = first + (1u << (maxLength_ - length));
            for (std::uint32_t index = first; index < last; ++index)
                lookup_[index] = {static_cast<std::int8_t>(length), static_cast<std::int16_t>(codes[i].value),
                                  static_cast<std::int16_t>(codes[i].level)};
        }
    }

    // Matches the next maxLength() bits (MSB first in `peeked`). Returns the
    // code length, or 0 when nothing matches.
    int match(std::uint32_t peeked, int peekedBits, int& value, int& level) const
    {
        const Entry& e = lookup_[peeked >> (peekedBits - maxLength_)];
        value = e.value;
        level = e.level;
        return e.length;
    }

    int maxLength() const { return maxLength_; }

private:
    struct Entry
    {
        std::int8_t length = 0;
        std::int16_t value = 0;
        std::int16_t level = 0;
    };
    std::vector<Entry> lookup_;
    int maxLength_ = 0;
};

// Table B-1: macroblock_address_increment. 34 is the escape (+33), 35 stuffing.
constexpr int kMbaEscape = 34;
constexpr int kMbaStuffing = 35;
const Vlc kMbaCodes[] = {
    {"1", 1, 0},           {"011", 2, 0},         {"010", 3, 0},         {"0011", 4, 0},
    {"0010", 5, 0},        {"00011", 6, 0},       {"00010", 7, 0},       {"0000111", 8, 0},
    {"0000110", 9, 0},     {"00001011", 10, 0},   {"00001010", 11, 0},   {"00001001", 12, 0},
    {"00001000", 13, 0},   {"00000111", 14, 0},   {"00000110", 15, 0},   {"0000010111", 16, 0},
    {"0000010110", 17, 0}, {"0000010101", 18, 0}, {"0000010100", 19, 0}, {"0000010011", 20, 0},
    {"0000010010", 21, 0}, {"00000100011", 22, 0}, {"00000100010", 23, 0}, {"00000100001", 24, 0},
    {"00000100000", 25, 0}, {"00000011111", 26, 0}, {"00000011110", 27, 0}, {"00000011101", 28, 0},
    {"00000011100", 29, 0}, {"00000011011", 30, 0}, {"00000011010", 31, 0}, {"00000011001", 32, 0},
    {"00000011000", 33, 0}, {"00000001000", kMbaEscape, 0}, {"00000001111", kMbaStuffing, 0},
};

// Tables B-12 and B-13: dct_dc_size_luminance / chrominance.
const Vlc kDcLumaCodes[] = {
    {"100", 0, 0},      {"00", 1, 0},        {"01", 2, 0},        {"101", 3, 0},
    {"110", 4, 0},      {"1110", 5, 0},      {"11110", 6, 0},     {"111110", 7, 0},
    {"1111110", 8, 0},  {"11111110", 9, 0},  {"111111110", 10, 0}, {"111111111", 11, 0},
};
const Vlc kDcChromaCodes[] = {
    {"00", 0, 0},        {"01", 1, 0},          {"10", 2, 0},          {"110", 3, 0},
    {"1110", 4, 0},      {"11110", 5, 0},       {"111110", 6, 0},      {"1111110", 7, 0},
    {"11111110", 8, 0},  {"111111110", 9, 0},   {"1111111110", 10, 0}, {"1111111111", 11, 0},
};

// Tables B-14 and B-15: DCT coefficients, as {code, run, level}; the sign bit
// follows. Run -1 is end of block, run -2 the escape.
constexpr int kEob = -1;
constexpr int kEscape = -2;

// Codes 14 to 16 bits long are the same in both tables.
#define TGR_DCT_LONG_CODES                                                                                  \
    {"00000000011111", 0, 16}, {"00000000011110", 0, 17}, {"00000000011101", 0, 18},                       \
        {"00000000011100", 0, 19}, {"00000000011011", 0, 20}, {"00000000011010", 0, 21},                   \
        {"00000000011001", 0, 22}, {"00000000011000", 0, 23}, {"00000000010111", 0, 24},                   \
        {"00000000010110", 0, 25}, {"00000000010101", 0, 26}, {"00000000010100", 0, 27},                   \
        {"00000000010011", 0, 28}, {"00000000010010", 0, 29}, {"00000000010001", 0, 30},                   \
        {"00000000010000", 0, 31}, {"000000000011000", 0, 32}, {"000000000010111", 0, 33},                 \
        {"000000000010110", 0, 34}, {"000000000010101", 0, 35}, {"000000000010100", 0, 36},                \
        {"000000000010011", 0, 37}, {"000000000010010", 0, 38}, {"000000000010001", 0, 39},                \
        {"000000000010000", 0, 40}, {"000000000011111", 1, 8}, {"000000000011110", 1, 9},                  \
        {"000000000011101", 1, 10}, {"000000000011100", 1, 11}, {"000000000011011", 1, 12},                \
        {"000000000011010", 1, 13}, {"000000000011001", 1, 14}, {"0000000000010011", 1, 15},               \
        {"0000000000010010", 1, 16}, {"0000000000010001", 1, 17}, {"0000000000010000", 1, 18},             \
        {"0000000000010100", 6, 3}, {"0000000000011010", 11, 2}, {"0000000000011001", 12, 2},              \
        {"0000000000011000", 13, 2}, {"0000000000010111", 14, 2}, {"0000000000010110", 15, 2},             \
        {"0000000000010101", 16, 2}, {"0000000000011111", 27, 1}, {"0000000000011110", 28, 1},             \
        {"0000000000011101", 29, 1}, {"0000000000011100", 30, 1}, {"0000000000011011", 31, 1}

// 13-bit codes shared by both tables except 0,12..0,15.
#define TGR_DCT_13BIT_CODES                                                                                 \
    {"0000000010110", 1, 6}, {"0000000010101", 1, 7}, {"0000000010100", 2, 5}, {"0000000010011", 3, 4},   \
        {"0000000010010", 5, 3}, {"0000000010001", 9, 2}, {"0000000010000", 10, 2},                         \
        {"0000000011111", 22, 1}, {"0000000011110", 23, 1}, {"0000000011101", 24, 1},                       \
        {"0000000011100", 25, 1}, {"0000000011011", 26, 1}

// 12-bit codes shared by both tables.
#define TGR_DCT_12BIT_CODES                                                                                 \
    {"000000011100", 3, 3}, {"000000010010", 4, 3}, {"000000011110", 6, 2}, {"000000010101", 7, 2},       \
        {"000000010001", 8, 2}, {"000000011111", 17, 1}, {"000000011010", 18, 1},                           \
        {"000000011001", 19, 1}, {"000000010111", 20, 1}, {"000000010110", 21, 1}

const Vlc kDctTable0[] = {
    {"10", kEob, 0},           {"11", 0, 1},             {"011", 1, 1},            {"0100", 0, 2},
    {"0101", 2, 1},            {"00101", 0, 3},          {"00111", 3, 1},          {"00110", 4, 1},
    {"000110", 1, 2},          {"000111", 5, 1},         {"000101", 6, 1},         {"000100", 7, 1},
    {"0000110", 0, 4},         {"0000100", 2, 2},        {"0000111", 8, 1},        {"0000101", 9, 1},
    {"000001", kEscape, 0},    {"00100110", 0, 5},       {"00100001", 0, 6},       {"00100101", 1, 3},
    {"00100100", 3, 2},        {"00100111", 10, 1},      {"00100011", 11, 1},      {"00100010", 12, 1},
    {"00100000", 13, 1},       {"0000001010", 0, 7},     {"0000001100", 1, 4},     {"0000001011", 2, 3},
    {"0000001111", 4, 2},      {"0000001001", 5, 2},     {"0000001110", 14, 1},    {"0000001101", 15, 1},
    {"0000001000", 16, 1},     {"000000011101", 0, 8},   {"000000011000", 0, 9},   {"000000010011", 0, 10},
    {"000000010000", 0, 11},   {"000000011011", 1, 5},   {"000000010100", 2, 4},   TGR_DCT_12BIT_CODES,
    {"0000000011010", 0, 12},  {"0000000011001", 0, 13}, {"0000000011000", 0, 14}, {"0000000010111", 0, 15},
    TGR_DCT_13BIT_CODES,       TGR_DCT_LONG_CODES,
};

const Vlc kDctTable1[] = {
    {"0110", kEob, 0},         {"10", 0, 1},             {"010", 1, 1},            {"110", 0, 2},
    {"00101", 2, 1},           {"0111", 0, 3},           {"00111", 3, 1},          {"000110", 4, 1},
    {"00110", 1, 2},           {"000111", 5, 1},         {"0000110", 6, 1},        {"0000100", 7, 1},
    {"11100", 0, 4},           {"0000111", 2, 2},        {"0000101", 8, 1},        {"1111000", 9, 1},
    {"000001", kEscape, 0},    {"11101", 0, 5},          {"000101", 0, 6},         {"1111001", 1, 3},
    {"00100110", 3, 2},        {"1111010", 10, 1},       {"00100001", 11, 1},      {"00100101", 12, 1},
    {"00100100", 13, 1},       {"000100", 0, 7},         {"00100111", 1, 4},       {"11111100", 2, 3},
    {"11111101", 4, 2},        {"000000100", 5, 2},      {"000000101", 14, 1},     {"000000111", 15, 1},
    {"0000001101", 16, 1},     {"1111011", 0, 8},        {"1111100", 0, 9},        {"00100011", 0, 10},
    {"00100010", 0, 11},       {"00100000", 1, 5},       {"0000001100", 2, 4},     TGR_DCT_12BIT_CODES,
    {"11111010", 0, 12},       {"11111011", 0, 13},      {"11111110", 0, 14},      {"11111111", 0, 15},
    TGR_DCT_13BIT_CODES,       TGR_DCT_LONG_CODES,
};

#undef TGR_DCT_LONG_CODES
#undef TGR_DCT_13BIT_CODES
#undef TGR_DCT_12BIT_CODES

const VlcTable& mbaTable()
{
    static const VlcTable table(kMbaCodes, std::size(kMbaCodes));
    return table;
}
const VlcTable& dcLumaTable()
{
    static const VlcTable table(kDcLumaCodes, std::size(kDcLumaCodes));
    return table;
}
const VlcTable& dcChromaTable()
{
    static const VlcTable table(kDcChromaCodes, std::size(kDcChromaCodes));
    return table;
}
const VlcTable& dctTable(bool one)
{
    static const VlcTable zero(kDctTable0, std::size(kDctTable0));
    static const VlcTable other(kDctTable1, std::size(kDctTable1));
    return one ? other : zero;
}

// 8x8 inverse DCT (separable, double precision): coefficients in raster order.
void inverseDct(const std::array<std::int16_t, 64>& in, double out[64])
{
    static const auto basis = [] {
        std::array<double, 64> b{};
        const double pi = std::acos(-1.0);
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                b[x * 8 + u] = (u == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * x + 1) * u * pi / 16.0);
        return b;
    }();
    double rows[64];
    for (int v = 0; v < 8; ++v)
        for (int x = 0; x < 8; ++x)
        {
            double sum = 0;
            for (int u = 0; u < 8; ++u)
                sum += basis[x * 8 + u] * in[v * 8 + u];
            rows[v * 8 + x] = sum;
        }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
            double sum = 0;
            for (int v = 0; v < 8; ++v)
                sum += basis[y * 8 + v] * rows[v * 8 + x];
            out[y * 8 + x] = sum;
        }
}

std::uint8_t clampByte(int v)
{
    return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
}

} // namespace

Ipu::Ipu()
{
    for (int i = 0; i < 64; ++i)
    {
        intraMatrix_[i] = kDefaultIntraMatrix[kZigzag[i]];
        nonIntraMatrix_[i] = 16;
    }
}

// ---- Bitstream -------------------------------------------------------------------

std::uint64_t Ipu::bitsAvailable() const
{
    return input_.size() * 8 - inputBit_;
}

std::uint32_t Ipu::peekBits(int count) const
{
    if (count == 0)
        return 0;
    if (inputBit_ + count > input_.size() * 8)
        underrun_ = true;
    // Five bytes always cover 32 bits at any bit offset.
    const std::size_t byte = static_cast<std::size_t>(inputBit_ >> 3);
    std::uint64_t window = 0;
    for (std::size_t i = 0; i < 5; ++i)
        window = window << 8 | (byte + i < input_.size() ? input_[byte + i] : 0u);
    const int shift = 40 - static_cast<int>(inputBit_ & 7) - count;
    return static_cast<std::uint32_t>((window >> shift) & ((std::uint64_t{1} << count) - 1));
}

void Ipu::skipBits(int count)
{
    inputBit_ = std::min<std::uint64_t>(inputBit_ + count, input_.size() * 8);
}

std::uint32_t Ipu::readBits(int count)
{
    const std::uint32_t value = peekBits(count);
    skipBits(count);
    return value;
}

std::uint32_t Ipu::peekWord() const
{
    return peekBits(32);
}

void Ipu::compactInput()
{
    // Drop whole quadwords already consumed.
    const std::size_t consumed = static_cast<std::size_t>(inputBit_ / 128) * 16;
    if (consumed >= 4096)
    {
        input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(consumed));
        inputBit_ -= consumed * 8;
    }
}

// ---- Registers and FIFOs ---------------------------------------------------------

void Ipu::writeRegister(std::uint32_t offset, std::uint32_t value)
{
    switch (offset)
    {
    case 0x00:
        start(value);
        break;
    case 0x10:
        if (value & kCtrlReset)
            reset();
        control_ = value & kCtrlFieldsMask;
        break;
    default:
        break;
    }
}

std::uint32_t Ipu::readRegister(std::uint32_t offset)
{
    const std::uint32_t inputQwords =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(bitsAvailable() / 128, 8));
    const std::uint32_t outputQwords = std::min<std::uint32_t>(outputAvailable(), 8);
    switch (offset)
    {
    case 0x00: // IPU_CMD.DATA
        return ((command_ >> 28) == FDEC || (command_ >> 28) == VDEC) ? commandData_ : peekWord();
    case 0x04:
    case 0x34: // IPU_CMD.BUSY, IPU_TOP.BUSY
        return busy_ ? 0x80000000u : 0u;
    case 0x10: // IPU_CTRL
        return inputQwords | outputQwords << 4 | (codedBlockPattern_ & 0x3F) << 8 |
               (errorCodeDetected_ ? 1u << 14 : 0u) | (startCodeDetected_ ? 1u << 15 : 0u) | control_ |
               (busy_ ? 0x80000000u : 0u);
    case 0x20: // IPU_BP
        return static_cast<std::uint32_t>(inputBit_ & 127) | inputQwords << 8;
    case 0x30: // IPU_TOP.BSTOP
        return peekWord();
    default:
        return 0;
    }
}

void Ipu::pushInput(const std::uint8_t* quadwords, std::uint32_t count)
{
    compactInput();
    input_.insert(input_.end(), quadwords, quadwords + std::size_t{count} * 16);
    if (busy_)
        run();
}

std::uint32_t Ipu::outputAvailable() const
{
    return static_cast<std::uint32_t>((output_.size() - outputRead_) / 16);
}

std::uint32_t Ipu::popOutput(std::uint8_t* quadwords, std::uint32_t maxCount)
{
    const std::uint32_t count = std::min(maxCount, outputAvailable());
    std::memcpy(quadwords, output_.data() + outputRead_, std::size_t{count} * 16);
    outputRead_ += std::size_t{count} * 16;
    if (outputRead_ == output_.size())
    {
        output_.clear();
        outputRead_ = 0;
    }
    return count;
}

bool Ipu::takeInterrupt()
{
    const bool raised = interrupt_;
    interrupt_ = false;
    return raised;
}

// ---- Commands --------------------------------------------------------------------

void Ipu::reset()
{
    input_.clear();
    inputBit_ = 0;
    output_.clear();
    outputRead_ = 0;
    command_ = 0;
    commandData_ = 0;
    busy_ = false;
    startCodeDetected_ = false;
    errorCodeDetected_ = false;
    codedBlockPattern_ = 0;
}

void Ipu::start(std::uint32_t command)
{
    command_ = command;
    startCodeDetected_ = false;
    errorCodeDetected_ = false;
    busy_ = true;

    switch (command >> 28)
    {
    case BCLR:
        input_.clear();
        inputBit_ = command & 0x7F;
        finish();
        return;
    case SETTH:
        threshold0_ = command & 0x1FF;
        threshold1_ = (command >> 16) & 0x1FF;
        finish();
        return;
    case IDEC:
        sliceStarted_ = false;
        break;
    default:
        break;
    }

    // Every other command first skips FB bits (PACK and SETVQ have none).
    if ((command >> 28) != PACK && (command >> 28) != SETVQ)
        skipBits(static_cast<int>(command & 0x3F));
    if ((command >> 28) == IDEC)
    {
        const int code = static_cast<int>((command >> 16) & 0x1F);
        quantizerScale_ = (control_ & kCtrlQst) ? kNonLinearScale[code] : code * 2;
        const int idp = static_cast<int>((control_ >> kCtrlIdpShift) & 3);
        dcPredictor_.fill(128 << idp);
    }
    run();
}

void Ipu::run()
{
    switch (command_ >> 28)
    {
    case IDEC:
        if (runIdec())
            finish();
        break;
    case FDEC:
        if (bitsAvailable() >= 32)
        {
            commandData_ = peekWord();
            finish();
        }
        break;
    case SETIQ:
        if (bitsAvailable() >= 64 * 8)
        {
            auto& matrix = (command_ & (1u << 27)) ? nonIntraMatrix_ : intraMatrix_;
            for (auto& entry : matrix)
                entry = static_cast<std::uint8_t>(readBits(8));
            finish();
        }
        break;
    case SETVQ:
        if (bitsAvailable() >= 32 * 8)
        {
            for (auto& entry : palette_)
            {
                const std::uint32_t lo = readBits(8);
                entry = static_cast<std::uint16_t>(lo | readBits(8) << 8);
            }
            finish();
        }
        break;
    case PACK:
        if (runPack())
            finish();
        break;
    default: // BDEC, VDEC, CSC: not used by the game
        finish();
        break;
    }
}

void Ipu::finish()
{
    busy_ = false;
    interrupt_ = true;
}

// ---- IDEC ------------------------------------------------------------------------

// Decodes macroblocks until the slice ends. Returns false when it has to wait
// for more input; the decode then resumes at the macroblock it stopped in.
bool Ipu::runIdec()
{
    for (;;)
    {
        const std::uint64_t restartBit = inputBit_;
        const std::array<int, 3> restartDc = dcPredictor_;
        const int restartScale = quantizerScale_;
        const auto wait = [&] {
            inputBit_ = restartBit;
            dcPredictor_ = restartDc;
            quantizerScale_ = restartScale;
            return false;
        };

        if (sliceStarted_)
        {
            // macroblock_address_increment of the next macroblock, if any.
            int increment = 0;
            for (;;)
            {
                if (bitsAvailable() < 11)
                    return wait();
                int value = 0;
                int unused = 0;
                const int length = mbaTable().match(peekBits(11), 11, value, unused);
                if (length == 0)
                {
                    endSlice();
                    return true;
                }
                skipBits(length);
                if (value == kMbaEscape)
                    increment += 33;
                else if (value != kMbaStuffing)
                {
                    increment += value;
                    break;
                }
            }
            if (increment > 1)
            {
                const int idp = static_cast<int>((control_ >> kCtrlIdpShift) & 3);
                dcPredictor_.fill(128 << idp); // skipped macroblocks reset the predictors
            }
        }

        // Decoding past the end of the input means the rest has not arrived yet.
        std::array<std::uint8_t, 384> pixels{};
        underrun_ = false;
        const bool decoded = decodeMacroblock(pixels);
        if (underrun_)
            return wait();
        if (!decoded)
        {
            endSlice(); // a bad code ends the decode, as on the IPU
            return true;
        }
        convertMacroblock(pixels);
        sliceStarted_ = true;
    }
}

bool Ipu::decodeMacroblock(std::array<std::uint8_t, 384>& pixels)
{
    // macroblock_type for intra pictures: "1" intra, "01" intra + quant.
    bool quant = false;
    if (readBits(1) == 0)
    {
        if (readBits(1) == 0)
            return false;
        quant = true;
    }
    const bool fieldDct = (command_ & kCmdDtd) && readBits(1);
    if (quant)
    {
        const int code = static_cast<int>(readBits(5));
        quantizerScale_ = (control_ & kCtrlQst) ? kNonLinearScale[code] : code * 2;
    }
    codedBlockPattern_ = 0x3F;

    for (int block = 0; block < 6; ++block)
    {
        std::array<std::int16_t, 64> coefficients{};
        if (!decodeIntraBlock(block < 4 ? 0 : block - 3, coefficients))
            return false;
        if (block < 4)
        {
            // Luma: frame DCT blocks are quadrants; field DCT blocks take
            // alternate lines.
            const int column = (block & 1) * 8;
            const int row = fieldDct ? (block >> 1) : (block >> 1) * 8;
            storeBlock(coefficients, pixels.data() + row * 16 + column, fieldDct ? 32 : 16);
        }
        else
        {
            storeBlock(coefficients, pixels.data() + 256 + (block - 4) * 64, 8);
        }
    }
    return true;
}

bool Ipu::decodeIntraBlock(int component, std::array<std::int16_t, 64>& coefficients)
{
    const int idp = static_cast<int>((control_ >> kCtrlIdpShift) & 3);
    const bool mpeg1 = (control_ & kCtrlMp1) != 0;
    const int* scan = (control_ & kCtrlAs) ? kAlternate : kZigzag;

    // DC: size, then a differential to the component's predictor.
    int size = 0;
    int unused = 0;
    const VlcTable& dcTable = component == 0 ? dcLumaTable() : dcChromaTable();
    const int dcLength = dcTable.match(peekBits(dcTable.maxLength()), dcTable.maxLength(), size, unused);
    if (dcLength == 0)
        return false;
    skipBits(dcLength);
    if (size > 0)
    {
        int diff = static_cast<int>(readBits(size));
        if ((diff & (1 << (size - 1))) == 0)
            diff -= (1 << size) - 1;
        dcPredictor_[component] += diff;
    }
    coefficients[0] = static_cast<std::int16_t>(dcPredictor_[component] << (3 - idp));

    // AC coefficients.
    const VlcTable& table = dctTable((control_ & kCtrlIvf) && !mpeg1);
    for (int i = 1;;)
    {
        int run = 0;
        int level = 0;
        const int length = table.match(peekBits(table.maxLength()), table.maxLength(), run, level);
        if (length == 0)
            return false;
        skipBits(length);
        if (run == kEob)
            return true;

        if (run == kEscape)
        {
            run = static_cast<int>(readBits(6));
            if (mpeg1)
            {
                level = static_cast<std::int8_t>(readBits(8));
                if ((level & 0x7F) == 0)
                    level = level == 0 ? static_cast<int>(readBits(8)) : static_cast<int>(readBits(8)) - 256;
            }
            else
            {
                level = static_cast<int>(readBits(12));
                if (level & 0x800)
                    level -= 0x1000;
            }
        }
        else if (readBits(1))
        {
            level = -level;
        }

        i += run;
        if (i > 63)
            return false;
        const int magnitude = std::abs(level) * quantizerScale_ * intraMatrix_[i] / 16;
        int value = level < 0 ? -magnitude : magnitude;
        if (mpeg1 && value != 0)
            value = value > 0 ? (value - 1) | 1 : -(((-value) - 1) | 1);
        coefficients[scan[i]] = static_cast<std::int16_t>(std::clamp(value, -2048, 2047));
        ++i;
    }
}

void Ipu::storeBlock(const std::array<std::int16_t, 64>& coefficients, std::uint8_t* target, int stride)
{
    double samples[64];
    inverseDct(coefficients, samples);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            target[y * stride + x] = clampByte(static_cast<int>(std::lround(samples[y * 8 + x])));
}

// YCbCr 4:2:0 to RGB32 (or RGB16), with the SETTH alpha rules.
void Ipu::convertMacroblock(const std::array<std::uint8_t, 384>& pixels)
{
    std::uint8_t rgba[16 * 16 * 4];
    for (int y = 0; y < 16; ++y)
    {
        for (int x = 0; x < 16; ++x)
        {
            const int luma = std::max(pixels[y * 16 + x] - 16, 0);
            const int cb = pixels[256 + (y / 2) * 8 + x / 2] - 128;
            const int cr = pixels[320 + (y / 2) * 8 + x / 2] - 128;
            int r = clampByte((luma * 149 + cr * 204 + 64) >> 7);
            int g = clampByte((luma * 149 - cb * 50 - cr * 104 + 64) >> 7);
            int b = clampByte((luma * 149 + cb * 258 + 64) >> 7);
            int a = 0x80;
            if (threshold0_ != 0 && r < static_cast<int>(threshold0_) && g < static_cast<int>(threshold0_) &&
                b < static_cast<int>(threshold0_))
                r = g = b = a = 0;
            else if (r < static_cast<int>(threshold1_) && g < static_cast<int>(threshold1_) &&
                     b < static_cast<int>(threshold1_))
                a = 0x40;
            if (command_ & kCmdSgn)
            {
                r ^= 0x80;
                g ^= 0x80;
                b ^= 0x80;
            }
            std::uint8_t* p = rgba + (y * 16 + x) * 4;
            p[0] = static_cast<std::uint8_t>(r);
            p[1] = static_cast<std::uint8_t>(g);
            p[2] = static_cast<std::uint8_t>(b);
            p[3] = static_cast<std::uint8_t>(a);
        }
    }

    if (command_ & kCmdOfm)
    {
        for (int i = 0; i < 256; ++i)
        {
            const std::uint8_t* p = rgba + i * 4;
            const std::uint16_t pixel = static_cast<std::uint16_t>(
                (p[0] >> 3) | (p[1] >> 3) << 5 | (p[2] >> 3) << 10 | (p[3] >= 0x40 ? 0x8000 : 0));
            output_.push_back(static_cast<std::uint8_t>(pixel & 0xFF));
            output_.push_back(static_cast<std::uint8_t>(pixel >> 8));
        }
    }
    else
    {
        output_.insert(output_.end(), rgba, rgba + sizeof(rgba));
    }
}

// After the last macroblock: find the start code (SCD) or note the error (ECD),
// leaving the bitstream at it.
void Ipu::endSlice()
{
    if (peekBits(8) == 0)
    {
        skipBits(static_cast<int>((8 - (inputBit_ & 7)) & 7));
        while (peekBits(24) != 1 && peekBits(8) == 0 && bitsAvailable() >= 32)
            skipBits(8);
        if (peekBits(24) == 1)
            startCodeDetected_ = true;
        else
            errorCodeDetected_ = true;
    }
    else
    {
        errorCodeDetected_ = true;
    }
    commandData_ = peekWord();
}

// ---- PACK ------------------------------------------------------------------------

// RGB32 macroblocks from the input FIFO to RGB16, or to 4-bit palette indices
// (nearest SETVQ colour).
bool Ipu::runPack()
{
    const std::uint32_t macroblocks = command_ & 0x7FF;
    if (bitsAvailable() < std::uint64_t{macroblocks} * 1024 * 8)
        return false;
    for (std::uint32_t mb = 0; mb < macroblocks; ++mb)
    {
        std::uint8_t rgba[1024];
        for (auto& byte : rgba)
            byte = static_cast<std::uint8_t>(readBits(8));
        if (command_ & kCmdOfm)
        {
            for (int i = 0; i < 256; ++i)
            {
                const std::uint8_t* p = rgba + i * 4;
                const std::uint16_t pixel = static_cast<std::uint16_t>(
                    (p[0] >> 3) | (p[1] >> 3) << 5 | (p[2] >> 3) << 10 | (p[3] >= 0x40 ? 0x8000 : 0));
                output_.push_back(static_cast<std::uint8_t>(pixel & 0xFF));
                output_.push_back(static_cast<std::uint8_t>(pixel >> 8));
            }
        }
        else
        {
            for (int i = 0; i < 256; i += 2)
            {
                std::uint8_t indices[2] = {};
                for (int k = 0; k < 2; ++k)
                {
                    const std::uint8_t* p = rgba + (i + k) * 4;
                    int best = 0;
                    int bestDistance = 1 << 30;
                    for (int c = 0; c < 16; ++c)
                    {
                        const int dr = p[0] - ((palette_[c] & 0x1F) << 3);
                        const int dg = p[1] - (((palette_[c] >> 5) & 0x1F) << 3);
                        const int db = p[2] - (((palette_[c] >> 10) & 0x1F) << 3);
                        const int distance = dr * dr + dg * dg + db * db;
                        if (distance < bestDistance)
                        {
                            bestDistance = distance;
                            best = c;
                        }
                    }
                    indices[k] = static_cast<std::uint8_t>(best);
                }
                output_.push_back(static_cast<std::uint8_t>(indices[0] | indices[1] << 4));
            }
        }
    }
    return true;
}

} // namespace tgr::hw
