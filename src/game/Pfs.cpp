// SPDX-License-Identifier: MIT
#include "game/Pfs.h"

#include "app/Log.h"
#include "disc/GameData.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace tgr::game
{

namespace
{

constexpr std::uint32_t kSectorSize = 2048;
constexpr std::uint32_t kMaxRequests = 64; // the IRX's request ring
constexpr std::uint32_t kRamSize = 32 * 1024 * 1024;

// RPC functions the EE calls on PFS (PFS_RpcServer).
enum Command : std::uint32_t
{
    QueueRead = 0,
    ClearQueue = 1,
    SetMedia = 2,
    LoadIopIndex = 3,
    LoadEeIndex = 4,
    DeleteIopIndex = 5,
    SetEeStream = 7,
    LoadIopStreamSlot = 8,
    PreloadIopXmlPack = 9,
    DropIopXmlPack = 10,
};

// RPC functions PFS calls on the game's EE server.
enum EeCallback : std::uint32_t
{
    ReadDone = 0,
    StreamSlotLoaded = 9, // data: slot, or slot + 100 when it failed
    XmlPackLoaded = 10,
    Ready = 11,
};

// IOP memory PFS reserves for sound data.
constexpr std::uint32_t kStreamSlotBytes = 0x5b800;
constexpr std::uint32_t kXmlPackBytes = 0xb7000;

// Layout of a queued read (PFS keeps 0x70 bytes of it).
constexpr std::uint32_t kRequestBytes = 0x70;
constexpr std::uint32_t kRequestPriority = 0x5f;
constexpr std::uint32_t kRequestStatus = 0x60;
constexpr std::uint32_t kRequestSector = 0x64;
constexpr std::uint32_t kRequestSize = 0x68;
constexpr std::uint32_t kRequestDestination = 0x6c;

// Status slot (0x80 bytes in EE memory): the last size % 64 bytes of the
// data, then their count and a result.
constexpr std::uint32_t kStatusTailCount = 0x40;
constexpr std::uint32_t kStatusResult = 0x44;

std::uint8_t* eeMemory(std::uint8_t* rdram, std::uint32_t address, std::uint32_t size)
{
    const std::uint32_t physical = address & 0x1FFFFFFF;
    if (physical > kRamSize || size > kRamSize - physical)
        return nullptr;
    return rdram + physical;
}

std::uint32_t readU32(const std::uint8_t* p)
{
    std::uint32_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

void writeU32(std::uint8_t* p, std::uint32_t value)
{
    std::memcpy(p, &value, sizeof(value));
}

std::string readString(const std::uint8_t* p, std::size_t max)
{
    const auto* end = static_cast<const std::uint8_t*>(std::memchr(p, 0, max));
    return std::string(reinterpret_cast<const char*>(p), end ? end - p : max);
}

// "cdrom0:\PACK_EE.PAK;1" -> "PACK_EE.PAK"
std::string discName(std::string_view name)
{
    for (std::string_view device : {"cdrom0:", "cdrom:", "host0:", "host:"})
    {
        if (name.size() >= device.size() &&
            std::equal(device.begin(), device.end(), name.begin(),
                       [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
        {
            name.remove_prefix(device.size());
            break;
        }
    }
    while (!name.empty() && (name.front() == '\\' || name.front() == '/'))
        name.remove_prefix(1);
    if (const std::size_t version = name.find(';'); version != std::string_view::npos)
        name = name.substr(0, version);

    std::string result(name);
    std::replace(result.begin(), result.end(), '\\', '/');
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

} // namespace

Pfs::Pfs(fs::path gameDataDir) : dataDir_(std::move(gameDataDir))
{
    loadLayout();
}

void Pfs::install(PS2Runtime& runtime)
{
    runtime_ = &runtime;
    runtime.registerHostIopModule("PFS.IRX");
    runtime.registerHostRpcServer(kRpcId, [this](std::uint8_t* rdram, const PS2Runtime::HostRpcCall& call) {
        serve(rdram, call.function, call.send, call.sendSize);
    });
    runtime.addHostVBlankHandler([this](std::uint8_t* rdram) { onVBlank(rdram); });
}

void Pfs::serve(std::uint8_t* rdram, std::uint32_t function, std::uint32_t send, std::uint32_t sendSize)
{
    // Reads queued by earlier calls finish now rather than at the next vblank:
    // the game streams movies with more reads per frame than it has status
    // slots (64), and stalls when they run out. (A read can't finish inside
    // its own call: the EE marks it in flight only after the call returns.)
    finishQueuedReads(rdram);

    const std::uint8_t* data = sendSize ? eeMemory(rdram, send, sendSize) : nullptr;
    switch (function)
    {
    case QueueRead:
        if (data && sendSize >= kRequestBytes)
            queueRead(rdram, send);
        break;
    case ClearQueue:
        requests_.clear();
        break;
    case SetMedia:
        if (data && sendSize >= 8)
        {
            iopIsCd_ = readU32(data) != 0;
            eeIsCd_ = readU32(data + 4) != 0;
            log::info("[PFS] EE reads from {}, IOP from {}", eeIsCd_ ? "disc" : "host", iopIsCd_ ? "disc" : "host");
        }
        break;
    case LoadEeIndex:
        if (data)
            openEePack(readString(data, sendSize));
        break;
    case LoadIopIndex:
        if (data)
            openIopPack(readString(data, sendSize));
        break;
    case DeleteIopIndex:
        break;
    case SetEeStream:
        if (data && sendSize >= 8)
        {
            eeStreamAddress_ = readU32(data);
            eeStreamSize_ = readU32(data + 4);
        }
        break;
    case LoadIopStreamSlot:
        if (data && sendSize > 4)
            loadStreamSlot(readU32(data), readString(data + 4, sendSize - 4));
        break;
    case PreloadIopXmlPack:
        if (data)
            loadXmlPack(readString(data, sendSize));
        break;
    case DropIopXmlPack:
        xmlPack_.clear();
        break;
    default:
        log::warn("[PFS] unknown command {}", function);
        break;
    }
}

void Pfs::queueRead(std::uint8_t* rdram, std::uint32_t send)
{
    const std::uint8_t* p = eeMemory(rdram, send, kRequestBytes);
    Request request;
    request.name = readString(p, kRequestPriority);
    request.priority = p[kRequestPriority];
    request.statusAddress = readU32(p + kRequestStatus);
    request.sector = static_cast<std::int32_t>(readU32(p + kRequestSector));
    request.size = readU32(p + kRequestSize);
    request.destination = readU32(p + kRequestDestination);
    if (eeIsPak_ && request.sector > 0)
        request.sector += static_cast<std::int32_t>(eeBaseSector_);

    if (requests_.size() >= kMaxRequests)
    {
        // The IRX answers a full ring with result -2 in the status slot.
        if (std::uint8_t* status = eeMemory(rdram, request.statusAddress, 0x80))
            writeU32(status + kStatusResult, static_cast<std::uint32_t>(-2));
        return;
    }
    requests_.push_back(std::move(request));
}

void Pfs::onVBlank(std::uint8_t* rdram)
{
    if (!announced_)
    {
        // PFS_MainThread's first act: tell the EE the IOP side is up. The EE
        // checks the three words and waits for this before going on.
        std::uint32_t ready[16] = {0x6a, 0x6b, 0x6c};
        announced_ = runtime_->callEeRpcServer(kRpcId, Ready, ready, sizeof(ready));
        if (announced_)
            log::info("[PFS] ready");
        return;
    }

    finishQueuedReads(rdram);
    while (!eeCalls_.empty())
    {
        const EeCall& call = eeCalls_.front();
        runtime_->callEeRpcServer(kRpcId, call.function, call.hasData ? call.data.data() : nullptr,
                                  call.hasData ? sizeof(call.data) : 0);
        eeCalls_.pop_front();
    }
}

void Pfs::openIopPack(const std::string& name)
{
    const DiscFile* file = fileNamed(name);
    std::string error;
    iopPackPath_ = file ? dataDir_ / file->path : fs::path();
    iopPack_ = file ? disc::PakArchive::openFile(iopPackPath_, error) : nullptr;
    if (!iopPack_)
        log::error("[PFS] cannot open IOP pack {}: {}", name, file ? error : "not on the disc");
    else
        log::info("[PFS] IOP pack {}: {} files", file->path, iopPack_->entries().size());
}

// IOP files are looked up by name in PACK_IOP.PAK, like PFS_Search does.
const disc::PakEntry* Pfs::findIopFile(const std::string& name) const
{
    if (!iopPack_)
        return nullptr;
    std::string wanted = discName(name);
    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const disc::PakEntry& entry : iopPack_->entries())
    {
        if (entry.path.size() < wanted.size() ||
            !std::equal(wanted.rbegin(), wanted.rend(), entry.path.rbegin(),
                        [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); }))
            continue;
        // A whole path component has to match, not just the end of a name.
        if (entry.path.size() > wanted.size() && entry.path[entry.path.size() - wanted.size() - 1] != '/')
            continue;
        return &entry;
    }
    return nullptr;
}

bool Pfs::readIopFile(const std::string& name, std::vector<std::uint8_t>& data)
{
    const disc::PakEntry* entry = findIopFile(name);
    if (!entry)
        return false;
    std::string error;
    auto contents = iopPack_->read(*entry, error);
    if (!contents)
    {
        log::error("[PFS] cannot read {}: {}", entry->path, error);
        return false;
    }
    data = std::move(*contents);
    return true;
}

double Pfs::iopSoundSeconds(const std::string& name)
{
    const disc::PakEntry* entry = findIopFile(name);
    if (!entry)
        return 0;
    std::vector<std::uint8_t> header;
    if (entry->compressed)
    {
        if (!readIopFile(name, header))
            return 0;
    }
    else if (!readFile(iopPackPath_, std::uint64_t{entry->sector} * kSectorSize, 0x30, header))
    {
        return 0;
    }
    // VAG header: "VAGp", then big-endian data size at 0x0C and sample rate at 0x10.
    const auto be32 = [&](std::size_t at) {
        return std::uint32_t{header[at]} << 24 | std::uint32_t{header[at + 1]} << 16 |
               std::uint32_t{header[at + 2]} << 8 | header[at + 3];
    };
    if (header.size() < 0x30 || std::memcmp(header.data(), "VAGp", 4) != 0)
        return 0;
    const std::uint32_t bytes = be32(0x0C);
    const std::uint32_t rate = be32(0x10);
    return rate ? (bytes / 16.0) * 28.0 / rate : 0;
}

void Pfs::loadStreamSlot(std::uint32_t slot, const std::string& name)
{
    if (slot >= streamSlots_.size())
        return;
    xmlPack_.clear();
    EeCall call{StreamSlotLoaded};
    call.hasData = true;
    std::vector<std::uint8_t> data;
    if (readIopFile(name, data) && data.size() <= kStreamSlotBytes)
    {
        streamSlots_[slot] = std::move(data);
        call.data[0] = slot;
    }
    else
    {
        log::error("[PFS] cannot load sound slot {} from {}", slot, name);
        streamSlots_[slot].clear();
        call.data[0] = slot + 100;
    }
    eeCalls_.push_back(call);
}

void Pfs::loadXmlPack(const std::string& name)
{
    for (auto& slot : streamSlots_)
        slot.clear();
    xmlPack_.clear();
    std::vector<std::uint8_t> data;
    if (!readIopFile(name, data) || data.size() > kXmlPackBytes)
    {
        // The IRX gives up quietly here too, and the EE keeps waiting.
        log::error("[PFS] cannot preload XML sound pack {}", name);
        return;
    }
    xmlPack_ = std::move(data);
    log::info("[PFS] XML sound pack {} ({} bytes)", name, xmlPack_.size());
    eeCalls_.push_back(EeCall{XmlPackLoaded});
}

void Pfs::finishQueuedReads(std::uint8_t* rdram)
{
    while (!requests_.empty())
    {
        const Request request = std::move(requests_.front());
        requests_.pop_front();
        finishRead(rdram, request);
    }
}

void Pfs::openEePack(const std::string& name)
{
    const DiscFile* file = fileNamed(name);
    eePack_ = file ? dataDir_ / file->path : dataDir_ / discName(name);

    std::vector<std::uint8_t> header;
    if (!readFile(eePack_, 0, 0x198, header))
    {
        log::error("[PFS] cannot open EE pack {}", name);
        return;
    }
    eeIsPak_ = header[6] == 'P';
    eeLocation_ = readString(header.data() + 0x24, 0x40);
    eeNetLocation_ = readString(header.data() + 0x64, 0x134);
    eeBaseSector_ = eeIsCd_ && file ? file->sector : 0;
    log::info("[PFS] EE pack {}{} at sector {}", eePack_.filename().string(), eeIsPak_ ? "" : " (not a PAK)",
              eeBaseSector_);
}

void Pfs::finishRead(std::uint8_t* rdram, const Request& request)
{
    log::debug("[PFS] read '{}' sector {} size {} to 0x{:08X}", request.name, request.sector, request.size,
               request.destination);
    std::vector<std::uint8_t> data;
    if (!readRequest(request, data))
    {
        log::error("[PFS] read failed: '{}' sector {} size {}", request.name, request.sector, request.size);
        data.assign(request.size, 0);
    }

    const std::uint32_t body = request.size & ~63u;
    const std::uint32_t tail = request.size & 63u;
    if (body)
    {
        if (std::uint8_t* destination = eeMemory(rdram, request.destination, body))
            std::memcpy(destination, data.data(), body);
        else
            log::error("[PFS] bad destination 0x{:08X} for '{}'", request.destination, request.name);
    }

    // The EE copies the tail into place itself, from the status slot.
    if (std::uint8_t* status = eeMemory(rdram, request.statusAddress, 0x80))
    {
        std::memcpy(status, data.data() + body, tail);
        writeU32(status + kStatusTailCount, tail);
        writeU32(status + kStatusResult, 0);
    }
    runtime_->callEeRpcServer(kRpcId, ReadDone, nullptr, 0);
}

bool Pfs::readRequest(const Request& request, std::vector<std::uint8_t>& data)
{
    if (eeIsCd_)
    {
        if (request.sector > 0)
        {
            const DiscFile* file = fileAtSector(static_cast<std::uint32_t>(request.sector));
            if (!file)
                return false;
            const std::uint64_t offset = std::uint64_t(std::uint32_t(request.sector) - file->sector) * kSectorSize;
            return readFile(dataDir_ / file->path, offset, request.size, data);
        }
        const DiscFile* file = fileNamed(request.name);
        return file && readFile(dataDir_ / file->path, 0, request.size, data);
    }

    if (eeIsPak_ && request.sector > 0)
        return readFile(eePack_, std::uint64_t(request.sector) * kSectorSize, request.size, data);

    // Host reads try the pack's two directories, then the bare name.
    const std::uint64_t offset = request.sector > 0 ? std::uint64_t(request.sector) * kSectorSize : 0;
    for (const std::string& prefix : {eeLocation_, eeNetLocation_, std::string()})
    {
        const fs::path path = dataDir_ / discName(prefix + request.name);
        if (fs::exists(path))
            return readFile(path, offset, request.size, data);
    }
    const DiscFile* file = fileNamed(request.name);
    return file && readFile(dataDir_ / file->path, offset, request.size, data);
}

bool Pfs::readFile(const fs::path& path, std::uint64_t offset, std::uint32_t size, std::vector<std::uint8_t>& data)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    data.assign(size, 0);
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(data.data()), size);
    return static_cast<std::uint32_t>(in.gcount()) == size;
}

void Pfs::loadLayout()
{
    std::ifstream in(dataDir_ / disc::kLayoutFileName);
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        DiscFile file;
        fields >> file.sector >> file.size;
        fields.ignore(1);
        std::getline(fields, file.path);
        if (!file.path.empty())
            layout_.push_back(std::move(file));
    }
    if (layout_.empty())
        log::error("[PFS] no {} in {}", disc::kLayoutFileName, dataDir_.string());
}

const Pfs::DiscFile* Pfs::fileAtSector(std::uint32_t sector) const
{
    for (const DiscFile& file : layout_)
    {
        const std::uint64_t sectors = (file.size + kSectorSize - 1) / kSectorSize;
        if (sector >= file.sector && sector < file.sector + sectors)
            return &file;
    }
    return nullptr;
}

const Pfs::DiscFile* Pfs::fileNamed(std::string_view name) const
{
    const std::string wanted = discName(name);
    for (const DiscFile& file : layout_)
    {
        if (discName(file.path) == wanted)
            return &file;
    }
    return nullptr;
}

} // namespace tgr::game
