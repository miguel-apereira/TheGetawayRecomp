// SPDX-License-Identifier: MIT
#pragma once

#include "hw/vu/VuCore.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tgr::hw::vu
{

// One microprogram compiled by tgr-vurecomp (recomp/generated/vu).
struct ProgramInfo
{
    int unit;
    std::uint32_t loadAddress; // bytes in VU code memory
    std::uint32_t size;        // bytes
    std::uint64_t hash;        // programHash of its code
    std::uint32_t elfAddress;  // where it comes from, for logs
    Program run;
};

// The table tgr-vurecomp generates; the last entry has run == nullptr.
extern const ProgramInfo kPrograms[];
extern const std::size_t kProgramCount;

// FNV-1a, 64-bit.
std::uint64_t programHash(const std::uint8_t* bytes, std::size_t size);

// Finds which compiled programs are in a VU's code memory now, and runs
// them. The VIF calls invalidate() after every MPG upload.
class CodeMemory
{
public:
    CodeMemory(int unit, const std::uint8_t* code, std::uint32_t size);

    void invalidate() { stale_ = true; }

    // Runs from `address` until the program ends. Returns false when the code
    // there is not a compiled program (s.pc then holds that address).
    bool run(State& s, std::uint32_t address);

private:
    const ProgramInfo* find(std::uint32_t address);

    int unit_;
    const std::uint8_t* code_;
    std::uint32_t size_;
    bool stale_ = true;
    std::vector<const ProgramInfo*> loaded_; // programs whose code is in memory now
};

} // namespace tgr::hw::vu
