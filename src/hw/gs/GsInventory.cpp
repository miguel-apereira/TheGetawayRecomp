// SPDX-License-Identifier: MIT
#include "hw/gs/GsInventory.h"

#include <algorithm>
#include <format>
#include <iterator>

namespace tgr::hw::gs
{

namespace
{

// Keys of GsInventory::misc_.
enum Misc : std::uint64_t
{
    DirectClears,
    ImageWithoutTransfer,
    AntialiasedDraws,
    PrmodeDraws, // attributes from PRMODE (PRMODECONT.AC = 0)
    ScanmskDraws,
    DitheredDraws,
    MipmapDraws,
    ColourMaskedDraws, // FBMSK leaves no bit written
    UploadsIntoTargets,
    Readbacks,
    LocalCopies,
    AntiBlurVsyncs, // both circuits show the same buffer one line apart
    BothCircuitVsyncs,
    InterlacedVsyncs,
    FieldModeVsyncs, // SMODE2.FFMD = 1: one field per frame buffer
    TexflushWrites,
    Signals,
    Finishes,
    Labels,
};

const char* miscName(std::uint64_t key)
{
    switch (key)
    {
    case DirectClears:
        return "whole-frame clears made by the runtime (no GS equivalent)";
    case ImageWithoutTransfer:
        return "IMAGE data with no host-to-local transfer waiting for it";
    case AntialiasedDraws:
        return "draws with AA1 (antialiasing)";
    case PrmodeDraws:
        return "draws taking their attributes from PRMODE (PRMODECONT.AC = 0)";
    case ScanmskDraws:
        return "draws with SCANMSK set";
    case DitheredDraws:
        return "draws with DTHE set";
    case MipmapDraws:
        return "textured draws with mipmaps (TEX1.MXL > 0)";
    case ColourMaskedDraws:
        return "draws with every colour bit masked (FBMSK)";
    case UploadsIntoTargets:
        return "host-to-local transfers into a buffer drawn since its last upload";
    case Readbacks:
        return "local-to-host transfers (readbacks)";
    case LocalCopies:
        return "local-to-local transfers";
    case AntiBlurVsyncs:
        return "vsyncs showing one buffer on both circuits one line apart (anti-blur candidate)";
    case BothCircuitVsyncs:
        return "vsyncs with both read circuits enabled";
    case InterlacedVsyncs:
        return "vsyncs in interlaced mode (SMODE2.INT)";
    case FieldModeVsyncs:
        return "vsyncs in field mode (SMODE2.FFMD)";
    case TexflushWrites:
        return "TEXFLUSH writes";
    case Signals:
        return "SIGNAL writes";
    case Finishes:
        return "FINISH writes";
    case Labels:
        return "LABEL writes";
    default:
        return "?";
    }
}

// Packed draw state: the switches that would pick a pipeline or a shader
// variant, without addresses or constants.
std::uint64_t stateKey(const GsState& state, PrimType type)
{
    const Prim prim = state.prim();
    const Frame frame(state.contextValue(reg::FRAME_1));
    const Test test(state.contextValue(reg::TEST_1));
    const Zbuf zbuf(state.contextValue(reg::ZBUF_1));
    std::uint64_t typeClass = 0;
    switch (type)
    {
    case PrimType::Point:
        typeClass = 0;
        break;
    case PrimType::Line:
    case PrimType::LineStrip:
        typeClass = 1;
        break;
    case PrimType::Sprite:
        typeClass = 3;
        break;
    default:
        typeClass = 2;
        break;
    }
    std::uint64_t key = typeClass;
    key |= std::uint64_t(prim.iip()) << 2;
    key |= std::uint64_t(prim.tme()) << 3;
    key |= std::uint64_t(prim.fge()) << 4;
    key |= std::uint64_t(prim.abe()) << 5;
    key |= std::uint64_t(prim.aa1()) << 6;
    key |= std::uint64_t(prim.fst()) << 7;
    key |= std::uint64_t(frame.psm()) << 8;
    key |= std::uint64_t(test.ate()) << 14;
    if (test.ate())
    {
        key |= std::uint64_t(test.atst()) << 15;
        key |= std::uint64_t(test.afail()) << 18;
    }
    key |= std::uint64_t(test.date()) << 20;
    key |= std::uint64_t(test.date() ? test.datm() : 0) << 21;
    key |= std::uint64_t(test.zte()) << 22;
    key |= std::uint64_t(test.ztst()) << 23;
    key |= std::uint64_t(zbuf.zmsk()) << 25;
    if (prim.abe())
    {
        key |= std::uint64_t(state.contextValue(reg::ALPHA_1) & 0xFF) << 26;
        key |= std::uint64_t(state.value(reg::PABE) & 1) << 45;
        key |= std::uint64_t(state.value(reg::COLCLAMP) & 1) << 46;
    }
    if (prim.tme())
    {
        const Tex0 tex0(state.contextValue(reg::TEX0_1));
        key |= std::uint64_t(tex0.psm()) << 34;
        key |= std::uint64_t(tex0.tfx()) << 40;
        key |= std::uint64_t(tex0.tcc()) << 42;
        key |= std::uint64_t(Tex1(state.contextValue(reg::TEX1_1)).mxl() != 0) << 52;
    }
    key |= std::uint64_t(frame.fbmsk() != 0) << 43;
    key |= std::uint64_t(state.contextValue(reg::FBA_1) & 1) << 44;
    key |= std::uint64_t(state.value(reg::DTHE) & 1) << 47;
    key |= std::uint64_t(zbuf.psm() & 0xF) << 48;
    return key;
}

const char* blendInput(std::uint64_t v) // A, B, D
{
    switch (v)
    {
    case 0:
        return "Cs";
    case 1:
        return "Cd";
    case 2:
        return "0";
    default:
        return "?";
    }
}

const char* blendFactor(std::uint64_t v) // C
{
    switch (v)
    {
    case 0:
        return "As";
    case 1:
        return "Ad";
    case 2:
        return "FIX";
    default:
        return "?";
    }
}

std::string blendText(std::uint64_t abcd)
{
    const Alpha a(abcd);
    return std::format("({} - {}) * {} + {}", blendInput(a.a()), blendInput(a.b()), blendFactor(a.c()),
                       blendInput(a.d()));
}

const char* atstName(std::uint64_t v)
{
    static const char* const names[] = {"NEVER", "ALWAYS", "LESS",    "LEQUAL",
                                        "EQUAL", "GEQUAL", "GREATER", "NOTEQUAL"};
    return names[v & 7];
}

const char* afailName(std::uint64_t v)
{
    static const char* const names[] = {"KEEP", "FB_ONLY", "ZB_ONLY", "RGB_ONLY"};
    return names[v & 3];
}

const char* ztstName(std::uint64_t v)
{
    static const char* const names[] = {"NEVER", "ALWAYS", "GEQUAL", "GREATER"};
    return names[v & 3];
}

const char* tfxName(std::uint64_t v)
{
    static const char* const names[] = {"MODULATE", "DECAL", "HIGHLIGHT", "HIGHLIGHT2"};
    return names[v & 3];
}

const char* wrapName(std::uint64_t v)
{
    static const char* const names[] = {"REPEAT", "CLAMP", "REGION_CLAMP", "REGION_REPEAT"};
    return names[v & 3];
}

const char* filterName(std::uint64_t v)
{
    static const char* const names[] = {"NEAREST",
                                        "LINEAR",
                                        "NEAREST_MIPMAP_NEAREST",
                                        "NEAREST_MIPMAP_LINEAR",
                                        "LINEAR_MIPMAP_NEAREST",
                                        "LINEAR_MIPMAP_LINEAR",
                                        "6?",
                                        "7?"};
    return names[v & 7];
}

std::string attributesText(std::uint64_t attributes) // PRIM bits 3-8, shifted down
{
    std::string text;
    const auto add = [&](bool on, const char* name)
    {
        if (on)
            text += text.empty() ? name : std::string(" ") + name;
    };
    add(attributes & 1, "IIP");
    add(attributes & 2, "TME");
    add(attributes & 4, "FGE");
    add(attributes & 8, "ABE");
    add(attributes & 16, "AA1");
    add(attributes & 32, "FST");
    return text.empty() ? "flat" : text;
}

std::string stateText(std::uint64_t key)
{
    static const char* const types[] = {"point", "line", "triangle", "sprite"};
    std::string text = types[key & 3];
    text += " " + attributesText((key >> 2) & 0x3F);
    text += " -> " + psmName(bits(key, 8, 6));
    if (bits(key, 43, 1))
        text += " FBMSK";
    if (bits(key, 44, 1))
        text += " FBA";
    if (bits(key, 47, 1))
        text += " DTHE";
    if (bits(key, 3, 1))
        text += std::format(" | tex {} {}{}{}", psmName(bits(key, 34, 6)), tfxName(bits(key, 40, 2)),
                            bits(key, 42, 1) ? " TCC" : "", bits(key, 52, 1) ? " mip" : "");
    if (bits(key, 5, 1))
        text += std::format(" | blend {}{}{}", blendText(bits(key, 26, 8)), bits(key, 45, 1) ? " PABE" : "",
                            bits(key, 46, 1) ? "" : " wrap");
    if (bits(key, 14, 1))
        text += std::format(" | atest {} fail {}", atstName(bits(key, 15, 3)), afailName(bits(key, 18, 2)));
    if (bits(key, 20, 1))
        text += std::format(" | DATE {}", bits(key, 21, 1));
    if (bits(key, 22, 1))
        text += std::format(" | z {} {}{}", ztstName(bits(key, 23, 2)), psmName(0x30 | bits(key, 48, 4)),
                            bits(key, 25, 1) ? " ZMSK" : "");
    return text;
}

// The `limit` biggest counts, largest first.
std::vector<std::pair<std::uint64_t, std::uint64_t>> top(const std::map<std::uint64_t, std::uint64_t>& counts,
                                                         std::size_t limit)
{
    std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted(counts.begin(), counts.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    if (sorted.size() > limit)
        sorted.resize(limit);
    return sorted;
}

std::uint64_t sum(const std::map<std::uint64_t, std::uint64_t>& counts)
{
    std::uint64_t total = 0;
    for (const auto& [key, count] : counts)
        total += count;
    return total;
}

std::string percent(std::uint64_t part, std::uint64_t whole)
{
    return whole ? std::format("{:5.1f}%", 100.0 * static_cast<double>(part) / static_cast<double>(whole))
                 : "    -";
}

} // namespace

std::uint32_t GsInventory::Target::blocks() const
{
    const PageSize page = psmPageSize(psm);
    const std::uint32_t pagesWide = std::max<std::uint32_t>(1, width * 64 / page.width);
    const std::uint32_t pagesHigh = std::max<std::uint32_t>(1, (height + page.height - 1) / page.height);
    return pagesWide * pagesHigh * 32;
}

GsInventory::GsInventory()
{
    state_.setListener(this);
}

void GsInventory::restore(const std::vector<std::pair<std::uint8_t, std::uint64_t>>& registers)
{
    for (const auto& [address, value] : registers)
        state_.restore(address, value);
}

void GsInventory::gifPacket(const std::uint8_t* data, std::uint32_t bytes)
{
    ++gifPackets_;
    gifBytes_ += bytes;
    ++frame_.gifPackets;
    frame_.gifBytes += bytes;
    state_.gifPacket(data, bytes);
}

void GsInventory::registerWrite(std::uint8_t address, std::uint64_t value)
{
    direct_ = true;
    state_.write(address, value);
    direct_ = false;
}

void GsInventory::imageUpload(const ImageUploadRegs& regs, const std::uint8_t* data, std::uint32_t bytes)
{
    state_.write(reg::BITBLTBUF, regs.bitbltbuf);
    state_.write(reg::TRXPOS, regs.trxpos);
    state_.write(reg::TRXREG, regs.trxreg);
    state_.write(reg::TRXDIR, regs.trxdir);
    state_.image(data, bytes);
}

void GsInventory::clearFrame(std::uint32_t, std::uint32_t)
{
    ++misc_[DirectClears];
}

void GsInventory::localToHost(std::uint32_t bytes)
{
    readbackBytes_ += bytes;
}

void GsInventory::vsync(std::uint64_t, const Privileged& p)
{
    ++vsyncs_;
    if (frame_.draws || frame_.uploads)
        endFrame();
    else
        ++vsyncsWithoutDraws_;

    const std::uint32_t pmode = static_cast<std::uint32_t>(p.pmode);
    const bool en1 = bits(pmode, 0, 1);
    const bool en2 = bits(pmode, 1, 1);
    std::string key =
        std::format("PMODE EN1={} EN2={} MMOD={} AMOD={} SLBG={} ALP={:#04x} | SMODE2 INT={} FFMD={}",
                    int(en1), int(en2), bits(pmode, 5, 1), bits(pmode, 6, 1), bits(pmode, 7, 1),
                    bits(pmode, 8, 8), bits(p.smode2, 0, 1), bits(p.smode2, 1, 1));
    const auto circuit = [&](int n, std::uint64_t dispfb, std::uint64_t display)
    {
        const DispFb fb(dispfb);
        const Display d(display);
        // DW and DX count video clocks; MAGH + 1 of them make a pixel.
        key += std::format(
            "\n      DISPFB{} FBP={:#x} FBW={} {} DBX={} DBY={} | DISPLAY{} {}x{} (DW={} MAGH={} MAGV={} "
            "DX={} DY={})",
            n, fb.fbp(), fb.fbw(), psmName(fb.psm()), fb.dbx(), fb.dby(), n, (d.dw() + 1) / (d.magh() + 1),
            (d.dh() + 1) / (d.magv() + 1), d.dw(), d.magh(), d.magv(), d.dx(), d.dy());
    };
    if (en1)
        circuit(1, p.dispfb1, p.display1);
    if (en2)
        circuit(2, p.dispfb2, p.display2);
    ++displays_[key];

    if (en1 && en2)
    {
        ++misc_[BothCircuitVsyncs];
        const DispFb fb1(p.dispfb1);
        const DispFb fb2(p.dispfb2);
        const Display d1(p.display1);
        const Display d2(p.display2);
        const auto apart = [](std::uint32_t a, std::uint32_t b) { return a + 1 == b || b + 1 == a; };
        if (fb1.fbp() == fb2.fbp() && (apart(fb1.dby(), fb2.dby()) || apart(d1.dy(), d2.dy())))
            ++misc_[AntiBlurVsyncs];
    }
    if (bits(p.smode2, 0, 1))
        ++misc_[InterlacedVsyncs];
    if (bits(p.smode2, 1, 1))
        ++misc_[FieldModeVsyncs];
}

void GsInventory::endFrame()
{
    frame_.textures = frameTextures_.size();
    frames_.push_back(frame_);
    frame_ = {};
    frameTextures_.clear();
}

void GsInventory::gifTag(const GifTag& tag)
{
    const std::uint64_t regs =
        tag.format == GifFormat::Packed || tag.format == GifFormat::Reglist ? tag.regs : 0;
    const std::uint64_t nreg = regs ? tag.nreg : 0;
    ++gifTags_[{static_cast<std::uint64_t>(tag.format) | (nreg << 2), regs}];
}

void GsInventory::registerWrite(const GsState& state, std::uint8_t address, std::uint64_t value)
{
    ++registerWrites_[address];
    if (direct_)
        ++directWrites_[address];

    switch (address)
    {
    case reg::TEX0_1:
    case reg::TEX0_2:
    case reg::TEX2_1:
    case reg::TEX2_2:
    {
        // TEX2 carries TEX0's CLUT fields; its PSM is the texture's.
        const Tex0 tex(value);
        if (tex.cld() != 0)
        {
            ++clutLoads_[tex.cld() | (tex.cpsm() << 3) | (tex.csm() << 7) | (std::uint64_t(tex.psm()) << 8)];
            ++frame_.clutLoads;
        }
        break;
    }
    case reg::FRAME_1:
    case reg::FRAME_2:
        if (static_cast<std::uint32_t>(address - reg::FRAME_1) == state.context() &&
            Frame(value).fbp() != Frame(state.value(address)).fbp())
            ++frame_.targetChanges;
        break;
    case reg::TEXFLUSH:
        ++misc_[TexflushWrites];
        break;
    case reg::SIGNAL:
        ++misc_[Signals];
        break;
    case reg::FINISH:
        ++misc_[Finishes];
        break;
    case reg::LABEL:
        ++misc_[Labels];
        break;
    default:
        break;
    }
}

void GsInventory::noteTarget(std::uint32_t block, const Target& target)
{
    Target& entry = targets_[block];
    const std::uint64_t uploaded = entry.uploaded;
    const std::uint32_t height = entry.psm == target.psm && entry.width == target.width ? entry.height : 0;
    entry = target;
    entry.height = std::max(height, target.height);
    entry.drawn = totalDraws_;
    entry.uploaded = uploaded;
}

const std::pair<const std::uint32_t, GsInventory::Target>* GsInventory::findTarget(std::uint32_t block) const
{
    auto it = targets_.upper_bound(block);
    if (it == targets_.begin())
        return nullptr;
    --it;
    if (block >= it->first + it->second.blocks())
        return nullptr;
    return &*it;
}

void GsInventory::draw(const GsState& state, PrimType type, std::uint32_t vertices)
{
    ++totalDraws_;
    ++frame_.draws;
    frame_.vertices += vertices;

    const Prim prim = state.prim();
    const std::uint64_t attributes = bits(prim.value, 3, 6);
    ++prims_[static_cast<std::uint64_t>(type) | (attributes << 3)];
    vertices_[static_cast<std::uint64_t>(type)] += vertices;
    if (!(state.value(reg::PRMODECONT) & 1))
        ++misc_[PrmodeDraws];
    if (prim.aa1())
        ++misc_[AntialiasedDraws];
    if (state.value(reg::SCANMSK) & 3)
        ++misc_[ScanmskDraws];
    if (state.value(reg::DTHE) & 1)
        ++misc_[DitheredDraws];

    const std::uint64_t key = stateKey(state, type);
    ++states_[key];
    if (key != lastStateKey_)
        ++frame_.stateChanges;
    lastStateKey_ = key;

    const Frame frame(state.contextValue(reg::FRAME_1));
    const Test test(state.contextValue(reg::TEST_1));
    const Zbuf zbuf(state.contextValue(reg::ZBUF_1));
    const Scissor scissor(state.contextValue(reg::SCISSOR_1));
    ++frameBuffers_[frame.fbp() | (std::uint64_t(frame.fbw()) << 9) | (std::uint64_t(frame.psm()) << 15)];
    if (frame.fbmsk() != 0)
        ++fbmsks_[frame.fbmsk()];
    const bool allMasked = frame.fbmsk() == 0xFFFFFFFFu ||
                           (psmBits(frame.psm()) == 16 && (frame.fbmsk() & 0x80F8F8F8u) == 0x80F8F8F8u) ||
                           (frame.psm() == psm::CT24 && (frame.fbmsk() & 0xFFFFFFu) == 0xFFFFFFu);
    if (allMasked)
        ++misc_[ColourMaskedDraws];

    if (test.ate())
        ++alphaTests_[test.atst() | (test.aref() << 3) | (test.afail() << 11)];
    ++depth_[test.zte() | (test.ztst() << 1) | (std::uint64_t(zbuf.zmsk()) << 3) |
             (std::uint64_t(zbuf.psm()) << 4)];
    if (test.date())
        ++dates_[test.datm()];
    if (prim.fge())
        ++fogs_[static_cast<std::uint64_t>(type)];

    if (prim.abe())
    {
        const Alpha alpha(state.contextValue(reg::ALPHA_1));
        std::uint64_t blend = alpha.value & 0xFF;
        if (alpha.c() == 2)
            blend |= std::uint64_t(alpha.fix()) << 8;
        ++blends_[blend];
        ++blendFlags_[(state.value(reg::PABE) & 1) | ((state.value(reg::COLCLAMP) & 1) << 1) |
                      ((state.contextValue(reg::FBA_1) & 1) << 2) | ((state.value(reg::DTHE) & 1) << 3)];
    }

    if (prim.tme())
    {
        const Tex0 tex0(state.contextValue(reg::TEX0_1));
        const Tex1 tex1(state.contextValue(reg::TEX1_1));
        const Clamp clamp(state.contextValue(reg::CLAMP_1));
        ++textures_[tex0.psm() | (tex0.tw() << 6) | (tex0.th() << 10) | (std::uint64_t(tex0.tfx()) << 14) |
                    (std::uint64_t(tex0.tcc()) << 16) | (std::uint64_t(tex0.cpsm()) << 17) |
                    (std::uint64_t(tex0.csm()) << 21)];
        ++texFilters_[tex1.mxl() | (tex1.mmag() << 3) | (tex1.mmin() << 4) | (tex1.lcm() << 7) |
                      (tex1.mtba() << 8)];
        ++clamps_[clamp.wms() | (clamp.wmt() << 2)];
        if (tex1.mxl() != 0)
            ++misc_[MipmapDraws];
        frameTextures_.insert(tex0.tbp0() | (std::uint64_t(tex0.psm()) << 14));

        // Sampling a buffer the game drew into (and has not uploaded over since)?
        if (const auto* hit = findTarget(tex0.tbp0()); hit && hit->second.drawn > hit->second.uploaded)
        {
            const std::uint32_t start = hit->first;
            const Target& target = hit->second;
            const std::uint64_t readKey = target.psm | (std::uint64_t(tex0.psm()) << 6) |
                                          (std::uint64_t(tex0.tbp0() != start) << 12) |
                                          (std::uint64_t(target.depth) << 13);
            if (++targetReads_[readKey] == 1)
                targetReadExample_[readKey] = {start, tex0.tbp0()};
        }
    }

    // Remember what this draw writes. Done after the texture check, so a draw
    // that reads its own target (feedback) still counts as a target read.
    const std::uint32_t height = scissor.y1() + 1;
    if (!allMasked)
        noteTarget(frame.fbp() * 32, Target{frame.fbw(), frame.psm(), height, false});
    if (test.zte() && test.ztst() != 0)
        noteTarget(zbuf.zbp() * 32, Target{frame.fbw(), zbuf.psm(), height, true});
}

void GsInventory::transfer(const GsState& state, std::uint32_t direction)
{
    const BitBltBuf blt(state.value(reg::BITBLTBUF));
    const TrxReg size(state.value(reg::TRXREG));
    switch (direction)
    {
    case 0:
    {
        ++uploads_[blt.dpsm()];
        ++uploadSizes_[(std::uint64_t(size.rrw()) << 16) | size.rrh()];
        ++frame_.uploads;
        frame_.uploadBytes += state.transferBytesLeft();
        auto it = targets_.upper_bound(blt.dbp());
        if (it != targets_.begin())
        {
            --it;
            if (blt.dbp() < it->first + it->second.blocks())
            {
                if (it->second.drawn > it->second.uploaded)
                    ++misc_[UploadsIntoTargets];
                it->second.uploaded = totalDraws_;
            }
        }
        break;
    }
    case 1:
        ++misc_[Readbacks];
        ++readbacks_[blt.spsm()];
        break;
    case 2:
        ++misc_[LocalCopies];
        ++localCopies_[(std::uint64_t(blt.spsm()) << 8) | blt.dpsm()];
        break;
    default:
        break;
    }
}

void GsInventory::transferData(const GsState&, std::uint32_t, bool expected)
{
    if (!expected)
        ++misc_[ImageWithoutTransfer];
}

std::string GsInventory::report() const
{
    std::string out;
    const auto line = [&](std::string text)
    {
        out += text;
        out += '\n';
    };
    const auto section = [&](const char* title)
    {
        out += '\n';
        out += title;
        out += '\n';
    };
    constexpr std::size_t kTop = 24;

    std::vector<FrameStats> frames = frames_;
    if (frame_.draws || frame_.uploads)
        frames.push_back(frame_);

    line("GS inventory");
    line(std::format(
        "vsyncs {}, frames with GS work {} (a frame here ends at a vsync after draws or uploads), "
        "vsyncs without any {}",
        vsyncs_, frames.size(), vsyncsWithoutDraws_));
    line(std::format("draws {}, GIF packets {}, GIF bytes {}", totalDraws_, gifPackets_, gifBytes_));

    section("Per frame (min / average / max)");
    const auto stat = [&](const char* name, auto member)
    {
        if (frames.empty())
            return;
        std::uint64_t lo = ~0ull, hi = 0, total = 0;
        for (const FrameStats& f : frames)
        {
            const std::uint64_t v = f.*member;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            total += v;
        }
        line(std::format("  {:<34} {:>10} {:>12.1f} {:>10}", name, lo,
                         static_cast<double>(total) / static_cast<double>(frames.size()), hi));
    };
    stat("draws (primitives)", &FrameStats::draws);
    stat("vertices", &FrameStats::vertices);
    stat("draw state changes", &FrameStats::stateChanges);
    stat("frame buffer changes", &FrameStats::targetChanges);
    stat("GIF packets", &FrameStats::gifPackets);
    stat("GIF bytes", &FrameStats::gifBytes);
    stat("uploads (host to local)", &FrameStats::uploads);
    stat("upload bytes", &FrameStats::uploadBytes);
    stat("CLUT loads", &FrameStats::clutLoads);
    stat("textures sampled (base, format)", &FrameStats::textures);

    section("Primitives (type, attributes): draws, share");
    for (const auto& [key, count] : top(prims_, kTop))
        line(std::format("  {:>10} {}  {} {}", count, percent(count, totalDraws_),
                         primTypeName(static_cast<PrimType>(key & 7)), attributesText(key >> 3)));
    for (const auto& [type, count] : vertices_)
        line(std::format("  vertices in {}: {}", primTypeName(static_cast<PrimType>(type)), count));

    section("Draw states (a pipeline/shader variant estimate): draws, share");
    line(std::format("  {} different states", states_.size()));
    for (const auto& [key, count] : top(states_, kTop * 2))
        line(std::format("  {:>10} {}  {}", count, percent(count, totalDraws_), stateText(key)));

    section("Blending (ABE draws): draws, equation");
    const std::uint64_t blended = sum(blends_);
    line(std::format("  {} blended draws ({} of all)", blended, percent(blended, totalDraws_)));
    for (const auto& [key, count] : top(blends_, kTop))
    {
        const Alpha a(key & 0xFF);
        line(std::format("  {:>10} {}  {}{}", count, percent(count, blended), blendText(key & 0xFF),
                         a.c() == 2 ? std::format(" FIX={:#04x}", key >> 8) : ""));
    }
    for (const auto& [key, count] : top(blendFlags_, kTop))
        line(std::format("  {:>10} with PABE={} COLCLAMP={} FBA={} DTHE={}", count, key & 1, (key >> 1) & 1,
                         (key >> 2) & 1, (key >> 3) & 1));

    section("Alpha test (ATE draws): draws, test");
    for (const auto& [key, count] : top(alphaTests_, kTop))
        line(std::format("  {:>10} {}  {} AREF={:#04x} fail {}", count, percent(count, totalDraws_),
                         atstName(key & 7), (key >> 3) & 0xFF, afailName((key >> 11) & 3)));
    for (const auto& [key, count] : dates_)
        line(std::format("  {:>10} with destination alpha test DATM={}", count, key));

    section("Depth: draws, ZTE ZTST ZMSK format");
    for (const auto& [key, count] : top(depth_, kTop))
        line(std::format("  {:>10} {}  {} {}{}", count, percent(count, totalDraws_),
                         key & 1 ? std::string("test ") + ztstName((key >> 1) & 3)
                                 : std::string("ZTE=0 (no test)"),
                         psmName(static_cast<std::uint32_t>(key >> 4)), (key >> 3) & 1 ? " ZMSK" : ""));

    section("Frame buffers drawn to: draws, FBP FBW format");
    for (const auto& [key, count] : top(frameBuffers_, kTop))
        line(std::format("  {:>10} {}  FBP={:#05x} FBW={} {}", count, percent(count, totalDraws_),
                         key & 0x1FF, (key >> 9) & 0x3F, psmName(static_cast<std::uint32_t>(key >> 15))));
    for (const auto& [key, count] : top(fbmsks_, kTop))
        line(std::format("  {:>10} with FBMSK={:#010x}", count, key));

    section("Textures (TME draws): draws, format size function CLUT");
    for (const auto& [key, count] : top(textures_, kTop))
    {
        const std::uint32_t texPsm = key & 0x3F;
        const bool paletted = psmBits(texPsm) == 8 || psmBits(texPsm) == 4 || texPsm == psm::T8H ||
                              texPsm == psm::T4HL || texPsm == psm::T4HH;
        line(std::format(
            "  {:>10}  {} {}x{} {}{}{}", count, psmName(texPsm), 1u << ((key >> 6) & 0xF),
            1u << ((key >> 10) & 0xF), tfxName((key >> 14) & 3), (key >> 16) & 1 ? " TCC" : "",
            paletted ? std::format(" CLUT {} CSM{}", psmName((key >> 17) & 0xF), ((key >> 21) & 1) + 1)
                     : ""));
    }
    for (const auto& [key, count] : top(texFilters_, kTop))
        line(std::format("  {:>10} TEX1 MXL={} MMAG={} MMIN={} LCM={} MTBA={}", count, key & 7,
                         filterName((key >> 3) & 1), filterName((key >> 4) & 7), (key >> 7) & 1,
                         (key >> 8) & 1));
    for (const auto& [key, count] : top(clamps_, kTop))
        line(std::format("  {:>10} CLAMP {} / {}", count, wrapName(key & 3), wrapName((key >> 2) & 3)));
    for (const auto& [key, count] : top(fogs_, kTop))
        line(std::format("  {:>10} fogged {}", count, primTypeName(static_cast<PrimType>(key))));

    section("CLUT loads (TEX0/TEX2 with CLD > 0): loads, CLD CLUT format CSM texture format");
    for (const auto& [key, count] : top(clutLoads_, kTop))
        line(std::format("  {:>10}  CLD={} {} CSM{} for {}", count, key & 7, psmName((key >> 3) & 0xF),
                         ((key >> 7) & 1) + 1, psmName(static_cast<std::uint32_t>(key >> 8))));

    section("Textures read from a buffer the game drew into (render target as texture)");
    if (targetReads_.empty())
        line("  none seen");
    for (const auto& [key, count] : top(targetReads_, kTop))
    {
        const auto example = targetReadExample_.at(key);
        line(std::format("  {:>10} draws  {} {} read as {}{}  (first: target block {:#06x}, TBP0 {:#06x})",
                         count, (key >> 13) & 1 ? "depth" : "colour", psmName(key & 0x3F),
                         psmName((key >> 6) & 0x3F), (key >> 12) & 1 ? ", inside it" : ", at its base",
                         example.first, example.second));
    }

    section("Transfers");
    for (const auto& [key, count] : top(uploads_, kTop))
        line(std::format("  {:>10} host to local as {}", count, psmName(static_cast<std::uint32_t>(key))));
    for (const auto& [key, count] : top(uploadSizes_, kTop))
        line(std::format("  {:>10} of {}x{}", count, key >> 16, key & 0xFFFF));
    for (const auto& [key, count] : top(localCopies_, kTop))
        line(std::format("  {:>10} local to local {} -> {}", count,
                         psmName(static_cast<std::uint32_t>(key >> 8)), psmName(key & 0xFF)));
    for (const auto& [key, count] : top(readbacks_, kTop))
        line(std::format("  {:>10} local to host from {}", count, psmName(static_cast<std::uint32_t>(key))));
    line(std::format("  {} bytes read back by the game", readbackBytes_));

    section("Display (at each vsync): vsyncs, mode");
    for (const auto& [key, count] : displays_)
        line(std::format("  {:>10}  {}", count, key));

    section("Other counts");
    for (const auto& [key, count] : misc_)
        line(std::format("  {:>10}  {}", count, miscName(key)));

    section("GIF tags: tags, format and register list (PACKED/REGLIST)");
    std::vector<std::pair<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t>> tags(gifTags_.begin(),
                                                                                        gifTags_.end());
    std::stable_sort(tags.begin(), tags.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    if (tags.size() > kTop)
        tags.resize(kTop);
    for (const auto& [key, count] : tags)
    {
        static const char* const formats[] = {"PACKED", "REGLIST", "IMAGE", "DISABLED"};
        std::string regs;
        const std::uint64_t nreg = key.first >> 2;
        for (std::uint64_t i = 0; i < nreg; ++i)
        {
            const std::uint32_t desc = (key.second >> (i * 4)) & 0xF;
            static const char* const names[] = {"PRIM",   "RGBAQ",  "ST",      "UV",      "XYZF2", "XYZ2",
                                                "TEX0_1", "TEX0_2", "CLAMP_1", "CLAMP_2", "FOG",   "res",
                                                "XYZF3",  "XYZ3",   "A+D",     "NOP"};
            regs += std::string(" ") + names[desc];
        }
        line(std::format("  {:>10}  {}{}", count, formats[key.first & 3], regs));
    }

    section("Register writes: writes (of which outside the GIF), register");
    for (const auto& [address, count] : registerWrites_)
    {
        const auto direct = directWrites_.find(address);
        const std::string_view name = registerName(static_cast<std::uint8_t>(address));
        line(std::format("  {:>10} ({:>8})  {}", count, direct == directWrites_.end() ? 0 : direct->second,
                         name.empty() ? std::format("{:#04x}", address) : std::string(name)));
    }
    return out;
}

} // namespace tgr::hw::gs
