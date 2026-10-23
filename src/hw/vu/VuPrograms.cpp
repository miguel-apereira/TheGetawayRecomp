// SPDX-License-Identifier: MIT
#include "hw/vu/VuPrograms.h"

#include "app/Log.h"

#include <unordered_set>

namespace tgr::hw::vu
{

std::uint64_t programHash(const std::uint8_t* bytes, std::size_t size)
{
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

CodeMemory::CodeMemory(int unit, const std::uint8_t* code, std::uint32_t size) : unit_(unit), code_(code), size_(size)
{
}

const ProgramInfo* CodeMemory::find(std::uint32_t address)
{
    if (stale_)
    {
        loaded_.clear();
        for (std::size_t n = 0; n < kProgramCount; ++n)
        {
            const ProgramInfo& program = kPrograms[n];
            if (program.unit != unit_ || program.loadAddress + program.size > size_)
                continue;
            if (programHash(code_ + program.loadAddress, program.size) == program.hash)
                loaded_.push_back(&program);
        }
        stale_ = false;
    }
    for (const ProgramInfo* program : loaded_)
    {
        if (address >= program->loadAddress && address < program->loadAddress + program->size)
            return program;
    }
    return nullptr;
}

bool CodeMemory::run(State& s, std::uint32_t address)
{
    for (;;)
    {
        const ProgramInfo* program = find(address);
        if (!program && !stale_)
        {
            stale_ = true; // the EE may have written code memory directly
            program = find(address);
        }
        if (!program)
        {
            s.pc = address;
            static std::unordered_set<std::uint64_t> reported;
            const std::uint64_t key = (static_cast<std::uint64_t>(unit_) << 32) | address;
            if (reported.insert(key).second)
                log::warn("VU{}: no compiled microprogram at 0x{:04X}", unit_, address);
            return false;
        }
        switch (program->run(s, address))
        {
        case 0:
            return true;
        case 1:
            address = s.pc; // continues in another program
            break;
        default:
            log::warn("VU{}: microprogram from ELF 0x{:08X} still running at 0x{:04X}; stopped it", unit_,
                      program->elfAddress, s.pc);
            return true;
        }
    }
}

} // namespace tgr::hw::vu
