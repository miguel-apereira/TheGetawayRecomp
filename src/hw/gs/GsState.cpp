// SPDX-License-Identifier: MIT
#include "hw/gs/GsState.h"

namespace tgr::hw::gs
{

void GsState::reset()
{
    regs_.fill(0);
    written_.fill(false);
    regs_[reg::PRMODECONT] = 1;
    queued_ = 0;
    transferLeft_ = 0;
}

void GsState::gifPacket(const std::uint8_t* data, std::uint32_t bytes)
{
    parseGifPacket(data, bytes, *this);
}

void GsState::tag(const GifTag& tag)
{
    if (listener_)
        listener_->gifTag(tag);
}

Prim GsState::prim() const
{
    const std::uint64_t prim = regs_[reg::PRIM];
    if (regs_[reg::PRMODECONT] & 1)
        return Prim(prim);
    return Prim((prim & 0x7) | (regs_[reg::PRMODE] & 0x7F8));
}

void GsState::write(std::uint8_t address, std::uint64_t value)
{
    if (address >= reg::kCount)
        return;
    if (listener_)
        listener_->registerWrite(*this, address, value);
    regs_[address] = value;
    written_[address] = true;

    switch (address)
    {
    case reg::PRIM:
        queued_ = 0;
        break;
    case reg::XYZ2:
    case reg::XYZF2:
        vertexKick(true);
        break;
    case reg::XYZ3:
    case reg::XYZF3:
        vertexKick(false);
        break;
    case reg::TRXDIR:
    {
        const std::uint32_t direction = value & 3;
        transferLeft_ = 0;
        if (direction == 0)
        {
            const TrxReg size(regs_[reg::TRXREG]);
            const std::uint32_t bitsPerPixel = psmTransferBits(BitBltBuf(regs_[reg::BITBLTBUF]).dpsm());
            transferLeft_ = (static_cast<std::uint64_t>(size.rrw()) * size.rrh() * bitsPerPixel + 7) / 8;
        }
        if (listener_ && direction != 3)
            listener_->transfer(*this, direction);
        break;
    }
    default:
        break;
    }
}

void GsState::restore(std::uint8_t address, std::uint64_t value)
{
    if (address >= reg::kCount)
        return;
    regs_[address] = value;
    written_[address] = true;
    if (address == reg::PRIM)
        queued_ = 0;
}

void GsState::image(const std::uint8_t*, std::uint32_t bytes)
{
    const bool expected = transferLeft_ != 0;
    transferLeft_ = bytes >= transferLeft_ ? 0 : transferLeft_ - bytes;
    if (listener_)
        listener_->transferData(*this, bytes, expected);
}

void GsState::vertexKick(bool draw)
{
    const PrimType type = static_cast<PrimType>(regs_[reg::PRIM] & 7);
    std::uint32_t needed = 0;
    std::uint32_t kept = 0; // vertices a strip or fan keeps for the next one
    switch (type)
    {
    case PrimType::Point:
        needed = 1;
        break;
    case PrimType::Line:
        needed = 2;
        break;
    case PrimType::LineStrip:
        needed = 2;
        kept = 1;
        break;
    case PrimType::Triangle:
        needed = 3;
        break;
    case PrimType::TriangleStrip:
    case PrimType::TriangleFan:
        needed = 3;
        kept = 2;
        break;
    case PrimType::Sprite:
        needed = 2;
        break;
    default:
        return; // type 7 draws nothing
    }
    if (++queued_ < needed)
        return;
    if (draw && listener_)
        listener_->draw(*this, type, needed);
    queued_ = kept;
}

} // namespace tgr::hw::gs
