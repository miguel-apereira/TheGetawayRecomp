// SPDX-License-Identifier: MIT
#include "game/RecompiledCode.h"

#include "game/GameBuild.h"
#include "game/Pfs.h"
#include "game/Stream.h"
#include "hw/Ipu.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <cstring>
#include <format>

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

Guest::Guest() : runtime_(std::make_unique<PS2Runtime>())
{
}

Guest::~Guest()
{
    stop();
}

bool Guest::start(const std::filesystem::path& gameDataDir, std::string& error)
{
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

    // The game keeps its own malloc and sizes its main heap up to just below
    // its stack (0x1FF4000), so it owns all RAM above its ELF. The runtime's own
    // guest memory goes below the ELF instead, in 0x80000-0x100000, which the
    // EE kernel leaves to programs and this game never uses: a small heap for
    // SIF and GS packets, then stacks for interrupt handlers and callbacks.
    runtime_->reserveRuntimeGuestMemory(0x00080000, 0x000A0000, 0x000A0000, 0x00100000);

    pfs_ = std::make_unique<Pfs>(gameDataDir);
    pfs_->install(*runtime_);
    stream_ = std::make_unique<Stream>(*pfs_);
    stream_->install(*runtime_);
    ipu_ = std::make_unique<hw::Ipu>();
    runtime_->memory().setIpuDevice(ipu_.get());

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

std::uint32_t Guest::peek(std::uint32_t address) const
{
    const std::uint32_t physical = address & 0x1FFFFFFC;
    if (!runtime_ || physical >= PS2_RAM_SIZE)
        return 0;
    std::uint32_t value = 0;
    std::memcpy(&value, runtime_->memory().getRDRAM() + physical, sizeof(value));
    return value;
}

std::string Guest::hardwareStats() const
{
    if (!runtime_)
        return {};
    const PS2Memory& memory = runtime_->memory();
    return std::format("dma={} gif={} gs={} vif={}", memory.dmaStartCount(), memory.gifCopyCount(),
                       memory.gsWriteCount(), memory.vifWriteCount());
}

std::string Guest::threadStates() const
{
    if (!runtime_)
        return {};
    const EeKernelSnapshot snapshot = runtime_->eeScheduler().snapshot();
    std::string text;
    for (const EeThreadSnapshot& thread : snapshot.threads)
    {
        if (thread.id < 0)
            continue; // dispatcher threads for interrupts and callbacks
        text += std::format("  thread {} prio {} status {} wait {}:{} pc 0x{:08X} ra 0x{:08X} entry 0x{:08X}\n",
                            thread.id, thread.currentPriority, static_cast<int>(thread.status),
                            static_cast<int>(thread.waitReason), thread.waitId, thread.pc, thread.ra, thread.entry);
    }
    for (const EeSemaphoreSnapshot& sema : snapshot.semaphores)
    {
        if (sema.waiters)
            text += std::format("  sema {} count {} waiters {}\n", sema.id, sema.count, sema.waiters);
    }
    return text;
}

void Guest::setPadReader(PadReader reader)
{
    runtime_->padBackend().setHostReader(
        [reader = std::move(reader)](int port, int, std::uint8_t* data, std::size_t size) {
            return reader(port, data, size);
        });
}

bool Guest::captureFrame(std::vector<std::uint8_t>& rgba, std::uint32_t& width, std::uint32_t& height)
{
    if (!started_)
        return false;
    runtime_->gs().latchHostPresentationFrame();
    return runtime_->gs().copyLatchedHostPresentationFrame(rgba, width, height) && width && height;
}

void Guest::stop()
{
    if (started_)
    {
        runtime_->stopGuest();
        started_ = false;
    }
    if (runtime_)
        runtime_->memory().setIpuDevice(nullptr);
}

} // namespace tgr::game
