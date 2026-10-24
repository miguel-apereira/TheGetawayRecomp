// SPDX-License-Identifier: MIT
#include "hw/Vif.h"

#include "app/Log.h"

#include <algorithm>
#include <cstring>

namespace tgr::hw
{

namespace
{

// VIFn_STAT bits.
constexpr std::uint32_t kStatVpsWaiting = 1u << 0; // waiting for the data of a command
constexpr std::uint32_t kStatMrk = 1u << 6;
constexpr std::uint32_t kStatDbf = 1u << 7;  // VIF1: which half of VU1 memory TOPS points at
constexpr std::uint32_t kStatVss = 1u << 8;  // stopped by FBRST.STP
constexpr std::uint32_t kStatVfs = 1u << 9;  // stopped by FBRST.FBK
constexpr std::uint32_t kStatVis = 1u << 10; // stopped by an I bit
constexpr std::uint32_t kStatInt = 1u << 11;
constexpr std::uint32_t kStatEr0 = 1u << 12; // DMAtag mismatch
constexpr std::uint32_t kStatEr1 = 1u << 13; // invalid VIFcode
constexpr std::uint32_t kStallBits = kStatVss | kStatVfs | kStatVis | kStatEr0 | kStatEr1;

// VIFn_ERR bits.
constexpr std::uint32_t kErrMii = 1u << 0; // ignore I bits
constexpr std::uint32_t kErrMe1 = 1u << 2; // ignore invalid VIFcodes

// VIFcode commands (bits 24-30).
enum : std::uint32_t
{
    kNop = 0x00,
    kStcycl = 0x01,
    kOffset = 0x02,
    kBase = 0x03,
    kItop = 0x04,
    kStmod = 0x05,
    kMskpath3 = 0x06,
    kMark = 0x07,
    kFlushe = 0x10,
    kFlush = 0x11,
    kFlusha = 0x13,
    kMscal = 0x14,
    kMscalf = 0x15,
    kMscnt = 0x17,
    kStmask = 0x20,
    kStrow = 0x30,
    kStcol = 0x31,
    kMpg = 0x4A,
    kDirect = 0x50,
    kDirecthl = 0x51,
};

std::uint32_t commandOf(std::uint32_t code)
{
    return (code >> 24) & 0x7F;
}

bool isUnpack(std::uint32_t command)
{
    return (command & 0x60) == 0x60;
}

// Reads the packed elements of an UNPACK, least significant bits first.
class BitReader
{
public:
    explicit BitReader(const std::deque<std::uint32_t>& words) : words_(words) {}

    std::uint32_t read(int bits)
    {
        const std::uint32_t value = peek(bits);
        position_ += static_cast<std::uint64_t>(bits);
        return value;
    }

    std::uint32_t peek(int bits) const
    {
        const std::size_t word = static_cast<std::size_t>(position_ / 32) + 1; // word 0 is the VIFcode
        const int shift = static_cast<int>(position_ % 32);
        std::uint64_t window = word < words_.size() ? words_[word] : 0;
        if (word + 1 < words_.size())
            window |= static_cast<std::uint64_t>(words_[word + 1]) << 32;
        window >>= shift;
        return bits == 32 ? static_cast<std::uint32_t>(window)
                          : static_cast<std::uint32_t>(window & ((1ull << bits) - 1));
    }

private:
    const std::deque<std::uint32_t>& words_;
    std::uint64_t position_ = 0;
};

std::uint32_t extend(std::uint32_t value, int bits, bool isUnsigned)
{
    if (bits == 32 || isUnsigned)
        return value;
    const std::uint32_t sign = 1u << (bits - 1);
    return (value ^ sign) - sign;
}

// Bytes of data after a GIFtag (PACKED, REGLIST or IMAGE).
std::uint64_t gifTagDataBytes(std::uint64_t tagLow)
{
    const std::uint64_t loops = tagLow & 0x7FFF;
    const std::uint64_t format = (tagLow >> 58) & 0x3;
    std::uint64_t registers = (tagLow >> 60) & 0xF;
    if (registers == 0)
        registers = 16;
    switch (format)
    {
    case 0: // PACKED: one quadword per register
        return loops * registers * 16;
    case 1: // REGLIST: one doubleword per register, padded to a quadword
        return (loops * registers + 1) / 2 * 16;
    default: // IMAGE
        return loops * 16;
    }
}

} // namespace

Vif::Vif(int unit, std::uint8_t* vuCode, std::uint8_t* vuData, Host host)
    : unit_(unit), codeSize_(unit == 0 ? 0x1000 : 0x4000), dataQuadwords_(unit == 0 ? 0x100 : 0x400), vuCode_(vuCode),
      vuData_(vuData), host_(std::move(host))
{
}

void Vif::reset()
{
    queue_.clear();
    stat_ = 0;
    err_ = 0;
    mark_ = 0;
    cycle_ = 0;
    mode_ = 0;
    num_ = 0;
    mask_ = 0;
    code_ = 0;
    itops_ = 0;
    itop_ = 0;
    base_ = 0;
    ofst_ = 0;
    tops_ = 0;
    top_ = 0;
    row_ = {};
    col_ = {};
    interrupt_ = false;
    directBuffer_.clear();
}

void Vif::writeRegister(std::uint32_t offset, std::uint32_t value)
{
    switch (offset)
    {
    case 0x10: // FBRST
        if (value & 0x1) // RST
            reset();
        if (value & 0x2) // FBK
            stat_ |= kStatVfs;
        if (value & 0x4) // STP: the queue only holds whole commands, so stop now
            stat_ |= kStatVss;
        if (value & 0x8) // STC
            stat_ &= ~(kStallBits | kStatInt);
        run();
        break;
    case 0x20:
        err_ = value & 0x7;
        break;
    case 0x30:
        mark_ = value & 0xFFFF;
        stat_ &= ~kStatMrk;
        break;
    case 0x40:
        cycle_ = value & 0xFFFF;
        break;
    case 0x50:
        mode_ = value & 0x3;
        break;
    case 0x70:
        mask_ = value;
        break;
    case 0x100:
    case 0x110:
    case 0x120:
    case 0x130:
        row_[(offset - 0x100) / 0x10] = value;
        break;
    case 0x140:
    case 0x150:
    case 0x160:
    case 0x170:
        col_[(offset - 0x140) / 0x10] = value;
        break;
    default:
        break;
    }
}

std::uint32_t Vif::readRegister(std::uint32_t offset)
{
    switch (offset)
    {
    case 0x00:
    {
        std::uint32_t stat = stat_ & ~(0x3u | (0x1Fu << 24));
        if (!queue_.empty())
            stat |= kStatVpsWaiting;
        const std::uint32_t fifoQuadwords = std::min<std::uint32_t>(static_cast<std::uint32_t>(queue_.size() / 4),
                                                                    unit_ == 0 ? 8u : 16u);
        stat |= fifoQuadwords << 24;
        return stat;
    }
    case 0x20:
        return err_;
    case 0x30:
        return mark_;
    case 0x40:
        return cycle_;
    case 0x50:
        return mode_;
    case 0x60:
        return num_;
    case 0x70:
        return mask_;
    case 0x80:
        return code_;
    case 0x90:
        return itops_;
    case 0xA0:
        return base_;
    case 0xB0:
        return ofst_;
    case 0xC0:
        return tops_;
    case 0xD0:
        return itop_;
    case 0xE0:
        return top_;
    case 0x100:
    case 0x110:
    case 0x120:
    case 0x130:
        return row_[(offset - 0x100) / 0x10];
    case 0x140:
    case 0x150:
    case 0x160:
    case 0x170:
        return col_[(offset - 0x140) / 0x10];
    default:
        return 0;
    }
}

void Vif::pushData(const std::uint32_t* words, std::uint32_t count)
{
    queue_.insert(queue_.end(), words, words + count);
    run();
}

bool Vif::stalled() const
{
    return (stat_ & kStallBits) != 0;
}

bool Vif::takeInterrupt()
{
    const bool raised = interrupt_;
    interrupt_ = false;
    return raised;
}

std::uint32_t Vif::commandLength(std::uint32_t code) const
{
    const std::uint32_t command = commandOf(code);
    const std::uint32_t num = (code >> 16) & 0xFF;
    const std::uint32_t imm = code & 0xFFFF;
    switch (command)
    {
    case kStmask:
        return 1;
    case kStrow:
    case kStcol:
        return 4;
    case kMpg:
        return (num == 0 ? 256 : num) * 2;
    case kDirect:
    case kDirecthl:
        return (imm == 0 ? 65536 : imm) * 4;
    default:
        break;
    }
    if (!isUnpack(command))
        return 0;

    const std::uint32_t vn = (command >> 2) & 0x3;
    const std::uint32_t vl = command & 0x3;
    const std::uint32_t writes = num == 0 ? 256 : num;
    std::uint32_t cl = cycle_ & 0xFF;
    std::uint32_t wl = (cycle_ >> 8) & 0xFF;
    if (cl == 0)
        cl = 1;
    if (wl == 0)
        wl = 1;
    const std::uint32_t vectors = wl <= cl ? writes : (writes / wl) * cl + std::min(writes % wl, cl);
    const std::uint32_t bitsPerVector = (vl == 3) ? 16 : (32u >> vl) * (vn + 1);
    return (vectors * bitsPerVector + 31) / 32;
}

void Vif::run()
{
    while (!queue_.empty() && !stalled())
    {
        const std::uint32_t code = queue_.front();
        const std::uint32_t length = commandLength(code);
        if (queue_.size() < 1 + static_cast<std::size_t>(length))
            return; // wait for the rest of its data

        execute(code);
        queue_.erase(queue_.begin(), queue_.begin() + 1 + length);

        if ((code & 0x80000000u) && !(err_ & kErrMii))
        {
            stat_ |= kStatInt | kStatVis;
            interrupt_ = true;
        }
    }
}

void Vif::startMicroprogram(std::uint32_t address)
{
    itop_ = itops_;
    if (unit_ == 1)
    {
        // The program gets the half TOPS points at; the next one gets the other.
        top_ = tops_;
        tops_ = (stat_ & kStatDbf) ? base_ : (base_ + ofst_) & 0x3FF;
        stat_ ^= kStatDbf;
    }
    lastPc_ = host_.runMicroprogram(address % codeSize_, top_, itop_);
}

void Vif::direct(bool directHl)
{
    // PATH2 carries a GS packet stream: a GIFtag's data can run on into the
    // next DIRECT. Send only whole GIFtags; keep a partial one for later.
    const std::uint32_t words = (code_ & 0xFFFF) == 0 ? 65536 * 4 : (code_ & 0xFFFF) * 4;
    const std::size_t start = directBuffer_.size();
    directBuffer_.resize(start + static_cast<std::size_t>(words) * 4);
    for (std::uint32_t i = 0; i < words; ++i)
        std::memcpy(directBuffer_.data() + start + i * 4, &queue_[1 + i], 4);

    std::size_t complete = 0;
    for (std::size_t at = 0; at + 16 <= directBuffer_.size();)
    {
        std::uint64_t tagLow;
        std::memcpy(&tagLow, directBuffer_.data() + at, 8);
        const std::uint64_t end = at + 16 + gifTagDataBytes(tagLow);
        if (end > directBuffer_.size())
            break;
        at = static_cast<std::size_t>(end);
        complete = at;
    }
    if (complete == 0)
        return;
    if (host_.sendPath2)
        host_.sendPath2(directBuffer_.data(), static_cast<std::uint32_t>(complete), directHl);
    directBuffer_.erase(directBuffer_.begin(), directBuffer_.begin() + static_cast<std::ptrdiff_t>(complete));
}

void Vif::execute(std::uint32_t code)
{
    const std::uint32_t command = commandOf(code);
    const std::uint32_t imm = code & 0xFFFF;
    code_ = code;
    num_ = (code >> 16) & 0xFF;

    if (isUnpack(command))
    {
        if ((command & 0x3) == 3 && ((command >> 2) & 0x3) != 3)
        {
            // Only V4-5 uses vl = 3; the rest are invalid. Treat like any invalid code.
        }
        else
        {
            unpack(code);
            return;
        }
    }

    switch (command)
    {
    case kNop:
    case kFlushe:
    case kFlush:
    case kFlusha:
        // Microprograms and GIF transfers finish before the VIF moves on.
        return;
    case kStcycl:
        cycle_ = imm;
        return;
    case kItop:
        itops_ = imm & 0x3FF;
        return;
    case kStmod:
        mode_ = imm & 0x3;
        return;
    case kMark:
        mark_ = imm;
        stat_ |= kStatMrk;
        return;
    case kOffset:
        if (unit_ == 1)
        {
            ofst_ = imm & 0x3FF;
            stat_ &= ~kStatDbf;
            tops_ = base_;
        }
        return;
    case kBase:
        if (unit_ == 1)
            base_ = imm & 0x3FF;
        return;
    case kMskpath3:
        if (unit_ == 1 && host_.maskPath3)
            host_.maskPath3((imm & 0x8000) != 0);
        return;
    case kDirect:
    case kDirecthl:
        if (unit_ == 1)
            direct(command == kDirecthl);
        return;
    case kMscal:
    case kMscalf:
        startMicroprogram(imm * 8u);
        return;
    case kMscnt:
        startMicroprogram(lastPc_);
        return;
    case kStmask:
        mask_ = queue_[1];
        return;
    case kStrow:
        for (int i = 0; i < 4; ++i)
            row_[i] = queue_[1 + i];
        return;
    case kStcol:
        for (int i = 0; i < 4; ++i)
            col_[i] = queue_[1 + i];
        return;
    case kMpg:
    {
        const std::uint32_t count = (num_ == 0 ? 256 : num_) * 2;
        const std::uint32_t start = (imm * 8u) % codeSize_;
        std::uint32_t address = start;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::memcpy(vuCode_ + address, &queue_[1 + i], 4);
            address = (address + 4) % codeSize_;
        }
        host_.codeUploaded(start, count * 4);
        return;
    }
    default:
        break;
    }

    // An invalid VIFcode stalls the VIF with ER1 unless ERR.ME1 masks it.
    static int reported = 0;
    if (reported++ < 8)
        log::warn("VIF{}: invalid VIFcode 0x{:08X}", unit_, code);
    if (!(err_ & kErrMe1))
    {
        stat_ |= kStatEr1 | kStatInt;
        interrupt_ = true;
    }
}

void Vif::unpack(std::uint32_t code)
{
    const std::uint32_t command = commandOf(code);
    const std::uint32_t imm = code & 0xFFFF;
    const std::uint32_t vn = (command >> 2) & 0x3;
    const std::uint32_t vl = command & 0x3;
    const bool masked = (command & 0x10) != 0;
    const bool isUnsigned = (imm & 0x4000) != 0;
    const std::uint32_t writes = num_ == 0 ? 256 : num_;
    std::uint32_t cl = cycle_ & 0xFF;
    std::uint32_t wl = (cycle_ >> 8) & 0xFF;
    if (cl == 0)
        cl = 1;
    if (wl == 0)
        wl = 1;
    std::uint32_t base = imm & 0x3FF;
    if (unit_ == 1 && (imm & 0x8000)) // FLG: relative to the double buffer TOPS points at
        base += tops_;
    const int bits = vl == 3 ? 16 : static_cast<int>(32u >> vl);

    BitReader reader(queue_);
    for (std::uint32_t n = 0; n < writes; ++n)
    {
        const std::uint32_t cyclePosition = n % wl;
        std::uint32_t address;
        bool fromData = true;
        if (wl <= cl)
        {
            address = base + (n / wl) * cl + cyclePosition;
        }
        else
        {
            address = base + n;
            fromData = cyclePosition < cl;
        }

        std::array<std::uint32_t, 4> data{};
        if (fromData)
        {
            if (vl == 3) // V4-5: RGBA 5:5:5:1
            {
                const std::uint32_t v = reader.read(16);
                data = {(v & 0x1F) << 3, ((v >> 5) & 0x1F) << 3, ((v >> 10) & 0x1F) << 3, ((v >> 15) & 0x1) << 7};
            }
            else
            {
                std::array<std::uint32_t, 4> element{};
                for (std::uint32_t c = 0; c <= vn; ++c)
                    element[c] = extend(reader.read(bits), bits, isUnsigned);
                switch (vn)
                {
                case 0: // S: one value in every field
                    data = {element[0], element[0], element[0], element[0]};
                    break;
                case 1: // V2: the hardware repeats X and Y in Z and W
                    data = {element[0], element[1], element[0], element[1]};
                    break;
                case 2: // V3: W is whatever the next element holds
                    data = {element[0], element[1], element[2], extend(reader.peek(bits), bits, isUnsigned)};
                    break;
                default:
                    data = element;
                    break;
                }
            }
        }
        // Without the m bit every field takes the data; filling writes always follow the mask.
        writeUnpacked(address, cyclePosition, data, fromData, (masked || !fromData) ? mask_ : 0);
    }
    num_ = 0;
}

void Vif::writeUnpacked(std::uint32_t address, std::uint32_t cyclePosition, const std::array<std::uint32_t, 4>& data,
                        bool fromData, std::uint32_t mask)
{
    std::uint8_t* target = vuData_ + (address % dataQuadwords_) * 16;
    const std::uint32_t maskRow = std::min<std::uint32_t>(cyclePosition, 3);
    for (std::uint32_t field = 0; field < 4; ++field)
    {
        const std::uint32_t select = (mask >> ((maskRow * 4 + field) * 2)) & 0x3;
        std::uint32_t value;
        switch (select)
        {
        case 0: // the data, through the addition mode
            if (!fromData)
            {
                value = row_[field];
                break;
            }
            value = data[field];
            if (mode_ == 1) // offset
            {
                value += row_[field];
            }
            else if (mode_ == 2) // difference: accumulate into the row register
            {
                row_[field] += value;
                value = row_[field];
            }
            break;
        case 1:
            value = row_[field];
            break;
        case 2:
            value = col_[maskRow];
            break;
        default: // write protected
            continue;
        }
        std::memcpy(target + field * 4, &value, 4);
    }
}

} // namespace tgr::hw
