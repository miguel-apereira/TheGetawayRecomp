// SPDX-License-Identifier: MIT
//
// Tests for our GS frontend pieces on hand-written GIF packets (no game data).
//
//   tgr-gstests                       runs the tests
//   tgr-gstests --write-dump <file>   writes a small synthetic GS dump for tgr-gsreplay

#include "hw/gs/GifParser.h"
#include "hw/gs/GsDump.h"
#include "hw/gs/GsInventory.h"
#include "hw/gs/GsState.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace tgr::hw::gs;

namespace
{

int g_failures = 0;

#define CHECK(condition)                                                                                     \
    do                                                                                                       \
    {                                                                                                        \
        if (!(condition))                                                                                    \
        {                                                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);               \
            ++g_failures;                                                                                    \
        }                                                                                                    \
    } while (0)

// Builds GIF packets quadword by quadword.
class Packet
{
public:
    // A GIFtag. `regs` lists register descriptors, first one first.
    Packet& tag(std::uint32_t nloop, GifFormat format, std::vector<std::uint8_t> regs, bool eop = true,
                std::optional<std::uint32_t> prim = std::nullopt)
    {
        std::uint64_t lo = (nloop & 0x7FFF) | (std::uint64_t(eop) << 15) | (std::uint64_t(format) << 58) |
                           (std::uint64_t(regs.size() & 0xF) << 60);
        if (prim)
            lo |= (1ull << 46) | (std::uint64_t(*prim & 0x7FF) << 47);
        std::uint64_t hi = 0;
        for (std::size_t i = 0; i < regs.size(); ++i)
            hi |= std::uint64_t(regs[i] & 0xF) << (i * 4);
        return quad(lo, hi);
    }
    Packet& quad(std::uint64_t lo, std::uint64_t hi)
    {
        words_.push_back(lo);
        words_.push_back(hi);
        return *this;
    }
    Packet& ad(std::uint8_t address, std::uint64_t value) { return quad(value, address); }
    Packet& word(std::uint64_t value)
    {
        words_.push_back(value);
        return *this;
    }
    const std::uint8_t* data() const { return reinterpret_cast<const std::uint8_t*>(words_.data()); }
    std::uint32_t size() const { return static_cast<std::uint32_t>(words_.size() * 8); }

private:
    std::vector<std::uint64_t> words_;
};

struct Recorder final : GifVisitor
{
    std::vector<std::pair<std::uint8_t, std::uint64_t>> writes;
    std::uint32_t imageBytes = 0;
    std::uint32_t tags = 0;
    void tag(const GifTag&) override { ++tags; }
    void registerWrite(std::uint8_t address, std::uint64_t value) override
    {
        writes.emplace_back(address, value);
    }
    void imageData(const std::uint8_t*, std::uint32_t bytes) override { imageBytes += bytes; }
};

constexpr std::uint64_t xyz(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
    return (x << 4) | (std::uint64_t(y << 4) << 16) | (std::uint64_t(z) << 32);
}

void testPackedConversion()
{
    Packet p;
    p.tag(1, GifFormat::Packed, {0x2, 0x1, 0x4, 0x5, 0x5, 0x3, 0xA, 0xE, 0xF}, true, 0x13);
    p.quad(0x3F0000003E800000ull, 0x40000000ull);         // ST: S 0.25, T 0.5, Q 2.0
    p.quad(0x0000002000000010ull, 0x0000004000000030ull); // RGBAQ 10 20 30 40
    p.quad((100ull << 4) | ((200ull << 4) << 32), (0x123456ull << 4) | (0x7Full << 36)); // XYZF2 kick
    p.quad(1 | (2ull << 32), 77ull | (1ull << 47)); // XYZ2 with ADC: no kick
    p.quad(3 | (4ull << 32), 88ull);                // XYZ2 kick
    p.quad(0x2ABC | (0x1DEFull << 32), 0);          // UV
    p.quad(0, 0x55ull << 36);                       // FOG
    p.quad(0xDEADBEEFull, reg::FRAME_1);            // A+D
    p.quad(0, 0);                                   // NOP
    Recorder r;
    CHECK(parseGifPacket(p.data(), p.size(), r) == p.size());
    CHECK(r.tags == 1);
    CHECK(r.writes.size() == 9);
    if (r.writes.size() != 9)
        return;
    CHECK(r.writes[0] == std::make_pair(reg::PRIM, std::uint64_t(0x13))); // PRE
    CHECK(r.writes[1].first == reg::ST && r.writes[1].second == 0x3F0000003E800000ull);
    CHECK(r.writes[2].first == reg::RGBAQ && r.writes[2].second == (0x40302010ull | (0x40000000ull << 32)));
    CHECK(r.writes[3].first == reg::XYZF2 &&
          r.writes[3].second == (1600ull | (3200ull << 16) | (0x123456ull << 32) | (0x7Full << 56)));
    CHECK(r.writes[4].first == reg::XYZ3 && r.writes[4].second == (1ull | (2ull << 16) | (77ull << 32)));
    CHECK(r.writes[5].first == reg::XYZ2 && r.writes[5].second == (3ull | (4ull << 16) | (88ull << 32)));
    CHECK(r.writes[6].first == reg::UV && r.writes[6].second == (0x2ABCull | (0x1DEFull << 16)));
    CHECK(r.writes[7].first == reg::FOG && r.writes[7].second == (0x55ull << 56));
    CHECK(r.writes[8].first == reg::FRAME_1 && r.writes[8].second == 0xDEADBEEFull);
}

void testQResetsPerTag()
{
    Packet p;
    p.tag(1, GifFormat::Packed, {0x2}, false).quad(0, 0x40000000ull);
    p.tag(1, GifFormat::Packed, {0x1}).quad(0, 0);
    Recorder r;
    parseGifPacket(p.data(), p.size(), r);
    CHECK(r.writes.size() == 2 && (r.writes[1].second >> 32) == 0x3F800000ull);
}

void testReglistAndImage()
{
    Packet p;
    p.tag(1, GifFormat::Reglist, {0x0, 0x1, 0x5}, false); // three words: padded to two quadwords
    p.word(6).word(0x80808080).word(xyz(1, 2, 3)).word(0xFFFFFFFF);
    p.tag(2, GifFormat::Image, {});
    p.quad(1, 2).quad(3, 4);
    Recorder r;
    CHECK(parseGifPacket(p.data(), p.size(), r) == p.size());
    CHECK(r.writes.size() == 3);
    CHECK(r.writes.size() == 3 && r.writes[2].first == reg::XYZ2);
    CHECK(r.imageBytes == 32);
}

struct DrawCounter final : GsState::Listener
{
    std::vector<PrimType> draws;
    std::vector<std::uint32_t> transfers;
    void draw(const GsState&, PrimType type, std::uint32_t) override { draws.push_back(type); }
    void transfer(const GsState&, std::uint32_t direction) override { transfers.push_back(direction); }
};

void testKicks()
{
    GsState state;
    DrawCounter counter;
    state.setListener(&counter);
    state.write(reg::PRIM, 4); // triangle strip: 5 vertices make 3 triangles
    for (int i = 0; i < 5; ++i)
        state.write(reg::XYZ2, xyz(i, i, 0));
    CHECK(counter.draws.size() == 3);
    state.write(reg::PRIM, 6); // sprite; XYZ3 adds a vertex without drawing
    state.write(reg::XYZ2, 0);
    state.write(reg::XYZ3, 0);
    CHECK(counter.draws.size() == 3);
    state.write(reg::XYZ2, 0);
    state.write(reg::XYZ2, 0);
    CHECK(counter.draws.size() == 4 && counter.draws.back() == PrimType::Sprite);
    state.write(reg::PRIM, 3); // PRIM empties the queue
    state.write(reg::XYZ2, 0);
    state.write(reg::XYZ2, 0);
    state.write(reg::PRIM, 3);
    state.write(reg::XYZ2, 0);
    CHECK(counter.draws.size() == 4);

    // PRMODE supplies the attributes when PRMODECONT.AC is 0.
    state.write(reg::PRMODECONT, 0);
    state.write(reg::PRMODE, 1 << 4);
    state.write(reg::PRIM, 6 | (1 << 6));
    CHECK(state.prim().tme() && !state.prim().abe() && state.prim().type() == PrimType::Sprite);
}

void testTransferSizes()
{
    GsState state;
    DrawCounter counter;
    state.setListener(&counter);
    state.write(reg::BITBLTBUF, std::uint64_t(psm::CT24) << 56);
    state.write(reg::TRXREG, 10 | (4ull << 32));
    state.write(reg::TRXDIR, 0);
    CHECK(state.transferBytesLeft() == 10 * 4 * 3);
    state.write(reg::BITBLTBUF, std::uint64_t(psm::T4) << 56);
    state.write(reg::TRXDIR, 0);
    CHECK(state.transferBytesLeft() == 20);
    std::uint8_t data[16] = {};
    state.image(data, 16);
    CHECK(state.transferBytesLeft() == 4);
    state.write(reg::TRXDIR, 1);
    state.write(reg::TRXDIR, 3);
    CHECK(counter.transfers.size() == 3 && counter.transfers[2] == 1);
}

const Privileged kDisplay = []
{
    Privileged p;
    p.pmode = 0x1 | (0xFFull << 8);                               // circuit 1, ALP 0xFF
    p.smode2 = 1;                                                 // interlaced
    p.dispfb1 = 0 | (10ull << 9);                                 // FBP 0, FBW 10, CT32
    p.display1 = (2559ull << 32) | (447ull << 44) | (3ull << 23); // 640x448: 2560 clocks, MAGH 3
    return p;
}();

// A 64x64 CT32 target at page 0x10 drawn with a sprite, then sampled as a texture.
Packet renderToTextureFrame()
{
    Packet p;
    p.tag(10, GifFormat::Packed, {0xE});
    p.ad(reg::FRAME_1, 0x10 | (1ull << 16));
    p.ad(reg::SCISSOR_1, 63ull << 16 | (63ull << 48));
    p.ad(reg::TEST_1, 1ull << 16 | (1ull << 17)); // depth test ALWAYS
    p.ad(reg::ZBUF_1, 0x100 | (1ull << 32));      // Z32, no depth writes
    p.ad(reg::PRIM, 6);
    p.ad(reg::RGBAQ, 0x80FF0000);
    p.ad(reg::XYZ2, xyz(0, 0, 0));
    p.ad(reg::XYZ2, xyz(64, 64, 0));
    p.ad(reg::FRAME_1, 0 | (10ull << 16));
    p.ad(reg::SCISSOR_1, 639ull << 16 | (447ull << 48));
    p.tag(7, GifFormat::Packed, {0xE});
    p.ad(reg::TEX0_1, (0x10ull * 32) | (1ull << 14) | (6ull << 26) | (6ull << 30) | (1ull << 34));
    p.ad(reg::ALPHA_1, 0x44);
    p.ad(reg::PRIM, 6 | (1 << 4) | (1 << 6) | (1 << 8));
    p.ad(reg::UV, 0);
    p.ad(reg::XYZ2, xyz(100, 100, 0));
    p.ad(reg::UV, (64 << 4) | ((64ull << 4) << 16));
    p.ad(reg::XYZ2, xyz(164, 164, 0));
    return p;
}

void testInventory()
{
    GsInventory inventory;
    const Packet p = renderToTextureFrame();
    inventory.gifPacket(p.data(), p.size());
    inventory.vsync(1, kDisplay);
    inventory.vsync(2, kDisplay);
    CHECK(inventory.draws() == 2);
    CHECK(inventory.frames() == 1);
    const std::string report = inventory.report();
    const auto has = [&](std::string_view text)
    {
        const bool found = report.find(text) != std::string::npos;
        if (!found)
            std::fprintf(stderr, "report lacks \"%.*s\"\n", static_cast<int>(text.size()), text.data());
        return found;
    };
    CHECK(has("colour CT32 read as CT32, at its base"));
    CHECK(has("(Cs - Cd) * As + Cd"));
    CHECK(has("sprite TME ABE FST"));
    CHECK(has("SMODE2 INT=1"));
    CHECK(has("DISPLAY1 640x448"));
    CHECK(has("Z32 ZMSK"));
}

std::filesystem::path tempPath(const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

void writeSyntheticDump(const std::filesystem::path& path, std::vector<std::uint8_t>& memory)
{
    GsDumpWriter writer(memory.data());
    // State written before the recording starts must come back from its start record.
    Packet setup;
    setup.tag(2, GifFormat::Packed, {0xE})
        .ad(reg::FRAME_1, 0 | (10ull << 16))
        .ad(reg::SCISSOR_1, 639ull << 16 | (447ull << 48));
    writer.gifPacket(setup.data(), setup.size());
    writer.vsync(1, kDisplay);
    writer.arm(path, 0, 2);
    writer.vsync(2, kDisplay); // armed: the next GS event starts the dump
    const Packet frame = renderToTextureFrame();
    writer.gifPacket(frame.data(), frame.size());
    writer.vsync(3, kDisplay);
    writer.clearFrame(0, 0xFF000000u);
    writer.registerWrite(reg::TEST_1, 1ull << 16 | (1ull << 17));
    writer.gifPacket(frame.data(), frame.size());
    writer.vsync(4, kDisplay); // second vsync: the dump closes
    writer.gifPacket(frame.data(), frame.size());
}

void testDumpRoundTrip()
{
    std::vector<std::uint8_t> memory(kLocalMemoryBytes, 0);
    memory[12345] = 0x5A;
    const std::filesystem::path path = tempPath("tgr-gstests.tgrgs");
    writeSyntheticDump(path, memory);

    GsDumpReader reader;
    GsDumpStart start;
    std::string error;
    CHECK(reader.open(path, error));
    CHECK(reader.readStart(start, error));
    CHECK(start.tick == 2);
    CHECK(start.localMemory.size() == kLocalMemoryBytes && start.localMemory[12345] == 0x5A);
    bool hasFrame = false;
    for (const auto& [address, value] : start.registers)
        hasFrame |= address == reg::FRAME_1 && value == (10ull << 16);
    CHECK(hasFrame);
    CHECK(start.privileged.smode2 == 1);

    struct Counter final : GsSink
    {
        int gif = 0, regs = 0, clears = 0, vsyncs = 0;
        void gifPacket(const std::uint8_t*, std::uint32_t) override { ++gif; }
        void registerWrite(std::uint8_t, std::uint64_t) override { ++regs; }
        void imageUpload(const ImageUploadRegs&, const std::uint8_t*, std::uint32_t) override {}
        void clearFrame(std::uint32_t, std::uint32_t) override { ++clears; }
        void localToHost(std::uint32_t) override {}
        void vsync(std::uint64_t, const Privileged&) override { ++vsyncs; }
    } counter;
    while (reader.next(counter, error))
    {
    }
    CHECK(error.empty());
    CHECK(counter.gif == 2 && counter.regs == 1 && counter.clears == 1 && counter.vsyncs == 2);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--write-dump")
    {
        std::vector<std::uint8_t> memory(kLocalMemoryBytes, 0);
        writeSyntheticDump(argv[2], memory);
        return std::filesystem::exists(argv[2]) ? 0 : 1;
    }

    testPackedConversion();
    testQResetsPerTag();
    testReglistAndImage();
    testKicks();
    testTransferSizes();
    testInventory();
    testDumpRoundTrip();
    if (g_failures)
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        std::printf("all GS tests passed\n");
    return g_failures ? 1 : 0;
}
