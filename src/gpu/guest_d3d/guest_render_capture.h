#pragma once

#include "gpu/d3d12/native_render_frame.h"
#include "guest_resolve_encoder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

using GuestWordReader = std::function<bool(std::uint32_t, std::uint32_t&)>;

struct GuestRect
{
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    friend bool   operator==(const GuestRect&, const GuestRect&) = default;
};

// Predicated tiling state that BeginTiling (0x8269CE90) and SetPredication
// (0x8269CD20) leave in the device.
struct GuestTiling
{
    std::uint32_t                flags = 0;                         // byte G+0x2ABC; active when & 0x20
    std::uint32_t                count = 0;                         // G+0x31CC, admitted 2..4
    std::array<GuestRect, 4>     rects{};                           // G+0x31D0 + 16*i
    std::uint32_t                screenWidth = 0, screenHeight = 0; // G+0x337C, G+0x3380
    std::array<std::uint32_t, 5> saved{};                           // G+0x31B8..0x31C8: RT0..RT3, depth
    std::uint32_t                predicateMask = 0;                 // G+0x31A4
    std::uint32_t                predicated    = 0;                 // G+0x31AC

    bool Active() const
    {
        return (flags & 0x20) != 0;
    }

    // Tiling under which the saved surfaces stand for the logical screen.
    bool Maps() const
    {
        return Active() && count >= 2 && count <= rects.size();
    }
};

// The predicate words always; the tile layout only while tiling is active.
inline bool ReadGuestTiling(const GuestWordReader& read, std::uint32_t g, GuestTiling& out)
{
    out                = {};
    std::uint32_t byte = 0;
    if (g > UINT32_MAX - 0x3384 || !read(g + 0x2ABC, byte) || !read(g + 0x31A4, out.predicateMask) || !read(g + 0x31AC, out.predicated))
        return false;
    out.flags = byte >> 24;
    if (!out.Active())
        return true;
    if (!read(g + 0x31CC, out.count) || !read(g + 0x337C, out.screenWidth) || !read(g + 0x3380, out.screenHeight))
        return false;
    for (std::uint32_t i = 0; i < out.saved.size(); ++i)
        if (!read(g + 0x31B8 + 4 * i, out.saved[i]))
            return false;
    for (std::uint32_t i = 0; i < out.count && i < out.rects.size(); ++i)
    {
        auto& rect = out.rects[i];
        if (!read(g + 0x31D0 + 16 * i, rect.x0) || !read(g + 0x31D4 + 16 * i, rect.y0) || !read(g + 0x31D8 + 16 * i, rect.x1) ||
            !read(g + 0x31DC + 16 * i, rect.y1))
            return false;
    }
    return true;
}

// Slot 4 of a resolve source: the depth surface.
inline constexpr std::uint32_t kGuestDepthSlot = 4;

// The EDRAM tiles [begin, end) one attachment occupies.
struct GuestEdramRange
{
    std::uint32_t begin = 0, end = 0;

    bool Overlaps(const GuestEdramRange& other) const
    {
        return begin < other.end && other.begin < end;
    }

    friend bool operator==(const GuestEdramRange&, const GuestEdramRange&) = default;
};

struct GuestTarget
{
    // The bound set: RT0's shape and EDRAM base plus RT1.. as extras; the
    // logical extent when tiled.
    NativeRenderFramePass::Target  native;
    std::uint32_t                  surface = 0, colorInfo = 0, surfaceInfo = 0; // RT0's; surfaceInfo is G+0x2880
    std::uint32_t                  surfaceWidth = 0, surfaceHeight = 0;         // the true surface, from +0x24
    std::uint32_t                  surfacePitch = 0;                            // samples per row, from +0x18; 0 when unknown
    std::optional<GuestTiling>     tiling;                                      // set only when the extent was mapped
    std::uint32_t                  slot       = 0;                              // the attachment read: 0..3 color, kGuestDepthSlot depth
    std::uint32_t                  colorCount = 1;                              // bound color targets RT0..RT(n-1)
    std::array<std::uint32_t, 4>   surfaces{};                                  // RT0..RT3 surface objects as decoded
    std::array<GuestEdramRange, 5> ranges{};                                    // each decoded slot's EDRAM, by slot
};

// With exponent bias 0, RB_COLOR_INFO bits 16..19 map k_8_8_8_8->Rgba8,
// k_2_10_10_10->Rgb10a2, k_16_16 range -32..32->Rg16Snorm at 1/32 value; other
// forms have no native target.
inline bool DecodeGuestColorFormat(std::uint32_t colorInfo, NativeDrawReplayTargetFormat& format)
{
    if ((colorInfo >> 20) & 0x3F)
        return false;
    switch ((colorInfo >> 16) & 0xF)
    {
        case 0:
            format = NativeDrawReplayTargetFormat::Rgba8;
            return true;
        case 2:
            format = NativeDrawReplayTargetFormat::Rgb10a2;
            return true;
        case 4:
            format = NativeDrawReplayTargetFormat::Rg16Snorm;
            return true;
        default:
            return false;
    }
}

// EDRAM tiles of one 32bpp surface from its +0x18 word (pitch bits 0..13,
// MSAA bits 16..17) and true height: 80x16 samples per tile, 2X samples on
// consecutive rows. Only 1X and 2X are decoded.
inline std::uint32_t GuestSurfaceEdramTiles(std::uint32_t surfaceInfo, std::uint32_t surfaceHeight)
{
    const auto pitch   = surfaceInfo & 0x3FFF;
    const auto samples = ((surfaceInfo >> 16) & 3) ? 2U : 1U;
    return ((pitch + 79) / 80) * ((surfaceHeight * samples + 15) / 16);
}

// The EDRAM tiles a clear rectangle of a 1X or 2X `surface` (as
// GuestSurfaceEdramTiles counts it) touches, as one span from its first to
// its last tile; nullopt when the pitch is unknown.
inline std::optional<GuestEdramRange> GuestEdramRectRange(const GuestEdramRange& surface, std::uint32_t pitch, std::uint32_t sampleCount, std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom)
{
    if (!pitch || surface.end <= surface.begin)
        return std::nullopt;
    const auto rows = sampleCount == 1 ? 1U : 2U;
    const auto x0 = static_cast<std::uint32_t>(std::max(left, 0)), y0 = static_cast<std::uint32_t>(std::max(top, 0));
    const auto x1 = static_cast<std::uint32_t>(std::max(right, 0)), y1 = static_cast<std::uint32_t>(std::max(bottom, 0));
    if (x1 <= x0 || y1 <= y0)
        return GuestEdramRange{ surface.begin, surface.begin };
    const auto rowTiles = (pitch + 79) / 80;
    const auto first    = std::uint64_t(y0 * rows / 16) * rowTiles + x0 / 80;
    const auto last     = std::uint64_t((y1 * rows - 1) / 16) * rowTiles + (x1 - 1) / 80;
    const auto size     = std::uint64_t(surface.end - surface.begin);
    return GuestEdramRange{ surface.begin + static_cast<std::uint32_t>(std::min(first, size)), surface.begin + static_cast<std::uint32_t>(std::min(last + 1, size)) };
}

struct GuestDepthSurface
{
    std::uint32_t   surface = 0, depthInfo = 0, edramBase = 0; // G+0x30A8; +0x1C & 0xFFF
    std::uint32_t   width = 0, height = 0, sampleCount = 0;    // logical when tiled; +0x24, +0x18
    bool            tiled = false;
    GuestEdramRange range;     // from +0x18 and the true height
    std::uint32_t   pitch = 0; // samples per row, from +0x18; 0 when unknown
};

// 0x826A1D90 receives this index for a depth or stencil clear without color.
inline constexpr std::uint32_t kGuestDepthOnlyClearIndex = 0xFFFFFFFFU;

struct GuestClear
{
    GuestTarget                      target;
    NativeRenderFramePass::Clear     clear;
    std::uint32_t                    flags = 0, index = 0, predicate = 0;
    GuestTiling                      tiling;
    std::optional<GuestDepthSurface> depth;
};

// Only presented slot 0 scans out; 1X, k_16_16, and k_2_10_10_10 copies remain
// off-screen.
inline bool GuestScanoutShape(const GuestTarget& target, const GuestResolveCopy& copy)
{
    const auto  width  = (copy.destFetch[2] & 0x1FFF) + 1;
    const auto  height = ((copy.destFetch[2] >> 13) & 0x1FFF) + 1;
    const auto& source = target.native;
    return width == 1280 && height == 720 && target.colorCount == 1 && target.slot == 0 && source.width == 1280 && source.height == 720 &&
           source.sampleCount == 4 && source.format == NativeDrawReplayTargetFormat::Rgba8 && source.extraColorTargets == 0;
}

// What the clear and resolve hooks report into; an interface so the hook
// translation unit does not link the assembler.
class IGuestRenderSink
{
public:
    virtual ~IGuestRenderSink()                                                                                                                 = default;
    virtual std::uint64_t Clear(const GuestClear& clear, std::string& error)                                                                    = 0;
    virtual std::uint64_t Resolve(const GuestTarget& target, const GuestResolveCopy& copy, bool frontbuffer, bool& partial, std::string& error) = 0;

    // A guest clear whose state did not decode still wrote EDRAM. `decoded` is
    // its color target when that alone decodes; without it any source may be hit.
    virtual void ClearUnreadable(const std::optional<GuestTarget>& decoded) = 0;

    // A guest resolve the hook refused before Resolve still wrote guest
    // memory: at `copy`'s destination when its words were read, else anywhere.
    virtual void ResolveUnmodeled(const std::optional<GuestResolveCopy>& copy) = 0;

    // Whether the next VdSwap may confirm this resolve as a scanout. A sink
    // that tracks passes refines the shape; see NativeFrameAssembler.
    virtual bool ScanoutCandidate(const GuestTarget& target, const GuestResolveCopy& copy) const
    {
        return GuestScanoutShape(target, copy);
    }

    // The calling thread acquired the guest device (0x8268CF10) and now
    // draws for it; the completion ledger checks that the previous owner
    // released it first.
    virtual void DeviceAcquired() noexcept
    {
    }
};

// Names why a decode failed when `detail` is given: an unreadable word by its
// address, or the guest value no native target models.
inline bool refuseGuestDecode(std::string* detail, std::string text)
{
    if (detail)
        *detail = std::move(text);
    return false;
}

inline bool ReadGuestTarget(const GuestWordReader& read, std::uint32_t g, std::uint32_t index, GuestTarget& out, std::string* detail = nullptr)
{
    std::uint32_t extent = 0, samples = 0;
    GuestTiling   tiling;
    out.tiling.reset();
    out.native = {};
    if (g > UINT32_MAX - 0x3384 || index > 3)
        return refuseGuestDecode(detail, std::format("target_index {:#x}", index));
    if (!read(g + 0x3098 + index * 4, out.surface))
        return refuseGuestDecode(detail, std::format("state_unreadable: G+{:#x}", 0x3098 + index * 4));
    if (!out.surface)
        return refuseGuestDecode(detail, std::format("target_unbound: RT{}", index));
    if (out.surface > UINT32_MAX - 0x28 || !read(out.surface + 0x24, extent) || !read(out.surface + 0x18, samples) ||
        !read(out.surface + 0x1C, out.colorInfo))
        return refuseGuestDecode(detail, std::format("state_unreadable: RT{} surface {:08X}", index, out.surface));
    if (!read(g + 0x2880, out.surfaceInfo) || !ReadGuestTiling(read, g, tiling))
        return refuseGuestDecode(detail, "state_unreadable: G+0x2880 or tiling");
    out.surfaceWidth       = ((extent >> 18) & 0x3FFF) + 1;
    out.surfaceHeight      = ((extent >> 3) & 0x7FFF) + 1;
    out.surfacePitch       = samples & 0x3FFF;
    out.native.width       = out.surfaceWidth;
    out.native.height      = out.surfaceHeight;
    out.native.sampleCount = ((samples >> 16) & 3) == 0 ? 1 : 4;
    out.native.edramBase   = out.colorInfo & 0xFFF;
    out.slot               = index;
    out.colorCount         = 1;
    out.surfaces           = {};
    out.surfaces[index]    = out.surface;
    out.ranges             = {};
    out.ranges[index]      = { out.native.edramBase, out.native.edramBase + GuestSurfaceEdramTiles(samples, out.surfaceHeight) };
    // A target BeginTiling saved is one tile of the logical screen. The guest
    // replays its work per tile; the host renders it once at full extent.
    if (tiling.Maps() && out.surface == tiling.saved[index])
    {
        out.native.width  = tiling.screenWidth;
        out.native.height = tiling.screenHeight;
        out.tiling        = tiling;
    }
    if (!out.native.width || !out.native.height)
        return refuseGuestDecode(detail, std::format("target_extent_zero: RT{}", index));
    // 0x826A1D90 clears every ColorRenderTargetFormat (+0x1C bits 16..19).
    if (!DecodeGuestColorFormat(out.colorInfo, out.native.format))
        return refuseGuestDecode(detail, std::format("target_format_unsupported: RT{} format {} color_info {:08X}", index, (out.colorInfo >> 16) & 0xF, out.colorInfo));
    if (const auto msaa = (samples >> 16) & 3; msaa > 1)
        return refuseGuestDecode(detail, std::format("target_msaa_unsupported: RT{} msaa {} surface_info {:08X}", index, msaa, samples));
    return true;
}

// The bound depth surface; while tiling maps, only the saved one is mapped.
inline bool ReadGuestDepthSurface(const GuestWordReader& read, std::uint32_t g, const GuestTiling& tiling, GuestDepthSurface& out, std::string* detail = nullptr)
{
    out                  = {};
    std::uint32_t extent = 0, samples = 0;
    if (g > UINT32_MAX - 0x30A8 || !read(g + 0x30A8, out.surface))
        return refuseGuestDecode(detail, "state_unreadable: G+0x30a8");
    if (!out.surface)
        return refuseGuestDecode(detail, "depth_unbound");
    if (out.surface > UINT32_MAX - 0x28 || !read(out.surface + 0x1C, out.depthInfo) || !read(out.surface + 0x24, extent) ||
        !read(out.surface + 0x18, samples))
        return refuseGuestDecode(detail, std::format("state_unreadable: depth surface {:08X}", out.surface));
    // DepthRenderTargetFormat bit 16: kD24FS8 has no native depth target.
    if (out.depthInfo & 0x10000)
        return refuseGuestDecode(detail, std::format("depth_format_unsupported: D24FS8 depth_info {:08X}", out.depthInfo));
    if (const auto msaa = (samples >> 16) & 3; msaa > 1)
        return refuseGuestDecode(detail, std::format("depth_msaa_unsupported: msaa {} surface_info {:08X}", msaa, samples));
    out.edramBase   = out.depthInfo & 0xFFF;
    out.width       = ((extent >> 18) & 0x3FFF) + 1;
    out.height      = ((extent >> 3) & 0x7FFF) + 1;
    out.sampleCount = ((samples >> 16) & 3) == 0 ? 1 : 4;
    out.range       = { out.edramBase, out.edramBase + GuestSurfaceEdramTiles(samples, out.height) };
    out.pitch       = samples & 0x3FFF;
    if (tiling.Maps() && out.surface == tiling.saved[4])
    {
        out.width  = tiling.screenWidth;
        out.height = tiling.screenHeight;
        out.tiled  = true;
    }
    return out.width && out.height;
}

// RT0..RT3 must bind contiguously with RT0 extent/samples; tiled slots must
// equal BeginTiling saved surfaces. Color EDRAM ranges, and depth for slot 4,
// must be disjoint; slot 0..3 selects color, kGuestDepthSlot depth.
inline bool ReadGuestTargetSet(const GuestWordReader& read, std::uint32_t g, std::uint32_t slot, GuestTarget& out, std::string* detail = nullptr)
{
    std::array<std::uint32_t, 4> bound{};
    if (g > UINT32_MAX - 0x3384 || slot > kGuestDepthSlot)
        return refuseGuestDecode(detail, std::format("target_index {:#x}", slot));
    for (std::uint32_t i = 0; i < bound.size(); ++i)
        if (!read(g + 0x3098 + 4 * i, bound[i]))
            return refuseGuestDecode(detail, std::format("state_unreadable: G+{:#x}", 0x3098 + 4 * i));
    std::uint32_t count = 0;
    while (count < bound.size() && bound[count])
        ++count;
    for (std::uint32_t j = count + 1; j < bound.size(); ++j)
        if (bound[j])
            return refuseGuestDecode(detail, std::format("target_set_sparse: RT{} bound after unbound RT{}", j, count));
    if (slot < bound.size() && slot >= count)
        return refuseGuestDecode(detail, std::format("target_unbound: RT{}", slot));
    if (!ReadGuestTarget(read, g, 0, out, detail))
        return false;

    // Each slot's EDRAM range, from its own +0x18 word and true height.
    std::vector<std::pair<std::string, GuestEdramRange>> ranges;
    const auto                                           shape = [](std::uint32_t width, std::uint32_t height, std::uint32_t samples)
    {
        return std::format("{}x{}x{}", width, height, samples);
    };
    const auto rt0 = shape(out.native.width, out.native.height, out.native.sampleCount);
    if (count > 1 || slot == kGuestDepthSlot)
        ranges.emplace_back("RT0", out.ranges[0]);
    for (std::uint32_t i = 1; i < count; ++i)
    {
        GuestTarget other;
        if (!ReadGuestTarget(read, g, i, other, detail))
            return false;
        const auto& native = other.native;
        if (native.width != out.native.width || native.height != out.native.height || native.sampleCount != out.native.sampleCount)
            return refuseGuestDecode(detail, std::format("target_set_shape_mismatch: RT{} {}, RT0 {}", i, shape(native.width, native.height, native.sampleCount), rt0));
        if (other.tiling.has_value() != out.tiling.has_value())
            return refuseGuestDecode(detail, std::format("target_set_not_tiling_saved: RT{}", i));
        // Native MRT slots are unsigned; a k_16_16 slot stays a lone target.
        if (native.format == NativeDrawReplayTargetFormat::Rg16Snorm || out.native.format == NativeDrawReplayTargetFormat::Rg16Snorm)
            return refuseGuestDecode(detail, std::format("target_set_format_unsupported: RT{} format 4", native.format == NativeDrawReplayTargetFormat::Rg16Snorm ? i : 0));
        out.native.extraFormats[i - 1]    = native.format;
        out.native.extraEdramBases[i - 1] = native.edramBase;
        out.surfaces[i]                   = other.surface;
        out.ranges[i]                     = other.ranges[i];
        ranges.emplace_back(std::format("RT{}", i), other.ranges[i]);
    }
    out.native.extraColorTargets = static_cast<std::uint8_t>(count - 1);
    out.colorCount               = count;
    out.slot                     = slot;
    if (slot == kGuestDepthSlot)
    {
        GuestTiling       tiling;
        GuestDepthSurface depth;
        std::string       why;
        if (!ReadGuestTiling(read, g, tiling))
            return refuseGuestDecode(detail, "state_unreadable: G+0x2880 or tiling");
        // An unbound depth surface fails depth_unbound.
        if (!ReadGuestDepthSurface(read, g, tiling, depth, &why))
            return refuseGuestDecode(detail, why);
        if (depth.width != out.native.width || depth.height != out.native.height || depth.sampleCount != out.native.sampleCount)
            return refuseGuestDecode(detail, std::format("target_set_shape_mismatch: depth {}, RT0 {}", shape(depth.width, depth.height, depth.sampleCount), rt0));
        if (depth.tiled != out.tiling.has_value())
            return refuseGuestDecode(detail, "target_set_not_tiling_saved: depth");
        out.ranges[kGuestDepthSlot] = depth.range;
        ranges.emplace_back("depth", depth.range);
    }
    for (std::size_t i = 0; i < ranges.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (ranges[i].second.Overlaps(ranges[j].second))
                return refuseGuestDecode(detail, std::format("target_set_edram_overlap: {} {}", ranges[j].first, ranges[i].first));
    return true;
}

// The two inputs of a per-target clear that live in the body frame of 0x826A22F8:
// the stencil word at r1+92 and the color words at r10. A consumed Clear never
// ran the body, so it supplies the words the host derived.
struct GuestClearInputs
{
    std::uint32_t                stencil = 0;
    std::array<std::uint32_t, 4> color{};
};

// 0x826A1D90 sees clipped rect plus one color target, or depth only; supplied
// words replace stack+92 and r[7].
inline bool ReadGuestClear(const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, std::uint32_t stack, double depth, GuestClear& out, std::string* detail = nullptr, const GuestClearInputs* supplied = nullptr)
{
    out                   = {};
    out.flags             = r[1];
    out.index             = r[2];
    const bool  depthOnly = out.index == kGuestDepthOnlyClearIndex;
    std::string why;
    const auto  refuse = [&](std::string text)
    {
        return refuseGuestDecode(detail, std::move(text));
    };
    if (stack > UINT32_MAX - 92 || !ReadGuestTiling(read, r[0], out.tiling))
        return refuse("clear_state_unreadable: tiling");
    out.predicate = out.tiling.predicated;
    if (depthOnly && ((out.flags & 0xF) || !(out.flags & 0x30)))
        return refuse(std::format("clear_depth_only_flags {:#x}", out.flags));
    // 0x826A22F8 clears each bound RT in turn, so the clear names one slot
    // of the bound set; only the first (index 0) carries depth and stencil.
    if (!depthOnly && !ReadGuestTargetSet(read, r[0], r[2], out.target, &why))
        return refuse("clear_" + why);
    if (!depthOnly && out.index != 0 && (out.flags & 0x30))
        return refuse(std::format("clear_mrt_flags {:#x}", out.flags));
    if (!depthOnly)
        out.clear.colorTargets = static_cast<std::uint8_t>(1U << out.index);
    std::uint32_t stencil = 0;
    if (supplied)
        stencil = supplied->stencil;
    else if (!read(stack + 92, stencil))
        return refuse("clear_state_unreadable: stencil at r1+92");
    out.clear.rectangle    = { static_cast<std::int32_t>(r[3]), static_cast<std::int32_t>(r[4]), static_cast<std::int32_t>(r[5]), static_cast<std::int32_t>(r[6]) };
    out.clear.color        = !depthOnly && (out.flags & (1U << out.index)) != 0;
    out.clear.depth        = (out.flags & 0x10) != 0;
    out.clear.stencil      = (out.flags & 0x20) != 0;
    out.clear.depthValue   = static_cast<float>(depth);
    out.clear.stencilValue = static_cast<std::uint8_t>(stencil);
    if (out.clear.depth || out.clear.stencil)
    {
        GuestDepthSurface surface;
        if (!ReadGuestDepthSurface(read, r[0], out.tiling, surface, &why))
            return refuse("clear_" + why);
        // Only the depth slot BeginTiling saved (G+0x31C8) is mapped with the screen.
        if ((depthOnly ? out.tiling.Maps() : out.target.tiling.has_value()) && !surface.tiled)
            return refuse("depth_surface_not_tiling_saved");
        if (depthOnly)
        {
            out.target.surface = surface.surface;
            out.target.native  = { surface.width, surface.height, surface.sampleCount, NativeDrawReplayTargetFormat::Rgba8, surface.edramBase };
            if (surface.tiled)
                out.target.tiling = out.tiling;
        }
        else if (surface.width != out.target.native.width || surface.height != out.target.native.height ||
                 surface.sampleCount != out.target.native.sampleCount)
            return refuse(std::format("clear_depth_extent_mismatch: depth {}x{}x{} at {:03X}, RT{} {}x{}x{}",
                                      surface.width,
                                      surface.height,
                                      surface.sampleCount,
                                      surface.edramBase,
                                      out.index,
                                      out.target.native.width,
                                      out.target.native.height,
                                      out.target.native.sampleCount));
        // A set's index-0 clear writes depth beside every color slot.
        for (std::uint32_t i = 0; !depthOnly && out.target.colorCount > 1 && i < out.target.colorCount; ++i)
            if (surface.range.Overlaps(out.target.ranges[i]))
                return refuse(std::format("clear_target_set_edram_overlap: RT{} depth", i));
        out.depth = surface;
    }
    if (out.clear.color)
        for (std::uint32_t i = 0; i < 4; ++i)
        {
            std::uint32_t bits = 0;
            if (supplied)
                bits = supplied->color[i];
            else if (r[7] > UINT32_MAX - 16 || !read(r[7] + 4 * i, bits))
                return refuse(std::format("clear_state_unreadable: color at {:08X}", r[7]));
            out.clear.colorValue[i] = std::bit_cast<float>(bits);
        }
    return true;
}

inline bool ReadGuestResolveCopy(const GuestWordReader& read, std::uint32_t g, GuestResolveCopy& out)
{
    return g <= UINT32_MAX - 0x2A34 && read(g + 0x2A18, out.copyControl) && read(g + 0x2A1C, out.destBase) &&
           read(g + 0x2A20, out.destPitch) && read(g + 0x2A24, out.destInfo) && read(g + 0x2A2C, out.depthClear) &&
           read(g + 0x2A30, out.colorClear) && read(g + 0x2A34, out.colorClearLow);
}

// `detail` names a source set that does not decode; other failures leave it empty.
inline bool ReadGuestResolveEntry(const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, GuestTarget& target, GuestResolveCopy& copy, std::string* detail = nullptr)
{
    if (r[5] || r[6] || r[3] > UINT32_MAX - 0x34)
        return false;
    if (!ReadGuestTargetSet(read, r[0], r[1] & 7, target, detail))
        return false;
    for (std::uint32_t i = 0; i < 6; ++i)
        if (!read(r[3] + 0x1C + i * 4, copy.destFetch[i]))
            return false;
    copy.rectWidth  = target.native.width;
    copy.rectHeight = target.native.height;
    if (r[2])
    {
        std::uint32_t right = 0, bottom = 0;
        if (r[2] > UINT32_MAX - 16 || !read(r[2], copy.rectX) || !read(r[2] + 4, copy.rectY) ||
            !read(r[2] + 8, right) || !read(r[2] + 12, bottom) || right <= copy.rectX || bottom <= copy.rectY)
            return false;
        copy.rectWidth  = right - copy.rectX;
        copy.rectHeight = bottom - copy.rectY;
    }
    return !r[4] || (r[4] <= UINT32_MAX - 8 && read(r[4], copy.destX) && read(r[4] + 4, copy.destY));
}

} // namespace rerevved::gpu::guest_d3d
