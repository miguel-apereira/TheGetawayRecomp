// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

// Native replacement for STREAM.IRX, The Getaway's IOP sound engine (sound
// effects, streamed music and speech, on the SPU2 through libsd). The game
// batches commands into one SIF RPC call to server 0x12345 per frame and gets
// back the state of every stream, voice and sample load.
//
// For now it is silent: it keeps the bookkeeping the game relies on (sample
// registration ids, resident bank loads, memory settings) the way the IRX
// does, plays streams for their real length (the game waits on them, as for
// the intro music before the intro movie), and reports every voice as idle,
// so sound effects finish as soon as they start. Audio output comes later.
namespace tgr::game
{

class Pfs;

class Stream
{
public:
    static constexpr std::uint32_t kRpcId = 0x12345;

    // Stream lengths come from the sound files PFS finds in PACK_IOP.PAK.
    explicit Stream(Pfs& pfs) : pfs_(pfs) {}

    void install(PS2Runtime& runtime);

private:
    // How a sample is played (STREAM.IRX's F3RegisterSample).
    enum class SampleKind
    {
        Spot,       // resident in SPU memory, id 0x1000 + n
        OnceOff,    // streamed once, id n
        Loop,       // streamed in a loop, id n
    };

    void runCommands(const std::uint8_t* data, std::uint32_t size);
    void startStream(std::uint32_t slotAndChannel, std::int32_t fileId, bool fromMemory);
    void stopStream(std::uint32_t which);
    std::int32_t registerSample(const std::string& name, int mode);
    std::int32_t addSample(std::string name, SampleKind kind);
    std::int32_t transferResidentBank(std::uint32_t index) const;
    void writeStatus(std::uint8_t* reply, std::uint32_t size);

    struct Sample
    {
        std::string name; // upper case, as the IRX compares them
        std::int32_t id = 0;
    };
    std::vector<Sample> samples_;

    // A stream slot (UserMaxStreams of them, at most 4). Silent: it only
    // counts down its playing time in vertical blanks.
    struct Playing
    {
        bool active = false;
        std::uint32_t channel = 0;
        std::int32_t fileId = 0;
        std::uint64_t endVBlank = 0; // 0: until stopped (loops)
        bool reported = false;       // the first status shows it still starting
    };
    std::array<Playing, 4> slots_{};
    std::uint32_t maxStreams_ = 0;
    std::uint64_t vblank_ = 0;
    Pfs& pfs_;
    std::int32_t spots_ = 0;
    std::int32_t streams_ = 0;
    std::int32_t residentStatus_ = 0;
    std::int32_t lastRegisteredId_ = 0;
    std::uint32_t streamMemoryEnd_ = 0;
};

} // namespace tgr::game
