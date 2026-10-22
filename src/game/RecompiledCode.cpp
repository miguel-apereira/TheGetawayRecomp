// SPDX-License-Identifier: MIT
#include "game/RecompiledCode.h"

#include "game/GameBuild.h"

#include "ps2_runtime.h"

namespace tgr::game
{

RecompiledCodeInfo recompiledCodeInfo()
{
    RecompiledCodeInfo info;
    info.tableBase = g_ps2RecompiledFunctionTableBase;
    info.tableEnd = g_ps2RecompiledFunctionTableEnd;
    for (std::uint32_t slot = 0; slot < g_ps2RecompiledFunctionTableSlotCount; ++slot)
    {
        if (g_ps2RecompiledFunctionTable[slot] != nullptr)
            ++info.entryCount;
    }
    return info;
}

Guest::Guest() = default;

Guest::~Guest()
{
    stop();
}

bool Guest::start(const std::filesystem::path& gameDataDir, std::string& error)
{
    runtime_ = std::make_unique<PS2Runtime>();
    if (!runtime_->initializeCore())
    {
        error = "The PS2 runtime failed to initialise";
        return false;
    }

    const std::filesystem::path elf = gameDataDir / kBootElf;
    if (!runtime_->loadELF(elf.string()))
    {
        error = "Could not load " + elf.string();
        return false;
    }

    runtime_->startGuest();
    started_ = true;
    return true;
}

bool Guest::finished() const
{
    return !started_ || runtime_->isGuestFinished();
}

std::uint32_t Guest::pc() const
{
    return runtime_ ? runtime_->m_debugPc.load(std::memory_order_relaxed) : 0;
}

void Guest::stop()
{
    if (started_)
    {
        runtime_->stopGuest();
        started_ = false;
    }
}

} // namespace tgr::game
