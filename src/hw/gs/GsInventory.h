// SPDX-License-Identifier: MIT
#pragma once

#include "hw/gs/GsSink.h"
#include "hw/gs/GsState.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The GS inventory: counts what the game asks of the GS (primitive types,
// formats, blending, tests, textures read from render targets, transfers,
// display modes...) so the renderer only has to cover what is measured.
// report() writes it as plain text.
namespace tgr::hw::gs
{

class GsInventory final : public GsSink, private GsState::Listener
{
public:
    GsInventory();

    void gifPacket(const std::uint8_t* data, std::uint32_t bytes) override;
    void registerWrite(std::uint8_t address, std::uint64_t value) override;
    void imageUpload(const ImageUploadRegs& regs, const std::uint8_t* data, std::uint32_t bytes) override;
    void clearFrame(std::uint32_t context, std::uint32_t rgba) override;
    void localToHost(std::uint32_t bytes) override;
    void vsync(std::uint64_t tick, const Privileged& privileged) override;

    // Takes over the register state a GS dump starts from (no counting).
    void restore(const std::vector<std::pair<std::uint8_t, std::uint64_t>>& registers);

    std::string report() const;

    std::uint64_t draws() const { return totalDraws_; }
    std::uint64_t frames() const { return frames_.size(); }

private:
    using Counts = std::map<std::uint64_t, std::uint64_t>;

    struct FrameStats
    {
        std::uint64_t draws = 0;
        std::uint64_t vertices = 0;
        std::uint64_t gifPackets = 0;
        std::uint64_t gifBytes = 0;
        std::uint64_t uploads = 0;
        std::uint64_t uploadBytes = 0;
        std::uint64_t clutLoads = 0;
        std::uint64_t targetChanges = 0;
        std::uint64_t stateChanges = 0; // draws whose state key differs from the previous draw's
        std::uint64_t textures = 0;     // different TEX0 (base, format) sampled
    };

    // A buffer the game has drawn into, by its first 256-byte block.
    struct Target
    {
        std::uint32_t width = 0; // FBW
        std::uint32_t psm = 0;
        std::uint32_t height = 0; // highest scissor bottom seen + 1
        bool depth = false;
        std::uint64_t drawn = 0;    // draw count at the last draw into it
        std::uint64_t uploaded = 0; // draw count at the last upload into it
        std::uint32_t blocks() const;
    };

    void gifTag(const GifTag& tag) override;
    void registerWrite(const GsState& state, std::uint8_t address, std::uint64_t value) override;
    void draw(const GsState& state, PrimType type, std::uint32_t vertices) override;
    void transfer(const GsState& state, std::uint32_t direction) override;
    void transferData(const GsState& state, std::uint32_t bytes, bool expected) override;

    void noteTarget(std::uint32_t block, const Target& target);
    const std::pair<const std::uint32_t, Target>* findTarget(std::uint32_t block) const;
    void endFrame();

    GsState state_;
    bool direct_ = false; // the write in progress did not come through the GIF

    std::uint64_t totalDraws_ = 0;
    std::uint64_t gifPackets_ = 0;
    std::uint64_t gifBytes_ = 0;
    std::uint64_t vsyncs_ = 0;
    std::uint64_t vsyncsWithoutDraws_ = 0;
    FrameStats frame_;
    std::vector<FrameStats> frames_;
    std::set<std::uint64_t> frameTextures_;
    std::uint64_t lastStateKey_ = ~0ull;
    std::uint64_t lastFrame_ = ~0ull;

    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> gifTags_; // (format | nreg << 2, regs)
    Counts registerWrites_;
    Counts directWrites_;
    Counts prims_;        // type | attribute bits << 3
    Counts vertices_;     // per type
    Counts states_;       // packed draw state (pipeline estimate)
    Counts blends_;       // ALPHA A B C D (+ FIX) for blended draws
    Counts blendFlags_;   // PABE, COLCLAMP, FBA, DTHE combinations for blended draws
    Counts alphaTests_;   // TEST alpha part
    Counts depth_;        // ZTE ZTST ZMSK ZPSM
    Counts dates_;        // DATE/DATM
    Counts frameBuffers_; // FRAME FBP FBW PSM
    Counts fbmsks_;       // FRAME.FBMSK
    Counts textures_;     // TEX0 PSM TW TH TFX TCC CPSM CSM
    Counts texFilters_;   // TEX1 MXL MMAG MMIN LCM MTBA
    Counts clamps_;       // CLAMP WMS WMT
    Counts clutLoads_;    // CLD CPSM CSM PSM
    Counts fogs_;         // fogged draws by type
    Counts misc_;         // single counters, keyed by an enum in the .cpp
    Counts targetReads_;  // textures read from a render target
    Counts uploads_;      // host->local by DPSM
    Counts uploadSizes_;  // RRW << 16 | RRH
    Counts localCopies_;  // SPSM << 8 | DPSM
    Counts readbacks_;    // local->host by SPSM
    std::map<std::string, std::uint64_t> displays_;
    std::map<std::uint32_t, Target> targets_;
    std::map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>>
        targetReadExample_; // key -> (target, TBP0)
    std::uint64_t readbackBytes_ = 0;
};

} // namespace tgr::hw::gs
