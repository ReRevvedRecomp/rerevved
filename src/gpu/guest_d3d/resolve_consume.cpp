#include "resolve_consume.h"

#include "ring_stubs.h"

#include <rex/cvar.h>

#include <atomic>
#include <format>

REXCVAR_DEFINE_BOOL(native_consume_resolve,
                    true,
                    "ReRevved",
                    "On by default for renderer=native with native_ring_stubs: skip D3DDevice_Resolve's Original on untiled calls, with or without "
                    "clear flags, whose exit state the host derives exactly, replaying the renderer observation, the ledger and assembler work of "
                    "its copy hook and its guest stores")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_consume_resolve_tiled,
                    true,
                    "ReRevved",
                    "On by default, with native_consume_resolve: also skip D3DDevice_Resolve's Original on the bracket form of a tiled call (tiling "
                    "byte bits 0x30 exactly 0x20, G+0x33B0 set), with G+0x31AC clear or set to a per-tile predicate, whose exit state the host derives "
                    "exactly")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

#if REREVVED_DEVELOPER
namespace
{

std::atomic<std::uint64_t>                                    consumedCount{ 0 }, consumedTiledCount{ 0 }, consumedPredicatedCount{ 0 };
std::atomic<std::uint64_t>                                    shapePredicated{ 0 }, shapePredicatedClear{ 0 };
std::array<std::atomic<std::uint64_t>, kResolveTileBuckets>   shapeTiles{};
std::array<std::atomic<std::uint64_t>, kResolveFallbackCount> fallbackCounts{};

} // namespace
#endif

bool ResolveConsumeRequested() noexcept
{
    return RingStubsPublished() && REXCVAR_GET(native_consume_resolve);
}

bool ResolveTiledConsumeRequested() noexcept
{
    return REXCVAR_GET(native_consume_resolve_tiled);
}

std::optional<ResolveFallback> ResolveConsumeVerdict(const ResolveConsumeState& state, const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, ResolvePlan* plan, const ResolveClearArgs* clear, ResolveShape* shape)
{
    if (!state.stubsReady)
        return ResolveFallback::Stubs;
    if (state.nested)
        return ResolveFallback::Nested;
    if (!state.ledgerReady)
        return ResolveFallback::Ledger;
    if (!state.entryState)
        return ResolveFallback::EntryState;
    if (state.diagnostics)
        return ResolveFallback::Diagnostics;

    const std::uint32_t g = r[0];
    // Every device offset the verdict and the watch read is below G+0x3A5C; the pool object and the progress block they point at are read through their own addresses.
    if (g > 0xFFFFFFFFU - kResolveDeviceSpan)
        return ResolveFallback::Unreadable;
    if (ResolveStackOverlaps(read, { state.pcr, state.sp }, r))
        return ResolveFallback::StackOverlap;
    std::uint32_t mode = 0, predicate = 0, bracket = 0;
    if (!read(g + 0x2ABC, mode) || !read(g + 0x31AC, predicate) || !read(g + 0x33B0, bracket))
        return ResolveFallback::Unreadable;
    const auto tiling = ClassifyResolveTiling(static_cast<std::uint8_t>(mode >> 24), predicate, bracket);
    if (shape && tiling == ResolveTiling::PredicatedBracket)
    {
        std::uint32_t tiles = 0;
        if (!read(g + 0x31CC, tiles))
            tiles = 0;
        *shape = { true, (r[1] & 0x300) != 0, tiles };
    }
    switch (tiling)
    {
        case ResolveTiling::None:
            break;
        case ResolveTiling::Bracket:
            if (!state.tiled)
                return ResolveFallback::Tiling;
            break;
        case ResolveTiling::PredicatedBracket:
            // With the cvar off the per-tile predicate keeps its own reason, so
            // the fallback counts stay comparable with the runs that never admitted it.
            if (!state.tiled)
                return ResolveFallback::TilingPredicate;
            break;
        case ResolveTiling::Byte:
            return ResolveFallback::Tiling;
        case ResolveTiling::Predicate:
            return ResolveFallback::TilingPredicate;
        case ResolveTiling::State:
            return ResolveFallback::TilingState;
    }

    std::string why;
    const auto  effects = DeriveResolveEffects(read, r, &why, clear);
    if (!effects)
        return why.starts_with("unreadable") ? ResolveFallback::Unreadable : ResolveFallback::DeriveUnsupported;
    ResolveWatch watch;
    if (!ReadResolveWatch(read, g, r[3], watch))
        return ResolveFallback::Unreadable;
    if (!watch.fence)
        return ResolveFallback::Fence;
    if (watch.allocByte & kResolveAllocFailedBit)
        return ResolveFallback::AllocMarker;
    ResolveExitRefusal refusal = ResolveExitRefusal::None;
    const auto         exit    = DeriveResolveExitState(*effects, watch, &refusal);
    if (!exit)
    {
        // The earlier tests of the derivation are this verdict's own, in the same order.
        switch (refusal)
        {
            case ResolveExitRefusal::Tiling:
                return ResolveFallback::Tiling;
            case ResolveExitRefusal::TilingPredicate:
                return ResolveFallback::TilingPredicate;
            case ResolveExitRefusal::TilingState:
                return ResolveFallback::TilingState;
            case ResolveExitRefusal::ClearOverlap:
                return ResolveFallback::ClearOverlap;
            case ResolveExitRefusal::ClearColor:
                return ResolveFallback::ClearColor;
            case ResolveExitRefusal::AllocCapacity:
                return ResolveFallback::AllocCapacity;
            case ResolveExitRefusal::AllocWait:
                return ResolveFallback::AllocWait;
            case ResolveExitRefusal::AllocPath:
                return ResolveFallback::AllocPath;
            case ResolveExitRefusal::RingSpace:
                return ResolveFallback::RingSpace;
            default:
                return ResolveFallback::DeriveUnsupported;
        }
    }
    if (plan)
    {
        plan->effects    = *effects;
        plan->watch      = watch;
        plan->exit       = *exit;
        plan->tiled      = ResolveBracketForm(tiling);
        plan->predicated = tiling == ResolveTiling::PredicatedBracket;
    }
    return std::nullopt;
}

GuestResolveCopy ResolvePlanCopy(const ResolvePlan& plan)
{
    GuestResolveCopy copy;
    copy.copyControl   = plan.effects.words.copyControl;
    copy.destBase      = plan.effects.words.destBase;
    copy.destPitch     = plan.effects.words.destPitch;
    copy.destInfo      = plan.effects.words.destInfo;
    copy.depthClear    = plan.watch.clear[1];
    copy.colorClear    = plan.watch.clear[2];
    copy.colorClearLow = plan.watch.clear[3];
    return copy;
}

#if REREVVED_DEVELOPER
void NoteResolveConsumed(bool tiled, bool predicated) noexcept
{
    consumedCount.fetch_add(1, std::memory_order_relaxed);
    if (tiled)
        consumedTiledCount.fetch_add(1, std::memory_order_relaxed);
    if (predicated)
        consumedPredicatedCount.fetch_add(1, std::memory_order_relaxed);
}

void NoteResolveShape(const ResolveShape& shape) noexcept
{
    if (!shape.predicated)
        return;
    shapePredicated.fetch_add(1, std::memory_order_relaxed);
    if (shape.clear)
        shapePredicatedClear.fetch_add(1, std::memory_order_relaxed);
    shapeTiles[ResolveTileBucket(shape.tiles)].fetch_add(1, std::memory_order_relaxed);
}

void NoteResolveFallback(ResolveFallback reason) noexcept
{
    fallbackCounts[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

ResolveConsumeCounts TakeResolveConsumeCounts() noexcept
{
    ResolveConsumeCounts counts;
    counts.consumed              = consumedCount.exchange(0, std::memory_order_relaxed);
    counts.consumedTiled         = consumedTiledCount.exchange(0, std::memory_order_relaxed);
    counts.consumedPredicated    = consumedPredicatedCount.exchange(0, std::memory_order_relaxed);
    counts.shape.predicated      = shapePredicated.exchange(0, std::memory_order_relaxed);
    counts.shape.predicatedClear = shapePredicatedClear.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kResolveTileBuckets; ++i)
        counts.shape.tiles[i] = shapeTiles[i].exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kResolveFallbackCount; ++i)
        counts.fallback[i] = fallbackCounts[i].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatResolveConsumeCounts(const ResolveConsumeCounts& counts)
{
    std::string fields = std::format(" resolve_consumed={}", counts.consumed);
    if (counts.consumedTiled)
        fields += std::format(" resolve_consumed_tiled={}", counts.consumedTiled);
    if (counts.consumedPredicated)
        fields += std::format(" resolve_consumed_predicated={}", counts.consumedPredicated);
    if (counts.shape.predicated)
        fields += FormatResolveShapeCounts(counts.shape, "resolve_");
    for (std::size_t i = 0; i < kResolveFallbackCount; ++i)
        if (counts.fallback[i])
            fields += std::format(" resolve_fallback_{}={}", kResolveFallbackNames[i], counts.fallback[i]);
    return fields;
}
#endif

} // namespace rerevved::gpu::guest_d3d
