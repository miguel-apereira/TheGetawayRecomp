// SPDX-License-Identifier: MIT
#include "game/RecompiledCode.h"

#include "game/GameBuild.h"
#include "game/Pfs.h"
#include "game/Stream.h"
#include "hw/Ipu.h"
#include "hw/Vif0.h"
#include "hw/vu/VuPrograms.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include "app/Log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <unordered_set>
#include <vector>

namespace tgr::game
{

namespace
{

bool environmentFlag(const char* name)
{
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    const bool set = _dupenv_s(&value, &size, name) == 0 && value != nullptr;
    std::free(value);
    return set;
#else
    return std::getenv(name) != nullptr;
#endif
}

std::uint32_t floatBits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}

float bitsFloat(std::uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

// For VU0 code that was written without MPG (the EE can store to VU0 code
// memory directly): finds the run of code memory around `address` that
// matches the ELF and logs where it comes from.
void reportMissing(PS2Runtime& runtime, std::uint32_t address)
{
    static std::unordered_set<std::uint32_t> reported;
    if (!reported.insert(address).second)
        return;
    const std::uint8_t* code = runtime.memory().getVU0Code();
    const std::uint8_t* rdram = runtime.memory().getRDRAM();
    const std::uint8_t* elfStart = rdram + 0x100000;
    const std::uint8_t* elfEnd = rdram + 0x400000;
    const std::uint32_t probe = std::min<std::uint32_t>(16, PS2_VU0_CODE_SIZE - address);
    const std::uint8_t* found = std::search(elfStart, elfEnd, code + address, code + address + probe);
    if (found == elfEnd)
    {
        log::warn("VU0: code at 0x{:04X} is not in the ELF; it cannot be compiled", address);
        return;
    }
    std::uint32_t start = address;
    const std::uint8_t* elf = found;
    while (start >= 8 && elf - 8 >= elfStart && std::memcmp(code + start - 8, elf - 8, 8) == 0)
    {
        start -= 8;
        elf -= 8;
    }
    std::uint32_t end = address;
    while (end + 8 <= PS2_VU0_CODE_SIZE && std::memcmp(code + end, found + (end - address), 8) == 0)
        end += 8;
    log::warn("VU0: code at 0x{:04X} is not compiled: ELF 0x{:08X}, {} bytes, loaded at 0x{:X}",
              address, static_cast<std::uint32_t>(elf - rdram), end - start, start);
}

// Runs a VU0 microprogram natively. VU0's registers are the EE's COP2
// registers, so the program starts from and leaves its results in the EE
// thread's context.
void runVu0(PS2Runtime& runtime, hw::vu::CodeMemory& code, R5900Context& context, std::uint32_t address)
{
    hw::vu::State s;
    for (int reg = 0; reg < 32; ++reg)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(s.vf[reg]), _mm_castps_si128(context.vu0_vf[reg]));
    for (int reg = 0; reg < 16; ++reg)
        s.vi[reg] = static_cast<std::uint16_t>(context.vi[reg]);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(s.acc), _mm_castps_si128(context.vu0_acc));
    s.q = floatBits(context.vu0_q);
    s.p = floatBits(context.vu0_p);
    s.i = floatBits(context.vu0_i);
    s.r = 0x3F800000u | (static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(context.vu0_r))) & 0x007FFFFFu);
    s.mac = context.vu0_mac_flags;
    s.status = context.vu0_status;
    s.clip = context.vu0_clip_flags;
    s.itop = context.vu0_itop;
    s.data = runtime.memory().getVU0Data();
    s.dataQuadwordMask = PS2_VU0_DATA_SIZE / 16 - 1;
    s.budget = 1u << 24;
    s.vf[0][0] = s.vf[0][1] = s.vf[0][2] = 0;
    s.vf[0][3] = 0x3F800000u;
    s.vi[0] = 0;

    if (!code.run(s, address & (PS2_VU0_CODE_SIZE - 8)))
        reportMissing(runtime, s.pc);

    for (int reg = 1; reg < 32; ++reg)
        context.vu0_vf[reg] = _mm_castsi128_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s.vf[reg])));
    for (int reg = 1; reg < 16; ++reg)
        context.vi[reg] = s.vi[reg];
    context.vu0_acc = _mm_castsi128_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s.acc)));
    context.vu0_q = bitsFloat(s.q);
    context.vu0_p = bitsFloat(s.p);
    context.vu0_i = bitsFloat(s.i);
    context.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(s.r)));
    context.vu0_mac_flags = s.mac;
    context.vu0_status = static_cast<std::uint16_t>(s.status);
    context.vu0_clip_flags = s.clip;
    context.vu0_clip_flags2 = s.clip;
    context.vu0_pc = s.pc;
    context.vu0_tpc = s.pc;
}

// Debugging aid (TGR_VU_CHECK=1): runs each VU0 microprogram both compiled
// and on PS2Recomp's VU interpreter, from the same state, keeps the compiled
// result and logs where the two disagree.
void checkVu0(PS2Runtime& runtime, hw::vu::CodeMemory& code, R5900Context& context, std::uint32_t address)
{
    std::uint8_t* data = runtime.memory().getVU0Data();
    const std::vector<std::uint8_t> dataBefore(data, data + PS2_VU0_DATA_SIZE);
    R5900Context interpreted = context;

    runVu0(runtime, code, context, address);
    const std::vector<std::uint8_t> dataNative(data, data + PS2_VU0_DATA_SIZE);

    std::memcpy(data, dataBefore.data(), dataBefore.size());
    runtime.setVu0MicroprogramHandler({});
    runtime.executeVU0Microprogram(runtime.memory().getRDRAM(), &interpreted, address);
    runtime.setVu0MicroprogramHandler([&runtime, &code](std::uint8_t*, R5900Context* c, std::uint32_t a) {
        checkVu0(runtime, code, *c, a);
    });

    static int reports = 0;
    std::string differences;
    for (int reg = 1; reg < 32; ++reg)
    {
        if (std::memcmp(&context.vu0_vf[reg], &interpreted.vu0_vf[reg], 16) != 0)
        {
            alignas(16) std::uint32_t n[4];
            alignas(16) std::uint32_t i[4];
            std::memcpy(n, &context.vu0_vf[reg], 16);
            std::memcpy(i, &interpreted.vu0_vf[reg], 16);
            differences += std::format(" vf{} native {:08X} {:08X} {:08X} {:08X} interp {:08X} {:08X} {:08X} {:08X};", reg,
                                       n[0], n[1], n[2], n[3], i[0], i[1], i[2], i[3]);
        }
    }
    for (int reg = 1; reg < 16; ++reg)
    {
        if (static_cast<std::uint16_t>(context.vi[reg]) != static_cast<std::uint16_t>(interpreted.vi[reg]))
            differences += std::format(" vi{} native {:04X} interp {:04X};", reg, static_cast<std::uint16_t>(context.vi[reg]),
                                       static_cast<std::uint16_t>(interpreted.vi[reg]));
    }
    for (std::uint32_t q = 0; q < PS2_VU0_DATA_SIZE / 16; ++q)
    {
        if (std::memcmp(dataNative.data() + q * 16, data + q * 16, 16) != 0)
        {
            std::uint32_t n[4];
            std::uint32_t i[4];
            std::memcpy(n, dataNative.data() + q * 16, 16);
            std::memcpy(i, data + q * 16, 16);
            differences += std::format(" mem[{:03X}] native {:08X} {:08X} {:08X} {:08X} interp {:08X} {:08X} {:08X} {:08X};", q,
                                       n[0], n[1], n[2], n[3], i[0], i[1], i[2], i[3]);
        }
    }
    if (context.vu0_mac_flags != interpreted.vu0_mac_flags || context.vu0_status != interpreted.vu0_status ||
        context.vu0_clip_flags != interpreted.vu0_clip_flags)
        differences += std::format(" flags native mac {:04X} status {:03X} clip {:06X} interp mac {:04X} status {:03X} clip {:06X};",
                                   context.vu0_mac_flags, context.vu0_status, context.vu0_clip_flags,
                                   interpreted.vu0_mac_flags, interpreted.vu0_status, interpreted.vu0_clip_flags);
    std::memcpy(data, dataNative.data(), dataNative.size());
    if (!differences.empty() && reports++ < 200)
        log::warn("VU0 check at 0x{:04X}:{}", address, differences.substr(0, 1500));
    else if (differences.empty() && reports < 200)
    {
        static int matches = 0;
        if (matches++ < 5)
            log::info("VU0 check at 0x{:04X}: same", address);
    }
}

// Logs VU0 code the game uploads that tgr-vurecomp has not compiled, with
// where it sits in the ELF image the runtime loaded into RAM.
void reportUpload(PS2Runtime& runtime, std::uint32_t address, std::uint32_t bytes)
{
    const std::uint8_t* code = runtime.memory().getVU0Code() + address;
    const std::uint64_t hash = hw::vu::programHash(code, bytes);
    for (std::size_t n = 0; n < hw::vu::kProgramCount; ++n)
    {
        const hw::vu::ProgramInfo& program = hw::vu::kPrograms[n];
        if (program.unit == 0 && program.loadAddress == address && program.size == bytes && program.hash == hash)
            return;
    }
    static std::unordered_set<std::uint64_t> reported;
    if (!reported.insert(hash).second)
        return;
    const std::uint8_t* rdram = runtime.memory().getRDRAM();
    const std::uint8_t* elfStart = rdram + 0x100000;
    const std::uint8_t* elfEnd = rdram + 0x400000;
    const std::uint8_t* found = std::search(elfStart, elfEnd, code, code + bytes);
    if (found == elfEnd)
    {
        log::warn("VU0: uploaded {} bytes at 0x{:04X} that are not in the ELF; they cannot be compiled", bytes,
                  address);
        return;
    }
    log::warn("VU0: uploaded code is not compiled: ELF 0x{:08X}, {} bytes, loaded at 0x{:X}",
              static_cast<std::uint32_t>(found - rdram), bytes, address);
}

} // namespace

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

    // VU0 microprograms run as C++ compiled ahead of time by tgr-vurecomp,
    // whether the EE starts them (VCALLMS) or VIF0 does (MSCAL).
    PS2Runtime* runtime = runtime_.get();
    vu0Code_ = std::make_unique<hw::vu::CodeMemory>(0, runtime->memory().getVU0Code(), PS2_VU0_CODE_SIZE);
    hw::vu::CodeMemory* vu0Code = vu0Code_.get();
    runtime_->setVu0MicroprogramHandler([runtime, vu0Code](std::uint8_t*, R5900Context* context, std::uint32_t address) {
        static const bool check = environmentFlag("TGR_VU_CHECK");
        if (check)
            checkVu0(*runtime, *vu0Code, *context, address);
        else
            runVu0(*runtime, *vu0Code, *context, address);
    });
    vif0_ = std::make_unique<hw::Vif0>(
        runtime->memory().getVU0Code(), runtime->memory().getVU0Data(),
        [runtime](std::uint32_t address, std::uint32_t itop) {
            R5900Context* context = runtime->eeScheduler().currentContext();
            if (!context)
                context = &runtime->cpu();
            context->vu0_itop = itop;
            runtime->executeVU0Microprogram(runtime->memory().getRDRAM(), context, address);
            return context->vu0_pc;
        },
        [runtime, vu0Code](std::uint32_t address, std::uint32_t bytes) {
            vu0Code->invalidate();
            reportUpload(*runtime, address, bytes);
        });
    runtime_->memory().setVif0Device(vif0_.get());

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
