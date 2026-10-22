// SPDX-License-Identifier: MIT
#include "game/Stream.h"

#include "app/Log.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace tgr::game
{

namespace
{

constexpr std::uint32_t kRamSize = 32 * 1024 * 1024;

// Commands in the batch (STREAM.IRX's AdpcmSetParam).
enum Command : std::uint16_t
{
    TransferResidentBank = 6,
    SetStreamMemoryEnd = 0x0e,
    RegisterSpot = 0x3c,
    RegisterStream = 0x3d,
    RegisterIopStream = 0x3e,
    RegisterOther = 0x41,
    ResetSamples = 0x52,
};

std::uint8_t* eeMemory(std::uint8_t* rdram, std::uint32_t address, std::uint32_t size)
{
    const std::uint32_t physical = address & 0x1FFFFFFF;
    if (physical > kRamSize || size > kRamSize - physical)
        return nullptr;
    return rdram + physical;
}

std::uint16_t readU16(const std::uint8_t* p)
{
    std::uint16_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

} // namespace

void Stream::install(PS2Runtime& runtime)
{
    runtime.registerHostIopModule("STREAM.IRX");
    runtime.registerHostRpcServer(kRpcId, [this](std::uint8_t* rdram, const PS2Runtime::HostRpcCall& call) {
        // Function 0 carries a command batch; every call answers with the status.
        if (call.function == 0 && call.sendSize)
        {
            if (const std::uint8_t* data = eeMemory(rdram, call.send, call.sendSize))
                runCommands(data, call.sendSize);
        }
        if (call.receiveSize)
        {
            if (std::uint8_t* reply = eeMemory(rdram, call.receive, call.receiveSize))
                writeStatus(reply, call.receiveSize);
        }
    });
}

// The batch is 16-bit words: a command count, then per command its id, its
// argument count and the arguments.
void Stream::runCommands(const std::uint8_t* data, std::uint32_t size)
{
    const std::uint32_t words = size / 2;
    std::uint32_t at = 0;
    const auto next = [&]() -> std::uint16_t { return at < words ? readU16(data + 2 * at++) : 0; };

    const int count = static_cast<std::int16_t>(next());
    for (int i = 0; i < count && at < words; ++i)
    {
        const std::uint16_t command = next();
        const int argCount = static_cast<std::int16_t>(next());
        std::vector<std::uint16_t> args;
        for (int a = 0; a < argCount && at < words; ++a)
            args.push_back(next());
        const auto arg = [&](std::size_t n) -> std::uint32_t { return n < args.size() ? args[n] : 0; };

        switch (command)
        {
        case TransferResidentBank:
            residentStatus_ = transferResidentBank(arg(0));
            break;
        case SetStreamMemoryEnd:
            streamMemoryEnd_ = arg(0) << 16 | arg(1);
            break;
        case RegisterSpot:
        case RegisterStream:
        case RegisterOther:
        {
            // The arguments are the sample's file name, two characters per word.
            std::string name;
            for (std::uint16_t word : args)
            {
                name.push_back(char(word & 0xff));
                name.push_back(char(word >> 8));
            }
            name.resize(std::strlen(name.c_str()));
            const int mode = command == RegisterSpot ? 0 : command == RegisterStream ? 1 : 2;
            lastRegisteredId_ = registerSample(name, mode);
            break;
        }
        case ResetSamples:
            // Keeps the first spots + streams registrations and drops the rest.
            spots_ = static_cast<std::int32_t>(arg(0));
            streams_ = static_cast<std::int32_t>(arg(1));
            if (samples_.size() > arg(0) + arg(1))
                samples_.resize(arg(0) + arg(1));
            break;
        case RegisterIopStream:
            // A stream the IOP plays from memory the EE loaded; it has no name.
            lastRegisteredId_ = addSample(std::string(), SampleKind::OnceOff);
            break;
        default:
            break;
        }
    }
}

// mode 0: a spot effect; 1 and 2: a stream, looping or not depending on the
// name ("LP." marks loops). Registering a name again returns its first id.
std::int32_t Stream::registerSample(const std::string& name, int mode)
{
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const Sample& sample : samples_)
    {
        if (sample.name == upper)
            return sample.id;
    }

    if (name.find(".gpk") != std::string::npos)
        return addSample(upper, SampleKind::OnceOff);
    if (mode == 0)
        return addSample(upper, SampleKind::Spot);
    const bool loopName = name.find("LP.") != std::string::npos;
    if (!loopName)
        return addSample(upper, mode == 1 ? SampleKind::OnceOff : SampleKind::Loop);
    if (mode == 1)
        return addSample(upper, SampleKind::Loop);
    return -1; // a looping name registered as mode 2: the IRX registers nothing
}

std::int32_t Stream::addSample(std::string name, SampleKind kind)
{
    const std::int32_t id = kind == SampleKind::Spot ? 0x1000 + spots_++ : streams_++;
    log::debug("[STREAM] sample {:#x} = {}", id, name);
    samples_.push_back(Sample{std::move(name), id});
    return id;
}

// Loads the index-th resident bank (spot effect) into SPU memory: 0 when it
// exists, 1 past the last one, 2 when the table is full.
std::int32_t Stream::transferResidentBank(std::uint32_t index) const
{
    constexpr std::size_t kMaxFiles = 0x1a4;
    std::uint32_t spot = 0;
    for (std::size_t i = 0; i < samples_.size() && i < kMaxFiles; ++i)
    {
        if (samples_[i].id < 0x1000)
            continue;
        if (spot++ == index)
            return 0;
    }
    return samples_.size() < kMaxFiles ? 1 : 2;
}

// The reply STREAM.IRX's AdpcmGetStatus builds: the active streams (none
// here), then the resident bank status, load and busy flags, the stream
// memory end, the stereo flag, 48 two-bit voice states (all idle), the
// external load registers and the last registered sample id.
void Stream::writeStatus(std::uint8_t* reply, std::uint32_t size) const
{
    std::int32_t status[15] = {};
    status[1] = residentStatus_;
    status[4] = static_cast<std::int32_t>(streamMemoryEnd_);
    status[14] = lastRegisteredId_;
    std::memset(reply, 0, size);
    std::memcpy(reply, status, std::min<std::size_t>(size, sizeof(status)));
}

} // namespace tgr::game
