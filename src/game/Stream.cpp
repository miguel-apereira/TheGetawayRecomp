// SPDX-License-Identifier: MIT
#include "game/Stream.h"

#include "app/Log.h"
#include "game/Pfs.h"

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
    StartStream = 1,
    StopStream = 5,
    TransferResidentBank = 6,
    InitStreams = 0x0c,
    SetMaxStreams = 0x10,
    StartMemoryStream = 0x47,
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
    runtime.addHostVBlankHandler([this](std::uint8_t*) {
        ++vblank_;
        for (Playing& stream : slots_)
        {
            if (stream.active && stream.endVBlank != 0 && vblank_ >= stream.endVBlank)
                stream.active = false;
        }
    });
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
        case StartStream:
            startStream(arg(1), static_cast<std::int16_t>(arg(0)), false);
            break;
        case StartMemoryStream:
            startStream(arg(1), static_cast<std::int16_t>(arg(0)), true);
            break;
        case StopStream:
            stopStream(arg(0));
            break;
        case InitStreams:
            for (Playing& stream : slots_)
                stream.active = false;
            break;
        case SetMaxStreams:
            if (arg(0) <= slots_.size())
                maxStreams_ = arg(0);
            break;
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

// USR_StartStream / USR_StartIOPStream: slot in the high byte (0x80: the
// first free one), SPU channel in the low byte.
void Stream::startStream(std::uint32_t slotAndChannel, std::int32_t fileId, bool fromMemory)
{
    const std::uint32_t channel = slotAndChannel & 0xFF;
    stopStream(channel);
    std::uint32_t slot = (slotAndChannel >> 8) & 0xFF;
    if (slot == 0x80)
    {
        slot = 0;
        while (slot < maxStreams_ && slots_[slot].active)
            ++slot;
    }
    if (slot >= maxStreams_)
        return;

    Playing& stream = slots_[slot];
    stream = Playing{};
    stream.active = true;
    stream.channel = channel;
    stream.fileId = fileId;

    // Loops play until stopped; other streams for the length of their file.
    double seconds = 10.0; // memory streams: no file to measure
    if (!fromMemory)
    {
        seconds = 0;
        for (const Sample& sample : samples_)
        {
            if (sample.id == fileId && sample.name.find("LP.") == std::string::npos)
                seconds = pfs_.iopSoundSeconds(sample.name);
        }
    }
    stream.endVBlank = seconds > 0 ? vblank_ + static_cast<std::uint64_t>(seconds * 60.0) + 1 : 0;
    log::debug("[STREAM] slot {} channel {} plays sample {:#x} for {:.1f} s", slot, channel, fileId, seconds);
}

// StopStream: a channel below 0x40, or 0x40 + a slot.
void Stream::stopStream(std::uint32_t which)
{
    for (std::uint32_t slot = 0; slot < slots_.size(); ++slot)
    {
        Playing& stream = slots_[slot];
        if (stream.active && (which < 0x40 ? stream.channel == which : (which & 0x3F) == slot))
            stream.active = false;
    }
}

// The reply STREAM.IRX's AdpcmGetStatus builds: three words per active
// stream, then the resident bank status, load and busy flags, the stream
// memory end, the stereo flag, 48 two-bit voice states (all idle), the
// external load registers and the last registered sample id.
void Stream::writeStatus(std::uint8_t* reply, std::uint32_t size)
{
    std::vector<std::int32_t> status(1, 0);
    for (std::uint32_t slot = 0; slot < maxStreams_ && slot < slots_.size(); ++slot)
    {
        const Playing& stream = slots_[slot];
        if (!stream.active)
            continue;
        // Channel, slot and stage; then the file id, with bit 31 once the
        // stream is playing (the IRX's states 0 and 7).
        status.push_back(static_cast<std::int32_t>(stream.channel << 10 | slot << 4));
        status.push_back(0);
        status.push_back(static_cast<std::int32_t>((static_cast<std::uint32_t>(stream.fileId) & 0xFFFF) |
                                                   (stream.reported ? 0x80000000u : 0u)));
        ++status[0];
    }
    const std::int32_t tail[14] = {residentStatus_, 0, 0, static_cast<std::int32_t>(streamMemoryEnd_),
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, lastRegisteredId_};
    status.insert(status.end(), std::begin(tail), std::end(tail));

    std::memset(reply, 0, size);
    std::memcpy(reply, status.data(), std::min<std::size_t>(size, status.size() * sizeof(std::int32_t)));

    // A stream shows as starting in the first status after its start command.
    for (Playing& stream : slots_)
        stream.reported = stream.active;
}

} // namespace tgr::game
