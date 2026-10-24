// SPDX-License-Identifier: MIT
#include "game/RecompiledCode.h"

#include "game/GameBuild.h"
#include "game/Pfs.h"
#include "game/Stream.h"
#include "hw/Ipu.h"
#include "hw/Vif.h"
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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <mutex>
#pragma comment(lib, "dbghelp.lib")
#endif

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

#ifdef _WIN32
// Debugging aid (TGR_WATCH_WRITE=0xADDR): makes the host page holding that EE
// address read-only and logs each new piece of code that writes the 16 bytes
// there, with the generated source line (whose comment names the guest
// instruction) and the value written.
struct WriteWatch
{
    std::uint8_t* target = nullptr;
    std::uint8_t* page = nullptr;
    std::uint32_t guestAddress = 0;
    std::mutex mutex;
    std::unordered_set<std::uintptr_t> writers;
};
WriteWatch g_watch;
thread_local std::uintptr_t t_watchWriter = 0;

void protectWatchPage()
{
    DWORD old;
    VirtualProtect(g_watch.page, 4096, PAGE_READONLY, &old);
}

LONG CALLBACK watchHandler(EXCEPTION_POINTERS* info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->ExceptionInformation[0] == 1)
    {
        auto* address = reinterpret_cast<std::uint8_t*>(info->ExceptionRecord->ExceptionInformation[1]);
        if (address < g_watch.page || address >= g_watch.page + 4096)
            return EXCEPTION_CONTINUE_SEARCH;
        DWORD old;
        VirtualProtect(g_watch.page, 4096, PAGE_READWRITE, &old);
        t_watchWriter = 0;
        if (address + 16 > g_watch.target && address < g_watch.target + 16)
        {
            // The store is usually in a memory helper; walk out to the
            // generated function that called it.
            std::lock_guard lock(g_watch.mutex);
            CONTEXT context = *info->ContextRecord;
            STACKFRAME64 frame = {};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            for (int depth = 0; depth < 6; ++depth)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context,
                                 nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                    break;
                alignas(8) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 displacement = 0;
                if (SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &displacement, symbol) &&
                    std::strncmp(symbol->Name, "Ps2", 3) != 0 && std::strstr(symbol->Name, "write") == nullptr &&
                    std::strstr(symbol->Name, "Write") == nullptr)
                {
                    t_watchWriter = static_cast<std::uintptr_t>(frame.AddrPC.Offset);
                    break;
                }
            }
        }
        info->ContextRecord->EFlags |= 0x100; // single-step the store, then protect again
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && g_watch.page)
    {
        protectWatchPage();
        if (const std::uintptr_t rip = t_watchWriter)
        {
            t_watchWriter = 0;
            std::lock_guard lock(g_watch.mutex);
            if (g_watch.writers.size() < 64 && g_watch.writers.insert(rip).second)
            {
                alignas(8) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 displacement = 0;
                const bool named = SymFromAddr(GetCurrentProcess(), rip, &displacement, symbol);
                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisplacement = 0;
                const bool lined = SymGetLineFromAddr64(GetCurrentProcess(), rip, &lineDisplacement, &line);
                float value[4];
                std::memcpy(value, g_watch.target, 16);
                log::warn("watch [{:08X}] written by {}+0x{:X} ({}:{}): {} {} {} {}", g_watch.guestAddress,
                          named ? symbol->Name : "?", displacement, lined ? line.FileName : "?",
                          lined ? line.LineNumber : 0, value[0], value[1], value[2], value[3]);
            }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void startWriteWatch(PS2Runtime& runtime)
{
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "TGR_WATCH_WRITE") != 0 || value == nullptr)
        return;
    g_watch.guestAddress = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0)) & ~15u;
    std::free(value);
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    g_watch.target = runtime.memory().getRDRAM() + g_watch.guestAddress;
    g_watch.page = reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(g_watch.target) & ~std::uintptr_t{4095});
    AddVectoredExceptionHandler(1, watchHandler);
    protectWatchPage();
    log::info("Watching writes to EE 0x{:08X}", g_watch.guestAddress);
}
#endif

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

// For VU code tgr-vurecomp did not compile (not in .vutext, or changed after
// upload): finds the run of code memory around `address` that matches the
// ELF and logs where it comes from.
void reportMissing(PS2Runtime& runtime, int unit, std::uint32_t address)
{
    static std::unordered_set<std::uint32_t> reported;
    if (!reported.insert(static_cast<std::uint32_t>(unit) << 16 | address).second)
        return;
    const std::uint8_t* code = unit == 0 ? runtime.memory().getVU0Code() : runtime.memory().getVU1Code();
    const std::uint32_t codeSize = unit == 0 ? PS2_VU0_CODE_SIZE : PS2_VU1_CODE_SIZE;
    const std::uint8_t* rdram = runtime.memory().getRDRAM();
    const std::uint8_t* elfStart = rdram + 0x100000;
    const std::uint8_t* elfEnd = rdram + 0x400000;
    const std::uint32_t probe = std::min<std::uint32_t>(16, codeSize - address);
    const std::uint8_t* found = std::search(elfStart, elfEnd, code + address, code + address + probe);
    if (found == elfEnd)
    {
        log::warn("VU{}: code at 0x{:04X} is not in the ELF; it cannot be compiled", unit, address);
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
    while (end + 8 <= codeSize && std::memcmp(code + end, found + (end - address), 8) == 0)
        end += 8;
    log::warn("VU{}: code at 0x{:04X} is not compiled: ELF 0x{:08X}, {} bytes, loaded at 0x{:X}", unit, address,
              static_cast<std::uint32_t>(elf - rdram), end - start, start);
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
        reportMissing(runtime, 0, s.pc);

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

// Debugging aid (TGR_VU_CHECK=1): runs each VU1 microprogram both compiled
// and on PS2Recomp's VU interpreter from the same state, keeps the compiled
// result and logs where registers or VU1 data memory disagree. (The
// interpreter's XGKICKs reach the GS too, so the picture is not meaningful.)
void checkVu1(PS2Runtime& runtime, hw::vu::CodeMemory& code, hw::vu::State& s, std::uint32_t address)
{
    std::uint8_t* data = runtime.memory().getVU1Data();
    const std::vector<std::uint8_t> dataBefore(data, data + PS2_VU1_DATA_SIZE);
    const hw::vu::State before = s;

    if (!code.run(s, address))
        reportMissing(runtime, 1, s.pc);
    const std::vector<std::uint8_t> dataNative(data, data + PS2_VU1_DATA_SIZE);

    std::memcpy(data, dataBefore.data(), dataBefore.size());
    VU1Interpreter& interpreter = runtime.vu1();
    VU1State& is = interpreter.state();
    for (int reg = 0; reg < 32; ++reg)
        std::memcpy(is.vf[reg], before.vf[reg], 16);
    for (int reg = 0; reg < 16; ++reg)
        is.vi[reg] = static_cast<std::int16_t>(before.vi[reg]);
    std::memcpy(is.acc, before.acc, 16);
    std::memcpy(&is.q, &before.q, 4);
    std::memcpy(&is.p, &before.p, 4);
    std::memcpy(&is.i, &before.i, 4);
    is.r = before.r;
    is.mac = before.mac;
    is.status = before.status;
    is.clip = before.clip;
    interpreter.execute(runtime.memory().getVU1Code(), PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, runtime.gs(),
                        &runtime.memory(), address, before.top, before.itop, 1u << 22);

    static int reports = 0;
    std::string differences;
    for (int reg = 1; reg < 32; ++reg)
    {
        if (std::memcmp(s.vf[reg], is.vf[reg], 16) != 0)
        {
            std::uint32_t i[4];
            std::memcpy(i, is.vf[reg], 16);
            differences += std::format(" vf{} native {:08X} {:08X} {:08X} {:08X} interp {:08X} {:08X} {:08X} {:08X};", reg,
                                       s.vf[reg][0], s.vf[reg][1], s.vf[reg][2], s.vf[reg][3], i[0], i[1], i[2], i[3]);
        }
    }
    for (int reg = 1; reg < 16; ++reg)
    {
        if (s.vi[reg] != static_cast<std::uint16_t>(is.vi[reg]))
            differences += std::format(" vi{} native {:04X} interp {:04X};", reg, s.vi[reg],
                                       static_cast<std::uint16_t>(is.vi[reg]));
    }
    int memoryDifferences = 0;
    for (std::uint32_t q = 0; q < PS2_VU1_DATA_SIZE / 16; ++q)
    {
        if (std::memcmp(dataNative.data() + q * 16, data + q * 16, 16) != 0 && memoryDifferences++ < 8)
        {
            std::uint32_t n[4];
            std::uint32_t i[4];
            std::memcpy(n, dataNative.data() + q * 16, 16);
            std::memcpy(i, data + q * 16, 16);
            differences += std::format(" mem[{:03X}] native {:08X} {:08X} {:08X} {:08X} interp {:08X} {:08X} {:08X} {:08X};", q,
                                       n[0], n[1], n[2], n[3], i[0], i[1], i[2], i[3]);
        }
    }
    if (memoryDifferences > 8)
        differences += std::format(" ({} quadwords differ);", memoryDifferences);
    std::memcpy(data, dataNative.data(), dataNative.size());
    if (!differences.empty() && reports++ < 100)
        log::warn("VU1 check at 0x{:04X} top {:03X}:{}", address, before.top, differences.substr(0, 2000));
    else if (differences.empty())
    {
        static int matches = 0;
        if (matches++ < 5)
            log::info("VU1 check at 0x{:04X}: same", address);
    }
}

// XGKICK: sends the GS packet at a VU1 data address (quadwords) down PATH1,
// up to its last GIFtag (EOP). The packet is copied at once; the game builds
// the next one in the other half of the double buffer.
void xgkick(hw::vu::State& s, std::uint32_t address)
{
    auto& runtime = *static_cast<PS2Runtime*>(s.user);
    const std::uint32_t quadwords = s.dataQuadwordMask + 1;
    std::vector<std::uint8_t> packet;
    std::uint32_t at = address & s.dataQuadwordMask;
    for (std::uint32_t guard = 0; guard < quadwords; ++guard)
    {
        std::uint64_t tagLow;
        std::memcpy(&tagLow, s.data + at * 16, 8);
        const std::uint64_t loops = tagLow & 0x7FFF;
        const std::uint64_t format = (tagLow >> 58) & 0x3;
        std::uint64_t registers = (tagLow >> 60) & 0xF;
        if (registers == 0)
            registers = 16;
        const std::uint64_t dataQuadwords = format == 0   ? loops * registers
                                            : format == 1 ? (loops * registers + 1) / 2
                                                          : loops;
        for (std::uint64_t q = 0; q <= dataQuadwords && q < quadwords; ++q)
        {
            const std::uint8_t* source = s.data + ((at + q) & s.dataQuadwordMask) * 16;
            packet.insert(packet.end(), source, source + 16);
        }
        at = static_cast<std::uint32_t>((at + 1 + dataQuadwords) & s.dataQuadwordMask);
        if (tagLow & 0x8000) // EOP
            break;
    }
    runtime.memory().submitGifPacket(GifPathId::Path1, packet.data(), static_cast<std::uint32_t>(packet.size()));
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

#ifdef _WIN32
    startWriteWatch(*runtime_);
#endif

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
    hw::Vif::Host vif0Host;
    vif0Host.runMicroprogram = [runtime](std::uint32_t address, std::uint32_t, std::uint32_t itop) {
        R5900Context* context = runtime->eeScheduler().currentContext();
        if (!context)
            context = &runtime->cpu();
        context->vu0_itop = itop;
        runtime->executeVU0Microprogram(runtime->memory().getRDRAM(), context, address);
        return context->vu0_pc;
    };
    vif0Host.codeUploaded = [vu0Code](std::uint32_t, std::uint32_t) { vu0Code->invalidate(); };
    vif0_ = std::make_unique<hw::Vif>(0, runtime->memory().getVU0Code(), runtime->memory().getVU0Data(),
                                      std::move(vif0Host));
    runtime_->memory().setVif0Device(vif0_.get());

    // VU1 runs compiled microprograms too. Its registers are its own and stay
    // between programs; XGKICK sends GS packets down PATH1.
    vu1Code_ = std::make_unique<hw::vu::CodeMemory>(1, runtime->memory().getVU1Code(), PS2_VU1_CODE_SIZE);
    vu1_ = std::make_unique<hw::vu::State>();
    vu1_->vf[0][3] = 0x3F800000u;
    vu1_->data = runtime->memory().getVU1Data();
    vu1_->dataQuadwordMask = PS2_VU1_DATA_SIZE / 16 - 1;
    vu1_->user = runtime;
    vu1_->xgkick = &xgkick;
    hw::vu::CodeMemory* vu1Code = vu1Code_.get();
    hw::vu::State* vu1 = vu1_.get();
    hw::Vif::Host vif1Host;
    vif1Host.runMicroprogram = [runtime, vu1Code, vu1](std::uint32_t address, std::uint32_t top, std::uint32_t itop) {
        static const bool check = environmentFlag("TGR_VU_CHECK");
        vu1->top = top;
        vu1->itop = itop;
        vu1->budget = 1u << 24;
        if (check)
            checkVu1(*runtime, *vu1Code, *vu1, address);
        else if (!vu1Code->run(*vu1, address))
            reportMissing(*runtime, 1, vu1->pc);
        return vu1->pc;
    };
    vif1Host.codeUploaded = [vu1Code](std::uint32_t, std::uint32_t) { vu1Code->invalidate(); };
    vif1Host.sendPath2 = [runtime](const std::uint8_t* data, std::uint32_t bytes, bool directHl) {
        runtime->memory().submitGifPacket(GifPathId::Path2, data, bytes, true, directHl);
    };
    vif1Host.maskPath3 = [runtime](bool masked) { runtime->memory().setPath3Masked(masked); };
    vif1_ = std::make_unique<hw::Vif>(1, runtime->memory().getVU1Code(), runtime->memory().getVU1Data(),
                                      std::move(vif1Host));
    runtime_->memory().setVif1Device(vif1_.get());

    if (gsToolOptions_.dumpPath || gsToolOptions_.inventoryPath)
        gsTools_ = std::make_unique<GsTools>(runtime_->gs(), runtime_->memory().gs(), runtime_->memory().getGSVRAM(),
                                             gsToolOptions_);

    runtime_->startGuest();
    started_ = true;
    return true;
}

void Guest::toggleGsDump()
{
    if (gsTools_)
        gsTools_->toggleDump();
    else
        log::warn("GS dump: start the game with --gs-dump <folder> to record dumps with F9");
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

std::uint64_t Guest::vsyncCount() const
{
    return runtime_ ? runtime_->memory().gs().vsyncTick.load(std::memory_order_acquire) : 0;
}

std::string_view Guest::rendererName() const
{
    // PS2Recomp's CPU GS backend, until the native Vulkan GS renderer replaces it.
    return "Software";
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
    if (gsTools_)
        gsTools_->finish();
    if (runtime_)
        runtime_->memory().setIpuDevice(nullptr);
}

} // namespace tgr::game
