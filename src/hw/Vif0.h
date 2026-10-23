// SPDX-License-Identifier: MIT
#pragma once

#include "runtime/ps2_vif0.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>

// Native replacement for the PS2's VIF0, the interface in front of VU0. It
// takes VIFcodes and their data from DMA channel 0 or its FIFO, unpacks
// vectors into VU0 data memory, uploads microprograms into VU0 code memory
// and starts them (MSCAL/MSCALF/MSCNT).
//
// The Getaway streams vertex batches through VU0 while loading a level: each
// batch ends in a VIFcode with the I bit set, VIF0 stalls there, and the game
// polls VIF0_STAT.INT, moves the results out through the scratchpad and
// resumes VIF0 with FBRST.STC. So stalls are real here: VIF0 stops after the
// I-bit command and keeps the rest of its input until STC.
//
// Implemented from the documented VIF behaviour. VIF1-only commands (BASE,
// OFFSET, MSKPATH3, FLUSH, FLUSHA, DIRECT, DIRECTHL) do nothing on VIF0.
namespace tgr::hw
{

class Vif0 final : public PS2Vif0Device
{
public:
    // Runs the VU0 microprogram at a code address (bytes) with the given
    // ITOP, and returns the address it stopped at.
    using RunMicroprogram = std::function<std::uint32_t(std::uint32_t address, std::uint32_t itop)>;
    // Called after MPG writes VU0 code memory (address and size in bytes).
    using CodeUploaded = std::function<void(std::uint32_t address, std::uint32_t bytes)>;

    Vif0(std::uint8_t* vuCode, std::uint8_t* vuData, RunMicroprogram run, CodeUploaded uploaded);

    void writeRegister(std::uint32_t offset, std::uint32_t value) override;
    std::uint32_t readRegister(std::uint32_t offset) override;
    void pushData(const std::uint32_t* words, std::uint32_t count) override;
    bool stalled() const override;
    bool takeInterrupt() override;

private:
    void reset();
    void run(); // processes queued commands until the queue runs dry or VIF0 stalls
    std::uint32_t commandLength(std::uint32_t code) const; // words of data after the VIFcode
    void execute(std::uint32_t code);
    void unpack(std::uint32_t code);
    void writeUnpacked(std::uint32_t address, std::uint32_t cyclePosition, const std::array<std::uint32_t, 4>& data,
                       bool fromData, std::uint32_t mask);

    std::uint8_t* vuCode_;
    std::uint8_t* vuData_;
    RunMicroprogram runMicroprogram_;
    CodeUploaded codeUploaded_;

    std::deque<std::uint32_t> queue_; // words received and not yet processed

    // ---- Registers ----
    std::uint32_t stat_ = 0;
    std::uint32_t err_ = 0;
    std::uint32_t mark_ = 0;
    std::uint32_t cycle_ = 0;
    std::uint32_t mode_ = 0;
    std::uint32_t num_ = 0;
    std::uint32_t mask_ = 0;
    std::uint32_t code_ = 0;
    std::uint32_t itops_ = 0;
    std::uint32_t itop_ = 0;
    std::array<std::uint32_t, 4> row_{};
    std::array<std::uint32_t, 4> col_{};
    std::uint32_t lastPc_ = 0; // where the last microprogram stopped, for MSCNT
    bool interrupt_ = false;
};

} // namespace tgr::hw
