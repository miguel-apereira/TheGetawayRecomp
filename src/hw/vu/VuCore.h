// SPDX-License-Identifier: MIT
#pragma once

// The execution model that VU microprograms compiled ahead of time by
// tgr-vurecomp run on (recomp/generated/vu). The translator turns every
// instruction pair into calls to the inline functions below, with the
// registers, fields and immediates as constants, so the C++ compiler sees
// straight-line code for each pair.
//
// What the model keeps from the hardware:
// - VU floats: no NaN, infinity or denormals. Operands with exponent 255 count
//   as the largest finite value, exponent 0 as zero; results round toward
//   zero, clamp on overflow (O flag) and flush on underflow (U and Z flags).
// - Both halves of a pair read the registers as they were before the pair;
//   when both write the same VF register, the upper result wins.
// - Timing: a pair waits (stalls) until the VF, VI and ACC fields it reads
//   are ready. Results that are not interlocked appear late, as on the chip:
//   Q four to twelve cycles after DIV/SQRT/RSQRT unless WAITQ waits for it,
//   P after the EFU latency unless WAITP waits, and the MAC, status and clip
//   flags four cycles after the instruction that produced them.
// - A conditional branch or JR/JALR that reads a VI register written by the
//   integer instruction just before it sees the old value.
// - E bit: the program ends after the next pair (the delay slot).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace tgr::hw::vu
{

struct FlagEvent
{
    std::uint64_t ready = 0;
    std::uint32_t mac = 0;
    std::uint32_t status = 0; // Z S U O in bits 0-3
    bool fsset = false;       // FSSET: status holds the sticky bits to set
};

struct ClipEvent
{
    std::uint64_t ready = 0;
    std::uint32_t value = 0;
    bool set = false; // FCSET: value replaces the clip flags
};

struct State
{
    // ---- Architectural registers ----
    alignas(16) std::uint32_t vf[32][4] = {}; // raw bits, x y z w
    std::uint16_t vi[16] = {};
    std::uint32_t acc[4] = {};
    std::uint32_t q = 0x3F800000; // 1.0f
    std::uint32_t p = 0;
    std::uint32_t i = 0;
    std::uint32_t r = 0x3F800000;
    std::uint32_t mac = 0;
    std::uint32_t status = 0;
    std::uint32_t clip = 0;
    std::uint32_t itop = 0;
    std::uint32_t top = 0;

    // ---- Memory ----
    std::uint8_t* data = nullptr;
    std::uint32_t dataQuadwordMask = 0xFF; // VU0: 256 quadwords, VU1: 1024

    // ---- Timing ----
    std::uint64_t cycle = 0;
    std::uint64_t vfReady[32][4] = {};
    std::uint64_t viReady[16] = {};
    std::uint64_t accReady[4] = {};

    bool qPending = false;
    std::uint64_t qReady = 0;
    std::uint32_t qValue = 0;
    std::uint32_t qStatus = 0; // I and D bits from the divide

    bool pPending = false;
    std::uint64_t pReady = 0;
    std::uint32_t pValue = 0;

    std::array<FlagEvent, 8> flagEvents{};
    std::uint32_t flagHead = 0;
    std::uint32_t flagCount = 0;
    std::array<ClipEvent, 8> clipEvents{};
    std::uint32_t clipHead = 0;
    std::uint32_t clipCount = 0;

    // Branch rule: the VI register the previous pair's integer op wrote, and its old value.
    std::uint32_t branchBackupReg = 0;
    std::uint16_t branchBackupValue = 0;

    // ---- Control ----
    std::uint32_t pc = 0; // where the program stopped
    std::uint64_t budget = 0; // pairs left before giving up on a runaway program
    void (*xgkick)(State& s, std::uint32_t address) = nullptr; // VU1: sends a GIF packet
    void* user = nullptr;                                       // for xgkick
};

// ============================================================================
// Floats
// ============================================================================

inline std::uint32_t normalize(std::uint32_t bits)
{
    const std::uint32_t exponent = (bits >> 23) & 0xFF;
    if (exponent == 0)
        return bits & 0x80000000u;
    if (exponent == 0xFF)
        return (bits & 0x80000000u) | 0x7F7FFFFFu;
    return bits;
}

inline double toDouble(std::uint32_t bits)
{
    bits = normalize(bits);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

// Per-field flag bits used while computing: Z=1 S=2 U=4 O=8.
inline std::uint32_t fromDouble(double value, std::uint32_t& flags)
{
    std::uint64_t bits;
    std::memcpy(&bits, &value, 8);
    const std::uint32_t sign = static_cast<std::uint32_t>(bits >> 32) & 0x80000000u;
    flags = sign ? 2u : 0u;
    const int exponent = static_cast<int>((bits >> 52) & 0x7FF);
    const std::uint64_t mantissa = bits & 0xFFFFFFFFFFFFFull;
    if (exponent == 0 && mantissa == 0)
    {
        flags |= 1u;
        return sign;
    }
    const int floatExponent = exponent - 1023 + 127;
    if (floatExponent >= 255)
    {
        flags |= 8u;
        return sign | 0x7F7FFFFFu;
    }
    if (floatExponent <= 0)
    {
        flags |= 1u | 4u;
        return sign;
    }
    return sign | (static_cast<std::uint32_t>(floatExponent) << 23) | static_cast<std::uint32_t>(mantissa >> 29);
}

inline std::uint32_t fromDouble(double value)
{
    std::uint32_t flags;
    return fromDouble(value, flags);
}

// MAX/MINI compare the raw bits as signed magnitudes.
inline std::int32_t orderKey(std::uint32_t bits)
{
    const std::int32_t value = static_cast<std::int32_t>(bits);
    return value < 0 ? static_cast<std::int32_t>(0x80000000u - (bits & 0x7FFFFFFFu)) : value;
}

inline std::uint32_t floatToInt(std::uint32_t bits, double scale)
{
    const double value = toDouble(bits) * scale;
    if (value >= 2147483647.0)
        return 0x7FFFFFFFu;
    if (value <= -2147483648.0)
        return 0x80000000u;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

inline std::uint32_t intToFloat(std::uint32_t bits, double scale)
{
    return fromDouble(static_cast<double>(static_cast<std::int32_t>(bits)) / scale);
}

// ============================================================================
// Timing
// ============================================================================

constexpr int kFmacLatency = 4;

inline void retireFlags(State& s)
{
    while (s.flagCount && s.flagEvents[s.flagHead].ready <= s.cycle)
    {
        const FlagEvent& event = s.flagEvents[s.flagHead];
        if (event.fsset)
        {
            s.status = (s.status & 0x3F) | (event.status & 0xFC0);
        }
        else
        {
            s.mac = event.mac;
            s.status = (s.status & ~0xFu) | event.status | (event.status << 6);
        }
        s.flagHead = (s.flagHead + 1) % s.flagEvents.size();
        --s.flagCount;
    }
    while (s.clipCount && s.clipEvents[s.clipHead].ready <= s.cycle)
    {
        const ClipEvent& event = s.clipEvents[s.clipHead];
        s.clip = event.set ? (event.value & 0xFFFFFF) : (((s.clip << 6) | event.value) & 0xFFFFFF);
        s.clipHead = (s.clipHead + 1) % s.clipEvents.size();
        --s.clipCount;
    }
    if (s.qPending && s.qReady <= s.cycle)
    {
        s.q = s.qValue;
        s.status = (s.status & ~0x30u) | s.qStatus | (s.qStatus << 6);
        s.qPending = false;
    }
    if (s.pPending && s.pReady <= s.cycle)
    {
        s.p = s.pValue;
        s.pPending = false;
    }
}

inline void pushFlags(State& s, const FlagEvent& event)
{
    if (s.flagCount == s.flagEvents.size())
    {
        // Older than the pipeline can hold: it has landed already.
        const std::uint64_t now = s.cycle;
        s.cycle = s.flagEvents[s.flagHead].ready;
        retireFlags(s);
        s.cycle = now;
    }
    s.flagEvents[(s.flagHead + s.flagCount) % s.flagEvents.size()] = event;
    ++s.flagCount;
}

inline void pushClip(State& s, const ClipEvent& event)
{
    if (s.clipCount == s.clipEvents.size())
    {
        const std::uint64_t now = s.cycle;
        s.cycle = s.clipEvents[s.clipHead].ready;
        retireFlags(s);
        s.cycle = now;
    }
    s.clipEvents[(s.clipHead + s.clipCount) % s.clipEvents.size()] = event;
    ++s.clipCount;
}

inline void waitUntil(State& s, std::uint64_t cycle)
{
    if (cycle > s.cycle)
        s.cycle = cycle;
}

// fields: x=8 y=4 z=2 w=1, as in the instruction's dest mask.
inline void stallVf(State& s, int reg, unsigned fields)
{
    if (reg == 0)
        return;
    for (int c = 0; c < 4; ++c)
    {
        if (fields & (8u >> c))
            waitUntil(s, s.vfReady[reg][c]);
    }
}

inline void stallVi(State& s, int reg)
{
    if (reg != 0)
        waitUntil(s, s.viReady[reg]);
}

inline void stallAcc(State& s, unsigned fields)
{
    for (int c = 0; c < 4; ++c)
    {
        if (fields & (8u >> c))
            waitUntil(s, s.accReady[c]);
    }
}

// Starts a pair once its stalls (stallVf/stallVi/stallAcc calls before it) are resolved.
inline void beginPair(State& s)
{
    retireFlags(s);
}

inline void endPair(State& s)
{
    ++s.cycle;
}

// Writes the VI register and its ready time, and arms the branch rule.
inline void writeVi(State& s, int reg, std::uint32_t value, int latency, bool delaysBranch)
{
    if (reg == 0)
        return;
    if (delaysBranch)
    {
        s.branchBackupReg = static_cast<std::uint32_t>(reg);
        s.branchBackupValue = s.vi[reg];
    }
    s.vi[reg] = static_cast<std::uint16_t>(value);
    s.viReady[reg] = s.cycle + static_cast<std::uint64_t>(latency);
}

// The VI value a branch sees.
inline std::uint16_t branchVi(const State& s, std::uint32_t previousBackupReg, std::uint16_t previousBackupValue, int reg)
{
    if (reg == 0)
        return 0;
    return static_cast<std::uint32_t>(reg) == previousBackupReg ? previousBackupValue : s.vi[reg];
}

inline void writeVfFields(State& s, int reg, unsigned fields, const std::uint32_t value[4], int latency)
{
    if (reg == 0)
        return;
    for (int c = 0; c < 4; ++c)
    {
        if (fields & (8u >> c))
        {
            s.vf[reg][c] = value[c];
            s.vfReady[reg][c] = s.cycle + static_cast<std::uint64_t>(latency);
        }
    }
}

// ============================================================================
// Upper (FMAC) instructions
// ============================================================================

// The result of an upper instruction, computed from the registers as they
// were before the pair and committed after the lower instruction.
struct Upper
{
    enum Target : std::uint8_t
    {
        None,
        Vf,
        Acc,
        Clip
    };
    Target target = None;
    std::uint8_t reg = 0;
    std::uint8_t fields = 0;
    bool setsFlags = false;
    std::uint32_t value[4] = {};
    std::uint32_t mac = 0;
    std::uint32_t status = 0;
    std::uint32_t clip = 0;
};

enum class Op : std::uint8_t
{
    Add,
    Sub,
    Mul,
    Madd,
    Msub,
    Max,
    Mini
};

// Operand selection for the second source: a VF register field-wise, one
// broadcast field, Q or I.
struct Source
{
    std::uint32_t lane[4];
};

inline Source sourceVf(const State& s, int reg)
{
    return {{s.vf[reg][0], s.vf[reg][1], s.vf[reg][2], s.vf[reg][3]}};
}

inline Source sourceBroadcast(const State& s, int reg, int field)
{
    const std::uint32_t v = s.vf[reg][field];
    return {{v, v, v, v}};
}

inline Source sourceScalar(std::uint32_t v)
{
    return {{v, v, v, v}};
}

inline Upper arithmetic(const State& s, Op op, bool toAcc, int fd, int fs, const Source& t, unsigned fields)
{
    Upper u;
    u.target = toAcc ? Upper::Acc : Upper::Vf;
    u.reg = static_cast<std::uint8_t>(fd);
    u.fields = static_cast<std::uint8_t>(fields);
    u.setsFlags = op != Op::Max && op != Op::Mini;
    std::uint32_t zsuo = 0;
    for (int c = 0; c < 4; ++c)
    {
        if (!(fields & (8u >> c)))
            continue;
        const std::uint32_t a = s.vf[fs][c];
        const std::uint32_t b = t.lane[c];
        std::uint32_t flags = 0;
        std::uint32_t result = 0;
        switch (op)
        {
        case Op::Add:
            result = fromDouble(toDouble(a) + toDouble(b), flags);
            break;
        case Op::Sub:
            result = fromDouble(toDouble(a) - toDouble(b), flags);
            break;
        case Op::Mul:
            result = fromDouble(toDouble(a) * toDouble(b), flags);
            break;
        case Op::Madd:
        case Op::Msub:
        {
            std::uint32_t productFlags = 0;
            const std::uint32_t product = fromDouble(toDouble(a) * toDouble(b), productFlags);
            const double sum = op == Op::Madd ? toDouble(s.acc[c]) + toDouble(product)
                                              : toDouble(s.acc[c]) - toDouble(product);
            result = fromDouble(sum, flags);
            flags |= productFlags & 8u; // an overflowing product still reports O
            break;
        }
        case Op::Max:
            result = orderKey(a) >= orderKey(b) ? a : b;
            break;
        case Op::Mini:
            result = orderKey(a) <= orderKey(b) ? a : b;
            break;
        }
        u.value[c] = result;
        // MAC flag: Z in bits 0-3, S 4-7, U 8-11, O 12-15; x is the high bit of each group.
        const unsigned bit = 3u - static_cast<unsigned>(c);
        if (flags & 1u)
            u.mac |= 1u << bit;
        if (flags & 2u)
            u.mac |= 1u << (4 + bit);
        if (flags & 4u)
            u.mac |= 1u << (8 + bit);
        if (flags & 8u)
            u.mac |= 1u << (12 + bit);
        zsuo |= flags;
    }
    u.status = zsuo & 0xF;
    return u;
}

// OPMULA / OPMSUB: outer product on xyz.
inline Upper outerProduct(const State& s, bool subtract, int fd, int fs, int ft, unsigned fields)
{
    const State& st = s;
    const int sx[3] = {1, 2, 0};
    const int tx[3] = {2, 0, 1};
    Upper u;
    u.target = subtract ? Upper::Vf : Upper::Acc;
    u.reg = static_cast<std::uint8_t>(fd);
    u.fields = static_cast<std::uint8_t>(fields & 0xE);
    u.setsFlags = true;
    std::uint32_t zsuo = 0;
    for (int c = 0; c < 3; ++c)
    {
        if (!(fields & (8u >> c)))
            continue;
        std::uint32_t flags = 0;
        std::uint32_t productFlags = 0;
        const std::uint32_t product =
            fromDouble(toDouble(st.vf[fs][sx[c]]) * toDouble(st.vf[ft][tx[c]]), productFlags);
        u.value[c] = subtract ? fromDouble(toDouble(st.acc[c]) - toDouble(product), flags) : product;
        if (!subtract)
            flags = productFlags;
        const unsigned bit = 3u - static_cast<unsigned>(c);
        if (flags & 1u)
            u.mac |= 1u << bit;
        if (flags & 2u)
            u.mac |= 1u << (4 + bit);
        if (flags & 4u)
            u.mac |= 1u << (8 + bit);
        if (flags & 8u)
            u.mac |= 1u << (12 + bit);
        zsuo |= flags;
    }
    u.status = zsuo & 0xF;
    return u;
}

inline Upper absolute(const State& s, int ft, int fs, unsigned fields)
{
    Upper u;
    u.target = Upper::Vf;
    u.reg = static_cast<std::uint8_t>(ft);
    u.fields = static_cast<std::uint8_t>(fields);
    for (int c = 0; c < 4; ++c)
        u.value[c] = s.vf[fs][c] & 0x7FFFFFFFu;
    return u;
}

inline Upper toInteger(const State& s, int ft, int fs, unsigned fields, double scale)
{
    Upper u;
    u.target = Upper::Vf;
    u.reg = static_cast<std::uint8_t>(ft);
    u.fields = static_cast<std::uint8_t>(fields);
    for (int c = 0; c < 4; ++c)
        u.value[c] = floatToInt(s.vf[fs][c], scale);
    return u;
}

inline Upper toFloat(const State& s, int ft, int fs, unsigned fields, double scale)
{
    Upper u;
    u.target = Upper::Vf;
    u.reg = static_cast<std::uint8_t>(ft);
    u.fields = static_cast<std::uint8_t>(fields);
    for (int c = 0; c < 4; ++c)
        u.value[c] = intToFloat(s.vf[fs][c], scale);
    return u;
}

// CLIP: judges fs.xyz against +-|ft.w|.
inline Upper clipJudge(const State& s, int fs, int ft)
{
    const double w = std::fabs(toDouble(s.vf[ft][3]));
    std::uint32_t flags = 0;
    for (int c = 0; c < 3; ++c)
    {
        const double v = toDouble(s.vf[fs][c]);
        if (v > w)
            flags |= 1u << (c * 2);
        if (v < -w)
            flags |= 1u << (c * 2 + 1);
    }
    Upper u;
    u.target = Upper::Clip;
    u.clip = flags;
    return u;
}

inline void commit(State& s, const Upper& u)
{
    switch (u.target)
    {
    case Upper::None:
        return;
    case Upper::Clip:
        pushClip(s, {s.cycle + kFmacLatency, u.clip, false});
        return;
    case Upper::Acc:
        for (int c = 0; c < 4; ++c)
        {
            if (u.fields & (8u >> c))
            {
                s.acc[c] = u.value[c];
                s.accReady[c] = s.cycle + 1; // ACC forwards to the next FMAC op
            }
        }
        break;
    case Upper::Vf:
        writeVfFields(s, u.reg, u.fields, u.value, kFmacLatency);
        break;
    }
    if (u.setsFlags)
        pushFlags(s, {s.cycle + kFmacLatency, u.mac, u.status, false});
}

// ============================================================================
// Lower instructions
// ============================================================================

inline std::uint8_t* quadword(State& s, std::uint32_t address)
{
    return s.data + (address & s.dataQuadwordMask) * 16;
}

inline void loadQuadword(State& s, int ft, unsigned fields, std::uint32_t address)
{
    std::uint32_t value[4];
    std::memcpy(value, quadword(s, address), 16);
    writeVfFields(s, ft, fields, value, kFmacLatency);
}

inline void storeQuadword(State& s, int fs, unsigned fields, std::uint32_t address)
{
    std::uint8_t* target = quadword(s, address);
    for (int c = 0; c < 4; ++c)
    {
        if (fields & (8u >> c))
            std::memcpy(target + c * 4, &s.vf[fs][c], 4);
    }
}

inline int firstField(unsigned fields)
{
    return (fields & 8) ? 0 : (fields & 4) ? 1 : (fields & 2) ? 2 : 3;
}

inline std::uint16_t loadInteger(State& s, unsigned fields, std::uint32_t address)
{
    std::uint32_t word;
    std::memcpy(&word, quadword(s, address) + firstField(fields) * 4, 4);
    return static_cast<std::uint16_t>(word);
}

inline void storeInteger(State& s, unsigned fields, std::uint32_t address, std::uint16_t value)
{
    std::uint8_t* target = quadword(s, address);
    const std::uint32_t word = value;
    for (int c = 0; c < 4; ++c)
    {
        if (fields & (8u >> c))
            std::memcpy(target + c * 4, &word, 4);
    }
}

// DIV/SQRT/RSQRT: Q arrives after the latency; a new divide waits for the last.
inline void divide(State& s, std::uint32_t numerator, std::uint32_t denominator, int kind)
{
    if (s.qPending)
    {
        waitUntil(s, s.qReady);
        retireFlags(s);
    }
    const double num = toDouble(numerator);
    const double den = toDouble(denominator);
    std::uint32_t status = 0;
    std::uint32_t result = 0;
    int latency = 7;
    if (kind == 1) // SQRT
    {
        if (den < 0)
            status = 0x10; // I
        result = fromDouble(std::sqrt(std::fabs(den)));
    }
    else
    {
        double divisor = den;
        if (kind == 2) // RSQRT
        {
            latency = 13;
            if (den < 0)
                status = 0x10;
            divisor = std::sqrt(std::fabs(den));
        }
        if (divisor == 0)
        {
            status = num == 0 ? 0x10u : 0x20u; // 0/0 invalid, x/0 divide by zero
            const bool negative = (normalize(numerator) ^ (kind == 2 ? 0u : normalize(denominator))) & 0x80000000u;
            result = (negative ? 0x80000000u : 0u) | 0x7F7FFFFFu;
        }
        else
        {
            result = fromDouble(num / divisor);
        }
    }
    s.qPending = true;
    s.qValue = result;
    s.qStatus = status;
    s.qReady = s.cycle + static_cast<std::uint64_t>(latency);
}

inline void waitQ(State& s)
{
    if (s.qPending)
    {
        waitUntil(s, s.qReady);
        retireFlags(s);
    }
}

inline void efu(State& s, double value, int latency)
{
    if (s.pPending)
    {
        waitUntil(s, s.pReady);
        retireFlags(s);
    }
    s.pPending = true;
    s.pValue = fromDouble(value);
    s.pReady = s.cycle + static_cast<std::uint64_t>(latency);
}

inline void waitP(State& s)
{
    if (s.pPending)
    {
        waitUntil(s, s.pReady);
        retireFlags(s);
    }
}

inline void fsset(State& s, std::uint32_t immediate)
{
    pushFlags(s, {s.cycle + kFmacLatency, 0, immediate & 0xFC0, true});
}

inline void fcset(State& s, std::uint32_t immediate)
{
    pushClip(s, {s.cycle + kFmacLatency, immediate, true});
}

// Ends a program: everything still in flight lands.
inline void finish(State& s, std::uint32_t pc)
{
    std::uint64_t last = s.cycle;
    for (std::uint32_t n = 0; n < s.flagCount; ++n)
        last = std::max(last, s.flagEvents[(s.flagHead + n) % s.flagEvents.size()].ready);
    for (std::uint32_t n = 0; n < s.clipCount; ++n)
        last = std::max(last, s.clipEvents[(s.clipHead + n) % s.clipEvents.size()].ready);
    if (s.qPending)
        last = std::max(last, s.qReady);
    if (s.pPending)
        last = std::max(last, s.pReady);
    s.cycle = last;
    retireFlags(s);
    s.pc = pc;
}

inline void xgkick(State& s, std::uint32_t address)
{
    if (s.xgkick)
        s.xgkick(s, address);
}

// A compiled program runs from an entry address (bytes) and returns
//   0 at its E bit (s.pc: where the VU stopped),
//   1 when execution leaves it (s.pc: the address it continues at), or
//   2 when s.budget runs out (s.pc: the pair it would run next).
using Program = int (*)(State& s, std::uint32_t entry);

} // namespace tgr::hw::vu
