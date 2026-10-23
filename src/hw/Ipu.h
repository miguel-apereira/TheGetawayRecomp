// SPDX-License-Identifier: MIT
#pragma once

#include "runtime/ps2_ipu.h"

#include <array>
#include <cstdint>
#include <vector>

// Native replacement for the PS2's IPU: the macroblock decoder the game
// drives itself to play its movies (The Getaway's IPUHugeMovie). It takes a
// bitstream through its input FIFO, decodes MPEG-2 intra macroblocks (IDEC)
// to RGB32 or RGB16, and hands them out through its output FIFO; the game
// moves both FIFOs with DMA channels 4 and 3.
//
// Implemented from the MPEG-2 video standard (ISO/IEC 13818-2) and the
// documented IPU command set. Commands the game does not use (BDEC, VDEC,
// CSC) are accepted and finish at once without output.
namespace tgr::hw
{

class Ipu final : public PS2IpuDevice
{
public:
    Ipu();

    void writeRegister(std::uint32_t offset, std::uint32_t value) override;
    std::uint32_t readRegister(std::uint32_t offset) override;
    void pushInput(const std::uint8_t* quadwords, std::uint32_t count) override;
    std::uint32_t outputAvailable() const override;
    std::uint32_t popOutput(std::uint8_t* quadwords, std::uint32_t maxCount) override;
    bool takeInterrupt() override;

private:
    // ---- Input bitstream (MSB first, bytes in address order) ----
    std::uint64_t bitsAvailable() const;
    std::uint32_t peekBits(int count) const; // count <= 32; zero past the end
    void skipBits(int count);
    std::uint32_t readBits(int count);
    std::uint32_t peekWord() const; // next 32 bits, as the IPU returns them
    void compactInput();

    // ---- Commands ----
    void reset();
    void start(std::uint32_t command);
    void run(); // continues the current command as far as the input allows
    void finish();

    bool runIdec();
    bool runPack();
    bool decodeMacroblock(std::array<std::uint8_t, 384>& pixels);
    bool decodeIntraBlock(int component, std::array<std::int16_t, 64>& coefficients);
    void storeBlock(const std::array<std::int16_t, 64>& coefficients, std::uint8_t* target, int stride);
    void convertMacroblock(const std::array<std::uint8_t, 384>& pixels);
    void endSlice();

    // ---- Registers ----
    std::uint32_t command_ = 0;     // IPU_CMD as written
    std::uint32_t commandData_ = 0; // IPU_CMD.DATA (FDEC/VDEC result)
    std::uint32_t control_ = 0;     // IPU_CTRL fields the game writes (bits 16-26)
    bool busy_ = false;
    bool startCodeDetected_ = false;
    bool errorCodeDetected_ = false;
    bool interrupt_ = false;
    std::uint32_t codedBlockPattern_ = 0;

    // ---- FIFOs ----
    std::vector<std::uint8_t> input_;
    std::uint64_t inputBit_ = 0; // bits of input_ already consumed
    mutable bool underrun_ = false; // a peek went past the end of the input
    std::vector<std::uint8_t> output_;
    std::size_t outputRead_ = 0;

    // ---- Tables set by SETIQ, SETVQ, SETTH ----
    std::array<std::uint8_t, 64> intraMatrix_{};    // in scan order
    std::array<std::uint8_t, 64> nonIntraMatrix_{}; // in scan order
    std::array<std::uint16_t, 16> palette_{};       // RGB555 for PACK INDX4
    std::uint32_t threshold0_ = 0;
    std::uint32_t threshold1_ = 0;

    // ---- IDEC state, kept between macroblocks so a decode can wait for data ----
    int quantizerScale_ = 0;
    std::array<int, 3> dcPredictor_{};
    bool sliceStarted_ = false;
};

} // namespace tgr::hw
