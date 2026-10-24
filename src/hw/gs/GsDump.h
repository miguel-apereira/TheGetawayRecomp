// SPDX-License-Identifier: MIT
#pragma once

#include "hw/gs/GsSink.h"
#include "hw/gs/GsState.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// GS dumps: everything that reached the GS over a run of frames, so the
// same input can be replayed through any renderer (tgr-gsreplay) and timed
// or compared. A dump starts with the GS state at its first frame (local
// memory and the register file), so it can begin anywhere in the game.
//
// Dumps hold game data (textures, the picture): keep them out of the repo.
//
// File layout (little-endian): "TGRGSDMP", u32 version, u32 0, then records
// of u8 kind, three zero bytes, u32 payload size and the payload.
namespace tgr::hw::gs
{

inline constexpr std::uint32_t kGsDumpVersion = 1;
inline constexpr std::uint32_t kLocalMemoryBytes = 4 * 1024 * 1024;

enum class GsDumpRecord : std::uint8_t
{
    Start = 1,       // u64 tick, u32 n, n x (u64 address, u64 value), Privileged, u32 bytes, local memory
    Gif = 2,         // the packet
    Register = 3,    // u64 address, u64 value
    Upload = 4,      // BITBLTBUF TRXPOS TRXREG TRXDIR (u64 each), then the data
    ClearFrame = 5,  // u32 context, u32 rgba
    LocalToHost = 6, // u32 bytes
    Vsync = 7,       // u64 tick, Privileged
};

// The GS state a dump starts from.
struct GsDumpStart
{
    std::uint64_t tick = 0;
    std::vector<std::pair<std::uint8_t, std::uint64_t>> registers; // in the order to write them
    Privileged privileged;
    std::vector<std::uint8_t> localMemory;
};

// Registers a dump restores at its start: every register the game wrote
// except the ones that act when written (vertex kicks, TRXDIR, SIGNAL,
// FINISH, LABEL, HWREG, TEXFLUSH). PRMODECONT and PRIM go first.
std::vector<std::pair<std::uint8_t, std::uint64_t>> restorableRegisters(const GsState& state);

// Records a dump while the game runs. Every GsSink call keeps a shadow of
// the GS registers; once armed, recording starts at the first GS event after
// the chosen vsync and stops after the chosen number of vsyncs. Calls may
// come from several threads; the GS calls must come with local memory
// stable (the runtime's GS host hook calls with the GS locked).
class GsDumpWriter final : public GsSink
{
public:
    explicit GsDumpWriter(const std::uint8_t* localMemory);
    ~GsDumpWriter() override;

    // Records into `path` from the first vsync numbered `tick` or later (0:
    // the next one), for `vsyncs` vsyncs. Ignored while a recording is armed
    // or running; after one ends, the next can be armed.
    void arm(std::filesystem::path path, std::uint64_t tick, std::uint64_t vsyncs);
    // Stops a recording now and closes its file.
    void stop();
    // Armed or recording.
    bool active() const;

    void gifPacket(const std::uint8_t* data, std::uint32_t bytes) override;
    void registerWrite(std::uint8_t address, std::uint64_t value) override;
    void imageUpload(const ImageUploadRegs& regs, const std::uint8_t* data, std::uint32_t bytes) override;
    void clearFrame(std::uint32_t context, std::uint32_t rgba) override;
    void localToHost(std::uint32_t bytes) override;
    void vsync(std::uint64_t tick, const Privileged& privileged) override;

private:
    enum class Phase
    {
        Idle,
        Armed,
        Starting, // the start vsync has passed; the next GS event writes the start
        Recording,
    };

    // Writes the start record if due; true while recording.
    bool beginEvent();
    void record(GsDumpRecord kind, std::initializer_list<std::pair<const void*, std::size_t>> parts);
    void close();

    const std::uint8_t* localMemory_;
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    GsState shadow_;
    Privileged privileged_;
    Phase phase_ = Phase::Idle;
    std::uint64_t startTick_ = 0;
    std::uint64_t startedAt_ = 0;
    std::uint64_t vsyncsLeft_ = 0;
    std::uint64_t lastTick_ = 0;
    std::uint64_t bytes_ = 0;
    std::ofstream out_;
    std::vector<char> buffer_;
};

// Reads a dump and hands its records to `sink`; `start` gets the start record.
class GsDumpReader
{
public:
    bool open(const std::filesystem::path& path, std::string& error);

    // Reads the start record into `start`. False if the dump has none first.
    bool readStart(GsDumpStart& start, std::string& error);

    // Hands the next record to `sink`; false at the end of the dump (or on a
    // damaged record, with `error` set).
    bool next(GsSink& sink, std::string& error);

private:
    bool readRecord(GsDumpRecord& kind, std::string& error);

    std::ifstream in_;
    std::vector<std::uint8_t> payload_;
};

} // namespace tgr::hw::gs
