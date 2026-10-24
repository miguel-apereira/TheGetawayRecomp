// SPDX-License-Identifier: MIT
#pragma once

#include "runtime/ps2_vif.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

// Native replacement for the PS2's VIFs, the interfaces in front of the vector
// units: VIF0 for VU0 and VIF1 for VU1. A VIF takes VIFcodes and their data
// from its DMA channel or FIFO, unpacks vectors into VU data memory, uploads
// microprograms into VU code memory and starts them (MSCAL/MSCALF/MSCNT).
// VIF1 also double-buffers VU1 data memory (BASE/OFFSET/TOPS) and feeds the
// GIF: DIRECT/DIRECTHL send GS packets down PATH2, and MSKPATH3 holds PATH3.
//
// Stalls are real: an I bit stops the VIF after its command and raises the
// VIF's interrupt, and the rest of its input waits until the game resumes it
// with FBRST.STC. The Getaway relies on both: VIF0 stops after each VU0
// vertex batch while the game moves the results out, and VIF1 stops where
// the textures for the next part of a frame must go up over PATH3 first.
//
// Implemented from the documented VIF behaviour.
namespace tgr::hw
{

class Vif final : public PS2VifDevice
{
public:
    struct Host
    {
        // Runs the VU microprogram at a code address (bytes) with the TOP and
        // ITOP the VIF hands it, and returns the address it stopped at.
        std::function<std::uint32_t(std::uint32_t address, std::uint32_t top, std::uint32_t itop)> runMicroprogram;
        // Called after MPG writes VU code memory (address and size in bytes).
        std::function<void(std::uint32_t address, std::uint32_t bytes)> codeUploaded;
        // VIF1: whole GS packets for PATH2 (DIRECTHL waits for PATH3 on the chip).
        std::function<void(const std::uint8_t* data, std::uint32_t bytes, bool directHl)> sendPath2;
        // VIF1: MSKPATH3.
        std::function<void(bool masked)> maskPath3;
    };

    Vif(int unit, std::uint8_t* vuCode, std::uint8_t* vuData, Host host);

    void writeRegister(std::uint32_t offset, std::uint32_t value) override;
    std::uint32_t readRegister(std::uint32_t offset) override;
    void pushData(const std::uint32_t* words, std::uint32_t count) override;
    bool stalled() const override;
    bool takeInterrupt() override;

private:
    void reset();
    void run(); // processes queued commands until the queue runs dry or the VIF stalls
    std::uint32_t commandLength(std::uint32_t code) const; // words of data after the VIFcode
    void execute(std::uint32_t code);
    void unpack(std::uint32_t code);
    void writeUnpacked(std::uint32_t address, std::uint32_t cyclePosition, const std::array<std::uint32_t, 4>& data,
                       bool fromData, std::uint32_t mask);
    void startMicroprogram(std::uint32_t address);
    void direct(bool directHl);

    const int unit_;
    const std::uint32_t codeSize_;      // bytes
    const std::uint32_t dataQuadwords_; // VU data memory size
    std::uint8_t* vuCode_;
    std::uint8_t* vuData_;
    Host host_;

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
    std::uint32_t base_ = 0; // VIF1
    std::uint32_t ofst_ = 0; // VIF1
    std::uint32_t tops_ = 0; // VIF1
    std::uint32_t top_ = 0;  // VIF1
    std::array<std::uint32_t, 4> row_{};
    std::array<std::uint32_t, 4> col_{};
    std::uint32_t lastPc_ = 0; // where the last microprogram stopped, for MSCNT
    bool interrupt_ = false;

    // DIRECT data not yet sent: a GIFtag whose data runs on into the next DIRECT.
    std::vector<std::uint8_t> directBuffer_;
};

} // namespace tgr::hw
