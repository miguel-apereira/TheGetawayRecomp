// SPDX-License-Identifier: MIT
#include "hw/gs/GsDump.h"

#include "app/Log.h"

#include <cstring>

namespace tgr::hw::gs
{

namespace
{

constexpr char kMagic[8] = {'T', 'G', 'R', 'G', 'S', 'D', 'M', 'P'};
constexpr std::uint32_t kMaxPayload = 64u * 1024 * 1024;

bool actsWhenWritten(std::uint8_t address)
{
    switch (address)
    {
    case reg::XYZF2:
    case reg::XYZ2:
    case reg::XYZF3:
    case reg::XYZ3:
    case reg::TRXDIR:
    case reg::HWREG:
    case reg::SIGNAL:
    case reg::FINISH:
    case reg::LABEL:
    case reg::TEXFLUSH:
        return true;
    default:
        return false;
    }
}

template <class T> T take(const std::uint8_t*& p)
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}

} // namespace

std::vector<std::pair<std::uint8_t, std::uint64_t>> restorableRegisters(const GsState& state)
{
    std::vector<std::pair<std::uint8_t, std::uint64_t>> registers;
    registers.emplace_back(reg::PRMODECONT, state.value(reg::PRMODECONT));
    registers.emplace_back(reg::PRIM, state.value(reg::PRIM));
    for (std::uint32_t address = 0; address < reg::kCount; ++address)
    {
        const auto a = static_cast<std::uint8_t>(address);
        if (a != reg::PRMODECONT && a != reg::PRIM && state.written(a) && !actsWhenWritten(a))
            registers.emplace_back(a, state.value(a));
    }
    return registers;
}

GsDumpWriter::GsDumpWriter(const std::uint8_t* localMemory)
    : localMemory_(localMemory), buffer_(4u * 1024 * 1024)
{
}

GsDumpWriter::~GsDumpWriter()
{
    stop();
}

void GsDumpWriter::arm(std::filesystem::path path, std::uint64_t tick, std::uint64_t vsyncs)
{
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::Idle)
        return;
    path_ = std::move(path);
    startTick_ = tick;
    vsyncsLeft_ = vsyncs ? vsyncs : 1;
    phase_ = Phase::Armed;
    if (tick)
        log::info("GS dump: recording {} vsyncs from vsync {} into {}", vsyncsLeft_, tick, path_.string());
    else
        log::info("GS dump: recording {} vsyncs from the next vsync into {}", vsyncsLeft_, path_.string());
}

void GsDumpWriter::stop()
{
    std::lock_guard lock(mutex_);
    if (phase_ == Phase::Recording)
        close();
    phase_ = Phase::Idle;
}

bool GsDumpWriter::active() const
{
    std::lock_guard lock(mutex_);
    return phase_ != Phase::Idle;
}

bool GsDumpWriter::beginEvent()
{
    if (phase_ == Phase::Starting)
    {
        std::error_code ec;
        if (path_.has_parent_path())
            std::filesystem::create_directories(path_.parent_path(), ec);
        out_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        out_.open(path_, std::ios::binary | std::ios::trunc);
        if (!out_)
        {
            log::error("GS dump: cannot write {}", path_.string());
            phase_ = Phase::Idle;
            return false;
        }
        const std::uint32_t header[2] = {kGsDumpVersion, 0};
        out_.write(kMagic, sizeof(kMagic));
        out_.write(reinterpret_cast<const char*>(header), sizeof(header));
        bytes_ = sizeof(kMagic) + sizeof(header);

        std::vector<std::uint64_t> registers;
        for (const auto& [address, value] : restorableRegisters(shadow_))
        {
            registers.push_back(address);
            registers.push_back(value);
        }
        const std::uint32_t count = static_cast<std::uint32_t>(registers.size() / 2);
        const std::uint32_t memoryBytes = localMemory_ ? kLocalMemoryBytes : 0;
        record(GsDumpRecord::Start, {{&startedAt_, 8},
                                     {&count, 4},
                                     {registers.data(), registers.size() * 8},
                                     {&privileged_, sizeof(privileged_)},
                                     {&memoryBytes, 4},
                                     {localMemory_, memoryBytes}});
        phase_ = Phase::Recording;
        log::info("GS dump: started at vsync {}", startedAt_);
    }
    return phase_ == Phase::Recording;
}

void GsDumpWriter::record(GsDumpRecord kind, std::initializer_list<std::pair<const void*, std::size_t>> parts)
{
    std::size_t size = 0;
    for (const auto& part : parts)
        size += part.second;
    const std::uint32_t head[2] = {static_cast<std::uint32_t>(kind), static_cast<std::uint32_t>(size)};
    out_.write(reinterpret_cast<const char*>(head), sizeof(head));
    for (const auto& [data, bytes] : parts)
    {
        if (bytes)
            out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    }
    bytes_ += sizeof(head) + size;
    if (!out_)
    {
        log::error("GS dump: writing {} failed (disk full?)", path_.string());
        close();
    }
}

void GsDumpWriter::close()
{
    out_.close();
    out_.clear();
    phase_ = Phase::Idle;
    log::info("GS dump: {} written ({:.1f} MiB, vsyncs {}-{})", path_.string(),
              static_cast<double>(bytes_) / (1024.0 * 1024.0), startedAt_, lastTick_);
}

void GsDumpWriter::gifPacket(const std::uint8_t* data, std::uint32_t bytes)
{
    std::lock_guard lock(mutex_);
    if (beginEvent())
        record(GsDumpRecord::Gif, {{data, bytes}});
    shadow_.gifPacket(data, bytes);
}

void GsDumpWriter::registerWrite(std::uint8_t address, std::uint64_t value)
{
    std::lock_guard lock(mutex_);
    if (beginEvent())
    {
        const std::uint64_t a = address;
        record(GsDumpRecord::Register, {{&a, 8}, {&value, 8}});
    }
    shadow_.write(address, value);
}

void GsDumpWriter::imageUpload(const ImageUploadRegs& regs, const std::uint8_t* data, std::uint32_t bytes)
{
    std::lock_guard lock(mutex_);
    if (beginEvent())
        record(GsDumpRecord::Upload, {{&regs, sizeof(regs)}, {data, bytes}});
    shadow_.write(reg::BITBLTBUF, regs.bitbltbuf);
    shadow_.write(reg::TRXPOS, regs.trxpos);
    shadow_.write(reg::TRXREG, regs.trxreg);
    shadow_.write(reg::TRXDIR, regs.trxdir);
    shadow_.image(data, bytes);
}

void GsDumpWriter::clearFrame(std::uint32_t context, std::uint32_t rgba)
{
    std::lock_guard lock(mutex_);
    if (beginEvent())
        record(GsDumpRecord::ClearFrame, {{&context, 4}, {&rgba, 4}});
}

void GsDumpWriter::localToHost(std::uint32_t bytes)
{
    std::lock_guard lock(mutex_);
    if (beginEvent())
        record(GsDumpRecord::LocalToHost, {{&bytes, 4}});
}

void GsDumpWriter::vsync(std::uint64_t tick, const Privileged& privileged)
{
    std::lock_guard lock(mutex_);
    privileged_ = privileged;
    lastTick_ = tick;
    switch (phase_)
    {
    case Phase::Armed:
        if (tick >= startTick_)
        {
            phase_ = Phase::Starting;
            startedAt_ = tick;
        }
        break;
    case Phase::Starting:
        startedAt_ = tick; // nothing reached the GS since: start from this vsync instead
        break;
    case Phase::Recording:
        record(GsDumpRecord::Vsync, {{&tick, 8}, {&privileged, sizeof(privileged)}});
        if (phase_ == Phase::Recording && --vsyncsLeft_ == 0)
            close();
        break;
    default:
        break;
    }
}

bool GsDumpReader::open(const std::filesystem::path& path, std::string& error)
{
    in_.open(path, std::ios::binary);
    if (!in_)
    {
        error = "cannot open " + path.string();
        return false;
    }
    char magic[8];
    std::uint32_t header[2];
    in_.read(magic, sizeof(magic));
    in_.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in_ || std::memcmp(magic, kMagic, sizeof(magic)) != 0)
    {
        error = path.string() + " is not a GS dump";
        return false;
    }
    if (header[0] != kGsDumpVersion)
    {
        error = std::format("{} is a version {} GS dump; this build reads version {}", path.string(),
                            header[0], kGsDumpVersion);
        return false;
    }
    return true;
}

bool GsDumpReader::readRecord(GsDumpRecord& kind, std::string& error)
{
    std::uint32_t head[2];
    in_.read(reinterpret_cast<char*>(head), sizeof(head));
    if (in_.gcount() == 0 && in_.eof())
        return false;
    if (!in_ || head[1] > kMaxPayload + kLocalMemoryBytes)
    {
        error = "damaged record header";
        return false;
    }
    kind = static_cast<GsDumpRecord>(head[0] & 0xFF);
    payload_.resize(head[1]);
    in_.read(reinterpret_cast<char*>(payload_.data()), head[1]);
    if (!in_)
    {
        error = "the dump ends in the middle of a record";
        return false;
    }
    return true;
}

bool GsDumpReader::readStart(GsDumpStart& start, std::string& error)
{
    GsDumpRecord kind;
    if (!readRecord(kind, error))
    {
        if (error.empty())
            error = "the dump is empty";
        return false;
    }
    if (kind != GsDumpRecord::Start || payload_.size() < 12)
    {
        error = "the dump does not begin with its start state";
        return false;
    }
    const std::uint8_t* p = payload_.data();
    const std::uint8_t* end = p + payload_.size();
    start.tick = take<std::uint64_t>(p);
    const std::uint32_t count = take<std::uint32_t>(p);
    if (static_cast<std::size_t>(end - p) < count * 16ull + sizeof(Privileged) + 4)
    {
        error = "damaged start record";
        return false;
    }
    start.registers.clear();
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto address = static_cast<std::uint8_t>(take<std::uint64_t>(p));
        start.registers.emplace_back(address, take<std::uint64_t>(p));
    }
    start.privileged = take<Privileged>(p);
    const std::uint32_t memoryBytes = take<std::uint32_t>(p);
    if (static_cast<std::size_t>(end - p) < memoryBytes)
    {
        error = "damaged start record";
        return false;
    }
    start.localMemory.assign(p, p + memoryBytes);
    return true;
}

bool GsDumpReader::next(GsSink& sink, std::string& error)
{
    GsDumpRecord kind;
    if (!readRecord(kind, error))
        return false;
    const std::uint8_t* p = payload_.data();
    const std::size_t size = payload_.size();
    const auto need = [&](std::size_t bytes)
    {
        if (size >= bytes)
            return true;
        error = std::format("record {} is too short", static_cast<int>(kind));
        return false;
    };
    switch (kind)
    {
    case GsDumpRecord::Gif:
        sink.gifPacket(p, static_cast<std::uint32_t>(size));
        break;
    case GsDumpRecord::Register:
    {
        if (!need(16))
            return false;
        const auto address = static_cast<std::uint8_t>(take<std::uint64_t>(p));
        sink.registerWrite(address, take<std::uint64_t>(p));
        break;
    }
    case GsDumpRecord::Upload:
    {
        if (!need(sizeof(ImageUploadRegs)))
            return false;
        const ImageUploadRegs regs = take<ImageUploadRegs>(p);
        sink.imageUpload(regs, p, static_cast<std::uint32_t>(size - sizeof(regs)));
        break;
    }
    case GsDumpRecord::ClearFrame:
    {
        if (!need(8))
            return false;
        const std::uint32_t context = take<std::uint32_t>(p);
        sink.clearFrame(context, take<std::uint32_t>(p));
        break;
    }
    case GsDumpRecord::LocalToHost:
        if (!need(4))
            return false;
        sink.localToHost(take<std::uint32_t>(p));
        break;
    case GsDumpRecord::Vsync:
    {
        if (!need(8 + sizeof(Privileged)))
            return false;
        const std::uint64_t tick = take<std::uint64_t>(p);
        sink.vsync(tick, take<Privileged>(p));
        break;
    }
    default:
        error = std::format("unknown record kind {}", static_cast<int>(kind));
        return false;
    }
    return true;
}

} // namespace tgr::hw::gs
