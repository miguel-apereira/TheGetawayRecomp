// SPDX-License-Identifier: MIT
#pragma once

#include "hw/gs/GifParser.h"
#include "hw/gs/GsRegs.h"

#include <array>
#include <cstdint>

// The GS as a register file plus the bits of state that decide what a
// register write does: the vertex queue (when a vertex kick draws) and the
// transfer in progress. It does not draw; a listener sees each primitive
// with the state it is drawn with.
namespace tgr::hw::gs
{

class GsState : private GifVisitor
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void gifTag(const GifTag&) {}
        // Every register write, before it takes effect.
        virtual void registerWrite(const GsState&, std::uint8_t /*address*/, std::uint64_t /*value*/) {}
        // A primitive is drawn (a vertex kick completed it).
        virtual void draw(const GsState&, PrimType, std::uint32_t /*vertices*/) {}
        // TRXDIR was written: 0 host to local, 1 local to host, 2 local to local.
        virtual void transfer(const GsState&, std::uint32_t /*direction*/) {}
        // IMAGE data; `expected` is false when no host-to-local transfer wanted it.
        virtual void transferData(const GsState&, std::uint32_t /*bytes*/, bool /*expected*/) {}
    };

    GsState() { reset(); }

    void setListener(Listener* listener) { listener_ = listener; }
    void reset();

    void gifPacket(const std::uint8_t* data, std::uint32_t bytes);
    void write(std::uint8_t address, std::uint64_t value);
    void image(const std::uint8_t* data, std::uint32_t bytes);

    // Sets a register without acting on it (no kick, no transfer, no
    // listener), as when a dump restores the state it starts from.
    void restore(std::uint8_t address, std::uint64_t value);

    std::uint64_t value(std::uint8_t address) const { return address < reg::kCount ? regs_[address] : 0; }
    bool written(std::uint8_t address) const { return address < reg::kCount && written_[address]; }

    // PRIM's type with the attributes in effect: PRIM's own, or PRMODE's when
    // PRMODECONT.AC is 0.
    Prim prim() const;
    std::uint32_t context() const { return prim().ctxt(); }
    // A per-context register (TEX0_1, FRAME_1, ...) for the context in use.
    std::uint64_t contextValue(std::uint8_t firstAddress) const { return regs_[firstAddress + context()]; }

    // Bytes still expected by the host-to-local transfer in progress.
    std::uint64_t transferBytesLeft() const { return transferLeft_; }

private:
    void tag(const GifTag& tag) override;
    void registerWrite(std::uint8_t address, std::uint64_t value) override { write(address, value); }
    void imageData(const std::uint8_t* data, std::uint32_t bytes) override { image(data, bytes); }
    void vertexKick(bool draw);

    std::array<std::uint64_t, reg::kCount> regs_{};
    std::array<bool, reg::kCount> written_{};
    std::uint32_t queued_ = 0; // vertices since PRIM, as far as the primitive needs them
    std::uint64_t transferLeft_ = 0;
    Listener* listener_ = nullptr;
};

} // namespace tgr::hw::gs
