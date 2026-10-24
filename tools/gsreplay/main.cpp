// SPDX-License-Identifier: MIT
//
// tgr-gsreplay: plays a GS dump (recorded by the game with --gs-dump) back
// through a GS renderer, headless. Today that is PS2Recomp's software GS,
// which makes the reference images and the baseline timings; the native
// Vulkan renderer will be measured on the same dumps.
//
//   tgr-gsreplay <dump.tgrgs> [--images <folder>] [--every-vsync] [--inventory <file>|-] [--no-render]

#include "app/Bmp.h"
#include "app/Log.h"
#include "hw/gs/GsDump.h"
#include "hw/gs/GsInventory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace tgr;
using namespace tgr::hw::gs;

namespace
{

struct Options
{
    fs::path dump;
    std::optional<fs::path> images;
    bool everyVsync = false;
    std::optional<std::string> inventory; // a file, or "-" for stdout
    bool render = true;
};

void setPrivileged(GSRegisters& r, const Privileged& p, std::uint64_t tick)
{
    r.pmode = p.pmode;
    r.smode1 = p.smode1;
    r.smode2 = p.smode2;
    r.srfsh = p.srfsh;
    r.synch1 = p.synch1;
    r.synch2 = p.synch2;
    r.syncv = p.syncv;
    r.dispfb1 = p.dispfb1;
    r.display1 = p.display1;
    r.dispfb2 = p.dispfb2;
    r.display2 = p.display2;
    r.extbuf = p.extbuf;
    r.extdata = p.extdata;
    r.extwrite = p.extwrite;
    r.bgcolor = p.bgcolor;
    r.csr.store(p.csr);
    r.vsyncTick.store(tick);
    r.imr = p.imr;
    r.busdir = p.busdir;
    r.siglblid = p.siglblid;
}

// Feeds the dump to PS2Recomp's software GS and saves what it displays.
class SoftwareReplay final : public GsSink
{
public:
    SoftwareReplay(const Options& options, const GsDumpStart& start)
        : options_(options), memory_(kLocalMemoryBytes), gs_(std::make_unique<GS>()),
          privileged_(std::make_unique<GSRegisters>())
    {
        setPrivileged(*privileged_, start.privileged, start.tick);
        gs_->init(memory_.data(), kLocalMemoryBytes, privileged_.get());
        // The dump's local memory, then its registers (TEX0 with CLD reloads the CLUT from it).
        std::memcpy(memory_.data(), start.localMemory.data(),
                    std::min<std::size_t>(start.localMemory.size(), kLocalMemoryBytes));
        for (const auto& [address, value] : start.registers)
            gs_->writeRegister(address, value);
    }

    void gifPacket(const std::uint8_t* data, std::uint32_t bytes) override
    {
        const auto begin = Clock::now();
        gs_->processGIFPacket(data, bytes);
        work(begin);
    }

    void registerWrite(std::uint8_t address, std::uint64_t value) override
    {
        const auto begin = Clock::now();
        gs_->writeRegister(address, value);
        work(begin);
    }

    void imageUpload(const ImageUploadRegs& r, const std::uint8_t* data, std::uint32_t bytes) override
    {
        const auto begin = Clock::now();
        gs_->uploadImageNative(r.bitbltbuf, r.trxpos, r.trxreg, r.trxdir, data, bytes);
        work(begin);
    }

    void clearFrame(std::uint32_t context, std::uint32_t rgba) override
    {
        const auto begin = Clock::now();
        gs_->clearFramebufferContext(context, rgba);
        work(begin);
    }

    void localToHost(std::uint32_t bytes) override
    {
        readback_.resize(bytes);
        gs_->consumeLocalToHostBytes(readback_.data(), bytes);
    }

    void vsync(std::uint64_t tick, const Privileged& privileged) override
    {
        ++vsyncs_;
        setPrivileged(*privileged_, privileged, tick);
        const bool newPicture = busy_;
        if (busy_)
        {
            ++frames_;
            busy_ = false;
        }
        if (options_.images && (newPicture || options_.everyVsync))
        {
            gs_->latchHostPresentationFrame();
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            if (gs_->copyLatchedHostPresentationFrame(picture_, width, height) && width && height)
            {
                std::error_code ec;
                fs::create_directories(*options_.images, ec);
                writeBmp(*options_.images / std::format("frame_{:05}_vsync_{}.bmp", frames_, tick), picture_,
                         width, height);
                ++images_;
            }
        }
    }

    std::string summary() const
    {
        const double ms = std::chrono::duration<double, std::milli>(busyTime_).count();
        return std::format(
            "software GS: {} vsyncs, {} frames with GS work, {:.1f} ms in the GS ({:.2f} ms per frame), "
            "{} images written",
            vsyncs_, frames_, ms, frames_ ? ms / static_cast<double>(frames_) : 0.0, images_);
    }

private:
    using Clock = std::chrono::steady_clock;

    void work(Clock::time_point begin)
    {
        busyTime_ += Clock::now() - begin;
        busy_ = true;
    }

    const Options& options_;
    std::vector<std::uint8_t> memory_;
    std::unique_ptr<GS> gs_;
    std::unique_ptr<GSRegisters> privileged_;
    std::vector<std::uint8_t> readback_;
    std::vector<std::uint8_t> picture_;
    Clock::duration busyTime_{};
    bool busy_ = false;
    std::uint64_t vsyncs_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t images_ = 0;
};

// Hands each record to several sinks.
class Tee final : public GsSink
{
public:
    std::vector<GsSink*> sinks;

    void gifPacket(const std::uint8_t* data, std::uint32_t bytes) override
    {
        for (GsSink* s : sinks)
            s->gifPacket(data, bytes);
    }
    void registerWrite(std::uint8_t address, std::uint64_t value) override
    {
        for (GsSink* s : sinks)
            s->registerWrite(address, value);
    }
    void imageUpload(const ImageUploadRegs& regs, const std::uint8_t* data, std::uint32_t bytes) override
    {
        for (GsSink* s : sinks)
            s->imageUpload(regs, data, bytes);
    }
    void clearFrame(std::uint32_t context, std::uint32_t rgba) override
    {
        for (GsSink* s : sinks)
            s->clearFrame(context, rgba);
    }
    void localToHost(std::uint32_t bytes) override
    {
        for (GsSink* s : sinks)
            s->localToHost(bytes);
    }
    void vsync(std::uint64_t tick, const Privileged& privileged) override
    {
        for (GsSink* s : sinks)
            s->vsync(tick, privileged);
    }
};

int usage()
{
    std::fputs("usage: tgr-gsreplay <dump.tgrgs> [--images <folder>] [--every-vsync] [--inventory <file>|-] "
               "[--no-render]\n"
               "  --images <folder>   save the displayed picture of each frame as BMP\n"
               "  --every-vsync       save one picture per vsync, not only when the GS drew something\n"
               "  --inventory <file>  write the GS inventory of the dump (- for the console)\n"
               "  --no-render         only read the dump (with --inventory)\n",
               stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--images" && i + 1 < argc)
            options.images = fs::path(argv[++i]);
        else if (arg == "--every-vsync")
            options.everyVsync = true;
        else if (arg == "--inventory" && i + 1 < argc)
            options.inventory = argv[++i];
        else if (arg == "--no-render")
            options.render = false;
        else if (!arg.empty() && arg[0] != '-' && options.dump.empty())
            options.dump = fs::path(argv[i]);
        else
            return usage();
    }
    if (options.dump.empty())
        return usage();

    std::string error;
    GsDumpReader reader;
    GsDumpStart start;
    if (!reader.open(options.dump, error) || !reader.readStart(start, error))
    {
        log::error("{}", error);
        return 1;
    }
    log::info("{}: starts at vsync {} with {} registers and {} KiB of local memory", options.dump.string(),
              start.tick, start.registers.size(), start.localMemory.size() / 1024);

    Tee tee;
    std::unique_ptr<SoftwareReplay> software;
    if (options.render)
    {
        software = std::make_unique<SoftwareReplay>(options, start);
        tee.sinks.push_back(software.get());
    }
    std::unique_ptr<GsInventory> inventory;
    if (options.inventory)
    {
        inventory = std::make_unique<GsInventory>();
        inventory->restore(start.registers);
        tee.sinks.push_back(inventory.get());
    }

    std::uint64_t records = 0;
    while (reader.next(tee, error))
        ++records;
    if (!error.empty())
        log::warn("Stopped after {} records: {}", records, error);
    else
        log::info("{} records replayed", records);

    if (software)
        log::info("{}", software->summary());
    if (inventory)
    {
        const std::string report = inventory->report();
        if (*options.inventory == "-")
            std::fputs(report.c_str(), stdout);
        else
        {
            std::ofstream out(*options.inventory);
            out << report;
            if (!out)
            {
                log::error("Cannot write {}", *options.inventory);
                return 1;
            }
            log::info("GS inventory written to {}", *options.inventory);
        }
    }
    return error.empty() ? 0 : 1;
}
