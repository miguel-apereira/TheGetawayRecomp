// SPDX-License-Identifier: MIT
#pragma once

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
// does, and reports every voice as idle, so sounds finish as soon as they
// start. Audio output comes later.
namespace tgr::game
{

class Stream
{
public:
    static constexpr std::uint32_t kRpcId = 0x12345;

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
    std::int32_t registerSample(const std::string& name, int mode);
    std::int32_t addSample(std::string name, SampleKind kind);
    std::int32_t transferResidentBank(std::uint32_t index) const;
    void writeStatus(std::uint8_t* reply, std::uint32_t size) const;

    struct Sample
    {
        std::string name; // upper case, as the IRX compares them
        std::int32_t id = 0;
    };
    std::vector<Sample> samples_;
    std::int32_t spots_ = 0;
    std::int32_t streams_ = 0;
    std::int32_t residentStatus_ = 0;
    std::int32_t lastRegisteredId_ = 0;
    std::uint32_t streamMemoryEnd_ = 0;
};

} // namespace tgr::game
