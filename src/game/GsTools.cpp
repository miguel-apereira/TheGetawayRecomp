// SPDX-License-Identifier: MIT
#include "game/GsTools.h"

#include "app/Log.h"
#include "hw/gs/GsDump.h"
#include "hw/gs/GsInventory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gs_host.h"
#include "runtime/ps2_memory.h"

#include <format>
#include <fstream>
#include <mutex>
#include <vector>

namespace tgr::game
{

namespace
{

hw::gs::Privileged readPrivileged(const GSRegisters& r)
{
    hw::gs::Privileged p;
    p.pmode = r.pmode;
    p.smode1 = r.smode1;
    p.smode2 = r.smode2;
    p.srfsh = r.srfsh;
    p.synch1 = r.synch1;
    p.synch2 = r.synch2;
    p.syncv = r.syncv;
    p.dispfb1 = r.dispfb1;
    p.display1 = r.display1;
    p.dispfb2 = r.dispfb2;
    p.display2 = r.display2;
    p.extbuf = r.extbuf;
    p.extdata = r.extdata;
    p.extwrite = r.extwrite;
    p.bgcolor = r.bgcolor;
    p.csr = r.csr.load(std::memory_order_relaxed);
    p.imr = r.imr;
    p.busdir = r.busdir;
    p.siglblid = r.siglblid;
    return p;
}

// Passes everything reaching the runtime's GS on to our sinks and lets the
// software GS carry on (R0 only observes).
class Monitor final : public PS2GsHost
{
public:
    explicit Monitor(const GSRegisters& privileged) : privileged_(privileged) {}

    void add(hw::gs::GsSink* sink) { sinks_.push_back(sink); }

    // Runs f with no GS event in flight.
    template <class F> void locked(F&& f)
    {
        std::lock_guard lock(mutex_);
        f();
    }

    bool gifPacket(const uint8_t* data, uint32_t sizeBytes) override
    {
        std::lock_guard lock(mutex_);
        for (hw::gs::GsSink* sink : sinks_)
            sink->gifPacket(data, sizeBytes);
        return false;
    }

    bool registerWrite(uint8_t reg, uint64_t value) override
    {
        std::lock_guard lock(mutex_);
        for (hw::gs::GsSink* sink : sinks_)
            sink->registerWrite(reg, value);
        return false;
    }

    bool imageUpload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint64_t trxdir,
                     const uint8_t* data, uint32_t sizeBytes) override
    {
        const hw::gs::ImageUploadRegs regs{bitbltbuf, trxpos, trxreg, trxdir};
        std::lock_guard lock(mutex_);
        for (hw::gs::GsSink* sink : sinks_)
            sink->imageUpload(regs, data, sizeBytes);
        return false;
    }

    bool clearFrame(uint32_t context, uint32_t rgba) override
    {
        std::lock_guard lock(mutex_);
        for (hw::gs::GsSink* sink : sinks_)
            sink->clearFrame(context, rgba);
        return false;
    }

    bool localToHost(uint8_t*, uint32_t maxBytes, uint32_t&) override
    {
        std::lock_guard lock(mutex_);
        for (hw::gs::GsSink* sink : sinks_)
            sink->localToHost(maxBytes);
        return false;
    }

    void vsync(uint64_t tick) override
    {
        const hw::gs::Privileged privileged = readPrivileged(privileged_);
        std::lock_guard lock(mutex_);
        for (hw::gs::GsSink* sink : sinks_)
            sink->vsync(tick, privileged);
    }

private:
    const GSRegisters& privileged_;
    std::mutex mutex_;
    std::vector<hw::gs::GsSink*> sinks_;
};

} // namespace

struct GsTools::Impl
{
    GS& gs;
    const std::uint8_t* localMemory;
    GsToolOptions options;
    Monitor monitor;
    std::unique_ptr<hw::gs::GsInventory> inventory;
    std::unique_ptr<hw::gs::GsDumpWriter> dump;
    int dumpNumber = 0;

    Impl(GS& g, const GSRegisters& privileged, const std::uint8_t* memory, GsToolOptions o)
        : gs(g), localMemory(memory), options(std::move(o)), monitor(privileged)
    {
    }

    std::filesystem::path nextDumpPath()
    {
        const std::filesystem::path& path = *options.dumpPath;
        if (path.extension() == ".tgrgs" && dumpNumber++ == 0)
            return path;
        const std::filesystem::path folder = path.extension() == ".tgrgs" ? path.parent_path() : path;
        const std::string stem = path.extension() == ".tgrgs" ? path.stem().string() : "gs";
        for (;;)
        {
            std::filesystem::path candidate = folder / std::format("{}-{:03}.tgrgs", stem, ++dumpNumber);
            std::error_code ec;
            if (!std::filesystem::exists(candidate, ec))
                return candidate;
        }
    }
};

GsTools::GsTools(GS& gs, const GSRegisters& privileged, const std::uint8_t* localMemory,
                 GsToolOptions options)
    : impl_(std::make_unique<Impl>(gs, privileged, localMemory, std::move(options)))
{
    Impl& impl = *impl_;
    if (impl.options.inventoryPath)
    {
        impl.inventory = std::make_unique<hw::gs::GsInventory>();
        impl.monitor.add(impl.inventory.get());
        log::info("GS inventory: report goes to {} when the game stops",
                  impl.options.inventoryPath->string());
    }
    if (impl.options.dumpPath)
    {
        impl.dump = std::make_unique<hw::gs::GsDumpWriter>(localMemory);
        impl.monitor.add(impl.dump.get());
        if (impl.options.dumpStart)
            impl.dump->arm(impl.nextDumpPath(), *impl.options.dumpStart, impl.options.dumpVsyncs);
        else
            log::info("GS dump: press F9 in the game window to record {} vsyncs", impl.options.dumpVsyncs);
    }
    gs.setHost(&impl.monitor);
}

GsTools::~GsTools()
{
    finish();
    impl_->gs.setHost(nullptr);
}

void GsTools::toggleDump()
{
    Impl& impl = *impl_;
    if (!impl.dump)
    {
        log::warn("GS dump: start the game with --gs-dump <folder> to record dumps with F9");
        return;
    }
    if (impl.dump->active())
        impl.dump->stop();
    else
        impl.dump->arm(impl.nextDumpPath(), 0, impl.options.dumpVsyncs);
}

void GsTools::finish()
{
    Impl& impl = *impl_;
    if (impl.dump)
        impl.dump->stop();
    if (impl.inventory && impl.options.inventoryPath)
    {
        std::string report;
        impl.monitor.locked([&] { report = impl.inventory->report(); });
        std::error_code ec;
        if (impl.options.inventoryPath->has_parent_path())
            std::filesystem::create_directories(impl.options.inventoryPath->parent_path(), ec);
        std::ofstream out(*impl.options.inventoryPath);
        out << report;
        if (out)
            log::info("GS inventory: {} draws over {} frames written to {}", impl.inventory->draws(),
                      impl.inventory->frames(), impl.options.inventoryPath->string());
        else
            log::error("GS inventory: cannot write {}", impl.options.inventoryPath->string());
        impl.options.inventoryPath.reset();
    }
}

} // namespace tgr::game
