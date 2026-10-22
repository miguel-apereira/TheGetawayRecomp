// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

// The C++ that ps2_recomp generated from SCUS_971.33 (recomp/generated),
// linked in when TGR_WITH_RECOMPILED_CODE is on.
namespace tgr::game
{

struct RecompiledCodeInfo
{
    std::uint32_t tableBase = 0;   // first guest PC in the function table
    std::uint32_t tableEnd = 0;    // one past the last guest PC
    std::size_t entryCount = 0;    // guest PCs with a recompiled function
};

// Reads the generated function table. Referencing it is also what makes the
// linker keep the generated code in the executable.
RecompiledCodeInfo recompiledCodeInfo();

} // namespace tgr::game
