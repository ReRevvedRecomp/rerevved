#include "native_frame_assembler.h"

#include "gfx_draw_inputs.h"
#include "label_draw_inputs.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rerevved::gpu::guest_d3d
{
namespace
{

using Target = NativeRenderFramePass::Target;

constexpr std::uint32_t kOutputWidth = 1280, kOutputHeight = 720;
// RB_COPY_CONTROL color_clear_enable (bit 8) and depth_clear_enable (bit 9),
// rex/graphics/registers.h. The resolve clears its EDRAM source after the copy.
constexpr std::uint32_t kCopyClearBits = 0x300;
// A scanout's copy control without its clear bits: color 0, sample select
// k01 (0x40), which Resolve (0x8269E520) sets only for a 2X source. The host
// presents samples 0 and 3, its form of k01.
constexpr std::uint32_t kScanoutControl = 0x00100040;
constexpr std::uint64_t kOutputTexture  = 1;
// RB_COPY_DEST_INFO copy_dest_swap (bit 24), rex/graphics/registers.h. A
// scanout resolve's only admitted form adds it to k_8_8_8_8 (format 6 at bit
// 7) with no endian swap, number format, exponent bias, array or slice.
constexpr std::uint32_t kScanoutDestSwap = 0x01000000, kScanoutDestForm = 0x300;
// RB_COPY_DEST_PITCH of the title's scanouts: pitch 1280 (bits 0..13) and
// height 720 (bits 16..29), the frontbuffer view a swap scans out.
constexpr std::uint32_t kScanoutDestPitch = 0x02D00500;

// Loading-frame pairs draw color-only into the ended MRT set; VS digests use
// post-fetch-patch words.
struct EndedSetForm
{
    NativeDrawHook   hook;
    std::uint32_t    caller;
    std::string_view vertex, pixel;
};

constexpr std::array<EndedSetForm, 3> kEndedSetForms{ {
    { NativeDrawHook::IndexedUp, kGfxDrawCaller, kGfxVertexWordDigest, kGfxPixelWordDigest },
    { NativeDrawHook::IndexedUp, kGfxDrawCaller, kGfxMenuVertexWordDigest, kGfxMenuPixelWordDigest },
    { NativeDrawHook::NonindexedUp, kLabelDrawCaller, kLabelVertexWordDigest, kLabelPixelWordDigest },
} };

// What an untiled draw writes into a color surface: RT0's surface object and
// EDRAM tiles, inside the window scissor's exclusive bottom-right corner, and
// the depth surface when the draw writes depth or stencil.
struct PresentedWrite
{
    std::uint32_t                    surface = 0;
    GuestEdramRange                  range;
    std::uint32_t                    right = 0, bottom = 0;
    std::optional<GuestDepthSurface> depth;
};

// Bounds an untiled draw's writes from guest state alone. Returns the refusal.
std::string boundPresentedWrite(const GuestWordReader& read, std::uint32_t g, const GuestTarget& target, const GuestTiling& tiling, PresentedWrite& out)
{
    // PA_SC_WINDOW_OFFSET (G+0x28C0), PA_SC_WINDOW_SCISSOR_BR (G+0x28C8: x
    // bits 0..14, y bits 16..30), and the ended-set path's guards: RB_COLOR_MASK,
    // the conditional and recorded modes, and the occlusion query state.
    std::uint32_t offset = 0, bottomRight = 0, depthControl = 0, colorMask = 0, modes = 0, sampleControl = 0, sampleAddress = 0;
    if (g > UINT32_MAX - 0x3384 || !read(g + 0x28C0, offset) || !read(g + 0x28C8, bottomRight) || !read(g + 0x2934, depthControl) ||
        !read(g + 0x28DC, colorMask) || !read(g + 0x2ABC, modes) || !read(g + 0x2A48, sampleControl) || !read(g + 0x2A4C, sampleAddress))
        return "state_unreadable";
    if (target.colorCount != 1)
        return std::format("color_targets {}", target.colorCount);
    if (offset)
        return std::format("window_offset {:08X}", offset);
    const auto right = bottomRight & 0x7FFF, bottom = (bottomRight >> 16) & 0x7FFF;
    const auto pitch = target.surfaceInfo & 0x3FFF;
    if (right > std::min(pitch, target.surfaceWidth) || bottom > target.surfaceHeight)
        return std::format("window_scissor {}x{} outside surface {}x{} pitch {}", right, bottom, target.surfaceWidth, target.surfaceHeight, pitch);
    if (colorMask >> 4)
        return std::format("color_mask {:#x}", colorMask);
    if (modes & 0x40004000U)
        return std::format("conditional_or_recorded_shader: {:08X}", modes);
    if (sampleControl != 2 || sampleAddress)
        return std::format("query_state: sample_count_ctl {:08X} addr {:08X}", sampleControl, sampleAddress);
    out = { target.surface, target.ranges[0], right, bottom, std::nullopt };
    // RB_DEPTHCONTROL stencil enable (bit 0) or depth write (bit 2).
    if (depthControl & 5)
    {
        GuestDepthSurface depth;
        if (!ReadGuestDepthSurface(read, g, tiling, depth) || depth.tiled)
            return std::format("depth_unreadable: depth_control {:08X}", depthControl);
        out.depth = depth;
    }
    return {};
}

// Begin, a held root's commit, admission and recording of one root run in
// order on its guest thread, so a per-thread draft carries the build to them.
struct Draft
{
    const void*                  owner  = nullptr;
    NativeDrawHook               hook   = NativeDrawHook::IndexedBuffer;
    std::uint32_t                caller = 0, stack = 0;
    std::array<std::uint32_t, 8> arguments{};
    bool                         admitted = false;
    std::string                  detail;
    NativeDrawReplayRecipe       draw;
    std::optional<Target>        decoded;   // the guest's color target 0, when it decodes
    std::optional<Target>        route;     // an admitted draw's decoded target; empty is the presented target
    std::optional<std::uint32_t> depthBase; // the EDRAM base of the depth surface an admitted depth draw reads
    // An untiled draw into the storage of an ended tiled MRT set: admitted
    // without host work, since nothing resolves that set again.
    bool endedSetWrite = false;
    // The guest's tiling state at the draw, when it reads.
    std::optional<GuestTiling> tiling;
    // The bound set as decoded at entry, and whether it mapped a tiled extent.
    GuestTarget target;
    bool        mapped = false;
    // A direct lowerer root whose build waits for its commit.
    bool deferred = false;
    // Inside config.build, whose resolved texture queries see `decoded`; and
    // whether a tiled texture pass may yet take the draw into source content.
    bool building = false, tiledPass = false;
    // Whether this build bound a converted depth texture.
    bool depthView = false;
    // The epoch whose state set tiledPass, and the one whose resolve the
    // draw bound (zero for none).
    std::uint64_t buildEpoch = 0, aliasEpoch = 0;
    // An untiled draw while a tiled scanout left the presented EDRAM
    // undefined: its bounded writes, or why they are unbounded.
    std::optional<PresentedWrite> presentedWrite;
    std::string                   presentedWriteRefusal;
    // An off-screen draw's opaque overwrite of this rectangle (overwrittenRect),
    // and for a draw into a tiled single target, the tiling it maps.
    std::optional<NativeDrawReplayScissor> overwrite;
    std::optional<GuestTiling>             overwriteTiling;

    bool Matches(const void* assembler, const NativeDrawSubmission& submission) const noexcept
    {
        return owner == assembler && hook == submission.hook && caller == submission.caller &&
               stack == submission.stack && arguments == submission.arguments;
    }
};

thread_local Draft draft;

// The previous Began's target and tiling decode at device `g`, with every
// word it read (sorted and unique). The decode reads nothing else, so it
// holds for a later draw while those words are unchanged.
struct TargetDecode
{
    std::uint32_t                                        g = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> words;
    bool                                                 decoded = false, mapped = false;
    GuestTarget                                          target;
    std::optional<GuestTiling>                           tiling;
};

thread_local std::optional<TargetDecode> targetDecode;

// Whether each word still reads as recorded: through `view` a page at a
// time, else through `read`. Any failure counts as a change.
bool wordsUnchanged(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& words, const GuestWordReader& read, const NativeFrameAssemblerConfig::TargetView& view) noexcept
{
    try
    {
        std::uint32_t                 base = 1; // no page starts here
        std::span<const std::uint8_t> page;
        for (const auto& [address, value] : words)
        {
            std::uint32_t word = 0;
            if (!view)
            {
                if (!read(address, word))
                    return false;
            }
            else
            {
                if ((address & 0xFFF) > 0xFFC)
                    return false;
                if ((address & ~0xFFFU) != base)
                {
                    base = address & ~0xFFFU;
                    page = view(base, 0x1000);
                    if (page.size() != 0x1000)
                        return false;
                }
                const auto* bytes = page.data() + (address - base);
                word              = (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
            }
            if (word != value)
                return false;
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// What tile `tile`'s EDRAM held for one attachment before the guest replayed
// the pass for it: the clear an earlier tile's resolve left behind.
struct TileBackground
{
    std::uint32_t                tile = 0;
    std::uint32_t                slot = 0; // 0..3 color, kGuestDepthSlot depth
    NativeRenderFramePass::Clear clear;    // rectangle = the tile's rectangle
};

// Recorded recipes are immutable and shared; refused admission keeps raw
// storage so frame validation reports the error.
struct SharedDraw
{
    std::shared_ptr<const NativeDrawReplayAdmission> admitted;
    std::shared_ptr<const NativeDrawReplayRecipe>    raw;

    const NativeDrawReplayRecipe& recipe() const noexcept
    {
        return admitted ? admitted->Recipe() : *raw;
    }
};

SharedDraw shareDraw(NativeDrawReplayRecipe&& recipe)
{
    SharedDraw  draw;
    std::string error;
    // A refused recipe stays with the caller.
    if (!TryAdmitNativeDrawReplayRecipe(recipe, draw.admitted, error))
        draw.raw = std::make_shared<const NativeDrawReplayRecipe>(std::move(recipe));
    return draw;
}

// Appends `draw` to `pass` in order. A pass holds admitted owners until a raw
// draw joins it, which turns the whole pass to raw recipes: a pass never
// mixes the two.
void addDraw(NativeRenderFramePass& pass, const SharedDraw& draw)
{
    if (draw.admitted && pass.draws.empty())
    {
        pass.admittedDraws.push_back(draw.admitted);
        return;
    }
    for (const auto& admitted : pass.admittedDraws)
        pass.draws.push_back(admitted->Recipe());
    pass.admittedDraws.clear();
    pass.draws.push_back(draw.recipe());
}

// What an off-screen target holds since its last full clear, in operation order.
struct SourceContent
{
    std::vector<NativeRenderFramePass::Clear> clears; // a tiled MRT pass: one per slot, on the tile-0 rectangle
    std::vector<SharedDraw>                   draws;
    std::uint32_t                             depthEdramBase = kNoDepthEdramBase; // the clear's depth surface
    std::optional<GuestDepthSurface>          depthSurface;                       // a tiled pass's index-0 depth surface
    // Set for a tiled off-screen MRT pass; the rest tracks its pairing and tiles.
    std::optional<GuestTiling> tiling;
    std::uint32_t              clearedSlots = 0; // color slots the paired clears covered
    bool                       depthCleared = false;
    // The clear defined these planes of its depth surface; host depth starts
    // at the far clear, so only then may a draw test them.
    bool                        depthDefined = false, stencilDefined = false;
    std::vector<TileBackground> backgrounds; // grows as tile i-1 resolves with clear bits
    std::uint32_t               resolvedTile = 0, resolvedSlots = 0;
    // Set when a draw's overwrite, or a clear short of the target, defined
    // it: only this rectangle of color 0 holds modeled content.
    std::optional<NativeDrawReplayScissor> defined;
    // resolveGraph appends draws only while a lineage's clears and backgrounds
    // stay unchanged; copies preserve lineage.
    std::uint64_t lineage         = 0;
    std::uint32_t backgroundEdits = 0;
};

struct Work
{
    std::uint64_t                               sequence = 0;
    SharedDraw                                  draw; // empty for a clear or resolve
    Target                                      target;
    std::optional<NativeRenderFramePass::Clear> clear;
    std::optional<GuestResolveCopy>             resolve;
    std::shared_ptr<const SourceContent>        source;   // a resolve's replayed content
    std::uint32_t                               slot = 0; // a tiled resolve's source attachment
};

// The per-tile frontbuffer resolves of one tiled frame, which together scan
// out the logical presented target once.
struct TiledScanout
{
    GuestTiling   tiling;
    Target        target;
    std::uint32_t destBase = 0, destInfo = 0; // tile 0's; every tile agrees
    std::uint64_t end         = 0;            // destination range end, exclusive
    std::uint64_t watchedFrom = 0;            // guest writes numbered above it reach it
    std::uint32_t received    = 0;
    std::string   overwriter; // another write into its range between its tiles
    // Tile i+1's EDRAM as tile i's resolve cleared it, on tile i+1's rectangle.
    std::vector<NativeRenderFramePass::Clear> backgrounds;
};

// What the epoch's last complete modeled scanout wrote, until another write
// reaches its range. A swap presents only its own epoch's record.
struct ScanoutRecord
{
    std::uint32_t                base        = 0;
    std::uint64_t                end         = 0; // destination range end, exclusive
    std::uint64_t                watchedFrom = 0;
    std::optional<std::uint32_t> destInfo; // unset once overwritten
    std::string                  overwriter;
};

// The image the last presenting swap showed: its scanout record, kept current
// by later writes, and the swizzle and gamma table it was presented with.
struct DisplayedImage
{
    ScanoutRecord                              scanout;
    std::optional<std::array<std::uint8_t, 4>> swizzle;
    std::array<std::uint32_t, 256>             gamma{};
};

// A guest write reported through GuestWrote, numbered in report order.
struct GuestWrite
{
    std::uint64_t number = 0, begin = 0, end = 0;
};

struct Epoch
{
    NativeFrameCoverage            coverage;
    std::vector<Work>              work;
    std::uint64_t                  firstSequence = 0, lastSequence = 0; // every recorded sequence
    std::optional<std::thread::id> thread;                              // the epoch's drawing thread
    std::string                    failure, firstRejection;
    bool                           frontbuffer = false;
    std::optional<TiledScanout>    tiled;
    std::optional<ScanoutRecord>   scanout;
    // The presented tile-0 predicated clear, which only tile 0 runs, and
    // the tiles it was issued under.
    std::optional<std::uint64_t> tiledClear;
    std::optional<GuestTiling>   tiledClearTiling;
    // Skip mode dropped a clear of the presented target (no draw verdict carries it).
    bool clearSkipped = false;

    // Rejected clears of other decoded bases while the presented base was unset.
    struct RejectedClear
    {
        std::uint32_t base = 0;
        std::string   rejection;
    };

    std::vector<RejectedClear> rejectedClears;

    // Untiled draws into the presented EDRAM a tiled scanout left undefined,
    // held without host work until the frame's tiled clear covers them; the
    // failure their first draw names if anything else comes first.
    struct HeldWrites
    {
        PresentedWrite write;
        std::string    failure;
    };

    std::optional<HeldWrites> presentedWrites;
    // The first held draw's operation, until a clear proves the held draws
    // dead; after a failed hold, flush fails every operation from it.
    std::uint64_t heldFrom = 0;

    bool Skipped() const
    {
        return clearSkipped || std::any_of(coverage.skipped.begin(), coverage.skipped.end(), [](std::uint64_t count)
                                           {
                                               return count != 0;
                                           });
    }
};

// Off-screen clear/draw content replays in each resolve frame across segments,
// terminals, swaps. Any operation on another target ends it, because EDRAM
// ranges may alias.
struct RenderSource
{
    std::optional<Target>          target;
    bool                           initialized = false, partial = false;
    std::shared_ptr<SourceContent> content;
};

// A tiled MRT set whose last tile was resolved, with the depth surface its
// index-0 clear defined.
struct EndedSet
{
    GuestTarget                      target;
    std::optional<GuestDepthSurface> depth;
};

struct Submitted
{
    std::uint64_t work  = 0;           // first operation with host work; zero for none
    std::uint64_t first = 0, last = 0; // the span's operation range
    std::uint64_t fence = 0;
};

void assign(std::string& target, const char* text) noexcept
{
    try
    {
        target = text;
    }
    catch (...)
    {
        target.clear();
    }
}

// Only NoNativeRoute may skip a raster draw; other refusals carry guest effects
// or unknown behavior.
bool rasterOnly(NativeDrawReason reason) noexcept
{
    return reason == NativeDrawReason::NoNativeRoute;
}

std::string threadName(std::thread::id id)
{
    std::ostringstream text;
    text << id;
    return text.str();
}

// RT0 is RGBA8, or a lone k_2_10_10_10 or k_16_16 target without a depth or
// stencil attachment: the host replays those formats only depth-free, and a
// family builds such a draw only from shaders specialized for that format.
bool fitsTarget(const NativeDrawReplayRecipe& draw)
{
    const auto extras = std::span(draw.extraTargets).first(std::min<std::size_t>(draw.extraTargetCount, draw.extraTargets.size()));
    const bool color  = draw.targetFormat == NativeDrawReplayTargetFormat::Rgba8 ||
                        ((draw.targetFormat == NativeDrawReplayTargetFormat::Rgb10a2 || draw.targetFormat == NativeDrawReplayTargetFormat::Rg16Snorm) &&
                         !draw.depth.enabled && !draw.depth.writeEnabled && !draw.stencil.enabled && !draw.extraTargetCount);
    return draw.schemaVersion == 2 && draw.width > 0 && draw.height > 0 && (draw.sampleCount == 1 || draw.sampleCount == 4) && color &&
           draw.initialSample0.empty() &&
           draw.initialSample1.empty() && draw.depth.initialSamples.empty() && draw.extraTargetCount <= draw.extraTargets.size() &&
           std::all_of(extras.begin(), extras.end(), [](const NativeDrawReplayExtraTarget& extra)
                       {
                           return extra.format == NativeDrawReplayTargetFormat::Rgba8 || extra.format == NativeDrawReplayTargetFormat::Rgb10a2;
                       });
}

// The swap presents only this shape; every other target is off-screen. An
// MRT set is never presented, even when its RT0 has the presented shape.
bool presentable(const Target& target)
{
    return target.width == kOutputWidth && target.height == kOutputHeight && target.sampleCount == 4 &&
           target.format == NativeDrawReplayTargetFormat::Rgba8 && target.extraColorTargets == 0;
}

// Color-0 coverage admits SRC_ALPHA/INV_SRC_ALPHA only when alpha is proven 1
// and depth/stencil are disabled.
std::optional<NativeDrawReplayScissor> overwrittenRect(const NativeDrawReplayRecipe& draw, const NativeFrameDrawCover& cover)
{
    using Factor        = NativeDrawReplayBlendFactor;
    const auto& blend   = draw.blend;
    const auto& covered = cover.rect;
    const auto  factors = [&](Factor source, Factor destination)
    {
        return blend.sourceColor == source && blend.destinationColor == destination && blend.colorOp == NativeDrawReplayBlendOp::Add &&
               blend.sourceAlpha == source && blend.destinationAlpha == destination && blend.alphaOp == NativeDrawReplayBlendOp::Add;
    };
    const auto opaque  = !blend.enabled || factors(Factor::One, Factor::Zero) || (cover.alphaOne && factors(Factor::SourceAlpha, Factor::InverseSourceAlpha));
    const auto samples = draw.sampleCount == 4 ? 9U : 1U;
    if (!opaque || draw.depthOnly || draw.depth.enabled || draw.stencil.enabled || blend.alphaToCoverage || blend.logicOpEnabled ||
        blend.writeMask != 0x0F || draw.extraTargetCount || (draw.sampleMask & samples) != samples)
        return std::nullopt;
    const NativeDrawReplayScissor rect{ std::max({ covered.left, draw.scissor.left, 0 }),
                                        std::max({ covered.top, draw.scissor.top, 0 }),
                                        std::min({ covered.right, draw.scissor.right, static_cast<std::int32_t>(draw.width) }),
                                        std::min({ covered.bottom, draw.scissor.bottom, static_cast<std::int32_t>(draw.height) }) };
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return std::nullopt;
    return rect;
}

// The tile rectangles lie inside the screen, are disjoint, and cover it.
bool tilesCoverScreen(const GuestTiling& tiling)
{
    if (!tiling.Maps() || !tiling.screenWidth || !tiling.screenHeight)
        return false;
    std::uint64_t area = 0;
    for (std::uint32_t i = 0; i < tiling.count; ++i)
    {
        const auto& rect = tiling.rects[i];
        if (rect.x0 >= rect.x1 || rect.y0 >= rect.y1 || rect.x1 > tiling.screenWidth || rect.y1 > tiling.screenHeight)
            return false;
        for (std::uint32_t j = 0; j < i; ++j)
        {
            const auto& other = tiling.rects[j];
            if (rect.x0 < other.x1 && other.x0 < rect.x1 && rect.y0 < other.y1 && other.y0 < rect.y1)
                return false;
        }
        area += std::uint64_t(rect.x1 - rect.x0) * (rect.y1 - rect.y0);
    }
    return area == std::uint64_t(tiling.screenWidth) * tiling.screenHeight;
}

// Only the tile-0 predicated clear of the whole screen (0x8250F988) is
// modeled; it stands once for the logical frame. Returns the refusal.
std::string tiledClearForm(const GuestClear& clear)
{
    const auto& tiling = clear.tiling;
    const auto& rect   = clear.clear.rectangle;
    if (!tiling.Maps())
        return std::format("tiled_count {}", tiling.count);
    if (tiling.predicateMask != 3)
        return std::format("tiled_predicate {:#x}", tiling.predicateMask);
    if (!tilesCoverScreen(tiling))
        return "tiled_rects";
    if (rect.left || rect.top || rect.right != static_cast<std::int32_t>(tiling.screenWidth) ||
        rect.bottom != static_cast<std::int32_t>(tiling.screenHeight))
        return std::format("tiled_rect {},{},{},{}", rect.left, rect.top, rect.right, rect.bottom);
    if (!clear.target.tiling)
        return "tiled_target_unmapped";
    return {};
}

// Refusal details: one log line names the operation's form.
std::string targetText(const Target& target)
{
    return std::format("{}x{}x{} f{} base {:03X} depth {} mrt {}",
                       target.width,
                       target.height,
                       target.sampleCount,
                       static_cast<unsigned>(target.format),
                       target.edramBase,
                       target.depthEdramBase == kNoDepthEdramBase ? std::string("none") : std::format("{:03X}", target.depthEdramBase),
                       target.extraColorTargets + 1);
}

std::string clearText(const GuestClear& clear)
{
    const auto& rect = clear.clear.rectangle;
    return std::format("index {:x} flags {:x} predicate {} rect {},{},{},{} cds {}{}{} target {} depth {} tiling {} mask {:x}",
                       clear.index,
                       clear.flags,
                       clear.predicate,
                       rect.left,
                       rect.top,
                       rect.right,
                       rect.bottom,
                       int(clear.clear.color),
                       int(clear.clear.depth),
                       int(clear.clear.stencil),
                       targetText(clear.target.native),
                       clear.depth ? std::format("{:03X} {}x{}x{}", clear.depth->edramBase, clear.depth->width, clear.depth->height, clear.depth->sampleCount) : std::string("none"),
                       clear.tiling.Active() ? clear.tiling.count : 0,
                       clear.tiling.predicateMask);
}

std::string copyText(const GuestTarget& target, const GuestResolveCopy& copy)
{
    return std::format("slot {} control {:08X} rect {},{},{},{} point {},{} dest {:08X} info {:08X} source {}",
                       target.slot,
                       copy.copyControl,
                       copy.rectX,
                       copy.rectY,
                       copy.rectX + copy.rectWidth,
                       copy.rectY + copy.rectHeight,
                       copy.destX,
                       copy.destY,
                       copy.destBase,
                       copy.destInfo,
                       targetText(target.native));
}

// The names of the conditions that hold, joined by '+'.
std::string failed(std::initializer_list<std::pair<bool, const char*>> conditions)
{
    std::string names;
    for (const auto& [holds, name] : conditions)
        if (holds)
            names += (names.empty() ? "" : "+") + std::string(name);
    return names;
}

// Predication selects tiles; a draw stands for the logical frame only when
// it is unpredicated or its mask selects every tile.
bool coversEveryTile(const GuestTiling& tiling)
{
    if (!tiling.predicated)
        return true;
    if (!tiling.Maps())
        return false;
    for (std::uint32_t i = 0; i < tiling.count; ++i)
        if (((tiling.predicateMask >> (2 * i)) & 3) != 3)
            return false;
    return true;
}

bool sameShape(const Target& left, const Target& right)
{
    return left.width == right.width && left.height == right.height && left.sampleCount == right.sampleCount &&
           left.format == right.format && left.extraColorTargets == right.extraColorTargets && left.extraFormats == right.extraFormats;
}

// The same color EDRAM storage and formats, whatever the extent.
bool sameStorage(const Target& left, const Target& right)
{
    return left.edramBase == right.edramBase && left.format == right.format && left.extraColorTargets == right.extraColorTargets &&
           left.extraFormats == right.extraFormats && left.extraEdramBases == right.extraEdramBases;
}

NativeDrawReplayScissor scissorOf(const GuestRect& rect)
{
    return { static_cast<std::int32_t>(rect.x0), static_cast<std::int32_t>(rect.y0), static_cast<std::int32_t>(rect.x1), static_cast<std::int32_t>(rect.y1) };
}

// An EDRAM clear word in the slot's layout as the clear value it stands for.
std::array<float, 4> clearWordColor(std::uint32_t word, NativeDrawReplayTargetFormat format)
{
    if (format == NativeDrawReplayTargetFormat::Rgb10a2)
        return { float(word & 1023) / 1023.0F, float((word >> 10) & 1023) / 1023.0F, float((word >> 20) & 1023) / 1023.0F, float(word >> 30) / 3.0F };
    return { float(word & 255) / 255.0F, float((word >> 8) & 255) / 255.0F, float((word >> 16) & 255) / 255.0F, float(word >> 24) / 255.0F };
}

// SDK render_target_cache.cpp PerformTransfersAndResolveClears: RGBA bytes
// red-up; depth upper 24 bits via UNorm24To32, low 8 stencil. RGBA8 ignores
// RB_COLOR_CLEAR_LO, upper 64bpp half.
NativeRenderFramePass::Clear scanoutClear(const GuestResolveCopy& copy, const GuestRect& rect)
{
    NativeRenderFramePass::Clear clear;
    clear.rectangle = scissorOf(rect);
    clear.color = clear.depth = clear.stencil = true;
    for (std::uint32_t i = 0; i < 4; ++i)
        clear.colorValue[i] = float((copy.colorClear >> (8 * i)) & 0xFF) * (1.0F / 255.0F);
    const auto depth   = (copy.depthClear >> 8) & 0xFFFFFF;
    clear.depthValue   = float(depth + (depth >> 23)) * (1.0F / float(1 << 24));
    clear.stencilValue = static_cast<std::uint8_t>(copy.depthClear & 0xFF);
    return clear;
}

// A color slot's words: k_8_8_8_8 as RGBA8 bytes or words, k_2_10_10_10 and
// k_16_16 as the words of their guest destination formats 7 and 31.
GuestResolvePacking colorPacking(NativeDrawReplayTargetFormat format, bool words)
{
    switch (format)
    {
        case NativeDrawReplayTargetFormat::Rgb10a2:
            return GuestResolvePacking::Color2101010Words;
        case NativeDrawReplayTargetFormat::Rg16Snorm:
            return GuestResolvePacking::Float16x2Words;
        default:
            return words ? GuestResolvePacking::Color8888Words : GuestResolvePacking::HostRgba8;
    }
}

// A clear value as the host target holds it: Rg16Snorm stores a 32nd of the
// k_16_16 value, clamped to the fixed-point range.
NativeRenderFramePass::Clear hostClear(NativeRenderFramePass::Clear clear, NativeDrawReplayTargetFormat format)
{
    if (format == NativeDrawReplayTargetFormat::Rg16Snorm)
        for (auto& value : clear.colorValue)
            value = std::clamp(value / 32.0F, -1.0F, 1.0F);
    return clear;
}

NativeGuestWordPacking framePacking(GuestResolvePacking packing)
{
    switch (packing)
    {
        case GuestResolvePacking::Color8888Words:
            return NativeGuestWordPacking::Color8888;
        case GuestResolvePacking::Color2101010Words:
            return NativeGuestWordPacking::Color2101010;
        case GuestResolvePacking::Depth24Stencil8Words:
            return NativeGuestWordPacking::Depth24Stencil8;
        case GuestResolvePacking::Float16x2Words:
            return NativeGuestWordPacking::Float16x2;
        default:
            return NativeGuestWordPacking::None;
    }
}

std::uint32_t word(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != 4)
        throw std::runtime_error("solid_panel_stride_extent");
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

// The same frontbuffer checks as the gameplay terminal swap: a 2D RGBA8
// fetch naming the physical frontbuffer at the swap's own extent.
bool selectOutput(const NativeFrameSwap& swap, std::array<std::uint8_t, 4>& swizzle, std::string& error)
{
    const auto format = swap.fetch[1] & 0x3FU;
    const auto width = (swap.fetch[2] & 0x1FFFU) + 1U, height = ((swap.fetch[2] >> 13) & 0x1FFFU) + 1U;
    if ((swap.fetch[0] & 3U) != 2U || format != 6 || swap.textureFormat != 6 || swap.colorSpace != 0 ||
        (swap.fetch[1] & 0xFFFFF000U) != swap.frontbufferPhysical || width != swap.width || height != swap.height)
    {
        error = "swap_frontbuffer_fetch_mismatch";
        return false;
    }
    if (swap.width != kOutputWidth || swap.height != kOutputHeight)
    {
        error = std::format("swap_frontbuffer_extent_unsupported: {}x{}", swap.width, swap.height);
        return false;
    }
    // The output never reads the frontbuffer's bytes, so only the byte order
    // the scanout resolve writes (none) may be the order the fetch reads.
    if (const auto endian = (swap.fetch[1] >> 6) & 3U; endian != 0)
    {
        error = std::format("swap_frontbuffer_endian_unsupported: {}", endian);
        return false;
    }
    for (std::size_t component = 0; component < swizzle.size(); ++component)
    {
        swizzle[component] = static_cast<std::uint8_t>((swap.fetch[3] >> (1U + 3U * component)) & 7U);
        if (swizzle[component] > 5)
        {
            error = "swap_frontbuffer_swizzle_unsupported";
            return false;
        }
    }
    return true;
}

// The host presents the target itself, not the frontbuffer bytes the guest
// resolve wrote. A scanout resolve's red/blue swap stored the target's X in
// byte 2 and Z in byte 0, so the fetch's X and Z selectors read them swapped.
void composeScanoutSwap(std::uint32_t destInfo, std::array<std::uint8_t, 4>& swizzle)
{
    if (!(destInfo & kScanoutDestSwap))
        return;
    for (auto& component : swizzle)
        if (component == 0 || component == 2)
            component = static_cast<std::uint8_t>(2 - component);
}

// Resolve extent bounds GetResolveInfo (src/graphics/util/draw.cpp): full 32x32
// destination pitch/height tiles, 8 array slices. Depth writes 32 bits;
// unlisted formats use widest 128 bits.
std::uint64_t resolveDestEnd(const GuestResolveCopy& copy)
{
    std::uint64_t bytes = 16;
    switch ((copy.destInfo >> 7) & 0x3F)
    {
        case 2:
        case 8:
        case 9: // k_8, k_8_A, k_8_B
            bytes = 1;
            break;
        case 3:
        case 4:
        case 5:
        case 10:
        case 15:
        case 24:
        case 30: // 16-bit formats
            bytes = 2;
            break;
        case 6:
        case 7:
        case 14:
        case 16:
        case 17:
        case 25:
        case 31:
        case 36:
        case 50:
        case 54:
        case 55:
        case 56: // 32-bit formats
            bytes = 4;
            break;
        case 26:
        case 32:
        case 37: // k_16_16_16_16, k_16_16_16_16_FLOAT, k_32_32_FLOAT
            bytes = 8;
            break;
        default:
            break;
    }
    if ((copy.copyControl & 7) >= 4)
        bytes = 4;
    const auto tiles = [](std::uint64_t value)
    {
        return (value + 31) & ~std::uint64_t(31);
    };
    const std::uint64_t pitch = std::max<std::uint64_t>(copy.destPitch & 0x3FFF, std::uint64_t(copy.destX) + copy.rectWidth);
    const std::uint64_t rows  = std::max<std::uint64_t>((copy.destPitch >> 16) & 0x3FFF, std::uint64_t(copy.destY) + copy.rectHeight);
    return copy.destBase + tiles(pitch) * tiles(rows) * bytes * ((copy.destInfo & 8) ? 8 : 1);
}

// An off-screen resolve's destination, and its encoded image once a read or
// the ledger's take produced it. It is pending until the ledger reports the
// image written; while `taking`, the source no longer holds it.
struct PendingImage
{
    std::uint64_t                            mark = 0, begin = 0, end = 0;
    std::shared_ptr<const GuestResolveImage> image;
    bool                                     taking = false;
    // The ledger applied the record and left the image unencoded here.
    std::shared_ptr<const IHeldGuestImage> held;
    std::uint64_t                          heldBytes = 0;
    // The registry dropped the image before the ledger took it: it never lands.
    bool dropped = false;
};

// A held image taken out for its write, in op-mark order.
struct ClaimedImage
{
    std::uint64_t                          mark = 0;
    std::shared_ptr<const IHeldGuestImage> held;
};

// Mirror bytes are written only under the map lock and when no reader shares
// the range.
struct MirroredRead
{
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
    std::uint64_t                              used = 0, id = 0, writes = 0;
    std::uint32_t                              equalRun = 0, armAfter = 1;
    bool                                       watched = false;
};

constexpr std::uint32_t kMirrorArmAfterLimit = 64;

// Off-screen resolves the ledger has not written yet. A resolve beyond this
// fails as resolve_images_unwritten rather than forget one.
constexpr std::size_t kPendingImages = 256;
// How long a read waits for a resolve's submission and readback.
constexpr std::chrono::milliseconds kResolvedWait{ 2000 };

// Lays the image's written bytes over `bytes`, read from `begin`.
void overlayImage(const GuestResolveImage& image, std::uint64_t begin, std::vector<std::uint8_t>& bytes)
{
    const auto copy = [&](std::uint64_t offset, std::uint64_t length)
    {
        const auto from = std::max<std::uint64_t>(image.physical + offset, begin);
        const auto to   = std::min<std::uint64_t>(image.physical + offset + length, begin + bytes.size());
        if (from < to)
            std::memcpy(bytes.data() + (from - begin), image.bytes.data() + (from - image.physical), to - from);
    };
    if (image.runs.empty())
        copy(0, image.bytes.size());
    for (const auto& run : image.runs)
        copy(run.offset, run.length);
}

// Appends one presented-target operation in order. A target change starts a
// fresh pass; a clear after draws on the same target starts a preserving one.
void appendWork(std::vector<NativeRenderFramePass>& passes, Work& item)
{
    auto* back = passes.empty() ? nullptr : &passes.back();
    if (!back || back->target != item.target || (item.clear && back->DrawCount()))
    {
        NativeRenderFramePass pass;
        pass.target                      = item.target;
        pass.preservePreviousAttachments = back && back->target == item.target;
        pass.presented                   = true;
        passes.push_back(std::move(pass));
        back = &passes.back();
    }
    if (item.clear)
        back->clears.push_back(*item.clear);
    else
        addDraw(*back, item.draw);
}

// resolveGraph resolve destinations: op marks above every fixed texture id.
constexpr std::uint64_t kGraphTextureBase = std::uint64_t(1) << 62;
// The one-texel retained texture a chain of only off-screen passes ends with.
constexpr std::uint64_t kChainEndTexture = kGraphTextureBase - 1, kChainEndKey = ~std::uint64_t(0);

// The off-screen pass recording `item`'s resolve into `textureId`: the
// source's clear and draws, or with `continueFrom` only the draws from there
// on, over the attachments the source's previous pass in the chain left.
NativeRenderFramePass sourcePass(const Work& item, std::optional<std::size_t> continueFrom, std::uint64_t textureId, NativeRenderFrameTexture& texture)
{
    const auto&           copy   = *item.resolve;
    const auto&           source = *item.source;
    NativeRenderFramePass pass;
    pass.target                = item.target;
    pass.target.depthEdramBase = source.depthEdramBase;
    if (continueFrom)
    {
        pass.preservePreviousAttachments = true;
        for (auto draw = source.draws.begin() + static_cast<std::ptrdiff_t>(*continueFrom); draw != source.draws.end(); ++draw)
            addDraw(pass, *draw);
    }
    else
    {
        pass.clears = source.clears;
        // A tiled pass: each later tile starts from what an earlier tile's
        // resolve cleared; the rectangles are disjoint.
        for (const auto& background : source.backgrounds)
            pass.clears.push_back(background.clear);
        for (const auto& draw : source.draws)
            addDraw(pass, draw);
    }
    NativeRenderFrameResolve resolve;
    resolve.textureId = textureId;
    resolve.width     = copy.rectWidth;
    resolve.height    = copy.rectHeight;
    resolve.sourceX   = copy.rectX;
    resolve.sourceY   = copy.rectY;
    // Samples follow the copy's select: k0 {0,0}, k1 {3,3}, k01 {0,3}
    // (guest 2X sample 1 is host sample 3, sample mask 9).
    const auto select = (copy.copyControl >> 4) & 7;
    resolve.samples   = select == 1 ? std::array<std::uint32_t, 2>{ 3, 3 } : select == 4 ? std::array<std::uint32_t, 2>{ 0, 3 }
                                                                                         : std::array<std::uint32_t, 2>{ 0, 0 };
    auto format       = NativeDrawReplayTextureFormat::Rgba8;
    if (source.tiling)
    {
        resolve.depth       = item.slot == kGuestDepthSlot;
        resolve.colorTarget = resolve.depth ? 0 : item.slot;
    }
    // An untiled k_2_10_10_10 or k_16_16 source packs words as a tile does.
    resolve.guestWords = framePacking(copy.packing);
    if (resolve.guestWords != NativeGuestWordPacking::None)
        format = NativeDrawReplayTextureFormat::R32Uint;
    pass.resolves.push_back(resolve);
    texture = { textureId, copy.rectWidth, copy.rectHeight, format };
    return pass;
}

// The import of resolve `item`'s host texture, as later frames bind it
// through the surface registry.
NativeRenderFrameTexture surfaceImport(const Work& item)
{
    const auto&              copy    = *item.resolve;
    const auto               packing = framePacking(copy.packing);
    NativeRenderFrameTexture texture{ ResolveSurfaceTexture(item.sequence), copy.rectWidth, copy.rectHeight, packing == NativeGuestWordPacking::None ? NativeDrawReplayTextureFormat::Rgba8 : NativeDrawReplayTextureFormat::R32Uint, ResolveSurfaceKey(item.sequence), true };
    texture.importedWords = packing;
    return texture;
}

// Declares the import of each resolve surface a draw of `frame` binds, which
// the renderer's surface store holds by then; a typed binding over guest
// words needs typedGuestWords.
void declareSurfaces(NativeRenderFrame& frame, const ResolveSurfaceRegistry* surfaces)
{
    if (!surfaces)
        return;
    for (const auto& pass : frame.passes)
        for (std::size_t index = 0; index < pass.DrawCount(); ++index)
            for (const auto& texture : pass.Draw(index).textures)
                if (IsResolveSurfaceTexture(texture.frameTextureId) && std::none_of(frame.textures.begin(), frame.textures.end(), [&](const NativeRenderFrameTexture& declared)
                                                                                    {
                                                                                        return declared.id == texture.frameTextureId;
                                                                                    }))
                    if (auto import = surfaces->Import(texture.frameTextureId))
                    {
                        frame.typedGuestWords |= import->importedWords != NativeGuestWordPacking::None;
                        frame.textures.push_back(std::move(*import));
                    }
}

// A self-contained producer frame: the source's clear and draws, then the
// resolve, so it never depends on attachments another span left behind.
// With surfaces its texture enters the renderer's surface store.
bool resolveFrame(const Work& item, NativeRenderFrame& frame, bool identity, std::string& error, const ResolveSurfaceRegistry* surfaces = nullptr)
{
    NativeRenderFrameTexture texture;
    frame.passes.push_back(sourcePass(item, std::nullopt, item.sequence, texture));
    texture.persistentKey = (std::uint64_t(item.resolve->destBase) << 32) | item.sequence;
    frame.textures.push_back(std::move(texture));
    frame.outputTextureId     = item.sequence;
    frame.retainOnly          = true;
    frame.returnRawOutput     = true;
    frame.identityAttachments = identity;
    if (const auto surface = surfaces ? surfaces->FrameSurface(item.sequence, item.sequence) : std::nullopt)
        frame.surfaces.push_back(*surface);
    declareSurfaces(frame, surfaces);
    return BatchNativeRenderFrame(frame, error) && ValidateNativeRenderFrame(frame, error);
}

// Whether a draw samples a graph resolve the frame does not declare, which
// only the open chain's segments produce.
bool samplesChain(const NativeRenderFrame& frame)
{
    for (const auto& pass : frame.passes)
        for (std::size_t index = 0; index < pass.DrawCount(); ++index)
            for (const auto& texture : pass.Draw(index).textures)
                if (texture.frameTextureId >= kGraphTextureBase && std::none_of(frame.textures.begin(), frame.textures.end(), [&](const NativeRenderFrameTexture& declared)
                                                                                {
                                                                                    return declared.id == texture.frameTextureId;
                                                                                }))
                    return true;
    return false;
}

} // namespace

struct NativeFrameAssembler::State
{
    NativeFrameAssemblerConfig config;
    mutable std::mutex         mutex;
    Epoch                      epoch;
    std::uint64_t              epochs = 0, lastSwap = 0, lastFence = 0;
    // Highest sequence closed by a swap; a later record at or below it crossed one.
    std::uint64_t closedSequence  = 0;
    std::uint64_t recorded        = 0; // all epochs
    unsigned      diagnosticDraws = 0;
    std::uint64_t opMark          = 0;
    std::uint64_t recordedMark    = 0;
    RenderSource  source;
    // EDRAM base of the presented target, from the last operation that decoded
    // it. Unset is distinct from base 0 until one does.
    std::optional<std::uint32_t> mainBase;
    // Guest writes GuestWrote reported, not yet applied to the scanouts. The
    // memory access handler holds the global memory lock while reporting, and
    // the assembler arms notices under `mutex`, so reports take only this lock.
    std::mutex              writesMutex;
    std::vector<GuestWrite> writes;
    std::uint64_t           writeCount = 0;
    bool                    writesLost = false; // a report failed to queue
    // What the presenter shows until the next presenting swap. A swap without
    // presented work leaves it on screen, so it must name the same unwritten
    // frontbuffer with the same interpretation.
    std::optional<DisplayedImage> displayed;
    // EDRAM base of the presented target's depth surface, adopted only from a
    // full presented clear that names it, so a change never splits kept work.
    std::optional<std::uint32_t> mainDepthBase;
    // A tiled scanout left the presented target's EDRAM as a cleared tile 0
    // beside tile-1 content, not one logical image, until a full clear. A
    // tiled MRT pass sets it too: its storage may alias the presented EDRAM.
    bool tilingUndefined = false;
    // A tiled scanout, not an MRT pass, left it undefined.
    bool undefinedByScanout = false;
    // The same for the presented depth surface's depth and stencil planes:
    // a full presented clear of a plane defines that plane again.
    bool depthUndefined = false, stencilUndefined = false;
    // An off-screen clear wrote EDRAM the presented color or depth occupies,
    // named until a full presented clear defines that storage again.
    std::string colorAliased, depthAliased;
    // The presented storage's EDRAM tiles, from the full presented clear that
    // named it; unset, only the bases are compared.
    std::optional<GuestEdramRange> mainRange, mainDepthRange;
    // A presented clear or draw since the last swap. Every tile replays it,
    // but only tile 0 replays a tile-0 predicated clear after it; a tiled
    // scanout freezes the presented target until the swap.
    bool presentedWork = false;
    // The tiled MRT set whose last tile was resolved, until the next clear or
    // swap; color-only untiled draws into its storage and depth-only clears of
    // its depth surface are dead writes.
    std::optional<EndedSet>    endedSet;
    std::optional<std::string> failure;
    // The completion source refused work because it is closing.
    std::atomic<bool> stopped{ false };
    // Serializes whole swaps and segments so spans reach the completion source in order.
    std::mutex swapMutex;
    // Submitted spans with work, oldest first.
    std::deque<Submitted> submitted;
    // Off-screen resolves, in op-mark order, whose images guest memory may
    // not hold yet: the ledger writes one only when it releases its span,
    // and draws after the resolve may already sample the destination.
    std::vector<PendingImage> pendingImages;
    // What draw builds read, for deferred draws to capture; its own lock.
    std::unique_ptr<GuestReadTemplates> readTemplates = std::make_unique<GuestReadTemplates>();
    // Signals a finished take of a pending image.
    std::condition_variable imageTaken;
    // Under mutex: the pending images holding PendingImage::held, and their bytes.
    std::size_t   heldImages = 0;
    std::uint64_t heldBytes  = 0;
    // The ledger's image-write gate, under imageWritesMutex alone.
    std::mutex                   imageWritesMutex;
    completion::GuestImageWrites imageWrites;
    // Mirrored reads by (address, size), under readMirrorMutex alone.
    std::mutex                                                      readMirrorMutex;
    std::map<std::pair<std::uint32_t, std::uint32_t>, MirroredRead> readMirror;
    std::uint64_t                                                   readMirrorClock = 0, readMirrorIds = 0;
    // The largest size ever mirrored; no entry reaches further past its start.
    std::size_t readMirrorLongest = 0;
    // Ledger host writes between HostWriting and HostWrote.
    std::uint32_t hostWritesInFlight = 0;
    // Bytes of every live mirror buffer, in the map or kept by a holder;
    // each buffer's deleter subtracts its own.
    std::shared_ptr<std::atomic<std::size_t>> readMirrorLive = std::make_shared<std::atomic<std::size_t>>(0);
    // Counted only with config.timing.
    NativeFrameAssembler::ReadMirrorStats readMirrorStats;
    // Under mutex, counted only with config.timing: ReadMirrorStats smallReads
    // and smallBytes.
    std::array<std::uint64_t, 4> smallReads{}, smallBytes{};

    // Under readMirrorMutex: a write to [begin, end) ends the watch of every
    // mirrored read it overlaps.
    void mirrorWritten(std::uint64_t begin, std::uint64_t end) noexcept
    {
        // Small mirrored reads multiply the entries; only those starting within
        // the longest mirrored size before the write can overlap it.
        const auto first = std::min<std::uint64_t>(begin - std::min<std::uint64_t>(begin, readMirrorLongest), UINT32_MAX);
        for (auto it = readMirror.lower_bound({ std::uint32_t(first), 0 }); it != readMirror.end() && it->first.first < end; ++it)
        {
            auto& [key, entry] = *it;
            if (!(begin < std::uint64_t(key.first) + key.second))
                continue;
            ++entry.writes;
            entry.equalRun = 0;
            if (std::exchange(entry.watched, false))
                entry.armAfter = std::min(entry.armAfter * 2, kMirrorArmAfterLimit);
        }
    }

    // Under mutex: the ledger wrote, or will never write, this pending image.
    void pendingImageEnded(const PendingImage& image)
    {
        {
            std::lock_guard lock(readMirrorMutex);
            mirrorWritten(image.begin, image.end);
            if (image.image)
                mirrorWritten(image.image->physical, std::uint64_t(image.image->physical) + image.image->bytes.size());
        }
        if (config.resourceWritten)
        {
            config.resourceWritten(image.begin, image.end, true);
            if (image.image)
                config.resourceWritten(image.image->physical, std::uint64_t(image.image->physical) + image.image->bytes.size(), true);
        }
    }

    completion::GuestImageWrites writesGate()
    {
        std::lock_guard lock(imageWritesMutex);
        return imageWrites;
    }

    // Under mutex: drops the host copy `image` held.
    void releaseHeld(PendingImage& image)
    {
        if (!image.held)
            return;
        image.held.reset();
        --heldImages;
        heldBytes -= image.heldBytes;
        image.heldBytes = 0;
        if (config.surfaces)
            config.surfaces->SetHeldBytes(heldBytes);
    }

    // Under mutex, claim chosen/older overlaps in op order; later writes never
    // land under older. taking lasts until claimed stores image. Never claim
    // registry-queued drops: dropping store may already have landed.
    std::vector<ClaimedImage> claimHeld(const std::function<bool(const PendingImage&)>& chosen, const PendingImage& writing = {})
    {
        const auto                       dropping = config.surfaces ? config.surfaces->PendingDrops() : std::vector<std::uint64_t>{};
        std::vector<const PendingImage*> written;
        if (writing.mark)
            written.push_back(&writing);
        std::vector<bool> claim(pendingImages.size());
        for (bool grew = true; grew;)
        {
            grew = false;
            for (std::size_t i = 0; i < pendingImages.size(); ++i)
            {
                const auto& image = pendingImages[i];
                if (claim[i] || !image.held || std::ranges::find(dropping, image.mark) != dropping.end())
                    continue;
                if (!chosen(image) && std::none_of(written.begin(), written.end(), [&](const PendingImage* later)
                                                   {
                                                       return later->mark > image.mark && later->begin < image.end && image.begin < later->end;
                                                   }))
                    continue;
                claim[i] = true;
                written.push_back(&image);
                grew = true;
            }
        }
        std::vector<ClaimedImage> claimed;
        for (std::size_t i = 0; i < pendingImages.size(); ++i)
            if (claim[i])
            {
                auto& image = pendingImages[i];
                claimed.push_back({ image.mark, image.held });
                releaseHeld(image);
                image.taking = true;
            }
        return claimed;
    }

    // Encodes the claimed images and keeps each copy laid over guest memory
    // until it is written; the take of a failed one names why.
    std::vector<completion::GuestImageTake> encodeClaimed(const std::vector<ClaimedImage>& claimed)
    {
        std::vector<completion::GuestImageTake> takes;
        for (const auto& image : claimed)
        {
            completion::GuestImageTake take;
            try
            {
                take = image.held->Encode();
            }
            catch (...)
            {
                take.error = "held_image_encode_threw";
            }
            if (!take.image && take.error.empty())
                take.error = "held_image_encode_failed";
            takes.push_back(std::move(take));
        }
        {
            std::lock_guard lock(mutex);
            for (std::size_t i = 0; i < claimed.size(); ++i)
                if (const auto found = std::ranges::find(pendingImages, claimed[i].mark, &PendingImage::mark); found != pendingImages.end())
                {
                    if (takes[i].image)
                        found->image = std::make_shared<const GuestResolveImage>(*takes[i].image);
                    found->taking = false;
                }
        }
        imageTaken.notify_all();
        return takes;
    }

    // Write chosen held images and older ones through ledger gate; each writes
    // or skips, failing session outside census. Count lock readbacks/flushes.
    // Caller holds no assembler locks.
    void writeHeld(const std::function<bool(const PendingImage&)>& chosen, bool forLock)
    {
        const auto gate = writesGate();
        if (!gate)
            return;
        gate([&](const completion::GuestImageWriter& write)
             {
                 std::vector<ClaimedImage> claimed;
                 {
                     std::lock_guard lock(mutex);
                     claimed = claimHeld(chosen);
                 }
                 if (claimed.empty())
                     return;
                 const auto  takes  = encodeClaimed(claimed);
                 std::size_t landed = 0;
                 for (std::size_t i = 0; i < claimed.size(); ++i)
                 {
                     const bool written = takes[i].image && write(*takes[i].image);
                     landed += written;
                     if (!written)
                     {
                         std::lock_guard lock(mutex);
                         if (!failure && config.mode != NativeDrawMode::Census)
                             failure = std::format("held_image_unwritten: op mark {}{}", claimed[i].mark, takes[i].error.empty() ? "" : ": " + takes[i].error);
                     }
                     imageEnded(claimed[i].mark, written);
                 }
                 if (config.surfaces)
                     forLock ? config.surfaces->CountLockReadbacks(landed) : config.surfaces->CountFlushes(landed);
             });
    }

    // The registry's owner: drops the held images of entries ended without
    // needing them, writes those ended owing them, then those a lock that
    // just opened names. Call without the assembler's locks.
    void settle(const ResolveSurfaceLockRanges* locked) noexcept
    {
        try
        {
            if (!config.surfaces)
                return;
            const auto marked = [](const std::vector<std::uint64_t>& marks)
            {
                return [&marks](const PendingImage& image)
                {
                    return std::find(marks.begin(), marks.end(), image.mark) != marks.end();
                };
            };
            // Under mutex with the drops' removal, so a claim sees each
            // dropped image either queued or gone.
            ResolveSurfaceRegistry::Endings endings;
            {
                std::lock_guard lock(mutex);
                endings            = config.surfaces->TakeEndings();
                const auto dropped = marked(endings.dropped);
                std::erase_if(pendingImages, [&](PendingImage& image)
                              {
                                  if (!dropped(image))
                                      return false;
                                  // Its take ends it unencoded.
                                  if (!image.held)
                                  {
                                      image.dropped = true;
                                      return false;
                                  }
                                  releaseHeld(image);
                                  pendingImageEnded(image);
                                  return true;
                              });
            }
            if (!endings.dropped.empty())
                imageTaken.notify_all();
            if (!endings.owed.empty())
                writeHeld(marked(endings.owed), false);
            if (locked)
            {
                const auto marks = config.surfaces->LockedMarks(*locked);
                if (!marks.empty())
                    writeHeld(marked(marks), true);
            }
        }
        catch (...)
        {
            std::lock_guard lock(mutex);
            if (!failure && config.mode != NativeDrawMode::Census)
                failure = "held_image_settle_failed";
        }
    }

    // Writes the held images over [begin, end), which a read needs.
    void flushHeld(std::uint64_t begin, std::uint64_t end)
    {
        writeHeld(
            [&](const PendingImage& image)
            {
                return image.begin < end && begin < image.end;
            },
            false);
    }

    // Whether the pending image at `mark` is held.
    bool heldAt(std::uint64_t mark)
    {
        std::lock_guard lock(mutex);
        const auto      found = std::ranges::find(pendingImages, mark, &PendingImage::mark);
        return found != pendingImages.end() && found->held;
    }

    // Whether the pending image at `mark` was dropped or has its drop
    // queued: it never lands, so guest memory holds the range's bytes.
    bool droppingAt(std::uint64_t mark)
    {
        if (!config.surfaces)
            return false;
        {
            std::lock_guard lock(mutex);
            const auto      found = std::ranges::find(pendingImages, mark, &PendingImage::mark);
            if (found == pendingImages.end())
                return false;
            if (found->dropped)
                return true;
        }
        const auto dropping = config.surfaces->PendingDrops();
        return std::ranges::find(dropping, mark) != dropping.end();
    }

    // Under mutex: an off-screen resolve may not be recorded while this many
    // images await the ledger.
    std::optional<std::string> pendingImagesFull() const
    {
        if (pendingImages.size() < kPendingImages)
            return std::nullopt;
        return std::format("resolve_images_unwritten: {} off-screen resolves await the ledger", pendingImages.size());
    }

    // Retired resolve guest bytes serve later reads; stop mirror watching
    // before those reads see them. Only a skipped image ends its surface,
    // because guest memory kept older bytes.
    void imageEnded(std::uint64_t mark, bool written)
    {
        std::lock_guard lock(mutex);
        std::erase_if(pendingImages, [&](const PendingImage& image)
                      {
                          if (image.mark != mark)
                              return false;
                          pendingImageEnded(image);
                          return true;
                      });
        if (written && config.surfaces)
            config.surfaces->Landed(mark);
        else if (config.surfaces)
            config.surfaces->Skipped(mark);
        if (!aliasing())
            return;
        // The watch armed at record already covers writes before this landing.
        std::lock_guard alias(aliasMutex);
        aliases.Landed(mark);
    }

    // Under mutex: the off-screen resolve at `mark` writes `copy`'s
    // destination; a tile of a predicated-tiling bracket is `staged`.
    void notePendingImage(std::uint64_t mark, const GuestResolveCopy& copy, bool staged = false)
    {
        pendingImages.push_back({ mark, copy.destBase, resolveDestEnd(copy) });
        if (config.resourceWritten)
            config.resourceWritten(copy.destBase, resolveDestEnd(copy), true);
        if (config.surfaces)
        {
            surfaceNoted = true;
            ResolveSurfaceRecord record;
            record.mark    = mark;
            record.copy    = copy;
            record.destEnd = resolveDestEnd(copy);
            record.packing = framePacking(copy.packing);
            record.staged  = staged;
            {
                // As below: the watch is armed before the image can land.
                std::lock_guard writing(writesMutex);
                record.watchedFrom = writeCount;
                config.surfaces->Record(record);
            }
            if (config.watchWrites)
                config.watchWrites(copy.destBase, static_cast<std::uint32_t>(std::min<std::uint64_t>(record.destEnd - copy.destBase, UINT32_MAX)));
        }
        if (!aliasing())
            return;
        NativePendingResolve resolve;
        resolve.mark      = mark;
        resolve.destFetch = copy.destFetch;
        resolve.destInfo  = copy.destInfo;
        resolve.destBase  = copy.destBase;
        resolve.destEnd   = resolveDestEnd(copy);
        resolve.destX     = copy.destX;
        resolve.destY     = copy.destY;
        resolve.width     = copy.rectWidth;
        resolve.height    = copy.rectHeight;
        resolve.packing   = framePacking(copy.packing);
        resolve.epoch     = epochs + 1;
        {
            // The watch is armed before the image can land, so every later
            // guest write reports; one the image overwrites ends it too.
            std::lock_guard writing(writesMutex);
            std::lock_guard lock(aliasMutex);
            resolve.watchedFrom = writeCount;
            aliases.Register(resolve);
            if (!config.watchWrites)
                aliases.Written(resolve.destBase, resolve.destEnd);
        }
        if (config.watchWrites)
            config.watchWrites(copy.destBase, static_cast<std::uint32_t>(std::min<std::uint64_t>(resolve.destEnd - copy.destBase, UINT32_MAX)));
    }

    // Attachment chain of the open epoch's segments; set once one was submitted.
    std::optional<NativeRenderFramePrefixState> chain;
    // Target of the last pass of the open epoch's last segment.
    std::optional<Target> lastTarget;
    // Presented-target work the open epoch's segments submitted, under
    // swapMutex. A terminal frame runs and ends their chain, so the next
    // segment or final span replays it; recipes are self-contained.
    std::vector<Work> mainSubmitted;

    // Off-screen passes retain their attachments; presented work resumes its
    // latest pass, while each off-screen source resumes itself.
    struct GraphChain
    {
        std::optional<Target> last; // the last pass's target
        bool                  lastPresented = false;
        std::optional<Target> presented; // the latest presented pass's target
        // The content the last pass recorded when it was off-screen.
        std::uint64_t lineage = 0, clears = 0, draws = 0;
        std::uint32_t backgroundEdits = 0;
    };

    GraphChain graph;

    // native_frame_timing counters, indexed by GraphCount; touched only when
    // config.timing is set.
    enum GraphCount : std::size_t
    {
        Continuation,
        Replay,
        NotReady,
        Signed16,
        NoSource,
        SwapBatch,
        GraphCounts,
    };

    std::array<std::atomic<std::uint64_t>, GraphCounts> graphCounts{};

    void count(GraphCount which)
    {
        if (config.timing)
            graphCounts[which].fetch_add(1, std::memory_order_relaxed);
    }

    // native_resolve_alias: off-screen resolves by destination. aliasMutex
    // is a leaf: ResolvedTexture takes nothing else, and GuestWrote reports
    // under writesMutex, so a registration and the writes it races stay in order.
    std::mutex              aliasMutex;
    NativeResolveAliasTable aliases;
    // Under aliasMutex: the open chain's serial, zero while none is open, and
    // the open epoch's number (epochs + 1).
    std::uint64_t aliasChain = 0, aliasChains = 0, aliasEpoch = 1;
    // Counted only with config.timing, as NativeResolveGraphStats.
    std::array<std::atomic<std::uint64_t>, kNativeAliasMissNames.size()> aliasMisses{};
    std::array<std::atomic<std::uint64_t>, kNativeAliasFormNames.size()> aliasForms{};
    std::atomic<std::uint64_t>                                           aliasHits{ 0 }, aliasReproduced{ 0 };
    // Under swapMutex: each admitted resolve's work by mark, with its epoch,
    // which a frame re-records after a chain reset, and the open graph
    // segment's resolves, admitted once it was submitted.
    std::map<std::uint64_t, std::pair<std::uint64_t, Work>> producers;
    std::vector<Work>                                       segmentResolves;

    // The surface registry replaces the alias table when both are configured.
    bool aliasing() const
    {
        return config.resolveAlias && config.resolveGraph && !config.surfaces;
    }

    // Draws of a graph frame may view guest words as Rgba8 or Rgb10a2.
    bool typedViews() const
    {
        return aliasing() && config.castableGuestWords;
    }

    // Under mutex: the Resolve in progress recorded a surface entry.
    bool surfaceNoted = false;

    // Under mutex: the frame texture ids the open source content's draws
    // bind, which a later resolve frame may still replay.
    bool sourceBinds(std::uint64_t id) const
    {
        if (!source.content)
            return false;
        for (const auto& draw : source.content->draws)
            for (const auto& texture : draw.recipe().textures)
                if (texture.frameTextureId == id)
                    return true;
        return false;
    }

    // Under swapMutex: the store keys of ended surfaces `frame` releases.
    void releaseSurfaces(NativeRenderFrame& frame)
    {
        if (!config.surfaces)
            return;
        std::lock_guard lock(mutex);
        auto            keys = config.surfaces->TakeReleases([&](std::uint64_t id)
                                                             {
                                                      return sourceBinds(id);
                                                             });
        frame.releasedSurfaces.insert(frame.releasedSurfaces.end(), keys.begin(), keys.end());
    }

    // Under swapMutex: a chain of `epoch` starts. Its admitted resolves alias
    // for its draws; it returns the bound resolves no chain holds, by mark,
    // which its first segment must re-record.
    std::vector<std::uint64_t> beginChain(std::uint64_t epoch)
    {
        chain.emplace();
        if (!aliasing())
            return {};
        std::lock_guard lock(aliasMutex);
        aliasChain = ++aliasChains;
        return aliases.Adopt(epoch, aliasChain);
    }

    // Under swapMutex: the chain ends, and no later frame holds its textures.
    void endChain()
    {
        chain.reset();
        if (!aliasing())
            return;
        std::lock_guard lock(aliasMutex);
        if (aliasChain)
            aliases.EndChain(std::exchange(aliasChain, 0));
    }

    // Under swapMutex: no frame of `epoch` re-records its resolves any more.
    void endAliasEpoch(std::uint64_t epoch)
    {
        if (!aliasing())
            return;
        {
            std::lock_guard lock(aliasMutex);
            aliases.EndEpoch(epoch);
        }
        std::erase_if(producers, [&](const auto& entry)
                      {
                          return entry.second.first <= epoch;
                      });
    }

    // Under swapMutex, once the segment recording segmentResolves of `epoch`
    // was submitted.
    void admitResolves(std::uint64_t epoch)
    {
        auto resolves = std::exchange(segmentResolves, {});
        if (config.surfaces)
            for (const auto& item : resolves)
                config.surfaces->Admit(item.sequence, surfaceImport(item));
        if (!aliasing() || resolves.empty())
            return;
        {
            std::lock_guard lock(aliasMutex);
            for (const auto& item : resolves)
                aliases.Admit(item.sequence, kGraphTextureBase + item.sequence, aliasChain);
        }
        for (auto& item : resolves)
            producers.insert_or_assign(item.sequence, std::pair{ epoch, std::move(item) });
    }

    // Under swapMutex: `frame` starts with the source pass and resolve of each
    // resolve at `marks`, into its own texture without a readback: the
    // bound draws the frame replays sample them after a chain reset.
    bool reproduce(NativeRenderFrame& frame, std::vector<std::uint64_t> marks, std::string& error)
    {
        std::sort(marks.begin(), marks.end());
        std::vector<NativeRenderFramePass> passes;
        for (const auto mark : marks)
        {
            const auto found = producers.find(mark);
            if (found == producers.end())
            {
                error = std::format("alias_producer_missing: resolve {}", mark);
                return false;
            }
            NativeRenderFrameTexture texture;
            passes.push_back(sourcePass(found->second.second, std::nullopt, kGraphTextureBase + mark, texture));
            if (std::none_of(frame.textures.begin(), frame.textures.end(), [&](const NativeRenderFrameTexture& declared)
                             {
                                 return declared.id == texture.id;
                             }))
                frame.textures.push_back(texture);
        }
        frame.passes.insert(frame.passes.begin(), std::make_move_iterator(passes.begin()), std::make_move_iterator(passes.end()));
        if (config.timing)
            aliasReproduced.fetch_add(marks.size(), std::memory_order_relaxed);
        return true;
    }

    // A resolve, modeled or not, wrote [begin, end) of guest memory.
    void aliasResolved(std::uint64_t begin, std::uint64_t end)
    {
        if (!aliasing())
            return;
        std::lock_guard lock(aliasMutex);
        aliases.Supersede(begin, end);
    }

    // Return refusal or GraphCounts on success. Prefix packets refuse k_16_16;
    // off-screen passes on presented EDRAM are allowed because attachment
    // identity separates them and presented work later resumes.
    GraphCount graphRefusal(const Work& item) const
    {
        const auto& target   = item.target;
        bool        signed16 = target.format == NativeDrawReplayTargetFormat::Rg16Snorm;
        for (std::uint32_t slot = 0; slot < target.extraColorTargets && slot < target.extraFormats.size(); ++slot)
            signed16 |= target.extraFormats[slot] == NativeDrawReplayTargetFormat::Rg16Snorm;
        if (!item.resolve || !item.source)
            return NoSource;
        if (signed16)
            return Signed16;
        return GraphCounts;
    }

    bool graphable(const Work& item) const
    {
        return graphRefusal(item) == GraphCounts;
    }

    // Under swapMutex: appends one operation to a graph frame. A resolve adds
    // its source pass with a readback, and names it in `span` when given.
    void appendGraph(NativeRenderFrame& frame, Work& item, NativeCompletionSpan* span)
    {
        auto& passes = frame.passes;
        if (item.resolve)
        {
            const auto& content                = *item.source;
            auto        target                 = item.target;
            target.depthEdramBase              = content.depthEdramBase;
            const bool               continued = graph.last && !graph.lastPresented && *graph.last == target && graph.lineage == content.lineage &&
                                                 graph.clears == content.clears.size() && graph.backgroundEdits == content.backgroundEdits && graph.draws <= content.draws.size();
            NativeRenderFrameTexture texture;
            const auto               textureId = kGraphTextureBase + item.sequence;
            passes.push_back(sourcePass(item, continued ? std::optional<std::size_t>(graph.draws) : std::nullopt, textureId, texture));
            passes.back().resolves.back().readback = true;
            frame.textures.push_back(std::move(texture));
            if (span)
            {
                count(continued ? Continuation : Replay);
                span->resolves.push_back({ item.sequence, textureId, *item.resolve });
                span->coverage.attempted += passes.back().DrawCount();
                span->coverage.admitted += passes.back().DrawCount();
            }
            graph.last            = target;
            graph.lastPresented   = false;
            graph.lineage         = content.lineage;
            graph.clears          = content.clears.size();
            graph.backgroundEdits = content.backgroundEdits;
            graph.draws           = content.draws.size();
            return;
        }
        // As appendWork: a target change starts a fresh pass, a clear after
        // draws on the same target a preserving one.
        if (passes.empty() || !graph.lastPresented || !graph.last || *graph.last != item.target || (item.clear && passes.back().DrawCount()))
        {
            NativeRenderFramePass pass;
            pass.target                      = item.target;
            pass.presented                   = true;
            pass.preservePreviousAttachments = graph.last && graph.lastPresented && *graph.last == item.target;
            pass.resumeAttachments           = graph.last && !graph.lastPresented && graph.presented && *graph.presented == item.target;
            passes.push_back(std::move(pass));
        }
        if (item.clear)
            passes.back().clears.push_back(*item.clear);
        else
            addDraw(passes.back(), item.draw);
        graph.last          = item.target;
        graph.lastPresented = true;
        graph.presented     = item.target;
    }

    // Graph segments record work in op order under swapMutex and epoch.
    bool graphSegment(std::vector<Work> work, const std::vector<Work>& main, std::uint64_t epoch, NativeCompletionSpan& span, std::vector<Work>& kept, std::string& error)
    {
        span.segment       = true;
        span.firstSequence = work.front().sequence;
        span.lastSequence  = work.back().sequence;
        segmentResolves.clear();
        NativeRenderFrame frame;
        frame.identityAttachments = true;
        std::vector<std::uint64_t> adopted;
        if (!chain)
        {
            graph   = {};
            adopted = beginChain(epoch);
            for (auto item : main)
                appendGraph(frame, item, nullptr);
        }
        for (auto& item : work)
        {
            if (!item.resolve)
            {
                kept.push_back(item);
                if (!item.clear)
                {
                    ++span.coverage.attempted;
                    ++span.coverage.admitted;
                }
            }
            else if (aliasing() || config.surfaces)
            {
                segmentResolves.push_back(item);
                if (const auto surface = config.surfaces ? config.surfaces->FrameSurface(item.sequence, kGraphTextureBase + item.sequence) : std::nullopt)
                    frame.surfaces.push_back(*surface);
            }
            appendGraph(frame, item, &span);
        }
        // Identity attachments keep the re-recorded passes apart, and the
        // first presented pass started fresh, as after them.
        if (!adopted.empty() && !reproduce(frame, std::move(adopted), error))
            return false;
        const auto presented          = std::find_if(frame.passes.begin(), frame.passes.end(), [](const NativeRenderFramePass& pass)
                                                     {
                                                return pass.presented;
                                                     });
        span.assumptions.clearAssumed = presented != frame.passes.end() && !presented->preservePreviousAttachments && !presented->resumeAttachments && presented->clears.empty();
        frame.typedGuestWords         = typedViews();
        declareSurfaces(frame, config.surfaces.get());
        if (!BatchNativeRenderFrame(frame, error) || !chain->Append(frame, error))
            return false;
        lastTarget = frame.passes.back().target;
        span.frame = std::move(frame);
        return true;
    }

    // Under swapMutex: a retained frame that ends a chain of only off-screen
    // passes without presenting, through a one-texel raw resolve of the last.
    bool chainEnd(NativeRenderFrame& frame, std::string& error) const
    {
        if (!graph.last || graph.lastPresented)
        {
            error = "graph_chain_end_without_source";
            return false;
        }
        NativeRenderFramePass pass;
        pass.target = *graph.last;
        NativeRenderFrameResolve resolve;
        resolve.textureId  = kChainEndTexture;
        resolve.width      = 1;
        resolve.height     = 1;
        resolve.guestWords = NativeRenderFrameSlotFormat(pass.target, 0) == NativeDrawReplayTargetFormat::Rgb10a2 ? NativeGuestWordPacking::Color2101010 : NativeGuestWordPacking::Color8888;
        pass.resolves.push_back(resolve);
        frame.passes.push_back(std::move(pass));
        frame.textures.push_back({ kChainEndTexture, 1, 1, NativeDrawReplayTextureFormat::R32Uint, kChainEndKey });
        frame.outputTextureId     = kChainEndTexture;
        frame.retainOnly          = true;
        frame.returnRawOutput     = true;
        frame.identityAttachments = true;
        frame.typedGuestWords     = typedViews();
        // Validated as a fresh target; the source continues the chain's last pass.
        if (!ValidateNativeRenderFrame(frame, error))
            return false;
        frame.passes.front().preservePreviousAttachments = true;
        return true;
    }

    // Census mode, under swapMutex: the last pass target of segments the
    // source holds until a terminal frame, and the newest swap's gamma table.
    std::optional<Target> openSegments;
    // Under mutex: the latest SourceContent lineage; zero without resolveGraph.
    std::uint64_t lineages = 0;

    std::uint64_t newLineage()
    {
        return config.resolveGraph ? ++lineages : 0;
    }

    // With resolveGraph, the graph chain as those segments left it; a failed
    // swap may already have taken state->graph.
    GraphChain                     openGraph;
    std::array<std::uint32_t, 256> censusGamma{};

    // Under mutex: a presented pass target, with the presented depth surface.
    Target presentedPass(Target target) const
    {
        target.depthEdramBase = mainDepthBase.value_or(kNoDepthEdramBase);
        return target;
    }

    // Under mutex: the guest target is the presented one, by shape or by
    // base. An MRT set never is, though its RT0 may alias the presented base.
    bool presented(const Target& target) const
    {
        return target.extraColorTargets == 0 && (presentable(target) || (mainBase && target.edramBase == *mainBase));
    }

    // Under mutex: a tiled scanout of the open epoch freezes the presented
    // EDRAM until the swap; once complete, only an MRT pass may follow it.
    bool scanoutFrozen(const Target& target) const
    {
        return epoch.tiled && (epoch.tiled->received < epoch.tiled->tiling.count || target.extraColorTargets == 0);
    }

    // Under mutex: a depth-only clear of the ended set's own depth surface,
    // untiled as the guest leaves the set bound after the pass.
    bool endedDepth(const GuestClear& clear) const
    {
        if (clear.index != kGuestDepthOnlyClearIndex || !endedSet || !endedSet->depth || !clear.depth)
            return false;
        const auto& set = *endedSet->depth;
        const auto& hit = *clear.depth;
        return !hit.tiled && hit.surface == set.surface && hit.range == set.range;
    }

    // Under mutex: the EDRAM base is the presented color or depth storage.
    bool scannedOut(std::uint32_t base) const
    {
        return mainBase == base || mainDepthBase == base;
    }

    // Under mutex: a full presented clear defined these planes of the
    // presented depth; the alias cause lasts while either stays undefined.
    void definePresentedDepth(bool depth, bool stencil)
    {
        depthUndefined   = depthUndefined && !depth;
        stencilUndefined = stencilUndefined && !stencil;
        if (!depthUndefined && !stencilUndefined)
            depthAliased.clear();
    }

    // Under mutex: an off-screen clear wrote `range` from `base`. Presented
    // color or depth storage it overlaps holds neither the presented content
    // nor a modeled image until a full presented clear.
    void aliasPresented(std::uint32_t base, const GuestEdramRange& range, const std::string& cause)
    {
        const auto hits = [&](const std::optional<std::uint32_t>& presentedBase, const std::optional<GuestEdramRange>& presentedRange)
        {
            if (!presentedBase)
                return false;
            return presentedRange && range.end > range.begin ? range.Overlaps(*presentedRange) : base == *presentedBase;
        };
        if (hits(mainBase, mainRange))
        {
            if (!tilingUndefined)
                undefinedByScanout = false;
            tilingUndefined = true;
            colorAliased    = cause;
        }
        if (hits(mainDepthBase, mainDepthRange))
        {
            depthUndefined = stencilUndefined = true;
            depthAliased                      = cause;
        }
    }

    // Under mutex: anything but the covering clear may read or keep the held
    // writes, so they fail the epoch as the draw before the clear.
    void endPresentedWrites(std::string_view reason)
    {
        auto& held = epoch.presentedWrites;
        if (!held)
            return;
        if (epoch.failure.empty())
            epoch.failure = std::format("{}; dead_write_unproven: {}", held->failure, reason);
        held.reset();
    }

    // Under mutex: holds an untiled draw's writes; false, ending the hold,
    // when they reach another surface than those already held.
    bool holdPresentedWrite(const PresentedWrite& write, const std::string& failure, std::uint64_t mark)
    {
        auto& held = epoch.presentedWrites;
        if (!held)
        {
            held = Epoch::HeldWrites{ write, failure };
            if (!epoch.heldFrom)
                epoch.heldFrom = mark;
            return true;
        }
        auto& all = held->write;
        if (all.surface != write.surface || all.range != write.range ||
            (all.depth && write.depth && (all.depth->surface != write.depth->surface || all.depth->range != write.depth->range)))
        {
            endPresentedWrites(std::format("then a draw into surface {:08X}", write.surface));
            return false;
        }
        all.right  = std::max(all.right, write.right);
        all.bottom = std::max(all.bottom, write.bottom);
        if (!all.depth)
            all.depth = write.depth;
        return true;
    }

    // Only tile-0 predicated whole-screen clear is proven to cover every held
    // write before tile reads.
    std::string presentedWritesUncovered(const GuestClear& clear) const
    {
        const auto& write = epoch.presentedWrites->write;
        if (clear.index == kGuestDepthOnlyClearIndex || !clear.predicate || !clear.tiling.Active())
            return "untiled";
        if (auto form = tiledClearForm(clear); !form.empty())
            return form;
        if (!clear.clear.color || !presentable(clear.target.native))
            return "not_presented_color";
        if (clear.target.surface != write.surface || clear.target.ranges[0] != write.range)
            return std::format("surface {:08X} edram {:03X}..{:03X}, draws {:08X} edram {:03X}..{:03X}",
                               clear.target.surface,
                               clear.target.ranges[0].begin,
                               clear.target.ranges[0].end,
                               write.surface,
                               write.range.begin,
                               write.range.end);
        const auto& tile = clear.tiling.rects[0];
        if (write.right > tile.x1 - tile.x0 || write.bottom > tile.y1 - tile.y0)
            return std::format("tile_0 {}x{}, draws {}x{}", tile.x1 - tile.x0, tile.y1 - tile.y0, write.right, write.bottom);
        if (write.depth && (!clear.clear.depth || !clear.clear.stencil || !clear.depth || clear.depth->surface != write.depth->surface ||
                            clear.depth->range != write.depth->range))
            return std::format("depth {:08X} uncleared", write.depth->surface);
        return {};
    }

    // Under mutex: skip mode dropped a clear the frame may depend on.
    void markClearSkipped(const std::string& rejection)
    {
        epoch.clearSkipped = true;
        if (epoch.firstRejection.empty())
            epoch.firstRejection = rejection;
    }

    // Under mutex: a rejected clear of the presented target leaves the frame
    // partial in skip mode. While the base is unset, a clear of another decoded
    // base is held until an operation names the presented base.
    void skipClear(const Target& target, const std::string& detail)
    {
        if (config.mode == NativeDrawMode::Strict)
            return;
        if (presented(target))
            markClearSkipped("clear: " + detail);
        else if (!mainBase && std::none_of(epoch.rejectedClears.begin(), epoch.rejectedClears.end(), [&](const auto& held)
                                           {
                                               return held.base == target.edramBase;
                                           }))
            epoch.rejectedClears.push_back({ target.edramBase, "clear: " + detail });
    }

    // Under mutex: an undecoded clear could have hit the presented target,
    // whether or not its base is known, so skip mode marks the frame partial.
    void skipUndecodedClear(const std::string& detail)
    {
        if (config.mode != NativeDrawMode::Strict)
            markClearSkipped("clear: " + detail);
    }

    // Under mutex: the operation that names the presented base also settles
    // the clears rejected before it.
    void adoptBase(std::uint32_t base)
    {
        const bool unset = !mainBase;
        if (mainBase != base)
            mainRange.reset();
        mainBase = base;
        if (!unset)
            return;
        const auto held = std::find_if(epoch.rejectedClears.begin(), epoch.rejectedClears.end(), [&](const auto& clear)
                                       {
                                           return clear.base == base;
                                       });
        if (held != epoch.rejectedClears.end())
            markClearSkipped(held->rejection);
        epoch.rejectedClears.clear();
    }

    // Under mutex: a resolve cleared its source after the copy, to values the
    // model does not know, so only a later modeled full clear defines it again.
    void clearedByResolve(const Target& target)
    {
        touch(target);
        source.initialized = false;
        source.content.reset();
        source.partial = true;
    }

    // Under mutex, writer changes [begin, end), anywhere if range absent:
    // reached display/scanout form unknown. Guest number reaches only older
    // watches, resolves all. tiles=false for open tiled scanout tile.
    void dropScanouts(const std::optional<std::pair<std::uint64_t, std::uint64_t>>& range, const std::string& writer, bool tiles = true, std::uint64_t number = UINT64_MAX)
    {
        const auto reaches = [&](std::uint64_t base, std::uint64_t end, std::uint64_t watchedFrom)
        {
            return number > watchedFrom && (!range || (range->first < end && base < range->second));
        };
        const auto drop = [&](ScanoutRecord& record)
        {
            if (record.destInfo && reaches(record.base, record.end, record.watchedFrom))
            {
                record.destInfo.reset();
                record.overwriter = writer;
            }
        };
        if (epoch.scanout)
            drop(*epoch.scanout);
        if (displayed)
            drop(displayed->scanout);
        if (auto& tiled = epoch.tiled; tiles && tiled && tiled->received < tiled->tiling.count && tiled->overwriter.empty() &&
                                       reaches(tiled->destBase, tiled->end, tiled->watchedFrom))
            tiled->overwriter = writer;
    }

    void dropScanouts(const GuestResolveCopy* copy, bool tiles = true)
    {
        if (copy)
            dropScanouts(std::pair{ std::uint64_t(copy->destBase), resolveDestEnd(*copy) }, std::format("a resolve to {:#010x}", copy->destBase), tiles);
        else
            dropScanouts(std::nullopt, "an unread resolve", tiles);
    }

    // Under mutex: applies the guest writes reported since the last call.
    void applyWrites()
    {
        std::vector<GuestWrite> reported;
        bool                    lost = false;
        {
            std::lock_guard lock(writesMutex);
            reported.swap(writes);
            lost = std::exchange(writesLost, false);
        }
        if (lost)
            dropScanouts(std::nullopt, "an unqueued guest write");
        for (const auto& write : reported)
            dropScanouts(std::pair{ write.begin, write.end }, std::format("a guest write at {:#010x}", write.begin), true, write.number);
    }

    // Under mutex: arms the guest write notice for a scanout destination and
    // returns the number guest writes after it exceed.
    std::uint64_t watch(std::uint32_t base, std::uint64_t end)
    {
        std::uint64_t from = 0;
        {
            std::lock_guard lock(writesMutex);
            from = writeCount;
        }
        if (config.watchWrites)
            config.watchWrites(base, static_cast<std::uint32_t>(std::min<std::uint64_t>(end - base, UINT32_MAX)));
        return from;
    }

    // Under mutex: an operation on `target` ends any other target's content.
    void touch(const Target& target)
    {
        if (!source.target || *source.target != target)
        {
            source        = {};
            source.target = target;
        }
    }

    // 0x826A22F8 clears each tiled MRT RT under tile-0 predicate, then
    // depth/stencil index 0; later tiles start from prior resolve.
    std::string clearSet(const GuestClear& clear, std::uint64_t& mark)
    {
        const auto& target = clear.target.native;
        const auto  count  = std::uint32_t(target.extraColorTargets) + 1;
        const auto  colors = (1U << count) - 1;
        if (failure || !clear.predicate || !clear.target.tiling || (clear.flags & ~0x3FU) || !clear.clear.color || clear.index >= count)
            return "clear_form_unsupported: mrt " + failed({ { failure.has_value(), "after_failure" },
                                                             { !clear.predicate, "unpredicated" },
                                                             { !clear.target.tiling, "untiled" },
                                                             { (clear.flags & ~0x3FU) != 0, "flags" },
                                                             { !clear.clear.color, "no_color" },
                                                             { clear.index >= count, "index" } });
        if ((clear.clear.depth || clear.clear.stencil) && !clear.depth)
            return "clear_form_unsupported: depth_surface_undecoded";
        auto slotClear          = clear.clear;
        slotClear.rectangle     = scissorOf(clear.tiling.rects[0]);
        const auto    depthBase = clear.depth ? clear.depth->edramBase : kNoDepthEdramBase;
        SourceContent next;
        next.lineage = newLineage();
        if (clear.index == 0)
        {
            if ((clear.flags & colors) != colors)
                return std::format("mrt_clear_partial: flags {:#x} count {}", clear.flags, count);
            next.clears         = { slotClear };
            next.depthEdramBase = depthBase;
            next.depthSurface   = clear.depth;
            next.tiling         = clear.tiling;
            next.clearedSlots   = 1;
            next.depthCleared   = clear.clear.depth && clear.clear.stencil;
            next.depthDefined   = clear.depth && clear.clear.depth;
            next.stencilDefined = clear.depth && clear.clear.stencil;
        }
        else
        {
            // The pair continues index 0's clear of the same set: same tiles,
            // same color, nothing drawn in between.
            const auto* content = source.target == target && source.initialized ? source.content.get() : nullptr;
            if (!content || !content->tiling || content->clearedSlots != (1U << clear.index) - 1 || !content->draws.empty() ||
                content->clears.front().colorValue != clear.clear.colorValue || content->tiling->count != clear.tiling.count ||
                content->tiling->rects != clear.tiling.rects)
                return std::format("mrt_clear_unpaired: RT{}", clear.index);
            next = *content;
            next.clears.push_back(slotClear);
            next.clearedSlots |= 1U << clear.index;
        }
        NativeRenderFrame     validation;
        NativeRenderFramePass pass;
        pass.target                = target;
        pass.target.depthEdramBase = next.depthEdramBase;
        pass.clears                = next.clears;
        validation.passes.push_back(std::move(pass));
        if (std::string error; !ValidateNativeRenderFramePrefix(validation, error))
            return "clear_form_unsupported: " + error;
        mark         = ++opMark;
        recordedMark = std::max(recordedMark, mark);
        if (!epoch.firstSequence)
            epoch.firstSequence = mark;
        epoch.lastSequence = mark;
        touch(target);
        if (clear.index == 0)
            source.partial = false;
        source.initialized = true;
        source.content     = std::make_shared<SourceContent>(std::move(next));
        // The set may alias the presented EDRAM; presented extents are not
        // decoded, so the next presented work needs a full clear.
        tilingUndefined = depthUndefined = stencilUndefined = true;
        undefinedByScanout                                  = false;
        colorAliased.clear();
        depthAliased.clear();
        return {};
    }

    // Under mutex: one per-tile resolve of the tiled off-screen MRT source,
    // which Resolve found modeled and complete. Returns the mark, or zero with
    // `error` set and the source partial.
    std::uint64_t resolveTile(const GuestTarget& target, const GuestResolveCopy& copy, std::string& error)
    {
        const auto fail = [&](std::string detail)
        {
            source.partial = true;
            error          = detail + " (" + copyText(target, copy) + ")";
            return std::uint64_t(0);
        };
        if (!source.content->tiling)
            return fail("resolve_source_unmodeled");
        const auto& content = *source.content;
        const auto& tiling  = *content.tiling;
        const auto& entry   = *target.tiling;
        // 0x8250AC90 predicates tile i's resolves to that tile alone (3 << 2i).
        std::optional<std::uint32_t> tile;
        for (std::uint32_t i = 0; i < tiling.count; ++i)
            if (entry.predicated == 1 && entry.predicateMask == 3U << (2 * i))
                tile = i;
        const auto& rect = tiling.rects[tile.value_or(0)];
        if (!tile || copy.rectX != rect.x0 || copy.rectY != rect.y0 || copy.rectWidth != rect.x1 - rect.x0 || copy.rectHeight != rect.y1 - rect.y0 ||
            copy.destX != rect.x0 || copy.destY != rect.y0)
            return fail(std::format("tiled_resolve_form: tile {} rect {},{},{},{} point {},{} mask {:#x}", tile ? std::to_string(*tile) : std::string("none"), copy.rectX, copy.rectY, copy.rectX + copy.rectWidth, copy.rectY + copy.rectHeight, copy.destX, copy.destY, entry.predicateMask));
        const auto slot  = target.slot;
        const bool depth = slot == kGuestDepthSlot;
        // A tiled single target that a draw overwrote, not cleared: color 0
        // holds modeled content where that draw covered the whole tile, which
        // then needs no background from an earlier tile.
        const auto& defined     = content.defined;
        const bool  overwritten = !depth && slot == 0 && defined && defined->left <= std::int32_t(rect.x0) && defined->top <= std::int32_t(rect.y0) &&
                                  defined->right >= std::int32_t(rect.x1) && defined->bottom >= std::int32_t(rect.y1);
        if (depth ? !content.depthCleared : !(((content.clearedSlots >> slot) & 1) || overwritten))
            return fail(depth ? "resolve_depth_uncleared" : "resolve_source_unmodeled");
        const auto required = ((1U << (target.native.extraColorTargets + 1)) - 1) | (content.depthCleared ? 1U << kGuestDepthSlot : 0);
        // The loading pass's order per tile: depth when cleared, then RT0, RT1, ...
        // A resolve's clear bits then reach only slots the tile already copied.
        std::array<std::uint32_t, 5> order{};
        std::uint32_t                slots = 0;
        if (content.depthCleared)
            order[slots++] = kGuestDepthSlot;
        for (std::uint32_t i = 0; i <= target.native.extraColorTargets; ++i)
            order[slots++] = i;
        const auto expected = order[std::popcount(content.resolvedSlots)];
        if (*tile != content.resolvedTile || slot != expected)
            return fail(std::format("tiled_resolve_order: tile {} slot {}, expected tile {} slot {}", *tile, slot, content.resolvedTile, expected));
        if (*tile && !overwritten && std::none_of(content.backgrounds.begin(), content.backgrounds.end(), [&](const TileBackground& background)
                                                  {
                                                      return background.tile == *tile && background.slot == slot;
                                                  }))
            return fail(std::format("tiled_background_undefined: tile {} slot {}", *tile, slot));
        // RB_COPY_CONTROL: source select = the slot (4 depth), copy command
        // 0..1, sample select k0, k1 or (color) k01; only a 2X source has k1.
        const auto control = copy.copyControl, select = (control >> 4) & 7;
        if ((control & 7) != slot || ((control >> 20) & 3) > 1 || (control & ~0x00300377U) || (select != 0 && select != 1 && select != 4) ||
            (select && target.native.sampleCount == 1) || (depth && (select == 4 || (control & kCopyClearBits))))
            return fail(std::format("resolve_form_unsupported: control {:08X}", control));
        const auto format = NativeRenderFrameSlotFormat(target.native, depth ? 0 : slot);
        // A k_16_16 tile's clear word has no modeled layout.
        if (!depth && format == NativeDrawReplayTargetFormat::Rg16Snorm)
            return fail("resolve_form_unsupported: tiled k_16_16");
        if (auto full = pendingImagesFull())
            return fail(std::move(*full));
        auto resolved    = copy;
        resolved.packing = depth ? GuestResolvePacking::Depth24Stencil8Words : colorPacking(format, true);
        const auto mark  = ++opMark;
        recordedMark     = std::max(recordedMark, mark);
        if (!epoch.firstSequence)
            epoch.firstSequence = mark;
        epoch.lastSequence = mark;
        epoch.work.push_back({ mark, {}, target.native, {}, resolved, source.content, slot });
        // 0x8250AC90 issues these inside the predicated bracket it ends at EndTiling.
        notePendingImage(mark, resolved, true);
        // Recorded resolves keep the content they read.
        if (source.content.use_count() > 1)
            source.content = std::make_shared<SourceContent>(*source.content);
        auto&      updated = *source.content;
        const auto next    = *tile + 1;
        // The clear bits leave the next tile's starting EDRAM; after the last
        // tile they clear storage nothing models.
        const auto background = [&](std::uint32_t backgroundSlot, NativeRenderFramePass::Clear clear)
        {
            clear.rectangle = scissorOf(tiling.rects[next]);
            std::erase_if(updated.backgrounds, [&](const TileBackground& held)
                          {
                              return held.tile == next && held.slot == backgroundSlot;
                          });
            updated.backgrounds.push_back({ next, backgroundSlot, clear });
            ++updated.backgroundEdits;
        };
        if (next < tiling.count && (control & 0x100))
        {
            NativeRenderFramePass::Clear clear;
            clear.color        = true;
            clear.colorTargets = static_cast<std::uint8_t>(1U << slot);
            clear.colorValue   = clearWordColor(copy.colorClear, format);
            background(slot, clear);
        }
        if (next < tiling.count && (control & 0x200))
        {
            // RB_DEPTH_CLEAR for D24S8: depth in bits 8..31, stencil in 0..7.
            NativeRenderFramePass::Clear clear;
            clear.depth        = true;
            clear.stencil      = true;
            clear.depthValue   = float(copy.depthClear >> 8) / 16777215.0F;
            clear.stencilValue = static_cast<std::uint8_t>(copy.depthClear & 0xFF);
            background(kGuestDepthSlot, clear);
        }
        updated.resolvedSlots |= 1U << slot;
        if (updated.resolvedSlots == required)
        {
            ++updated.resolvedTile;
            updated.resolvedSlots = 0;
        }
        // The pass ended: nothing resolves the set again. Only an MRT set
        // stays bound as an ended set; a presented-shape pass leaves its
        // EDRAM undefined until full presented clears.
        if (updated.resolvedTile == tiling.count)
        {
            if (target.native.extraColorTargets)
                endedSet = EndedSet{ target, updated.depthCleared ? updated.depthSurface : std::nullopt };
            source = {};
        }
        return mark;
    }

    // A readable, nonpartial Resolve on the presented tiled target supplies one
    // per-tile image; the first tile starts the source.
    std::uint64_t resolvePresentedTile(const GuestTarget& target, const GuestResolveCopy& copy, std::string& error)
    {
        if (source.initialized)
            return resolveTile(target, copy, error);
        const auto fail = [&](std::string detail)
        {
            source.partial = true;
            error          = detail + " (" + copyText(target, copy) + ")";
            return std::uint64_t(0);
        };
        const auto& tiling = *target.tiling;
        if (epoch.tiled)
            return fail("tiled_texture_pass_after_scanout");
        if (!epoch.tiledClear || !epoch.tiledClearTiling)
            return fail("tiled_texture_pass_uncleared");
        const auto clear = std::find_if(epoch.work.begin(), epoch.work.end(), [&](const Work& item)
                                        {
                                            return item.clear && item.sequence == *epoch.tiledClear;
                                        });
        // A segment already rendered the clear as presented work.
        if (clear == epoch.work.end())
            return fail("tiled_texture_pass_segmented");
        const auto& issued = *epoch.tiledClearTiling;
        if (issued.count != tiling.count || issued.rects != tiling.rects)
            return fail("tiled_texture_pass_tiles");
        const auto    first = clear->sequence;
        SourceContent next;
        next.lineage        = newLineage();
        auto value          = *clear->clear;
        value.rectangle     = scissorOf(tiling.rects[0]);
        next.clears         = { value };
        next.depthEdramBase = clear->target.depthEdramBase;
        next.tiling         = tiling;
        next.clearedSlots   = 1;
        next.depthCleared   = value.depth && value.stencil && next.depthEdramBase != kNoDepthEdramBase;
        next.depthDefined   = value.depth && next.depthEdramBase != kNoDepthEdramBase;
        next.stencilDefined = value.stencil && next.depthEdramBase != kNoDepthEdramBase;
        for (const auto& item : epoch.work)
            if (item.sequence > first && (item.resolve || item.clear || item.target != clear->target))
                return fail(std::format("tiled_texture_pass_form: operation {} after clear {}", item.sequence, first));
        // Source content outlives the epoch whose resolves its aliases name.
        for (const auto& item : epoch.work)
            if (item.sequence > first && std::ranges::any_of(item.draw.recipe().textures, [](const NativeDrawReplayTexture& texture)
                                                             {
                                                                 return texture.frameTextureId >= kGraphTextureBase;
                                                             }))
                return fail(std::format("tiled_texture_pass_alias: operation {}", item.sequence));
        std::vector<Work> kept;
        for (auto& item : epoch.work)
        {
            if (item.sequence < first)
                kept.push_back(std::move(item));
            else if (item.sequence > first)
                next.draws.push_back(std::move(item.draw));
        }
        epoch.work = std::move(kept);
        epoch.tiledClear.reset();
        epoch.tiledClearTiling.reset();
        // The tiled clear needed no presented work before it, and the rest
        // left presentation.
        presentedWork   = false;
        tilingUndefined = depthUndefined = stencilUndefined = true;
        colorAliased.clear();
        depthAliased.clear();
        source.initialized = true;
        source.partial     = false;
        source.content     = std::make_shared<SourceContent>(std::move(next));
        return resolveTile(target, copy, error);
    }
};

namespace
{

constexpr std::size_t kSubmittedHistory = 64;

} // namespace

std::uint64_t CpuCompletionSource::Submit(NativeCompletionSpan span, std::string& error)
{
    if (span.frame)
    {
        error = "cpu_completion_cannot_render";
        return 0;
    }
    return completed.fetch_add(1, std::memory_order_acq_rel) + 1;
}

std::uint64_t CpuCompletionSource::Completed() const
{
    return completed.load(std::memory_order_acquire);
}

bool CpuCompletionSource::WaitFor(std::uint64_t value, std::chrono::milliseconds)
{
    return Completed() >= value;
}

const char* NativeFrameStatusName(NativeFrameStatus status) noexcept
{
    switch (status)
    {
        case NativeFrameStatus::Complete:
            return "complete";
        case NativeFrameStatus::Provisional:
            return "provisional";
        case NativeFrameStatus::Partial:
            return "partial";
        case NativeFrameStatus::Empty:
            return "empty";
        case NativeFrameStatus::Failed:
            return "failed";
        case NativeFrameStatus::Stopped:
            return "stopped";
    }
    return "unknown";
}

NativeFrameAssembler::NativeFrameAssembler(NativeFrameAssemblerConfig config)
: state(std::make_unique<State>())
{
    state->config = std::move(config);
    if (state->config.surfaces)
        state->config.surfaces->SetSettle([owner = state.get()](const ResolveSurfaceLockRanges* locked)
                                          {
                                              owner->settle(locked);
                                          });
}

NativeFrameAssembler::~NativeFrameAssembler()
{
    if (state->config.surfaces)
        state->config.surfaces->SetSettle(nullptr);
}

void NativeFrameAssembler::SetGuestImageWrites(completion::GuestImageWrites writes)
{
    std::lock_guard lock(state->imageWritesMutex);
    state->imageWrites = std::move(writes);
}

void NativeFrameAssembler::Attach(NativeDrawDispatchConfig& dispatch)
{
    dispatch.numberOperation = [this]
    {
        return NumberOperation();
    };
    dispatch.mode  = state->config.mode;
    dispatch.began = [this](const NativeDrawSubmission& submission)
    {
        Began(submission);
    };
    dispatch.admit = [this](const NativeDrawSubmission& submission)
    {
        return Admit(submission);
    };
    dispatch.committed = [this](const NativeDrawSubmission& submission)
    {
        Committed(submission);
    };
    dispatch.recorded = [this](const NativeDrawSubmission& submission, std::optional<NativeDrawReason> verdict)
    {
        Recorded(submission, verdict);
    };
}

void NativeFrameAssembler::Began(const NativeDrawSubmission& submission) noexcept
{
    auto& current     = draft;
    current           = {};
    current.owner     = this;
    current.hook      = submission.hook;
    current.caller    = submission.caller;
    current.stack     = submission.stack;
    current.arguments = submission.arguments;
    try
    {
        std::optional<GuestTiling> tiling;
        bool                       mapped = false;
        const auto&                read   = state->config.readTarget;
        const auto                 g      = submission.Graphics();
        GuestTarget                target;
        if (read)
        {
            auto& memo = targetDecode;
            if (memo && memo->g == g && wordsUnchanged(memo->words, read, state->config.viewTarget))
            {
                target = memo->target;
                tiling = memo->tiling;
                mapped = memo->mapped;
                if (memo->decoded)
                    current.decoded = target.native;
            }
            else
            {
                memo.reset();
                std::vector<std::pair<std::uint32_t, std::uint32_t>> words;
                bool                                                 complete = true;
                const GuestWordReader                                journal  = [&](std::uint32_t address, std::uint32_t& word)
                {
                    const bool readable = read(address, word);
                    if (readable)
                        words.emplace_back(address, word);
                    complete = complete && readable;
                    return readable;
                };
                std::uint32_t rt1 = 0;
                // The bound set is the draw's target. A reader without the RT1
                // word decodes RT0 alone, as before sets were decoded.
                const bool sets = g <= UINT32_MAX - 0x3384 && journal(g + 0x309C, rt1);
                if (sets ? ReadGuestTargetSet(journal, g, 0, target) : ReadGuestTarget(journal, g, 0, target))
                {
                    current.decoded = target.native;
                    mapped          = target.tiling.has_value();
                }
                if (GuestTiling value; ReadGuestTiling(journal, g, value))
                    tiling = value;
                // A decode that met an unreadable word is not kept.
                if (complete)
                {
                    std::ranges::sort(words);
                    words.erase(std::unique(words.begin(), words.end()), words.end());
                    memo = TargetDecode{ g, std::move(words), current.decoded.has_value(), mapped, target, tiling };
                }
            }
            current.tiling = tiling;
        }
        if (current.decoded && !mapped && current.decoded->extraColorTargets)
        {
            bool ended = false;
            {
                std::lock_guard lock(state->mutex);
                const auto&     set = state->endedSet;
                ended               = set && sameStorage(set->target.native, *current.decoded) &&
                                      std::equal(target.ranges.begin(), target.ranges.begin() + target.colorCount, set->target.ranges.begin());
            }
            if (ended)
            {
                // Drop dead writes only when G+0x2934/G+0x28DC disable outputs;
                // G+0x2ABC/G+0x2ABE bit 6 may select VS; G+0x2A48/G+0x2A4C stay
                // 2/0.
                std::uint32_t depthControl = 0, colorMask = 0, modes = 0, sampleControl = 0, sampleAddress = 0;
                if (!read(g + 0x2934, depthControl) || !read(g + 0x28DC, colorMask) || !read(g + 0x2ABC, modes) ||
                    !read(g + 0x2A48, sampleControl) || !read(g + 0x2A4C, sampleAddress))
                    current.detail = "ended_set_state_unreadable: set " + targetText(*current.decoded);
                else if ((depthControl & 7) || (colorMask >> (4 * target.colorCount)))
                    current.detail = std::format("ended_set_write_unsupported: depth_control {:08X} color_mask {:#x}", depthControl, colorMask);
                else if (modes & 0x40004000U)
                    current.detail = std::format("ended_set_conditional_or_recorded_shader: {:08X}", modes);
                else if (sampleControl != 2 || sampleAddress)
                    current.detail = std::format("ended_set_query_state: sample_count_ctl {:08X} addr {:08X}", sampleControl, sampleAddress);
                else
                {
                    // The registers bound only attachments; the shaders bound
                    // every other effect, such as a memory export.
                    NativeDrawShaderWords words;
                    std::string           error;
                    bool                  named = false;
                    try
                    {
                        named = state->config.readShaders && state->config.readShaders(submission, words, error);
                    }
                    catch (const std::exception& exception)
                    {
                        error = exception.what();
                    }
                    if (!named)
                        current.detail = error.empty() ? "ended_set_shader_unreadable" : "ended_set_shader_unreadable: " + error;
                    else if (std::none_of(kEndedSetForms.begin(), kEndedSetForms.end(), [&](const EndedSetForm& form)
                                          {
                                              return form.hook == submission.hook && form.caller == submission.caller &&
                                                     form.vertex == words.vertex && form.pixel == words.pixel;
                                          }))
                        current.detail = std::format("ended_set_shader_unpinned: vs {} ps {}", words.vertex, words.pixel);
                    else
                    {
                        current.admitted      = true;
                        current.endedSetWrite = true;
                    }
                }
                return;
            }
        }
        current.target = target;
        current.mapped = mapped;
        // Its geometry exists only once the caller fills and commits it.
        if (NativeDrawDefersToCommit(submission.hook, submission.caller))
        {
            current.deferred = true;
            return;
        }
    }
    catch (const std::exception& exception)
    {
        current.admitted = false;
        assign(current.detail, exception.what());
        return;
    }
    catch (...)
    {
        current.admitted = false;
        assign(current.detail, "frame_draw_builder_failed");
        return;
    }
    build(submission);
}

void NativeFrameAssembler::Committed(const NativeDrawSubmission& submission) noexcept
{
    auto& current = draft;
    if (!current.Matches(this, submission) || !current.deferred)
        return;
    current.deferred = false;
    build(submission);
}

void NativeFrameAssembler::build(const NativeDrawSubmission& submission) noexcept
{
    auto& current = draft;
    try
    {
        const auto& read   = state->config.readTarget;
        const auto  g      = submission.Graphics();
        const auto& target = current.target;
        const auto& tiling = current.tiling;
        const bool  mapped = current.mapped;
        if (!state->config.build)
            return;

        // ResolvedTexture binds only for the draw this build makes.
        struct Building
        {
            Draft& draft;

            ~Building()
            {
                draft.building = false;
            }
        } building{ current };

        if (state->aliasing())
        {
            // Only resolvePresentedTile moves draws after the open tiled clear;
            // later draws cannot join a prior pass.
            std::lock_guard lock(state->mutex);
            current.tiledPass  = !state->presentedWork || (state->epoch.tiledClear && !state->epoch.tiled);
            current.buildEpoch = state->epochs + 1;
        }
        current.building  = true;
        current.depthView = false;
        current.admitted  = state->config.build(submission, current.draw, current.detail);
        if (current.depthView && state->config.surfaces)
            state->config.surfaces->NoteDepthConsumer(current.draw.pixelShaderHash, submission.caller);
        if (current.admitted && !fitsTarget(current.draw))
        {
            current.admitted = false;
            current.detail   = "frame_target_mismatch: draw_form " + targetText(NativeRenderFrameDrawTarget(current.draw));
        }
        else if (current.admitted)
        {
            const auto                   shape = NativeRenderFrameDrawTarget(current.draw);
            std::optional<std::uint32_t> mainBase;
            bool                         undefined = false;
            {
                std::lock_guard lock(state->mutex);
                mainBase  = state->mainBase;
                undefined = state->tilingUndefined && state->undefinedByScanout;
            }
            // Presented-shape draws keep their prior single-target route;
            // off-screen or MRT draws require decoded targets.
            if (current.decoded && sameShape(*current.decoded, shape))
                current.route = current.decoded;
            else if ((current.decoded && current.decoded->extraColorTargets) || !presentable(shape))
            {
                current.admitted = false;
                current.detail   = current.decoded ? std::format("frame_target_mismatch: decoded {} draw {}", targetText(*current.decoded), targetText(shape))
                                                   : "draw_target_unreadable: draw " + targetText(shape);
            }
            // A readable decode elsewhere means the guest draws into another
            // target, so the presented target would get a draw it never had.
            // While no base is set, the first decode names the presented one.
            else if (current.decoded && mainBase && current.decoded->edramBase != *mainBase)
            {
                current.admitted = false;
                current.detail   = std::format("frame_target_mismatch: presented base {:03X} decoded {}", *mainBase, targetText(*current.decoded));
            }
            // Unread predicate or layout words leave the draw's tile coverage
            // unproven, and a depth-disabled draw would pass without them.
            if (current.admitted && state->config.readTarget && !tiling)
            {
                current.admitted = false;
                current.detail   = "draw_tiling_unreadable: draw " + targetText(shape);
            }
            // The host renders the logical frame once, so a draw limited to
            // some tiles would land on all of them.
            else if (current.admitted && tiling && !coversEveryTile(*tiling))
            {
                current.admitted = false;
                current.detail   = std::format("draw_predicated {:#x}", tiling->predicateMask);
            }
            // Depth work must hit the depth surface its pass holds: the
            // presented one, or the one the off-screen source was cleared with.
            if (current.admitted && (current.draw.depth.enabled || current.draw.stencil.enabled))
            {
                GuestDepthSurface depth;
                if (!tiling || !ReadGuestDepthSurface(state->config.readTarget, submission.Graphics(), *tiling, depth))
                {
                    current.admitted = false;
                    current.detail   = "draw_depth_unreadable: draw " + targetText(shape);
                }
                else
                {
                    std::lock_guard lock(state->mutex);
                    const auto&     source  = state->source;
                    const bool      matches = (mapped == depth.tiled) && depth.width == shape.width && depth.height == shape.height &&
                                              depth.sampleCount == shape.sampleCount &&
                                              (presentable(shape) ? state->mainDepthBase == depth.edramBase
                                                                  : !source.initialized || source.target != current.route ||
                                                                        source.content->depthEdramBase == depth.edramBase);
                    // The host starts an off-screen pass's depth at the far
                    // clear, so a test needs the planes its clear defined.
                    const auto* content   = !presentable(shape) && source.initialized && source.target == current.route ? source.content.get() : nullptr;
                    const bool  undefined = content && ((current.draw.depth.enabled && current.draw.depth.compare != 8 && !content->depthDefined) ||
                                                        (current.draw.stencil.enabled && !content->stencilDefined));
                    if (matches && undefined)
                    {
                        current.admitted = false;
                        current.detail   = std::format("offscreen_depth_undefined: depth {:03X} draw {}", depth.edramBase, targetText(shape));
                    }
                    else if (!matches)
                    {
                        current.admitted = false;
                        current.detail   = std::format("frame_depth_target_mismatch: depth {:03X} {}x{}x{}{} presented depth {} draw {}",
                                                       depth.edramBase,
                                                       depth.width,
                                                       depth.height,
                                                       depth.sampleCount,
                                                       depth.tiled ? " tiled" : "",
                                                       state->mainDepthBase ? std::format("{:03X}", *state->mainDepthBase) : std::string("none"),
                                                       targetText(shape));
                    }
                    else
                        current.depthBase = depth.edramBase;
                }
            }
            // Recorded holds such a draw for the next clear to cover.
            if (current.admitted && undefined && current.decoded && !mapped && tiling && !tiling->Active() && !tiling->predicated)
            {
                PresentedWrite write;
                current.presentedWriteRefusal = boundPresentedWrite(read, g, target, *tiling, write);
                if (current.presentedWriteRefusal.empty())
                    current.presentedWrite = write;
            }
            // Recorded lets such a draw define an uncleared off-screen target.
            // A tiled pass (K17) draws once per tile, so the draw stands for
            // the logical target only when its tiles cover that screen.
            const bool tiledTarget = mapped && tiling && tiling->Maps() && tilesCoverScreen(*tiling) && current.route &&
                                     !current.route->extraColorTargets && tiling->screenWidth == current.route->width &&
                                     tiling->screenHeight == current.route->height;
            if (current.admitted && current.route && !presentable(*current.route) && (!mapped || tiledTarget) && state->config.coverage)
                if (const auto covered = state->config.coverage(submission, current.draw))
                {
                    current.overwrite = overwrittenRect(current.draw, *covered);
                    if (current.overwrite && mapped)
                        current.overwriteTiling = tiling;
                }
        }
    }
    catch (const std::exception& exception)
    {
        current.admitted = false;
        assign(current.detail, exception.what());
    }
    catch (...)
    {
        current.admitted = false;
        assign(current.detail, "frame_draw_builder_failed");
    }
    if (!current.admitted)
        current.draw = {};
}

std::optional<NativeDrawReason> NativeFrameAssembler::Admit(const NativeDrawSubmission& submission) const noexcept
{
    const auto& current = draft;
    if (current.Matches(this, submission) && current.admitted)
        return std::nullopt;
    return NativeDrawReason::NoNativeRoute;
}

std::uint64_t NativeFrameAssembler::Recorded(const NativeDrawSubmission& submission, std::optional<NativeDrawReason> verdict, std::string* refusal) noexcept
{
    std::uint64_t mark    = 0;
    auto&         current = draft;
    const bool    matched = current.Matches(this, submission);
    try
    {
        std::lock_guard lock(state->mutex);
        auto&           epoch = state->epoch;
        ++epoch.coverage.attempted;
        ++state->recorded;
        mark                = submission.operationMark ? submission.operationMark : ++state->opMark;
        state->opMark       = std::max(state->opMark, mark);
        state->recordedMark = std::max(state->recordedMark, mark);
        if (!epoch.firstSequence || mark < epoch.firstSequence)
            epoch.firstSequence = mark;
        epoch.lastSequence  = std::max(epoch.lastSequence, mark);
        const auto describe = [&](const std::string& reason)
        {
            return std::format("{} from {:#010x}: {}{}{}", NativeDrawHookName(submission.hook), submission.caller, reason, matched && !current.detail.empty() ? "; " : "", matched ? current.detail : std::string{});
        };
        // Any draw but another bounded untiled one ends held writes first,
        // so the failure names the draw that may read or keep them.
        bool       held = false;
        const auto then = [&]
        {
            return std::format("then {} from {:#010x}", NativeDrawHookName(submission.hook), submission.caller);
        };
        if (epoch.presentedWrites && !(matched && !verdict && current.admitted && current.presentedWrite))
            state->endPresentedWrites(then());
        // The guest thread that drew it, which a sequencer's record is not.
        const auto thread = submission.thread != std::thread::id{} ? submission.thread : std::this_thread::get_id();
        if (!epoch.thread)
            epoch.thread = thread;
        else if (*epoch.thread != thread && epoch.failure.empty())
            epoch.failure = describe("second_draw_thread: " + threadName(*epoch.thread) + " and " + threadName(thread));
        // Backstop for a record that arrives after its swap closed the epoch.
        if (submission.sequence <= state->closedSequence && epoch.failure.empty())
            epoch.failure = describe("draw_crossed_swap");
        // An admitted draw that bound a resolve of an epoch a swap closed
        // during its build samples a texture no frame of this epoch re-records.
        if (matched && !verdict && current.admitted && current.aliasEpoch && current.aliasEpoch != state->epochs + 1 && epoch.failure.empty())
            epoch.failure = describe(std::format("alias_epoch_crossed: bound in epoch {}", current.aliasEpoch));
        if (!verdict)
        {
            ++epoch.coverage.admitted;
            if (matched && current.admitted && current.endedSetWrite)
            {
                // Dead color writes: a later resolve of this storage finds no
                // modeled source, and presented work after the MRT pass already
                // needs full color and depth clears (tilingUndefined, depthUndefined, stencilUndefined).
                const auto& target = *current.decoded;
                if (state->scanoutFrozen(target) && epoch.failure.empty())
                    epoch.failure = describe("tiled_scanout_in_progress: ended set " + targetText(target));
                state->touch(target);
                state->source.partial = true;
            }
            else if (matched && current.admitted)
            {
                const auto target = current.route.value_or(Target{ kOutputWidth, kOutputHeight, 4, NativeDrawReplayTargetFormat::Rgba8, state->mainBase.value_or(current.decoded ? current.decoded->edramBase : 0) });
                if (state->config.observeDraw && state->diagnosticDraws < 4)
                {
                    const auto ordinal = ++state->diagnosticDraws;
                    try
                    {
                        state->config.observeDraw(ordinal, submission, current.draw);
                    }
                    catch (...)
                    {
                        // Diagnostics must not change admission or completion.
                    }
                }
                state->touch(target);
                auto& source = state->source;
                // Tile 0 was already scanned out, so later work on the
                // presented EDRAM, whatever its shape, would change the guest
                // frame after it was copied. Any attachment of a set may alias it.
                if (state->scanoutFrozen(target) && epoch.failure.empty() &&
                    (target.extraColorTargets || state->presented(target) || state->scannedOut(target.edramBase) ||
                     (current.depthBase && state->scannedOut(*current.depthBase))))
                    epoch.failure = describe("tiled_scanout_in_progress: target " + targetText(target));
                if (presentable(target))
                {
                    // Gamebryo tiled frame begins at 0x8250F988; 0x8250B168
                    // clears tile 0 only for nonzero mode, so prior EDRAM may
                    // show.
                    const auto extras      = std::span(current.draw.extraTargets).first(std::min<std::size_t>(current.draw.extraTargetCount, current.draw.extraTargets.size()));
                    const bool writesColor = (current.draw.blend.writeMask & 0xF) != 0 ||
                                             std::ranges::any_of(extras, [](const NativeDrawReplayExtraTarget& extra)
                                                                 {
                                                                     return (extra.blend.writeMask & 0xF) != 0;
                                                                 });
                    if (state->tilingUndefined && writesColor && epoch.failure.empty())
                    {
                        const auto& tiling  = current.tiling;
                        auto        failure = describe(!state->colorAliased.empty() && !state->undefinedByScanout ? "presented_edram_aliased: " + state->colorAliased + " target " + targetText(target)
                                                       : !state->undefinedByScanout                               ? "tiled_target_uncleared: target " + targetText(target)
                                                       : !tiling                                                  ? std::string("presented_draw_before_clear: tiling unread")
                                                       : !tiling->Active()                                        ? std::string("presented_draw_before_clear: untiled")
                                                                                                                  : std::format("presented_draw_before_clear: tiles {} predicated {} mask {:#x}", tiling->count, tiling->predicated, tiling->predicateMask));
                        // A dead write: the frame's tiled clear may still
                        // overwrite it before any tile reads it (Clear).
                        if (current.presentedWrite && state->holdPresentedWrite(*current.presentedWrite, failure, mark))
                            held = true;
                        else if (epoch.failure.empty())
                            epoch.failure = current.presentedWriteRefusal.empty() ? failure : failure + "; dead_write_unproven: " + current.presentedWriteRefusal;
                    }
                    // A decoded draw needs only the planes it uses: a depth
                    // test or write needs depth, an enabled stencil needs stencil.
                    else if (current.depthBase && epoch.failure.empty() &&
                             (current.decoded ? (current.draw.depth.enabled && state->depthUndefined) || (current.draw.stencil.enabled && state->stencilUndefined)
                                              : state->depthUndefined || state->stencilUndefined))
                        epoch.failure = describe(std::format("{}: depth {:03X} target {}", state->depthAliased.empty() ? "tiled_depth_uncleared" : "presented_edram_aliased: " + state->depthAliased, *current.depthBase, targetText(target)));
                    if (!held)
                    {
                        // An undecoded draw says nothing about the base.
                        if (current.decoded)
                            state->adoptBase(target.edramBase);
                        epoch.work.push_back({ mark, shareDraw(std::move(current.draw)), state->presentedPass(target) });
                        state->presentedWork = true;
                    }
                }
                else if (source.initialized)
                {
                    const auto& content = *source.content;
                    // A tiled MRT pass draws once every slot is cleared and
                    // before its first resolve, which starts the tile replay.
                    if (content.tiling && !content.defined && content.clearedSlots != (1U << (target.extraColorTargets + 1)) - 1)
                    {
                        if (epoch.failure.empty())
                            epoch.failure = describe(std::format("mrt_clear_incomplete: slots {:#x}", content.clearedSlots));
                    }
                    else if (content.tiling && (content.resolvedTile || content.resolvedSlots))
                    {
                        if (epoch.failure.empty())
                            epoch.failure = describe(std::format("tiled_source_resolving: tile {} slots {:#x} target {}", content.resolvedTile, content.resolvedSlots, targetText(target)));
                    }
                    // Resolves already recorded keep the content they read.
                    if (source.content.use_count() > 1)
                        source.content = std::make_shared<SourceContent>(*source.content);
                    source.content->draws.push_back(shareDraw(std::move(current.draw)));
                }
                else if (current.overwrite)
                {
                    // Its overwrite defines the uncleared target within the
                    // rectangle, whatever skipped work left there.
                    SourceContent content;
                    content.lineage = state->newLineage();
                    content.draws.push_back(shareDraw(std::move(current.draw)));
                    content.defined    = current.overwrite;
                    content.tiling     = current.overwriteTiling;
                    source.initialized = true;
                    source.partial     = false;
                    source.content     = std::make_shared<SourceContent>(std::move(content));
                    // A tiled pass's storage may alias the presented EDRAM, as
                    // a tiled MRT pass's may: the next presented work needs a
                    // full clear.
                    if (content.tiling)
                    {
                        state->tilingUndefined    = true;
                        state->undefinedByScanout = false;
                        state->colorAliased.clear();
                    }
                }
                // Any other off-screen draw over unmodeled content can never
                // be resolved, so it has no host work.
            }
            else if (epoch.failure.empty())
                epoch.failure = describe("admitted_without_native_draw");
        }
        else
        {
            // The dropped draw still wrote its target on the guest.
            if (matched && current.decoded)
                state->touch(*current.decoded);
            state->source.partial = true;
            if (state->config.mode != NativeDrawMode::Strict)
            {
                ++epoch.coverage.skipped[static_cast<std::size_t>(*verdict)];
                // Census mode skips every refusal; the ledger logs its form.
                if (!rasterOnly(*verdict) && state->config.mode == NativeDrawMode::Skip)
                {
                    if (epoch.failure.empty())
                        epoch.failure = describe(std::string(NativeDrawReasonName(*verdict)) + "_not_skippable");
                }
                else if (epoch.firstRejection.empty())
                    epoch.firstRejection = describe(NativeDrawReasonName(*verdict));
            }
            else
            {
                ++epoch.coverage.rejected[static_cast<std::size_t>(*verdict)];
                if (epoch.failure.empty())
                    epoch.failure = describe(NativeDrawReasonName(*verdict));
            }
        }
        if (!held && epoch.presentedWrites)
            state->endPresentedWrites(then());
    }
    catch (...)
    {
        // A lost record must still fail the frame rather than vanish from it.
        try
        {
            std::lock_guard lock(state->mutex);
            if (state->epoch.failure.empty())
                state->epoch.failure = "frame_record_failed";
            state->source.partial = true;
        }
        catch (...)
        {
        }
    }
    if (matched)
    {
        if (refusal && verdict)
        {
            try
            {
                *refusal = current.detail;
            }
            catch (...)
            {
                // Diagnostics must not change the record.
            }
        }
        current = {};
    }
    return mark;
}

std::uint64_t NativeFrameAssembler::NumberOperation()
{
    std::lock_guard lock(state->mutex);
    return ++state->opMark;
}

std::uint64_t NativeFrameAssembler::Clear(const GuestClear& clear, std::string& error)
{
    std::lock_guard lock(state->mutex);
    const auto&     target = clear.target.native;
    auto&           epoch  = state->epoch;
    // The guest still cleared: a later guest-memory resolve of this target
    // reports partial (held in skip mode) instead of encoding unknown content.
    // Each refusal ends with the clear's form, so one log line names it.
    const auto rejectFormed = [&](std::string detail)
    {
        state->touch(target);
        state->source.partial = true;
        state->skipClear(target, detail);
        error = std::move(detail);
        return std::uint64_t(0);
    };
    const auto reject = [&](std::string detail)
    {
        return rejectFormed(detail + " (" + clearText(clear) + ")");
    };
    // A clear of the presented target that no skip can drop.
    const auto refuse = [&](std::string detail)
    {
        detail += " (" + clearText(clear) + ")";
        if (epoch.failure.empty())
            epoch.failure = detail;
        return rejectFormed(std::move(detail));
    };
    // The guest overwrote the held writes whether or not this clear is
    // modeled below; they never reach a resolve.
    if (epoch.presentedWrites)
    {
        if (const auto gap = state->presentedWritesUncovered(clear); gap.empty())
        {
            epoch.presentedWrites.reset();
            epoch.heldFrom = 0;
        }
        else
            state->endPresentedWrites("then clear: " + gap);
    }
    const bool  depthOnly = clear.index == kGuestDepthOnlyClearIndex;
    const auto& rect      = clear.clear.rectangle;
    const bool  ended     = state->endedDepth(clear);
    if (!ended)
        state->endedSet.reset();
    // Tile 0 was already scanned out, so a clear of the presented color or
    // depth EDRAM, in any shape or form and through either attachment, changes
    // the guest frame after the copy. Any attachment of a set may alias it.
    if (state->scanoutFrozen(target) &&
        (target.extraColorTargets || state->presented(target) || (clear.clear.color && state->scannedOut(target.edramBase)) ||
         (clear.depth && (clear.clear.depth || clear.clear.stencil) && state->scannedOut(clear.depth->edramBase))))
        return refuse("tiled_scanout_in_progress");
    if (ended)
    {
        // Loading depth is cleared before batches and never read; aliased
        // presented depth stays undefined until full clears.
        if (clear.predicate || (clear.flags & ~0x30U) || clear.clear.color)
        {
            state->endedSet.reset();
            return reject("clear_form_unsupported: ended_set " +
                          failed({ { clear.predicate != 0, "predicated" }, { (clear.flags & ~0x30U) != 0, "flags" }, { clear.clear.color, "color" } }));
        }
        const auto mark     = ++state->opMark;
        state->recordedMark = std::max(state->recordedMark, mark);
        if (!epoch.firstSequence)
            epoch.firstSequence = mark;
        epoch.lastSequence = mark;
        return mark;
    }
    if (clear.predicate && clear.tiling.Active() && !depthOnly)
    {
        if (const auto form = tiledClearForm(clear); !form.empty())
            return reject("clear_form_unsupported: " + form);
        // Tile 1 skips this clear but replays the presented work before it.
        if (presentable(target) && state->presentedWork)
            return reject("clear_form_unsupported: tiled_clear_after_work");
    }
    // A depth clear stands for the logical frame only when every tile runs it.
    else if (clear.predicate && depthOnly)
        return reject(std::format("clear_form_unsupported: depth_clear_predicated {:#x}", clear.tiling.predicateMask));
    else if (clear.predicate)
        return reject(std::format("clear_form_unsupported: predicated_untiled {:#x}", clear.tiling.predicateMask));
    if (!depthOnly && clear.target.colorCount > 1)
    {
        std::uint64_t mark = 0;
        if (auto refusal = state->clearSet(clear, mark); !refusal.empty())
            return reject(std::move(refusal));
        return mark;
    }
    // An off-screen color clear may stop short of the surface's tile-rounded
    // extent, as the Hall of Glory panels' 900x675 clears of their 928x688
    // surface do; only that rectangle then holds modeled content.
    const bool            wholeTarget = !rect.left && !rect.top && rect.right == static_cast<std::int32_t>(target.width) && rect.bottom == static_cast<std::int32_t>(target.height);
    const bool            originRect  = !rect.left && !rect.top && rect.right > 0 && rect.bottom > 0 && rect.right <= static_cast<std::int32_t>(target.width) &&
                                        rect.bottom <= static_cast<std::int32_t>(target.height) && !presentable(target);
    std::optional<Target> work;
    if (depthOnly)
    {
        // Only the presented target's own depth surface is modeled. No pass
        // holds another one, and a clear-only depth pass has no known consumer.
        if (state->failure || (clear.flags & ~0x30U) || clear.clear.color || !clear.depth)
            return reject("clear_form_unsupported: depth_only " + failed({ { state->failure.has_value(), "after_failure" },
                                                                           { (clear.flags & ~0x30U) != 0, "flags" },
                                                                           { clear.clear.color, "color" },
                                                                           { !clear.depth, "no_depth_surface" } }));
        if (!presentable(target) || !state->mainBase || state->mainDepthBase != clear.depth->edramBase)
            return reject(std::format("depth_clear_target_unmodeled: {:03X}", clear.depth->edramBase));
        // Whole-depth clears cover the presented surface; stencil-only clears
        // cover the logical rectangle on every tile.
        const bool whole = !rect.left && !rect.top && rect.right == static_cast<std::int32_t>(kOutputWidth) && rect.bottom == static_cast<std::int32_t>(kOutputHeight);
        if (!whole && (clear.clear.depth || !clear.clear.stencil))
            return reject(std::format("clear_form_unsupported: depth_clear_rect {},{},{},{}", rect.left, rect.top, rect.right, rect.bottom));
        work = state->presentedPass(Target{ kOutputWidth, kOutputHeight, 4, NativeDrawReplayTargetFormat::Rgba8, *state->mainBase });
    }
    else if (state->failure || clear.index != 0 || (clear.flags & ~0x3FU) || !clear.clear.color || (!wholeTarget && !originRect))
        return reject("clear_form_unsupported: color " +
                      failed({ { state->failure.has_value(), "after_failure" },
                               { clear.index != 0, "index" },
                               { (clear.flags & ~0x3FU) != 0, "flags" },
                               { !clear.clear.color, "no_color" },
                               { !wholeTarget && !originRect, "rect" } }));
    else if ((clear.clear.depth || clear.clear.stencil) && !clear.depth)
        return reject("clear_form_unsupported: depth_surface_undecoded");
    // The host replays k_2_10_10_10 and k_16_16 targets without a depth surface.
    else if ((clear.clear.depth || clear.clear.stencil) && target.format != NativeDrawReplayTargetFormat::Rgba8)
        return reject("clear_form_unsupported: depth_clear_packed_target");
    else if (presentable(target))
    {
        work = state->presentedPass(target);
        // The clear covers the whole target, so a fresh pass on a new depth
        // surface loses nothing the model keeps.
        if (clear.depth)
            work->depthEdramBase = clear.depth->edramBase;
    }
    if (depthOnly && state->tilingUndefined)
        return refuse("tiled_target_uncleared");
    const auto            depthBase = clear.depth ? clear.depth->edramBase : kNoDepthEdramBase;
    const auto            value     = hostClear(clear.clear, target.format);
    NativeRenderFrame     validation;
    NativeRenderFramePass pass;
    pass.target                = work.value_or(target);
    pass.target.depthEdramBase = work ? work->depthEdramBase : depthBase;
    pass.presented             = work.has_value();
    pass.clears.push_back(value);
    // Only producer frames replay off-screen content, and they admit the
    // signed target the presented prefix refuses; the clear's own checks
    // do not depend on the format.
    if (!work && pass.target.format == NativeDrawReplayTargetFormat::Rg16Snorm)
        pass.target.format = NativeDrawReplayTargetFormat::Rgba8;
    validation.passes.push_back(std::move(pass));
    if (!ValidateNativeRenderFramePrefix(validation, error))
        return reject("clear_form_unsupported: " + error);
    const auto mark     = ++state->opMark;
    state->recordedMark = std::max(state->recordedMark, mark);
    if (!epoch.firstSequence)
        epoch.firstSequence = mark;
    epoch.lastSequence = mark;
    auto& source       = state->source;
    if (depthOnly)
    {
        // Depth alone leaves the presented color as it was. A full clear
        // defines its planes; a stencil rectangle defines nothing whole.
        if (!rect.left && !rect.top && rect.right == static_cast<std::int32_t>(kOutputWidth) && rect.bottom == static_cast<std::int32_t>(kOutputHeight))
            state->definePresentedDepth(clear.clear.depth, clear.clear.stencil);
        state->touch(Target{ kOutputWidth, kOutputHeight, 4, NativeDrawReplayTargetFormat::Rgba8, *state->mainBase });
        epoch.work.push_back({ mark, {}, *work, clear.clear });
        state->presentedWork = true;
        return mark;
    }
    state->touch(target);
    source.partial = false;
    if (work)
    {
        // A tiled texture pass over the presented EDRAM ends here.
        source.initialized = false;
        source.content.reset();
        state->adoptBase(target.edramBase);
        if (const auto& range = clear.target.ranges[0]; range.end > range.begin)
            state->mainRange = range;
        if (clear.depth)
        {
            if (state->mainDepthBase != depthBase || clear.depth->range.end <= clear.depth->range.begin)
                state->mainDepthRange.reset();
            if (clear.depth->range.end > clear.depth->range.begin)
                state->mainDepthRange = clear.depth->range;
            state->mainDepthBase = depthBase;
        }
        state->tilingUndefined = false;
        state->colorAliased.clear();
        if (clear.depth)
            state->definePresentedDepth(clear.clear.depth, clear.clear.stencil);
        epoch.work.push_back({ mark, {}, *work, clear.clear });
        state->presentedWork = true;
        if (clear.predicate)
        {
            epoch.tiledClear       = mark;
            epoch.tiledClearTiling = clear.tiling;
        }
    }
    else
    {
        // Off-screen content is replayed by each resolve that reads it. The
        // host clears the same rectangle, and resolves must lie inside it:
        // depth, stencil and color outside it stay as the guest left them.
        source.initialized             = true;
        source.content                 = std::make_shared<SourceContent>(SourceContent{ { value }, {}, depthBase });
        source.content->lineage        = state->newLineage();
        source.content->depthDefined   = clear.depth && clear.clear.depth;
        source.content->stencilDefined = clear.depth && clear.clear.stencil;
        if (!depthOnly && !wholeTarget)
            source.content->defined = rect;
        // Only the tiles the rectangle touches change; a tiled or
        // pitchless surface counts as its whole range.
        const auto cause   = "off-screen clear " + targetText(target);
        const auto touched = [&](const GuestEdramRange& range, std::uint32_t pitch, std::uint32_t samples, bool tiled)
        {
            const auto tiles = tiled ? std::nullopt : GuestEdramRectRange(range, pitch, samples, rect.left, rect.top, rect.right, rect.bottom);
            return tiles.value_or(range);
        };
        if (const auto tiles = touched(clear.target.ranges[0], clear.target.surfacePitch, target.sampleCount, clear.target.tiling.has_value()); tiles.end > tiles.begin || tiles == clear.target.ranges[0])
            state->aliasPresented(target.edramBase, tiles, cause);
        if (clear.depth && (clear.clear.depth || clear.clear.stencil))
            if (const auto tiles = touched(clear.depth->range, clear.depth->pitch, clear.depth->sampleCount, clear.depth->tiled); tiles.end > tiles.begin || tiles == clear.depth->range)
                state->aliasPresented(clear.depth->edramBase, tiles, cause);
    }
    return mark;
}

namespace
{

// Settles the surface registry once the scope's locks are released, so the
// images entries ended owing land before a later resolve's.
struct SettleOnExit
{
    const std::shared_ptr<ResolveSurfaceRegistry>& surfaces;

    ~SettleOnExit()
    {
        if (surfaces)
            surfaces->Settle();
    }
};

} // namespace

std::uint64_t NativeFrameAssembler::Resolve(const GuestTarget& target, const GuestResolveCopy& copy, bool frontbuffer, bool& partial, std::string& error)
{
    const SettleOnExit settled{ state->config.surfaces };
    std::unique_lock   lock(state->mutex);
    const auto&        native = target.native;
    auto&              source = state->source;
    const bool         reads  = source.target && *source.target == native;
    partial                   = !frontbuffer && reads && source.partial;

    state->endPresentedWrites("then resolve");
    // The guest wrote the destination whatever the verdict below; a modeled
    // scanout records its own form below. A frontbuffer tile continues the
    // open tiled scanout or fails the epoch.
    state->applyWrites();
    state->dropScanouts(&copy, !(frontbuffer && target.tiling));
    state->aliasResolved(copy.destBase, resolveDestEnd(copy));

    // A resolve that records no surface entry still wrote the destination.
    struct SurfaceOverwrite
    {
        NativeFrameAssembler::State& state;
        std::uint64_t                begin, end;

        ~SurfaceOverwrite()
        {
            if (state.config.surfaces && !state.surfaceNoted)
                state.config.surfaces->Overwritten(begin, end);
        }
    } surfaceOverwrite{ *state, copy.destBase, resolveDestEnd(copy) };

    state->surfaceNoted = false;

    // The guest cleared the source whatever the verdict below, so every exit
    // drops it; host replay of a later resolve would read the pre-clear bytes.
    struct ClearAfter
    {
        NativeFrameAssembler::State& state;
        const Target&                target;
        bool                         enabled;

        ~ClearAfter()
        {
            if (enabled)
                state.clearedByResolve(target);
        }
    };

    // Tiled scanout clear bits initialize EDRAM only for the next tile; swap
    // uses them as that tile's background. Uncleared guard applies to target
    // afterward; off-screen tiles use next-tile backgrounds too.
    const bool tiled = frontbuffer && target.tiling;
    ClearAfter clearAfter{ *state, native, !tiled && !(!frontbuffer && target.tiling) && (copy.copyControl & kCopyClearBits) != 0 };

    // The presented target is a swap's frame or, under predicated tiling, a
    // tiled texture pass whose per-tile resolves make textures.
    const bool presentedTiles = !frontbuffer && presentable(native) && target.tiling;
    if (!state->failure && !frontbuffer && presentable(native) && !presentedTiles)
    {
        error = "resolve_presented_unmodeled (" + copyText(target, copy) + ")";
        return 0;
    }
    if (state->failure || (!frontbuffer && (!reads || (!source.initialized && !presentedTiles))))
    {
        error = std::format("resolve_source_unmodeled: {} ({})",
                            failed({ { state->failure.has_value(), "after_failure" },
                                     { !frontbuffer && !reads, "other_source" },
                                     { !frontbuffer && reads && !source.initialized && !presentedTiles, "uninitialized" } }),
                            copyText(target, copy));
        return 0;
    }
    if (partial)
    {
        error = "resolve_source_partial (" + copyText(target, copy) + ")";
        return 0;
    }
    // The swap presents the assembled presented target, so a scanout copy of
    // another base or format would present content the guest never scanned
    // out. The extent is not compared: the presented target is named by base.
    if (frontbuffer && (native.format != NativeDrawReplayTargetFormat::Rgba8 || (state->mainBase && native.edramBase != *state->mainBase)))
    {
        error = std::format("resolve_frontbuffer_source: {}x{}x{} format {} at {:#x}, presented base {:#x}", native.width, native.height, native.sampleCount, static_cast<unsigned>(native.format), native.edramBase, state->mainBase.value_or(native.edramBase));
        return 0;
    }
    auto&      epoch  = state->epoch;
    const auto refuse = [&](std::string detail)
    {
        error = std::move(detail);
        if (epoch.failure.empty())
            epoch.failure = error;
        return std::uint64_t(0);
    };
    if (frontbuffer && state->tilingUndefined && (!tiled || !epoch.tiled))
        return refuse((state->colorAliased.empty() || state->undefinedByScanout ? std::string("tiled_target_uncleared") : "presented_edram_aliased: " + state->colorAliased) + " (" +
                      copyText(target, copy) + ")");
    // Submit graph segments before guest image reads so GPU work overlaps guest
    // execution. Each tile retains the content it read, so cuts between tiles
    // are self-contained; unready sources leave resolve submission to the read.
    const auto submitsEagerly = [&](std::uint64_t mark)
    {
        return mark && !frontbuffer && state->config.resolveGraph && state->config.eagerGraphSegments && state->config.completion && !epoch.work.empty() &&
               epoch.work.back().sequence == mark && state->graphable(epoch.work.back()) && state->config.completion->ReadyForSegment();
    };
    // Runs the cut unlocked, as a read's flush does; clearAfter is settled first.
    const auto submitted = [&](std::uint64_t mark)
    {
        if (submitsEagerly(mark))
        {
            lock.unlock();
            (void)FlushThrough(mark);
        }
        return mark;
    };
    if (presentedTiles)
        return submitted(state->resolvePresentedTile(target, copy, error));
    if (!frontbuffer && target.tiling)
        return submitted(state->resolveTile(target, copy, error));
    if (frontbuffer && (copy.destInfo & ~kScanoutDestSwap) != kScanoutDestForm)
        return refuse(std::format("resolve_frontbuffer_dest_unsupported: {:08X}", copy.destInfo));
    if (frontbuffer && copy.destPitch != kScanoutDestPitch)
        return refuse(std::format("resolve_frontbuffer_pitch_unsupported: {:08X}", copy.destPitch));
    // An untiled scanout replaces the frontbuffer's form, so it must be the
    // whole presented frame resolved as the host presents it.
    if (frontbuffer && !tiled &&
        (copy.rectX || copy.rectY || copy.rectWidth != kOutputWidth || copy.rectHeight != kOutputHeight || copy.destX || copy.destY ||
         (copy.copyControl & ~kCopyClearBits) != kScanoutControl))
        return refuse(std::format("resolve_frontbuffer_form: rect {},{},{},{} point {},{} control {:08X}", copy.rectX, copy.rectY, copy.rectX + copy.rectWidth, copy.rectY + copy.rectHeight, copy.destX, copy.destY, copy.copyControl));
    if (tiled)
    {
        // Each tile copies its own rectangle to the same place in the
        // frontbuffer; together they scan out the logical target once.
        if (!epoch.tiled)
        {
            const auto end = resolveDestEnd(copy);
            epoch.tiled    = TiledScanout{ *target.tiling, native, copy.destBase, copy.destInfo, end, state->watch(copy.destBase, end) };
        }
        auto&      scanout = *epoch.tiled;
        const auto index   = scanout.received;
        // One scanout has one form; separate scanouts may differ.
        if (copy.destBase != scanout.destBase || copy.destInfo != scanout.destInfo)
            return refuse(std::format("resolve_frontbuffer_dest_mismatch: tile {} {:08X} at {:08X} after {:08X} at {:08X}", index, copy.destInfo, copy.destBase, scanout.destInfo, scanout.destBase));
        if (scanout.target != native || scanout.tiling.count != target.tiling->count || scanout.tiling.rects != target.tiling->rects ||
            index >= scanout.tiling.count || !tilesCoverScreen(scanout.tiling))
            return refuse(std::format("tiled_scanout_form: tile {} of {}", index, target.tiling->count));
        const auto& rect = scanout.tiling.rects[index];
        const bool  last = index + 1 == scanout.tiling.count;
        // 0x8250AC90 predicates tile i's resolve to that tile alone (3 << 2i).
        if (target.tiling->predicated != 1 || target.tiling->predicateMask != 3U << (2 * index))
            return refuse(std::format("tiled_scanout_form: tile {} predicate {} mask {:#x}", index, target.tiling->predicated, target.tiling->predicateMask));
        if (copy.rectX != rect.x0 || copy.rectY != rect.y0 || copy.rectWidth != rect.x1 - rect.x0 || copy.rectHeight != rect.y1 - rect.y0 ||
            copy.destX != rect.x0 || copy.destY != rect.y0 || (copy.copyControl & ~kCopyClearBits) != kScanoutControl ||
            (copy.copyControl & kCopyClearBits) != (last ? 0 : kCopyClearBits))
            return refuse(std::format("tiled_scanout_form: tile {} rect {},{},{},{} point {},{} control {:08X}", index, copy.rectX, copy.rectY, copy.rectX + copy.rectWidth, copy.rectY + copy.rectHeight, copy.destX, copy.destY, copy.copyControl));
        if (!last)
        {
            // Every tile renders into the same EDRAM, so the next tile starts
            // from this resolve's clear, which spans only this tile's extent.
            const auto& next = scanout.tiling.rects[index + 1];
            if (next.x1 - next.x0 > rect.x1 - rect.x0 || next.y1 - next.y0 > rect.y1 - rect.y0)
                return refuse(std::format("tiled_background_extent: tile {} is {}x{}, tile {} cleared {}x{}", index + 1, next.x1 - next.x0, next.y1 - next.y0, index, rect.x1 - rect.x0, rect.y1 - rect.y0));
            scanout.backgrounds.push_back(scanoutClear(copy, next));
        }
        ++scanout.received;
        // The clear bits also cleared the presented depth for the next tile.
        state->tilingUndefined = state->depthUndefined = state->stencilUndefined = true;
        state->undefinedByScanout                                                = true;
        state->depthAliased.clear();
    }
    // Off-screen modeling admits only one copied supported surface inside
    // modeled content without clear bits; other forms name a refusal.
    auto resolved = copy;
    if (!frontbuffer)
    {
        std::string form;
        const auto  select  = (copy.copyControl >> 4) & 7;
        const auto  defined = source.content && source.content->defined ? *source.content->defined
                                                                        : NativeDrawReplayScissor{ 0, 0, static_cast<std::int32_t>(native.width), static_cast<std::int32_t>(native.height) };
        if (native.extraColorTargets)
            form = std::format("resolve_form_unsupported: format {} targets {}", static_cast<unsigned>(native.format), native.extraColorTargets + 1);
        else if (copy.rectX || copy.rectY || copy.destX || copy.destY || defined.left || defined.top || !copy.rectWidth || !copy.rectHeight ||
                 copy.rectWidth > static_cast<std::uint32_t>(defined.right) || copy.rectHeight > static_cast<std::uint32_t>(defined.bottom))
            form = std::format("resolve_form_unsupported: rect {},{},{}x{} point {},{} of {},{},{},{}", copy.rectX, copy.rectY, copy.rectWidth, copy.rectHeight, copy.destX, copy.destY, defined.left, defined.top, defined.right, defined.bottom);
        else if ((copy.copyControl & ~0x70U) != 0x100000 || (select && (native.sampleCount == 1 || (select != 1 && select != 4))))
            form = std::format("resolve_form_unsupported: control {:08X} samples {}", copy.copyControl, native.sampleCount);
        else if (auto full = state->pendingImagesFull())
            form = std::move(*full);
        if (!form.empty())
        {
            error = std::move(form);
            return 0;
        }
        resolved.packing = colorPacking(native.format, false);
    }
    const auto mark     = ++state->opMark;
    state->recordedMark = std::max(state->recordedMark, mark);
    if (!epoch.firstSequence)
        epoch.firstSequence = mark;
    epoch.lastSequence = mark;
    if (!frontbuffer)
    {
        epoch.work.push_back({ mark, {}, native, {}, resolved, source.content });
        state->notePendingImage(mark, resolved);
        if (clearAfter.enabled && submitsEagerly(mark))
        {
            state->clearedByResolve(native);
            clearAfter.enabled = false;
        }
        return submitted(mark);
    }
    else
    {
        // A scanout resolve is host-only; its clear bits drop the source above.
        state->touch(native);
        // Only the last tile completes a tiled scanout; the swap checks the
        // rest. A complete scanout replaces the frontbuffer's form.
        if (!tiled || epoch.tiled->received == epoch.tiled->tiling.count)
        {
            epoch.frontbuffer = true;
            ScanoutRecord record{ copy.destBase, resolveDestEnd(copy) };
            if (tiled)
            {
                record.end         = epoch.tiled->end;
                record.watchedFrom = epoch.tiled->watchedFrom;
                record.overwriter  = epoch.tiled->overwriter;
            }
            else
                record.watchedFrom = state->watch(copy.destBase, record.end);
            if (record.overwriter.empty())
                record.destInfo = copy.destInfo;
            epoch.scanout = std::move(record);
        }
        if (!state->mainBase)
            state->adoptBase(native.edramBase);
    }
    return mark;
}

completion::GuestImageTake NativeFrameAssembler::TakeGuestImage(std::uint64_t mark)
{
    if (!state->config.completion)
        return { std::nullopt, "no_completion_source", false };
    if (state->config.surfaces && state->writesGate())
        return takeHeld(mark);
    // A read that finds the source's image gone waits for this take, which
    // keeps a copy laid over guest memory until the ledger reports it written.
    const auto pending = [&](auto&& apply)
    {
        std::lock_guard lock(state->mutex);
        if (const auto found = std::ranges::find(state->pendingImages, mark, &PendingImage::mark); found != state->pendingImages.end())
            apply(*found);
    };
    pending([](PendingImage& image)
            {
                image.taking = true;
            });
    completion::GuestImageTake take;
    try
    {
        take = state->config.completion->TakeGuestImage(mark);
    }
    catch (...)
    {
        pending([](PendingImage& image)
                {
                    image.taking = false;
                });
        state->imageTaken.notify_all();
        throw;
    }
    pending([&](PendingImage& image)
            {
                if (!image.image && take.image)
                    image.image = std::make_shared<const GuestResolveImage>(*take.image);
                image.taking = false;
            });
    state->imageTaken.notify_all();
    return take;
}

completion::GuestImageTake NativeFrameAssembler::takeHeld(std::uint64_t mark)
{
    auto&                                  surfaces = *state->config.surfaces;
    completion::GuestImageTake             take;
    std::shared_ptr<const IHeldGuestImage> held;
    const auto                             find = [&]
    {
        return std::ranges::find(state->pendingImages, mark, &PendingImage::mark);
    };
    {
        std::lock_guard lock(state->mutex);
        if (const auto found = find(); found != state->pendingImages.end())
            found->taking = true;
    }
    PendingImage writing;
    auto         decision = ResolveSurfaceHold::Write;
    try
    {
        take = state->config.completion->HoldGuestImage(mark, held);
        {
            // With the registry's answer, so a settle after an ending sees this hold.
            std::lock_guard lock(state->mutex);
            const auto      found = find();
            decision              = surfaces.Holds(mark);
            const auto bytes      = held ? held->Bytes() : 0;
            const auto dropping   = surfaces.PendingDrops();
            const bool dropped    = found != state->pendingImages.end() && (found->dropped || std::ranges::find(dropping, mark) != dropping.end());
            if (dropped && !take.closing && take.error.empty())
            {
                // Dropped before this take: guest memory no longer needs the
                // image, so it ends here unencoded, as a settle's drop does,
                // and the ledger counts its record applied.
                state->pendingImageEnded(*found);
                state->pendingImages.erase(found);
                take      = {};
                take.held = true;
            }
            else if (found != state->pendingImages.end())
            {
                if (held && decision == ResolveSurfaceHold::Hold && state->heldImages < kHeldImages && state->heldBytes + bytes <= kHeldBytes)
                {
                    found->held      = std::move(held);
                    found->heldBytes = bytes;
                    found->taking    = false;
                    ++state->heldImages;
                    state->heldBytes += bytes;
                    surfaces.SetHeldBytes(state->heldBytes);
                    take.held = true;
                }
                else
                    writing = *found;
            }
        }
        if (take.held)
        {
            state->imageTaken.notify_all();
            return take;
        }
        if (held)
            take = held->Encode();
        if (take.image)
            decision == ResolveSurfaceHold::Locked ? surfaces.CountLockReadbacks(1) : surfaces.CountFlushes(1);
        // Older held images this one overwrites land first.
        if (take.image && writing.mark)
        {
            std::vector<ClaimedImage> claimed;
            {
                std::lock_guard lock(state->mutex);
                writing.image = nullptr;
                claimed       = state->claimHeld([](const PendingImage&)
                                                 {
                                               return false;
                                                 },
                                                 writing);
            }
            // One that fails to encode ends as skipped, failing the session
            // outside census mode, as a failed write does; the rest land.
            auto takes = state->encodeClaimed(claimed);
            for (std::size_t i = 0; i < claimed.size(); ++i)
            {
                if (takes[i].image)
                {
                    take.before.emplace_back(claimed[i].mark, std::move(*takes[i].image));
                    continue;
                }
                {
                    std::lock_guard lock(state->mutex);
                    if (!state->failure && state->config.mode != NativeDrawMode::Census)
                        state->failure = std::format("held_image_unwritten: op mark {}: {}", claimed[i].mark, takes[i].error);
                }
                state->imageEnded(claimed[i].mark, false);
            }
            surfaces.CountFlushes(take.before.size());
        }
    }
    catch (...)
    {
        {
            std::lock_guard lock(state->mutex);
            if (const auto found = find(); found != state->pendingImages.end())
                found->taking = false;
        }
        state->imageTaken.notify_all();
        throw;
    }
    {
        std::lock_guard lock(state->mutex);
        if (const auto found = find(); found != state->pendingImages.end())
        {
            if (!found->image && take.image)
                found->image = std::make_shared<const GuestResolveImage>(*take.image);
            found->taking = false;
        }
    }
    state->imageTaken.notify_all();
    return take;
}

void NativeFrameAssembler::GuestImageWritten(std::uint64_t mark)
{
    state->imageEnded(mark, true);
}

void NativeFrameAssembler::GuestImageSkipped(std::uint64_t mark)
{
    // Guest memory keeps its old bytes; reads stop waiting for the image.
    if (state->aliasing())
    {
        std::lock_guard alias(state->aliasMutex);
        state->aliases.Unwritten(mark, mark);
    }
    state->imageEnded(mark, false);
}

NativeResolveGraphStats NativeFrameAssembler::ResolveGraphStats() const noexcept
{
    const auto read = [&](State::GraphCount which)
    {
        return state->graphCounts[which].load(std::memory_order_relaxed);
    };
    NativeResolveGraphStats stats;
    stats.continuations = read(State::Continuation);
    stats.replays       = read(State::Replay);
    stats.notReady      = read(State::NotReady);
    stats.signed16      = read(State::Signed16);
    stats.noSource      = read(State::NoSource);
    stats.swapBatch     = read(State::SwapBatch);
    for (std::size_t which = 0; which < stats.aliasMisses.size(); ++which)
        stats.aliasMisses[which] = state->aliasMisses[which].load(std::memory_order_relaxed);
    for (std::size_t which = 0; which < stats.aliasForms.size(); ++which)
        stats.aliasForms[which] = state->aliasForms[which].load(std::memory_order_relaxed);
    stats.aliasHits       = state->aliasHits.load(std::memory_order_relaxed);
    stats.aliasReproduced = state->aliasReproduced.load(std::memory_order_relaxed);
    stats.castable        = state->config.castableGuestWords;
    return stats;
}

std::uint64_t NativeFrameAssembler::LastSubmitted() const
{
    std::lock_guard serialized(state->swapMutex);
    return state->lastFence;
}

void NativeFrameAssembler::DiscardGuestImagesThrough(std::uint64_t mark)
{
    {
        // The ledger dropped these spans; their images are never written,
        // unless in part before a failed run, so watched reads over them end.
        std::lock_guard lock(state->mutex);
        std::erase_if(state->pendingImages, [&](const PendingImage& image)
                      {
                          if (image.mark > mark || image.held)
                              return false;
                          state->pendingImageEnded(image);
                          return true;
                      });
        if (state->aliasing())
        {
            std::lock_guard alias(state->aliasMutex);
            state->aliases.Unwritten(0, mark);
        }
    }
    if (state->config.completion)
        state->config.completion->DiscardGuestImagesThrough(mark);
}

bool NativeFrameAssembler::ReadResolved(std::uint32_t                                     address,
                                        std::uint32_t                                     size,
                                        const std::function<std::vector<std::uint8_t>()>& read,
                                        std::vector<std::uint8_t>&                        bytes,
                                        std::string&                                      error,
                                        ReadResolvedTrace*                                trace)
{
    error.clear();
    const std::uint64_t begin = address, end = begin + size;
    const auto          overlapping = [&]
    {
        std::vector<std::pair<std::uint64_t, std::shared_ptr<const GuestResolveImage>>> found;
        std::lock_guard                                                                 lock(state->mutex);
        for (const auto& image : state->pendingImages)
            if (image.begin < end && begin < image.end)
                found.emplace_back(image.mark, image.image);
        return found;
    };
    try
    {
        // Held images over the range land before the read; one whose drop
        // a store queued never does.
        state->flushHeld(begin, end);
        for (const auto& [mark, image] : overlapping())
        {
            if (trace && !trace->overlapped)
            {
                trace->overlapped = true;
                trace->mark       = mark;
            }
            if (image)
                continue;
            // A dropped image never lands: the read takes guest memory.
            if (state->droppingAt(mark))
                continue;
            // Held by a take since the flush above: it lands now.
            if (state->heldAt(mark))
            {
                state->flushHeld(begin, end);
                continue;
            }
            if (trace)
                trace->flushed = true;
            if (!state->config.completion)
            {
                error = "resolve_image_unavailable: no_completion_source";
                return false;
            }
            // The resolve's producer frame may still be in the open epoch.
            completion::DrawFlush flush;
            const auto            deadline = std::chrono::steady_clock::now() + kResolvedWait;
            while (!(flush = FlushThrough(mark)).fence && flush.retry && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!flush.fence)
            {
                error = std::format("resolve_image_unsubmitted: op mark {}{}", mark, flush.error.empty() ? "" : ": " + flush.error);
                return false;
            }
            auto             take = state->config.completion->PeekGuestImage(mark, kResolvedWait);
            std::unique_lock lock(state->mutex);
            const auto       find = [&]
            {
                return std::ranges::find(state->pendingImages, mark, &PendingImage::mark);
            };
            // A take that removed the image from the source finishes by
            // keeping a copy here or reporting it written.
            if (!take.image)
                state->imageTaken.wait_until(lock, deadline + kResolvedWait, [&]
                                             {
                                                 const auto found = find();
                                                 return found == state->pendingImages.end() || !found->taking;
                                             });
            const auto found = find();
            if (found == state->pendingImages.end() || found->image)
                continue;
            if (found->held)
            {
                lock.unlock();
                state->flushHeld(begin, end);
                continue;
            }
            if (!take.image)
            {
                error = std::format("resolve_image_unavailable: op mark {}: {}", mark, take.closing ? std::string(kNativeCompletionClosing) : take.error);
                return false;
            }
            found->image = std::make_shared<const GuestResolveImage>(std::move(*take.image));
        }
        // A retirement before snapshot is written before the read; a concurrent
        // retirement triggers another guest-memory read.
        auto images = overlapping();
        for (;;)
        {
            bytes = read();
            std::lock_guard lock(state->mutex);
            const auto      pending = [&](const auto& entry)
            {
                return std::ranges::find(state->pendingImages, entry.first, &PendingImage::mark) != state->pendingImages.end();
            };
            if (std::ranges::all_of(images, pending))
                break;
            std::erase_if(images, [&](const auto& entry)
                          {
                              return !pending(entry);
                          });
        }
        for (const auto& [mark, image] : images)
        {
            if (state->droppingAt(mark))
                continue;
            if (!image)
            {
                error = std::format("resolve_image_unavailable: op mark {}", mark);
                return false;
            }
            overlayImage(*image, begin, bytes);
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("resolve_image_unavailable: ") + exception.what();
        return false;
    }
}

bool NativeFrameAssembler::ReadResolved(std::uint32_t                                         address,
                                        std::uint32_t                                         size,
                                        const std::function<std::span<const std::uint8_t>()>& view,
                                        GuestBytes&                                           bytes,
                                        std::string&                                          error,
                                        ReadResolvedTrace*                                    trace)
{
    error.clear();
    const std::uint64_t                        begin = address, end = begin + size;
    const auto                                 key        = std::pair{ address, size };
    const bool                                 mirrored   = size != 0 && size >= state->config.mirroredReadMinBytes;
    bool                                       overlapped = false;
    std::shared_ptr<std::vector<std::uint8_t>> watched;
    {
        // With the scan, so a pending image's write cannot fall between them.
        const auto      locking = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        std::lock_guard lock(state->mutex);
        if (trace)
            trace->lockNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - locking).count());
        const auto found = std::ranges::find_if(state->pendingImages, [&](const PendingImage& image)
                                                {
                                                    return image.begin < end && begin < image.end;
                                                });
        overlapped       = found != state->pendingImages.end();
        if (trace && overlapped)
        {
            trace->overlapped = true;
            trace->mark       = found->mark;
            trace->begin      = found->begin;
            trace->end        = found->end;
        }
        if (!overlapped && !mirrored && size < kMirroredReadBytes && state->config.timing)
        {
            const auto& limits = ReadMirrorStats::kSmallReadLimits;
            const auto  slot   = std::size_t(std::ranges::upper_bound(limits, size) - limits.begin());
            ++state->smallReads[slot];
            state->smallBytes[slot] += size;
        }
        if (!overlapped && mirrored)
        {
            std::lock_guard mirror(state->readMirrorMutex);
            if (const auto found = state->readMirror.find(key); found != state->readMirror.end() && found->second.watched)
            {
                watched            = found->second.bytes;
                found->second.used = ++state->readMirrorClock;
            }
        }
    }
    if (overlapped)
    {
        std::vector<std::uint8_t> copy;
        if (!ReadResolved(address, size, [&]
                          {
                              const auto source = view();
                              return std::vector<std::uint8_t>(source.begin(), source.end());
                          },
                          copy,
                          error,
                          trace))
            return false;
        bytes = std::move(copy);
        return true;
    }
    // Without an overlapping image the read above is one copy of guest memory.
    try
    {
        const auto source = view();
        if (!mirrored || source.size() != size)
        {
            bytes = std::vector<std::uint8_t>(source.begin(), source.end());
            return true;
        }
        // No write reached the range since it compared equal under the watch.
        if (watched && watched->size() == source.size())
        {
            if (state->config.timing)
            {
                std::lock_guard lock(state->readMirrorMutex);
                ++state->readMirrorStats.hits;
                ++state->readMirrorStats.watchedHits;
                state->readMirrorStats.hitBytes += size;
                state->readMirrorStats.watchedHitBytes += size;
            }
            bytes = GuestBytes::Shared(std::move(watched));
            return true;
        }
        watched.reset();
        std::shared_ptr<std::vector<std::uint8_t>> held;
        std::uint64_t                              id = 0, writes = 0;
        bool                                       arm = false;
        {
            std::lock_guard lock(state->readMirrorMutex);
            if (const auto found = state->readMirror.find(key); found != state->readMirror.end())
            {
                held               = found->second.bytes;
                found->second.used = ++state->readMirrorClock;
                id                 = found->second.id;
                writes             = found->second.writes;
                arm                = held && state->config.watchMirror && found->second.equalRun + 1 >= found->second.armAfter;
            }
        }
        // Armed before the compare, so a write after it is reported and one
        // before it is in the bytes compared.
        const bool covered = arm && state->config.watchMirror(address, size);
        // Equal bytes are what a copy made now would hold.
        if (held && held->size() == source.size() && std::memcmp(held->data(), source.data(), source.size()) == 0)
        {
            std::lock_guard lock(state->readMirrorMutex);
            if (state->config.timing)
            {
                ++state->readMirrorStats.hits;
                state->readMirrorStats.hitBytes += size;
            }
            if (const auto found = state->readMirror.find(key); found != state->readMirror.end() && found->second.id == id && found->second.writes == writes)
            {
                if (covered && state->hostWritesInFlight == 0)
                    found->second.watched = true;
                else
                    ++found->second.equalRun;
            }
            bytes = GuestBytes::Shared(std::move(held));
            return true;
        }
        held.reset();
        std::lock_guard lock(state->readMirrorMutex);
        if (state->config.timing)
        {
            ++state->readMirrorStats.misses;
            state->readMirrorStats.missBytes += size;
        }
        auto& live  = *state->readMirrorLive;
        auto  found = state->readMirror.find(key);
        // Mirror copies come only from locked map or holder GuestBytes. count 1
        // under lock means no holder can exist/appear before unlock. Acquire
        // fence orders former holder's release/last reads before refresh.
        if (found != state->readMirror.end() && found->second.bytes.use_count() == 1)
        {
            std::atomic_thread_fence(std::memory_order_acquire);
            std::memcpy(found->second.bytes->data(), source.data(), source.size());
            found->second.used     = ++state->readMirrorClock;
            found->second.equalRun = 0;
            found->second.watched  = false;
            bytes                  = GuestBytes::Shared(found->second.bytes);
            return true;
        }
        // A buffer a holder keeps stays counted until its last holder drops it.
        // The range keeps its arming backoff.
        const auto armAfter = found != state->readMirror.end() ? found->second.armAfter : 1;
        if (found != state->readMirror.end())
            state->readMirror.erase(found);
        const auto over = [&]
        {
            return live.load(std::memory_order_relaxed) + size > state->config.mirroredReadBudget;
        };
        // Least recently read first. One eviction scans for the oldest entry;
        // a read that needs more orders the entries once instead of rescanning.
        if (over() && !state->readMirror.empty())
            state->readMirror.erase(std::ranges::min_element(state->readMirror, {}, [](const auto& item)
                                                             {
                                                                 return item.second.used;
                                                             }));
        if (over() && !state->readMirror.empty())
        {
            std::vector<decltype(state->readMirror)::iterator> order;
            order.reserve(state->readMirror.size());
            for (auto it = state->readMirror.begin(); it != state->readMirror.end(); ++it)
                order.push_back(it);
            std::ranges::sort(order, {}, [](const auto& it)
                              {
                                  return it->second.used;
                              });
            for (auto it = order.begin(); it != order.end() && over(); ++it)
                state->readMirror.erase(*it);
        }
        if (over())
        {
            bytes = std::vector<std::uint8_t>(source.begin(), source.end());
            return true;
        }
        std::unique_ptr<std::vector<std::uint8_t>> copy(new std::vector<std::uint8_t>(source.begin(), source.end()));
        live.fetch_add(size, std::memory_order_relaxed);
        std::shared_ptr<std::vector<std::uint8_t>> buffer(copy.release(), [counter = state->readMirrorLive, size](std::vector<std::uint8_t>* released)
                                                          {
                                                              delete released;
                                                              counter->fetch_sub(size, std::memory_order_relaxed);
                                                          });
        state->readMirror[key]   = { .bytes = buffer, .used = ++state->readMirrorClock, .id = ++state->readMirrorIds, .armAfter = armAfter };
        state->readMirrorLongest = std::max<std::size_t>(state->readMirrorLongest, size);
        bytes                    = GuestBytes::Shared(std::move(buffer));
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("resolve_image_unavailable: ") + exception.what();
        return false;
    }
}

std::optional<NativeResolvedTexture> NativeFrameAssembler::ResolvedTexture(const std::array<std::uint32_t, 6>& fetch, std::uint64_t opMark, NativeResolvedTextureMiss& miss) noexcept
{
    miss = NativeResolvedTextureMiss::Unmodeled;
    if (const auto& surfaces = state->config.surfaces)
    {
        // Design 3.4: any draw whose target decoded, inside this build.
        const bool                          building    = draft.owner == this && draft.building && draft.decoded;
        std::size_t                         formRefusal = kNativeAliasFormNames.size();
        ResolveSurfaceFormPair              pair;
        bool                                pending     = false;
        std::uint64_t                       unsubmitted = 0;
        std::optional<ResolveSurfaceLookup> lookup;
        try
        {
            lookup = surfaces->Find(fetch, state->config.castableGuestWords, &formRefusal, &pair, &pending, building ? &unsubmitted : nullptr);
            // The entry's resolve frames are still in the open epoch, so it
            // is not yet an import. Submitting them now, as a read of its
            // bytes would, binds the host texture without the readback.
            if (unsubmitted)
            {
                FlushThrough(unsubmitted);
                lookup = surfaces->Find(fetch, state->config.castableGuestWords, &formRefusal, &pair, &pending);
                if (lookup && std::holds_alternative<ResolveSurfaceBinding>(*lookup))
                    surfaces->CountSubmit();
            }
            if (lookup && std::holds_alternative<NativeResolvedTextureMiss>(*lookup) && std::get<NativeResolvedTextureMiss>(*lookup) == NativeResolvedTextureMiss::Form)
                surfaces->NoteFormPair(pair);
        }
        catch (...)
        {
            return std::nullopt;
        }
        if (!lookup)
        {
            if (pending)
                surfaces->CountPending();
            return std::nullopt;
        }
        const auto* binding = std::get_if<ResolveSurfaceBinding>(&*lookup);
        if (binding && building)
        {
            surfaces->CountLookup(true);
            if (binding->texture.frameTextureId & kResolveSurfaceDepthTag)
            {
                surfaces->CountDepthView();
                draft.depthView = true;
            }
            return binding->texture;
        }
        miss = binding ? NativeResolvedTextureMiss::Offscreen : std::get<NativeResolvedTextureMiss>(*lookup);
        surfaces->CountLookup(false);
        surfaces->CountMiss(miss);
        if (const auto which = static_cast<std::size_t>(miss); state->config.timing && which < state->aliasMisses.size())
            state->aliasMisses[which].fetch_add(1, std::memory_order_relaxed);
        if (state->config.timing && miss == NativeResolvedTextureMiss::Form && formRefusal < state->aliasForms.size())
            state->aliasForms[formRefusal].fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    if (!state->aliasing())
        return std::nullopt;
    // Only presented draws bind; chain reset re-records their producer, while
    // off-screen/tiled draws join source content.
    const bool                       presented = draft.owner == this && draft.building && !draft.tiledPass && draft.decoded && presentable(*draft.decoded);
    std::optional<NativeAliasLookup> lookup;
    std::size_t                      formRefusal = kNativeAliasFormNames.size();
    try
    {
        std::lock_guard lock(state->aliasMutex);
        lookup = state->aliases.Find(fetch, opMark, state->aliasEpoch, state->aliasChain, state->config.castableGuestWords, &formRefusal);
        // A draw binds resolves of the epoch tiledPass describes, the one
        // Recorded checks.
        if (lookup && presented)
            if (const auto* alias = std::get_if<NativeResolvedAlias>(&*lookup))
            {
                if (draft.buildEpoch != state->aliasEpoch)
                    lookup = NativeResolvedTextureMiss::Retired;
                else
                {
                    state->aliases.Bind(alias->mark);
                    draft.aliasEpoch = state->aliasEpoch;
                }
            }
    }
    catch (...)
    {
        return std::nullopt;
    }
    if (!lookup)
        return std::nullopt;
    if (const auto* alias = std::get_if<NativeResolvedAlias>(&*lookup))
    {
        if (presented)
        {
            if (state->config.timing)
                state->aliasHits.fetch_add(1, std::memory_order_relaxed);
            return alias->texture;
        }
        miss = NativeResolvedTextureMiss::Offscreen;
    }
    else
        miss = std::get<NativeResolvedTextureMiss>(*lookup);
    if (const auto which = static_cast<std::size_t>(miss); state->config.timing && which < state->aliasMisses.size())
        state->aliasMisses[which].fetch_add(1, std::memory_order_relaxed);
    if (state->config.timing && miss == NativeResolvedTextureMiss::Form && formRefusal < state->aliasForms.size())
        state->aliasForms[formRefusal].fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
}

void NativeFrameAssembler::BeganSequenced(const NativeDrawSubmission& submission, const GuestReadSnapshot* snapshot) noexcept
{
    GuestReadSet  reads, dynamic;
    std::uint64_t late = 0;
    {
        GuestReadSnapshotScope scope(snapshot, &reads);
        Began(submission);
        try
        {
            if (const auto& read = state->config.readTarget)
                dynamic = GuestReadTemplates::DynamicReads(submission, read);
        }
        catch (...)
        {
        }
        late = scope.Late();
    }
    const bool refused = snapshot && late;
    try
    {
        auto& templates = ReadTemplates();
        templates.Learn(submission, reads, late, dynamic);
        if (refused)
            templates.Exclude(submission);
    }
    catch (...)
    {
    }
    auto& current = draft;
    if (!refused || !current.Matches(this, submission))
        return;
    current.admitted = false;
    try
    {
        current.detail = std::format("deferred_snapshot_incomplete: {} reads outside the snapshot", late);
    }
    catch (...)
    {
    }
}

GuestReadTemplates& NativeFrameAssembler::ReadTemplates() const noexcept
{
    return *state->readTemplates;
}

NativeFrameAssembler::ReadResolvedTrace NativeFrameAssembler::ProbeResolved(std::uint32_t address, std::uint32_t size) const
{
    const std::uint64_t begin = address, end = begin + size;
    ReadResolvedTrace   trace;
    std::lock_guard     lock(state->mutex);
    for (const auto& image : state->pendingImages)
        if (image.begin < end && begin < image.end)
        {
            if (!trace.overlapped)
                trace = { true, false, image.mark, image.begin, image.end, 0 };
            trace.flushed = trace.flushed || (!image.image && !image.held);
        }
    return trace;
}

std::span<const std::uint8_t> NativeFrameAssembler::ViewResolved(std::uint32_t address, std::uint32_t size, const std::function<std::span<const std::uint8_t>()>& view) const
{
    const std::uint64_t begin = address, end = begin + size;
    {
        std::lock_guard lock(state->mutex);
        if (std::ranges::any_of(state->pendingImages, [&](const PendingImage& image)
                                {
                                    return image.begin < end && begin < image.end;
                                }))
            return {};
    }
    return view();
}

GuestMemoryView MakeResolvedGuestView(std::function<const NativeFrameAssembler*()> assembler, GuestMemoryView raw)
{
    return [assembler = std::move(assembler), raw = std::move(raw)](std::uint64_t address, std::uint64_t size, bool physical) -> std::span<const std::uint8_t>
    {
        try
        {
            const auto view = [&]
            {
                return raw(address, size, physical);
            };
            const auto* frames = assembler ? assembler() : nullptr;
            if (!frames || !physical || address > UINT32_MAX || size > UINT32_MAX)
                return view();
            return frames->ViewResolved(static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(size), view);
        }
        catch (const std::exception&)
        {
            return {};
        }
    };
}

NativeFrameAssembler::ReadMirrorStats NativeFrameAssembler::ReadMirror() const
{
    std::lock_guard lock(state->mutex);
    std::lock_guard mirror(state->readMirrorMutex);
    auto            stats = state->readMirrorStats;
    stats.retainedBytes   = state->readMirrorLive->load(std::memory_order_relaxed);
    stats.smallReads      = state->smallReads;
    stats.smallBytes      = state->smallBytes;
    return stats;
}

bool NativeFrameAssembler::ScanoutCandidate(const GuestTarget& target, const GuestResolveCopy& copy) const
{
    if (!GuestScanoutShape(target, copy))
        return false;
    std::lock_guard lock(state->mutex);
    const auto&     source = state->source;
    // Only resolvePresentedTile gives the presented target a tiled source.
    return !(target.tiling && source.initialized && source.target && *source.target == target.native && source.content && source.content->tiling);
}

void NativeFrameAssembler::ResolveUnmodeled(const std::optional<GuestResolveCopy>& copy)
{
    const SettleOnExit settled{ state->config.surfaces };
    std::lock_guard    lock(state->mutex);
    state->endPresentedWrites("then resolve_unmodeled");
    state->applyWrites();
    state->dropScanouts(copy ? &*copy : nullptr);
    state->aliasResolved(copy ? copy->destBase : 0, copy ? resolveDestEnd(*copy) : UINT64_MAX);
    if (state->config.surfaces)
        state->config.surfaces->Overwritten(copy ? copy->destBase : 0, copy ? resolveDestEnd(*copy) : UINT64_MAX);
}

void NativeFrameAssembler::GuestWrote(std::uint32_t physical, std::uint32_t length) noexcept
{
    {
        std::lock_guard lock(state->writesMutex);
        const auto      number = ++state->writeCount;
        try
        {
            state->writes.push_back({ number, physical, std::uint64_t(physical) + length });
        }
        catch (...)
        {
            state->writesLost = true;
        }
        if (state->aliasing())
        {
            std::lock_guard alias(state->aliasMutex);
            state->aliases.Written(physical, std::uint64_t(physical) + length, number);
        }
        if (state->config.surfaces)
            state->config.surfaces->Written(physical, std::uint64_t(physical) + length, number);
    }
    std::lock_guard lock(state->readMirrorMutex);
    state->mirrorWritten(physical, std::uint64_t(physical) + length);
}

void NativeFrameAssembler::HostWriting(std::uint32_t physical, std::uint32_t length) noexcept
{
    // A host write, such as ledger progress, changes the bytes an alias stands for.
    if (state->aliasing())
    {
        std::lock_guard alias(state->aliasMutex);
        state->aliases.Written(physical, std::uint64_t(physical) + length);
    }
    // Its images drop: a later landing would overwrite the host's words.
    if (state->config.surfaces)
        state->config.surfaces->Overwritten(physical, std::uint64_t(physical) + length, false);
    {
        std::lock_guard lock(state->readMirrorMutex);
        ++state->hostWritesInFlight;
        state->mirrorWritten(physical, std::uint64_t(physical) + length);
    }
    if (state->config.resourceWritten)
        state->config.resourceWritten(physical, std::uint64_t(physical) + length, false);
}

void NativeFrameAssembler::HostWrote(std::uint32_t physical, std::uint32_t length) noexcept
{
    {
        std::lock_guard lock(state->readMirrorMutex);
        if (state->hostWritesInFlight)
            --state->hostWritesInFlight;
        state->mirrorWritten(physical, std::uint64_t(physical) + length);
    }
    if (state->config.resourceWritten)
        state->config.resourceWritten(physical, std::uint64_t(physical) + length, false);
}

std::pair<std::uint32_t, std::uint32_t> NativeFrameAssembler::WriteNotice(void* context, std::uint32_t physical, std::uint32_t length, bool) noexcept
{
    static_cast<NativeFrameAssembler*>(context)->GuestWrote(physical, length);
    return { physical, length };
}

void NativeFrameAssembler::ClearUnreadable(const std::optional<GuestTarget>& decoded)
{
    std::lock_guard lock(state->mutex);
    state->endPresentedWrites("then clear_state_unreadable");
    // As a rejected clear: the touched target, or without a decode the
    // current source, reports partial to a later guest-memory resolve.
    if (decoded)
    {
        state->touch(decoded->native);
        state->skipClear(decoded->native, "clear_state_unreadable");
    }
    else
        state->skipUndecodedClear("clear_state_unreadable");
    state->source.partial = true;
}

void NativeFrameAssembler::DeviceAcquired() noexcept
{
    try
    {
        std::lock_guard lock(state->mutex);
        if (state->epoch.thread)
            state->epoch.thread = std::this_thread::get_id();
    }
    catch (...)
    {
        // A failed lock leaves the owner; a later draw names both threads.
    }
}

NativeAssembledFrame NativeFrameAssembler::Swap(const NativeFrameSwap& swap) noexcept
{
    // Settle never throws, and nothing may unwind into the VdSwap delegate's guest thread.
    const SettleOnExit settled{ state->config.surfaces };
    try
    {
        return assemble(swap);
    }
    catch (...)
    {
    }
    NativeAssembledFrame result;
    result.swapSequence = swap.sequence;
    assign(result.detail, "swap_failed");
    try
    {
        std::lock_guard lock(state->mutex);
        if (!state->failure && state->config.mode != NativeDrawMode::Census)
            state->failure = "swap_failed";
    }
    catch (...)
    {
    }
    return result;
}

std::uint64_t NativeFrameAssembler::submit(NativeCompletionSpan span, std::uint64_t work, std::string& error)
{
    const Submitted       entry{ work, span.firstSequence, span.lastSequence };
    std::uint64_t         fence  = 0;
    const bool            census = state->config.mode == NativeDrawMode::Census;
    const bool            ending = span.frame && !span.segment;
    std::optional<Target> segmentEnd;
    if (census && span.segment && span.frame && !span.frame->passes.empty())
        segmentEnd = span.frame->passes.back().target;
    try
    {
        fence = state->config.completion->Submit(std::move(span), error);
    }
    catch (const std::exception& exception)
    {
        assign(error, exception.what());
    }
    catch (...)
    {
        assign(error, "completion_submit_threw");
    }
    if (!fence && error == kNativeCompletionClosing)
    {
        state->stopped = true;
        return 0;
    }
    if (!fence || fence <= state->lastFence)
    {
        error = "completion_submit_failed: " + (error.empty() ? std::string("fence_not_increasing") : error);
        return 0;
    }
    state->lastFence = fence;
    if (segmentEnd)
    {
        state->openSegments = segmentEnd;
        state->openGraph    = state->graph;
    }
    else if (census && ending)
    {
        state->openSegments.reset();
        state->openGraph = {};
    }
    if (entry.last)
    {
        state->submitted.push_back(entry);
        state->submitted.back().fence = fence;
        if (state->submitted.size() > kSubmittedHistory)
            state->submitted.pop_front();
    }
    return fence;
}

void NativeFrameAssembler::abandonEpoch(std::uint64_t epoch)
{
    state->graph = {};
    // Kept until the ending submits, which clears it with openSegments, so a
    // rejected ending can be retried.
    const auto graph = state->openGraph;
    {
        std::lock_guard lock(state->mutex);
        state->endChain();
        state->lastTarget.reset();
    }
    // Census flush leaves epoch open and bound resolves orphaned for the next
    // chain/terminal; swap closes epoch and retires them. Source refuses later
    // epochs while segments remain open.
    if (!state->openSegments || !state->config.completion || state->stopped)
        return;
    NativeRenderFrame frame;
    std::string       error;
    const bool        identity = state->config.resolveGraph;
    // A graph chain of only off-screen passes ends without presenting.
    if (identity && !graph.presented)
    {
        const auto saved = std::exchange(state->graph, graph);
        const bool ended = state->chainEnd(frame, error);
        state->graph     = saved;
        if (!ended)
            return;
        NativeCompletionSpan span;
        span.epoch  = epoch;
        span.status = NativeFrameStatus::Partial;
        span.frame  = std::move(frame);
        submit(std::move(span), 0, error);
        return;
    }
    frame.identityAttachments = identity;
    frame.textures.push_back({ kOutputTexture, kOutputWidth, kOutputHeight, NativeDrawReplayTextureFormat::Rgba8 });
    frame.passes.emplace_back();
    frame.passes.back().presented = true;
    frame.passes.back().target    = identity ? *graph.presented : *state->openSegments;
    NativeRenderFrameResolve resolve;
    resolve.textureId = kOutputTexture;
    resolve.width     = kOutputWidth;
    resolve.height    = kOutputHeight;
    resolve.samples   = { 0, 3 };
    frame.passes.back().resolves.push_back(resolve);
    frame.outputTextureId      = kOutputTexture;
    frame.gammaTable           = state->censusGamma;
    frame.returnGpuOutput      = !state->config.readback;
    frame.returnResolvedOutput = state->config.readback;
    if (!BatchNativeRenderFrame(frame, error) || !ValidateNativeRenderFrame(frame, error))
        return;
    // A graph chain resumes its presented target after off-screen passes.
    if (identity && !graph.lastPresented)
        frame.passes.front().resumeAttachments = true;
    else
        frame.passes.front().preservePreviousAttachments = true;
    NativeCompletionSpan span;
    span.epoch  = epoch;
    span.status = NativeFrameStatus::Partial;
    span.frame  = std::move(frame);
    submit(std::move(span), 0, error);
}

NativeAssembledFrame NativeFrameAssembler::assemble(const NativeFrameSwap& swap)
{
    std::lock_guard      serialized(state->swapMutex);
    NativeAssembledFrame result;
    Epoch                epoch;
    {
        std::lock_guard lock(state->mutex);
        state->applyWrites();
        state->endPresentedWrites("then swap");
        epoch        = std::exchange(state->epoch, {});
        result.epoch = ++state->epochs;
        // Draws built from here on belong to the next epoch, so none binds
        // this one's resolves.
        if (state->aliasing())
        {
            std::lock_guard alias(state->aliasMutex);
            state->aliasEpoch = state->epochs + 1;
        }
        // An epoch with a scanout presents it or fails the session, so its
        // record becomes the displayed image now, while later writes still
        // reach it; the swap adds the interpretation.
        if (epoch.scanout)
            state->displayed = DisplayedImage{ *epoch.scanout };
        state->endedSet.reset();
        state->presentedWork  = false;
        state->closedSequence = swap.numberedThrough;
        // A draw numbered before the swap but not yet recorded would silently
        // move to the next epoch; one numbered after it does not belong here.
        if (epoch.failure.empty() && (state->recorded != swap.numberedThrough))
            epoch.failure = std::format("draw_numbered_not_recorded: recorded {}, numbered through {}", state->recorded, swap.numberedThrough);
        if (state->failure)
            epoch.failure = "session_failed: " + *state->failure;
    }

    // Whatever the outcome, no later frame re-records the epoch's resolves.
    struct AliasEpochEnd
    {
        NativeFrameAssembler::State& state;
        std::uint64_t                epoch;

        ~AliasEpochEnd()
        {
            state.endAliasEpoch(epoch);
        }
    } aliasEpochEnd{ *state, result.epoch };

    auto submittedMain = std::exchange(state->mainSubmitted, {});
    if (state->config.mode == NativeDrawMode::Census)
        state->censusGamma = swap.gammaTable;
    result.swapSequence  = swap.sequence;
    result.coverage      = epoch.coverage;
    result.firstSequence = epoch.firstSequence;
    result.lastSequence  = epoch.lastSequence;
    const auto stop      = [&]
    {
        result.status = NativeFrameStatus::Stopped;
        result.detail = std::string(kNativeCompletionClosing);
        return std::move(result);
    };
    // A closing source takes no more work; the epoch is dropped, not failed.
    if (state->stopped)
        return stop();
    // Recording order can differ from numbering order across threads.
    std::sort(epoch.work.begin(), epoch.work.end(), [](const Work& left, const Work& right)
              {
                  return left.sequence < right.sequence;
              });
    const auto fail = [&](std::string error)
    {
        // Census mode drops only this epoch; the caller reports the failure.
        if (state->config.mode == NativeDrawMode::Census)
            abandonEpoch(result.epoch);
        std::lock_guard lock(state->mutex);
        if (!state->failure && state->config.mode != NativeDrawMode::Census)
            state->failure = std::format("epoch {}: {}", result.epoch, error);
        result.status = NativeFrameStatus::Failed;
        result.detail = std::move(error);
        return std::move(result);
    };
    if (!epoch.failure.empty())
        return fail(std::move(epoch.failure));
    if (epoch.tiled && epoch.tiled->received != epoch.tiled->tiling.count)
        return fail(std::format("tiled_scanout_incomplete: {} of {} tiles", epoch.tiled->received, epoch.tiled->tiling.count));
    if (swap.sequence <= state->lastSwap)
        return fail("swap_sequence_not_increasing");
    state->lastSwap = swap.sequence;

    std::string error;
    const bool  skipped = epoch.Skipped();
    const bool  graph   = state->config.resolveGraph;
    // resolveGraph: open off-screen resolves go first as one segment of their
    // source passes when the source can cut one and the graph takes each;
    // otherwise each is its own producer frame, as below.
    if (graph && state->config.completion && std::any_of(epoch.work.begin(), epoch.work.end(), [](const Work& item)
                                                         {
                                                             return item.resolve.has_value();
                                                         }))
    {
        const bool all   = std::all_of(epoch.work.begin(), epoch.work.end(), [&](const Work& item)
                                       {
                                         return !item.resolve || state->graphable(item);
                                       });
        const bool ready = all && state->config.completion->ReadyForSegment();
        if (!ready && state->config.timing)
        {
            // Each open resolve falls back below; count why before the loop
            // resets the chain the refusal reads.
            for (const auto& item : epoch.work)
                if (item.resolve)
                {
                    const auto refusal = state->graphRefusal(item);
                    state->count(refusal != State::GraphCounts ? refusal : all ? State::NotReady
                                                                               : State::SwapBatch);
                }
        }
        if (ready)
        {
            std::vector<Work> resolves, kept;
            for (auto item = epoch.work.begin(); item != epoch.work.end();)
                if (item->resolve)
                {
                    resolves.push_back(std::move(*item));
                    item = epoch.work.erase(item);
                }
                else
                    ++item;
            NativeCompletionSpan span;
            span.epoch      = result.epoch;
            span.status     = skipped ? NativeFrameStatus::Partial : NativeFrameStatus::Provisional;
            const auto from = resolves.front().sequence;
            if (!state->graphSegment(std::move(resolves), submittedMain, result.epoch, span, kept, error))
                return fail("segment_invalid: " + error);
            if (!submit(std::move(span), from, error))
                return state->stopped ? stop() : fail(std::move(error));
            state->admitResolves(result.epoch);
        }
    }
    // Off-screen resolves still open go first as their own producer frames.
    // Terminal frames never wait for an earlier one, so nothing here retries.
    for (auto item = epoch.work.begin(); item != epoch.work.end();)
    {
        if (!item->resolve)
        {
            ++item;
            continue;
        }
        if (!state->config.completion)
            return fail("no_completion_source");
        state->endChain();
        state->lastTarget.reset();
        state->graph = {};
        NativeCompletionSpan span;
        span.epoch         = result.epoch;
        span.firstSequence = span.lastSequence = item->sequence;
        span.status                            = NativeFrameStatus::Provisional;
        span.resolveMark                       = item->sequence;
        span.guestResolve                      = item->resolve;
        span.coverage.attempted = span.coverage.admitted = item->source->draws.size();
        span.frame.emplace();
        if (!resolveFrame(*item, *span.frame, graph, error, state->config.surfaces.get()))
            return fail("resolve_frame_invalid: " + error);
        if (!submit(std::move(span), item->sequence, error))
            return state->stopped ? stop() : fail(std::move(error));
        if (state->config.surfaces)
            state->config.surfaces->Admit(item->sequence, surfaceImport(*item));
        item = epoch.work.erase(item);
    }
    // Tile 0 runs the predicated clear; later tiles start from the preceding
    // scanout clear, with differing clears narrowed to tile 0.
    const auto isTiledClear = [&](const Work& item)
    {
        return item.clear && item.sequence == epoch.tiledClear;
    };
    std::vector<NativeRenderFramePass::Clear> backgrounds;
    bool                                      replay     = false;
    const Work*                               tiledClear = nullptr;
    for (auto* list : { &epoch.work, &submittedMain })
        if (const auto found = std::find_if(list->begin(), list->end(), isTiledClear); epoch.tiled && found != list->end())
        {
            tiledClear = &*found;
            replay     = list == &submittedMain;
            break;
        }
    if (tiledClear)
    {
        const auto& value = *tiledClear->clear;
        // A pass without a depth surface models the color alone.
        const bool depth   = tiledClear->target.depthEdramBase != kNoDepthEdramBase;
        bool       differs = false;
        for (auto background : epoch.tiled->backgrounds)
        {
            background.depth = background.stencil = depth;
            differs |= background.colorValue != value.colorValue ||
                       (depth && (!value.depth || !value.stencil || background.depthValue != value.depthValue || background.stencilValue != value.stencilValue));
            backgrounds.push_back(background);
        }
        if (!differs)
            backgrounds.clear();
        replay = replay && differs;
    }
    // Earlier segments of this epoch may already hold its target.
    auto segmentTarget = std::exchange(state->lastTarget, std::nullopt);
    if (replay)
        segmentTarget.reset();
    // Host work starts after any replayed operations, which completed earlier.
    const auto first = epoch.work.empty() ? 0 : epoch.work.front().sequence;
    // A terminal frame ended the segment chain, so the final span starts the
    // presented target again from the work those segments submitted.
    if ((!state->chain || replay) && !submittedMain.empty())
    {
        epoch.work.insert(epoch.work.begin(), std::make_move_iterator(submittedMain.begin()), std::make_move_iterator(submittedMain.end()));
        std::stable_sort(epoch.work.begin(), epoch.work.end(), [](const Work& left, const Work& right)
                         {
                             return left.sequence < right.sequence;
                         });
    }
    // The graph chain the final span continues, unless it replays the epoch.
    const bool chainOpen = state->chain.has_value();
    const auto context   = chainOpen && !replay ? std::exchange(state->graph, {}) : State::GraphChain{};
    // The source runs the terminal frame after the chain's segments, so with
    // aliasing its draws are validated against the chain.
    std::optional<NativeRenderFramePrefixState> prefix;
    // Bound resolves no chain holds, which only a chainless terminal can
    // follow: an open chain adopted every one when it began.
    std::vector<std::uint64_t> adopted;
    if (state->aliasing())
    {
        prefix = std::move(state->chain);
        std::lock_guard lock(state->aliasMutex);
        adopted = state->aliases.Adopt(result.epoch, 0);
    }
    state->graph = {};
    state->endChain();
    if (chainOpen && !adopted.empty())
        return fail("frame_invalid: alias_producer_unplaced");
    if (const auto clear = std::find_if(epoch.work.begin(), epoch.work.end(), isTiledClear); !backgrounds.empty() && clear != epoch.work.end())
    {
        clear->clear->rectangle = scissorOf(epoch.tiled->tiling.rects[0]);
        std::vector<Work> later;
        for (const auto& background : backgrounds)
            later.push_back({ clear->sequence, {}, clear->target, background });
        epoch.work.insert(clear + 1, later.begin(), later.end());
    }

    std::optional<NativeRenderFrame> output;
    // An epoch without presented work keeps the prior image, so swap checks the
    // unwritten frontbuffer the same way.
    if (epoch.work.empty() && !epoch.frontbuffer && !(graph ? context.presented : segmentTarget))
    {
        if (graph && chainOpen)
        {
            NativeRenderFrame ending;
            state->graph     = context;
            const bool ended = state->chainEnd(ending, error);
            state->graph     = {};
            if (!ended)
                return fail("frame_invalid: " + error);
            output = std::move(ending);
        }
        std::optional<DisplayedImage> shown;
        {
            std::lock_guard lock(state->mutex);
            shown = state->displayed;
        }
        if (shown)
        {
            std::array<std::uint8_t, 4> swizzle{};
            if (!selectOutput(swap, swizzle, error))
                return fail(std::move(error));
            const auto& record = shown->scanout;
            if (record.base != swap.frontbufferPhysical)
                return fail(std::format("swap_display_mismatch: {:#010x} is not swap {:#010x}", record.base, swap.frontbufferPhysical));
            if (!record.destInfo)
                return fail(std::format("swap_display_overwritten: {:#010x} by {}", swap.frontbufferPhysical, record.overwriter));
            composeScanoutSwap(*record.destInfo, swizzle);
            // An adopted table changes nothing shown; the next presented frame carries the swap's table.
            if (shown->swizzle != swizzle || (shown->gamma != swap.gammaTable && !state->config.adoptGammaWithoutWork))
                return fail(std::format("swap_display_form_changed: {:#010x}", swap.frontbufferPhysical));
        }
        result.status = skipped ? NativeFrameStatus::Partial : NativeFrameStatus::Empty;
    }
    else
    {
        NativeRenderFrame frame;
        if (!selectOutput(swap, frame.outputSwizzle, error))
            return fail(std::move(error));
        // The swapped frontbuffer must match this epoch's last complete modeled
        // scanout; other writes make its bytes unknown.
        const auto& scanout = epoch.scanout;
        if (scanout && scanout->base != swap.frontbufferPhysical)
            return fail(std::format("swap_scanout_destination_mismatch: {:#010x} is not swap {:#010x}", scanout->base, swap.frontbufferPhysical));
        if (!scanout)
            return fail(std::format("swap_scanout_unmodeled: {:#010x}", swap.frontbufferPhysical));
        if (!scanout->destInfo)
            return fail(std::format("swap_scanout_overwritten: {:#010x} by {}", swap.frontbufferPhysical, scanout->overwriter));
        composeScanoutSwap(*scanout->destInfo, frame.outputSwizzle);
        if (std::lock_guard lock(state->mutex); state->displayed)
        {
            state->displayed->swizzle = frame.outputSwizzle;
            state->displayed->gamma   = swap.gammaTable;
        }
        // No guest frontbuffer resolve is applied yet, so the target and resolve are assumed.
        result.assumptions = { true, true, swap.gammaAssumed };
        result.status      = skipped ? NativeFrameStatus::Partial : NativeFrameStatus::Provisional;
        frame.textures.push_back({ kOutputTexture, kOutputWidth, kOutputHeight, NativeDrawReplayTextureFormat::Rgba8 });
        // A graph frame's passes start fresh here; the chain flags follow validation.
        frame.identityAttachments = graph;
        for (auto& work : epoch.work)
        {
            if (!presentable(work.target))
                return fail("swap_target_unresolved");
            if (graph)
                state->appendGraph(frame, work, nullptr);
            else
                appendWork(frame.passes, work);
        }
        state->graph = {};
        // With every operation already in segments, the pass continues their target.
        if (frame.passes.empty())
        {
            frame.passes.emplace_back();
            frame.passes.back().presented = true;
            std::lock_guard lock(state->mutex);
            frame.passes.back().target = (graph ? context.presented : segmentTarget).value_or(state->presentedPass(Target{ kOutputWidth, kOutputHeight, 4, NativeDrawReplayTargetFormat::Rgba8, state->mainBase.value_or(0) }));
        }
        // Guest 2x MSAA maps to host samples 0 and 3, as the native resolves do.
        NativeRenderFrameResolve resolve;
        resolve.textureId = kOutputTexture;
        resolve.width     = kOutputWidth;
        resolve.height    = kOutputHeight;
        resolve.samples   = { 0, 3 };
        frame.passes.back().resolves.push_back(resolve);
        // A chainless graph frame starts fresh, so the re-recorded passes go first.
        if (!adopted.empty() && !state->reproduce(frame, std::move(adopted), error))
            return fail("frame_invalid: " + error);
        frame.outputTextureId      = kOutputTexture;
        frame.gammaTable           = swap.gammaTable;
        frame.returnGpuOutput      = !state->config.readback;
        frame.returnResolvedOutput = state->config.readback;
        frame.typedGuestWords      = graph && state->typedViews();
        declareSurfaces(frame, state->config.surfaces.get());
        if (!BatchNativeRenderFrame(frame, error) || !(prefix && samplesChain(frame) ? prefix->ValidateTerminal(frame, error) : ValidateNativeRenderFrame(frame, error)))
            return fail("frame_invalid: " + error);
        // Validated as a fresh target; the source continues the segment chain
        // instead, but only on the target the last segment ended with. A graph
        // chain resumes its latest presented target after off-screen passes.
        auto&      front = frame.passes.front();
        const auto start = GetNativeRenderFrameTarget(front);
        if (graph)
        {
            front.preservePreviousAttachments = context.last && context.lastPresented && *context.last == start;
            front.resumeAttachments           = context.last && !context.lastPresented && context.presented && *context.presented == start;
        }
        else if (segmentTarget && *segmentTarget == start)
            front.preservePreviousAttachments = true;
        output = std::move(frame);
    }
    if (result.status == NativeFrameStatus::Partial)
        result.detail = std::move(epoch.firstRejection);
    if (!state->config.completion)
    {
        result.frame = std::move(output);
        return result;
    }
    // Ended surfaces leave the renderer's store once the swap's frame recorded.
    if (output)
        state->releaseSurfaces(*output);
    const auto fence = submit({ result.epoch, result.firstSequence, result.lastSequence, result.status, result.coverage, result.assumptions, std::move(output) }, first, error);
    if (!fence)
        return state->stopped ? stop() : fail(std::move(error));
    result.hostFence = fence;
    return result;
}

std::optional<std::string> NativeFrameAssembler::Failure() const
{
    std::lock_guard lock(state->mutex);
    return state->failure;
}

bool NativeFrameAssembler::Stopped() const
{
    return state->stopped.load();
}

std::uint64_t NativeFrameAssembler::Completed() const
{
    return state->config.completion ? state->config.completion->Completed() : 0;
}

completion::DrawFlush NativeFrameAssembler::FlushThrough(std::uint64_t sequence)
{
    // Every resolve through `sequence` is submitted by this call, each its own
    // terminal frame in op-mark order, instead of one per caller retry.
    completion::DrawFlush result;
    bool                  resolveOwed = false;
    do
    {
        result       = {};
        resolveOwed  = false;
        result.fence = flush(sequence, result.error, result.retry, resolveOwed);
    } while (resolveOwed);
    return result;
}

std::uint64_t NativeFrameAssembler::flush(std::uint64_t sequence, std::string& error, bool& retry, bool& resolveOwed)
{
    try
    {
        std::lock_guard   serialized(state->swapMutex);
        std::vector<Work> work;
        std::uint64_t     epochNumber = 0;
        bool              skipped = false, terminal = false, more = false, nextResolve = false, graphCut = false;
        {
            std::lock_guard lock(state->mutex);
            auto&           epoch = state->epoch;
            const auto      owed  = [&]
            {
                return std::any_of(epoch.work.begin(), epoch.work.end(), [&](const Work& item)
                                   {
                                       return item.sequence <= sequence;
                                   });
            };
            // Held draws release as no-op(no host work), so ring waits reach
            // the deciding operation. On hold failure, nothing from its first
            // draw completes; no later span crosses that frontier.
            if (epoch.heldFrom && epoch.heldFrom <= sequence && !epoch.failure.empty())
            {
                error = epoch.failure;
                return 0;
            }
            if (!owed() && state->recordedMark >= sequence)
            {
                // Every span holding work at or below `sequence` was submitted,
                // so the newest of them completes it. Operations without host
                // work need only some submitted fence.
                std::uint64_t fence = 0;
                bool          exact = false;
                for (const auto& entry : state->submitted)
                {
                    if (entry.work && entry.work <= sequence)
                        fence = std::max(fence, entry.fence);
                    exact |= entry.first <= sequence && sequence <= entry.last;
                }
                // A failed epoch's work was dropped, so only a span that
                // contains `sequence` counts after a failure.
                if ((exact || !state->failure) && (fence || state->lastFence))
                    return fence ? fence : state->lastFence;
            }
            if (!state->config.completion)
            {
                error = "no_completion_source";
                return 0;
            }
            // A closing source never completes the span; the ledger keeps holding it.
            if (state->stopped)
            {
                retry = true;
                error = std::string(kNativeCompletionClosing);
                return 0;
            }
            if (state->failure)
            {
                error = "session_failed: " + *state->failure;
                return 0;
            }
            if (!epoch.failure.empty())
            {
                error = epoch.failure;
                return 0;
            }
            // Dispatcher sequences are dense, so every draw through `sequence` is recorded.
            if (state->recordedMark < sequence)
            {
                retry = true;
                error = std::format("flush_draw_not_recorded: recorded {}, flush through {}", state->opMark, sequence);
                return 0;
            }
            std::sort(epoch.work.begin(), epoch.work.end(), [](const Work& left, const Work& right)
                      {
                          return left.sequence < right.sequence;
                      });
            epochNumber = state->epochs + 1;
            skipped     = epoch.Skipped();
            // One terminal resolve frame per call keeps later segments pending
            // in the epoch until ledger retry.
            const auto resolve = std::find_if(epoch.work.begin(), epoch.work.end(), [&](const Work& item)
                                              {
                                                  return item.resolve && item.sequence <= sequence;
                                              });
            // resolveGraph: one segment takes every owed operation, resolves
            // included, up to the first resolve the graph cannot take, which
            // the next pass of FlushThrough submits as its own frame.
            if (state->config.resolveGraph && owed() && state->config.completion->ReadyForSegment())
            {
                const auto cut  = std::find_if(epoch.work.begin(), epoch.work.end(), [&](const Work& item)
                                               {
                                                  return item.sequence > sequence;
                                               });
                const auto stop = std::find_if(epoch.work.begin(), cut, [&](const Work& item)
                                               {
                                                   return item.resolve && !state->graphable(item);
                                               });
                if (stop != epoch.work.begin())
                {
                    work.assign(std::make_move_iterator(epoch.work.begin()), std::make_move_iterator(stop));
                    epoch.work.erase(epoch.work.begin(), stop);
                    graphCut    = true;
                    more        = owed();
                    nextResolve = more;
                }
            }
            if (!graphCut && resolve != epoch.work.end())
            {
                if (state->config.resolveGraph && state->config.timing)
                {
                    const auto refusal = state->graphRefusal(*resolve);
                    state->count(refusal != State::GraphCounts ? refusal : State::NotReady);
                }
                work.push_back(std::move(*resolve));
                epoch.work.erase(resolve);
                terminal    = true;
                more        = owed();
                nextResolve = std::any_of(epoch.work.begin(), epoch.work.end(), [&](const Work& item)
                                          {
                                              return item.resolve && item.sequence <= sequence;
                                          });
            }
            else if (!graphCut && owed())
            {
                if (!state->config.completion->ReadyForSegment())
                {
                    retry = true;
                    error = "segment_not_ready";
                    return 0;
                }
                const auto cut = std::find_if(epoch.work.begin(), epoch.work.end(), [&](const Work& item)
                                              {
                                                  return item.sequence > sequence;
                                              });
                work.assign(std::make_move_iterator(epoch.work.begin()), std::make_move_iterator(cut));
                epoch.work.erase(epoch.work.begin(), cut);
            }
        }
        const auto fail = [&](std::string detail)
        {
            if (state->config.mode == NativeDrawMode::Census)
                abandonEpoch(epochNumber);
            std::lock_guard lock(state->mutex);
            if (!state->failure && state->config.mode != NativeDrawMode::Census)
                state->failure = std::format("epoch {} segment: {}", epochNumber, detail);
            error = std::move(detail);
            return std::uint64_t(0);
        };

        NativeCompletionSpan span;
        span.epoch              = epochNumber;
        span.status             = skipped ? NativeFrameStatus::Partial : NativeFrameStatus::Provisional;
        std::uint64_t     first = 0;
        std::vector<Work> kept; // a segment's work, recorded once submitted
        if (terminal)
        {
            const auto& item   = work.front();
            span.firstSequence = span.lastSequence = first = item.sequence;
            span.resolveMark                               = item.sequence;
            span.guestResolve                              = item.resolve;
            span.coverage.attempted = span.coverage.admitted = item.source->draws.size();
            span.frame.emplace();
            if (!resolveFrame(item, *span.frame, state->config.resolveGraph, error, state->config.surfaces.get()))
                return fail("resolve_frame_invalid: " + error);
            state->endChain();
            state->lastTarget.reset();
            state->graph = {};
        }
        else if (graphCut)
        {
            first = work.front().sequence;
            if (!state->graphSegment(std::move(work), state->mainSubmitted, epochNumber, span, kept, error))
                return fail("segment_invalid: " + error);
        }
        else if (!work.empty())
        {
            span.segment       = true;
            span.firstSequence = first = work.front().sequence;
            span.lastSequence          = work.back().sequence;
            NativeRenderFrame frame;
            // The source runs open segments inside any terminal frame and then
            // drops their target, so a new chain replays them first.
            if (!state->chain)
                for (auto item : state->mainSubmitted)
                    appendWork(frame.passes, item);
            kept = work;
            for (auto& item : work)
            {
                if (!item.clear)
                {
                    ++span.coverage.attempted;
                    ++span.coverage.admitted;
                }
                appendWork(frame.passes, item);
            }
            // Only the target the previous segment ended with carries over.
            frame.passes.front().preservePreviousAttachments = state->lastTarget && *state->lastTarget == frame.passes.front().target;
            span.assumptions.clearAssumed                    = !frame.passes.front().preservePreviousAttachments && frame.passes.front().clears.empty();
            frame.typedGuestWords                            = state->typedViews();
            declareSurfaces(frame, state->config.surfaces.get());
            // Only a graph segment re-records bound resolves; with the graph
            // a segment of plain work is always one.
            if (!state->chain && !state->beginChain(epochNumber).empty())
                return fail("segment_invalid: alias_producer_unplaced");
            if (!state->chain->Append(frame, error))
                return fail("segment_invalid: " + error);
            state->lastTarget = frame.passes.back().target;
            span.frame        = std::move(frame);
        }
        else
        {
            // Only operations without host work are owed (off-screen clears and
            // draws, which their resolve replays), and nothing was submitted yet.
            span.status        = NativeFrameStatus::Empty;
            span.firstSequence = span.lastSequence = sequence;
        }
        const auto fence = submit(std::move(span), first, error);
        if (!fence)
        {
            if (state->stopped)
            {
                retry = true;
                return 0;
            }
            return fail(std::move(error));
        }
        state->mainSubmitted.insert(state->mainSubmitted.end(), std::make_move_iterator(kept.begin()), std::make_move_iterator(kept.end()));
        if (graphCut)
            state->admitResolves(epochNumber);
        if (terminal && state->config.surfaces)
            state->config.surfaces->Admit(work.front().sequence, surfaceImport(work.front()));
        if (more)
        {
            resolveOwed = nextResolve;
            retry       = true;
            error       = "flush_continues_after_resolve";
            return 0;
        }
        return fence;
    }
    catch (...)
    {
        assign(error, "flush_failed");
        return 0;
    }
}

bool ReadSolidPanelInputs(const NativeDrawSubmission& submission, const ReadMemory& read, SolidDrawInputs& inputs, std::string& error)
{
    inputs = {};
    error.clear();
    if (submission.hook != NativeDrawHook::IndexedUp || (submission.caller != 0x82303E40 && submission.caller != 0x82303E90))
        return false;
    std::uint32_t stride = 0;
    try
    {
        // Indexed-UP passes its vertex stride at r1+84, read at entry as M1a did.
        stride = word(read(std::uint64_t(submission.stack) + 84, 4, false));
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
    const auto&              r = submission.arguments;
    const IndexedUpArguments arguments{ r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], stride, submission.caller };
    return ReadSolidDrawInputs(arguments, read, inputs, error);
}

bool BuildSolidPanelDraw(const SolidDrawInputs& inputs, const NativeGameplayShaderInterface& shader, NativeDrawReplayRecipe& draw, std::string& detail)
{
    detail.clear();
    SolidDrawRecipe recipe;
    if (!inputs.solidCandidate)
        return false;
    if (!inputs.inputsComplete)
    {
        detail = "solid_panel_inputs_incomplete";
        return false;
    }
    if (!BuildSolidDrawRecipe(inputs, shader, recipe, detail))
    {
        if (detail.empty())
            detail = "solid_panel_recipe_rejected";
        return false;
    }
    draw = std::move(recipe.native);
    return true;
}

NativeFrameDrawBuilder MakeSolidPanelDrawBuilder(ReadMemory read, std::shared_ptr<const NativeGameplayShaderInterface> shader)
{
    return [read = std::move(read), shader = std::move(shader)](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
    {
        SolidDrawInputs inputs;
        if (!ReadSolidPanelInputs(submission, read, inputs, detail))
            return false;
        if (!shader)
        {
            detail = "solid_panel_shader_unavailable";
            return false;
        }
        return BuildSolidPanelDraw(inputs, *shader, draw, detail);
    };
}

std::string DescribeNativeDrawVisibility(const NativeDrawSubmission& submission, const NativeDrawReplayRecipe& draw)
{
    const auto scalar = [](std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        if (offset > bytes.size() || bytes.size() - offset < 4)
            throw std::runtime_error("visibility_input_extent");
        const auto bits = std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
                          (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
        return std::bit_cast<float>(bits);
    };
    std::array<float, 4> minimum{}, maximum{};
    bool                 first = true;
    for (const auto index : draw.indexSpan())
    {
        for (std::size_t component = 0; component < 4; ++component)
        {
            const auto value = scalar(draw.vertexBytes(), std::size_t(index) * draw.vertexStrideBytes + component * 4);
            if (!std::isfinite(value))
                throw std::runtime_error("visibility_nonfinite_position");
            minimum[component] = first ? value : std::min(minimum[component], value);
            maximum[component] = first ? value : std::max(maximum[component], value);
        }
        first = false;
    }
    const auto vector = [](const std::array<float, 4>& v)
    {
        return std::format("[{},{},{},{}]", v[0], v[1], v[2], v[3]);
    };
    std::string constants;
    for (std::size_t offset = 0; offset + 16 <= draw.pixelConstants.size(); offset += 16)
    {
        std::array<float, 4> value;
        for (std::size_t component = 0; component < 4; ++component)
            value[component] = scalar(draw.pixelConstants, offset + component * 4);
        if (!offset || std::any_of(value.begin(), value.end(), [](float v)
                                   {
                                       return v != 0;
                                   }))
            constants += std::format("{}c{}={}", constants.empty() ? "" : " ", offset / 16, vector(value));
    }
    const auto& viewport = draw.viewport;
    return std::format("{} from {:#010x} seq={} viewport=[{},{},{},{},{},{}] raw_position_min={} raw_position_max={} primitives={} ps_constants=[{}] ps_omitted=zero",
                       NativeDrawHookName(submission.hook),
                       submission.caller,
                       submission.sequence,
                       viewport.x,
                       viewport.y,
                       viewport.width,
                       viewport.height,
                       viewport.minDepth,
                       viewport.maxDepth,
                       vector(minimum),
                       vector(maximum),
                       draw.indexCount / 3,
                       constants);
}

std::string DescribeNativeDrawSubmission(const NativeDrawSubmission& submission, const ReadMemory& read)
{
    const auto field = [](const auto& value)
    {
        try
        {
            return value();
        }
        catch (const std::exception& exception)
        {
            return std::string("unreadable(") + exception.what() + ")";
        }
        catch (...)
        {
            return std::string("unreadable");
        }
    };
    const auto  stride  = submission.hook == NativeDrawHook::IndexedUp      ? field([&]
                                                                                    {
                                                                                 return std::to_string(word(read(std::uint64_t(submission.stack) + 84, 4, false)));
                                                                                    })
                          : submission.hook == NativeDrawHook::NonindexedUp ? std::to_string(submission.arguments[4])
                                                                            : std::string("n/a");
    std::string shaders = "unread", pixelWords = "unread";
    if (submission.stateRead)
    {
        shaders = std::format("vs={:#010x} ps={:#010x}", submission.vertexShader, submission.pixelShader);
        if (submission.pixelShader)
            pixelWords = field([&]
                               {
                                   return PixelShaderWordDigest(submission.pixelShader, read);
                               });
    }
    const auto fetch = field([&]
                             {
                                 // 0x826ACEA0 emits the fetch constants from G+0x480 in six-word groups.
                                 const auto  bytes = read(std::uint64_t(submission.Graphics()) + 0x480, 24, false);
                                 std::string text;
                                 for (std::size_t offset = 0; offset < 24; offset += 4)
                                     text += std::format("{}{:08X}", offset ? " " : "", word(std::span(bytes).subspan(offset, 4)));
                                 return text;
                             });
    return std::format("{} from {:#010x} seq={} stride={} {} ps_words={} fetch0=[{}]", NativeDrawHookName(submission.hook), submission.caller, submission.sequence, stride, shaders, pixelWords, fetch);
}

std::span<const std::uint8_t> GuestReadSnapshot::Find(std::uint64_t address, std::uint64_t size) const noexcept
{
    const auto after = std::ranges::upper_bound(ranges, address, {}, [](const Range& range)
                                                {
                                                    return std::uint64_t(range.address);
                                                });
    if (after == ranges.begin())
        return {};
    const auto& range  = *std::prev(after);
    const auto  offset = address - range.address;
    if (offset > range.bytes.size() || range.bytes.size() - offset < size)
        return {};
    return std::span(range.bytes).subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
}

std::uint64_t GuestReadSnapshot::Bytes() const noexcept
{
    std::uint64_t total = 0;
    for (const auto& range : ranges)
        total += range.bytes.size();
    return total;
}

namespace
{

thread_local GuestReadSnapshotScope* snapshotScope = nullptr;

using GuestSpans = std::vector<std::pair<std::uint64_t, std::uint64_t>>; // [begin, end), by begin

} // namespace

GuestReadSnapshotScope::GuestReadSnapshotScope(const GuestReadSnapshot* snapshot, GuestReadSet* reads) noexcept
: snapshot(snapshot)
, reads(reads)
, outer(snapshotScope)
{
    snapshotScope = this;
}

GuestReadSnapshotScope::~GuestReadSnapshotScope()
{
    snapshotScope = outer;
}

std::optional<std::span<const std::uint8_t>> GuestReadSnapshotScope::View(std::uint64_t address, std::uint64_t size) noexcept
{
    auto* scope = snapshotScope;
    if (!scope || address > UINT32_MAX || size > UINT32_MAX - address)
        return std::nullopt;
    if (scope->reads)
        try
        {
            scope->reads->emplace_back(static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(size));
        }
        catch (...)
        {
        }
    if (!scope->snapshot)
        return std::nullopt;
    if (const auto held = scope->snapshot->Find(address, size); held.size() == size)
        return held;
    ++scope->late;
    return std::nullopt;
}

ReadMemory SnapshotReads(ReadMemory read)
{
    auto wrapped                           = read;
    static_cast<GuestMemoryRead&>(wrapped) = [inner = static_cast<const GuestMemoryRead&>(read)](std::uint64_t address, std::uint64_t size, bool physical)
    {
        if (!physical)
            if (const auto held = GuestReadSnapshotScope::View(address, size))
                return GuestBytes(std::vector<std::uint8_t>(held->begin(), held->end()));
        return inner(address, size, physical);
    };
    if (read.view)
        wrapped.view = [inner = read.view](std::uint64_t address, std::uint64_t size, bool physical)
        {
            if (!physical)
                if (const auto held = GuestReadSnapshotScope::View(address, size))
                    return *held;
            return inner(address, size, physical);
        };
    return wrapped;
}

struct GuestReadTemplates::Impl
{
    struct Key
    {
        NativeDrawHook hook   = NativeDrawHook::IndexedBuffer;
        std::uint32_t  caller = 0, vertexShader = 0, pixelShader = 0, declaration = 0;
        bool           operator==(const Key&) const = default;
    };

    struct KeyHash
    {
        std::size_t operator()(const Key& key) const noexcept
        {
            const auto shaders = (std::uint64_t(key.vertexShader) << 32) | key.pixelShader;
            const auto rest    = (std::uint64_t(key.declaration) << 32) | (std::uint64_t(key.caller) ^ (std::uint64_t(key.hook) << 24));
            return std::hash<std::uint64_t>{}(shaders ^ (rest * 0x9E3779B97F4A7C15ULL));
        }
    };

    struct Entry
    {
        GuestSpans ranges;
        bool       overgrown = false;
    };

    static Key keyOf(const NativeDrawSubmission& submission) noexcept
    {
        return { submission.hook, submission.caller, submission.vertexShader, submission.pixelShader, submission.declaration };
    }

    mutable std::mutex                                             mutex;
    std::unordered_map<Key, std::shared_ptr<const Entry>, KeyHash> entries;
    std::atomic<std::uint64_t>                                     late{ 0 };
};

GuestReadTemplates::GuestReadTemplates()
: impl(std::make_unique<Impl>())
{
}

GuestReadTemplates::~GuestReadTemplates() = default;

std::uint64_t GuestReadTemplates::LateReads() const noexcept
{
    return impl->late.load(std::memory_order_relaxed);
}

GuestReadSet GuestReadTemplates::DynamicReads(const NativeDrawSubmission& submission, const std::function<bool(std::uint32_t, std::uint32_t&)>& read)
{
    GuestReadSet  dynamic;
    const auto    g           = submission.Graphics();
    std::uint32_t indexBuffer = 0;
    // ReadIndexedBufferDrawInputs reads the 32-byte header of G+0x3094's object.
    if (submission.hook == NativeDrawHook::IndexedBuffer && read && g <= UINT32_MAX - 0x3098 && read(g + 0x3094, indexBuffer) && indexBuffer && indexBuffer <= UINT32_MAX - 32)
        dynamic.emplace_back(indexBuffer, 32);
    return dynamic;
}

void GuestReadTemplates::Exclude(const NativeDrawSubmission& submission)
{
    auto entry          = std::make_shared<Impl::Entry>();
    entry->overgrown    = true;
    const auto      key = Impl::keyOf(submission);
    std::lock_guard lock(impl->mutex);
    if (impl->entries.size() >= kMaxEntries && !impl->entries.contains(key))
        impl->entries.clear();
    impl->entries[key] = std::move(entry);
}

void GuestReadTemplates::Learn(const NativeDrawSubmission& submission, const GuestReadSet& reads, std::uint64_t late, const GuestReadSet& dynamic)
{
    if (late)
        impl->late.fetch_add(late, std::memory_order_relaxed);
    const auto                         key = Impl::keyOf(submission);
    std::shared_ptr<const Impl::Entry> known;
    {
        std::lock_guard lock(impl->mutex);
        if (const auto found = impl->entries.find(key); found != impl->entries.end())
            known = found->second;
    }
    if (known && known->overgrown)
        return;
    GuestSpans spans = known ? known->ranges : GuestSpans{};
    bool       grown = !known;
    for (const auto& [address, size] : reads)
    {
        const std::uint64_t begin = address, end = begin + size;
        if (std::ranges::any_of(dynamic, [&](const auto& range)
                                {
                                    return range.first <= begin && end <= std::uint64_t(range.first) + range.second;
                                }))
            continue;
        // A read inside a known range changes nothing.
        const auto after = std::ranges::upper_bound(spans, begin, {}, &GuestSpans::value_type::first);
        if (after != spans.begin() && end <= std::prev(after)->second)
            continue;
        spans.insert(after, { begin, end });
        GuestSpans merged;
        for (const auto& span : spans)
            if (!merged.empty() && span.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, span.second);
            else
                merged.push_back(span);
        spans = std::move(merged);
        grown = true;
    }
    if (!grown)
        return;
    auto          entry = std::make_shared<Impl::Entry>();
    std::uint64_t bytes = 0;
    for (const auto& [begin, end] : spans)
        bytes += end - begin;
    entry->overgrown = spans.size() > kMaxRanges || bytes > kMaxBytes;
    if (!entry->overgrown)
        entry->ranges = std::move(spans);
    std::lock_guard lock(impl->mutex);
    if (impl->entries.size() >= kMaxEntries && !impl->entries.contains(key))
        impl->entries.clear();
    impl->entries[key] = std::move(entry);
}

bool GuestReadTemplates::Capture(const NativeDrawSubmission& submission, const GuestMemoryView& view, GuestReadSnapshot& snapshot) const
{
    std::shared_ptr<const Impl::Entry> entry;
    {
        std::lock_guard lock(impl->mutex);
        if (const auto found = impl->entries.find(Impl::keyOf(submission)); found != impl->entries.end())
            entry = found->second;
    }
    if (!entry || entry->overgrown || entry->ranges.empty() || !view || submission.hook == NativeDrawHook::IndexedUp || submission.hook == NativeDrawHook::NonindexedUp)
        return false;
    // The template's ranges with this draw's own, merged.
    GuestSpans spans   = entry->ranges;
    const auto dynamic = DynamicReads(submission, [&](std::uint32_t address, std::uint32_t& word)
                                      {
                                          const auto bytes = view(address, 4, false);
                                          if (bytes.size() != 4)
                                              return false;
                                          word = (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
                                          return true;
                                      });
    for (const auto& [address, size] : dynamic)
        spans.emplace_back(address, std::uint64_t(address) + size);
    std::ranges::sort(spans);
    GuestSpans merged;
    for (const auto& span : spans)
        if (!merged.empty() && span.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, span.second);
        else
            merged.push_back(span);
    snapshot.ranges.resize(merged.size());
    for (std::size_t i = 0; i < merged.size(); ++i)
    {
        const auto [begin, end] = merged[i];
        const auto bytes        = view(begin, end - begin, false);
        if (bytes.size() != end - begin)
            return false;
        snapshot.ranges[i].address = static_cast<std::uint32_t>(begin);
        snapshot.ranges[i].bytes.assign(bytes.begin(), bytes.end());
    }
    return true;
}

} // namespace rerevved::gpu::guest_d3d
