#include "clear_consume.h"

#include "ring_stubs.h"

#include <rex/cvar.h>

#include <atomic>
#include <format>

REXCVAR_DEFINE_BOOL(native_consume_clear,
                    true,
                    "ReRevved",
                    "On by default for renderer=native with native_ring_stubs: skip the Clear wrapper (0x826A2970) Original on untiled calls whose "
                    "exit state the host derives exactly, replaying the renderer observation of each per-target clear from the derived calls and "
                    "the guest stores of the exit state")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_consume_clear_tiled,
                    true,
                    "ReRevved",
                    "On by default, with native_consume_clear: also skip the Clear wrapper's Original on a tiled device (the tile loop runs, or the "
                    "bracket G+0x33B0 is set) whose exit state the host derives exactly, including the side buffers of each selected tile")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

namespace
{

#if REREVVED_DEVELOPER
std::atomic<std::uint64_t>                                  consumedCount{ 0 }, consumedTiledCount{ 0 };
std::array<std::atomic<std::uint64_t>, kClearShapeCount>    shapeCounts{};
std::array<std::atomic<std::uint64_t>, kClearFallbackCount> fallbackCounts{};
#endif

bool unreadableReason(const std::string& why)
{
    return why.starts_with("unreadable");
}

} // namespace

bool ClearConsumeRequested() noexcept
{
    return RingStubsPublished() && REXCVAR_GET(native_consume_clear);
}

bool ClearTiledConsumeRequested() noexcept
{
    return REXCVAR_GET(native_consume_clear_tiled);
}

std::optional<ClearFallback> ClearConsumeVerdict(const ClearConsumeState& state, const GuestWordReader& read, const ClearEntry& entry, ClearPlan* plan)
{
    if (!state.stubsReady)
        return ClearFallback::Stubs;
    if (state.nested)
        return ClearFallback::Nested;
    if (!state.ledgerReady)
        return ClearFallback::Ledger;
    if (!state.entryState)
        return ClearFallback::EntryState;
    if (state.diagnostics)
        return ClearFallback::Diagnostics;

    const std::uint32_t g = entry.g;
    // Every device offset the verdict reads is below kClearDeviceSpan; the pool object and the progress block it points at are read through their own addresses.
    if (g > 0xFFFFFFFFU - kClearDeviceSpan)
        return ClearFallback::Unreadable;
    if (ClearStackOverlaps(read, entry))
        return ClearFallback::StackOverlap;
    // Tiled means per-target 0x826A1D90 device tiling OR allocator bracket
    // G+0x33B0. Without native_consume_clear_tiled, fall back before
    // derivation; otherwise exit model judges it.
    bool          tiled   = false;
    std::uint32_t bracket = 0;
    if (!ClearDeviceTiled(read, g, tiled) || !read(g + 0x33B0, bracket))
        return ClearFallback::Unreadable;
    if ((tiled || bracket) && !state.tiled)
        return ClearFallback::Tiled;

    std::string why;
    const auto  calls = DeriveClearCalls(read, entry, &why);
    if (!calls)
        return unreadableReason(why) ? ClearFallback::Unreadable : ClearFallback::DeriveUnsupported;
    // A body that leaves before its flush groups changes nothing; there is
    // nothing to replay and Original costs next to nothing.
    if (!calls->reachedFlush)
        return ClearFallback::EarlyExit;
    const auto effects = DeriveClearEffects(read, entry, *calls, &why);
    if (!effects)
        return unreadableReason(why) ? ClearFallback::Unreadable : ClearFallback::DeriveUnsupported;
    // The tile block A is not modeled (the exit derivation refuses it too).
    if (effects->tiledBlockCalls)
        return ClearFallback::TiledBlock;
    ClearWatch watch;
    if (!ReadClearWatch(read, g, watch))
        return ClearFallback::Unreadable;
    if (watch.allocByte & kClearAllocFailedBit)
        return ClearFallback::AllocMarker;
    const auto sides = DeriveClearSideBuffers(read, entry, *calls, &why);
    if (!sides)
        return unreadableReason(why) ? ClearFallback::Unreadable : ClearFallback::DeriveUnsupported;
    ClearExitRefusal refusal = ClearExitRefusal::None;
    const auto       exit    = DeriveClearExitState(*effects, *sides, watch, &refusal);
    if (!exit)
    {
        // The earlier tests of the derivation are this verdict's own, in the same order.
        switch (refusal)
        {
            case ClearExitRefusal::TiledBlock:
                return ClearFallback::TiledBlock;
            case ClearExitRefusal::EarlyExit:
                return ClearFallback::EarlyExit;
            case ClearExitRefusal::AllocMarker:
                return ClearFallback::AllocMarker;
            case ClearExitRefusal::AllocPath:
                return ClearFallback::AllocPath;
            case ClearExitRefusal::RingSpace:
                return ClearFallback::RingSpace;
            case ClearExitRefusal::AllocCapacity:
                return ClearFallback::AllocCapacity;
            case ClearExitRefusal::AllocWait:
                return ClearFallback::AllocWait;
            default:
                return ClearFallback::DeriveUnsupported;
        }
    }
    if (plan)
    {
        plan->entry   = entry;
        plan->calls   = *calls;
        plan->effects = *effects;
        plan->sides   = *sides;
        plan->watch   = watch;
        plan->exit    = *exit;
    }
    return std::nullopt;
}

std::array<std::uint32_t, 8> ClearPlanArguments(const ClearPlan& plan, std::size_t index)
{
    const ClearCall& call = plan.calls.calls[index];
    return { plan.entry.g,
             call.flags,
             call.index,
             static_cast<std::uint32_t>(call.rect[0]),
             static_cast<std::uint32_t>(call.rect[1]),
             static_cast<std::uint32_t>(call.rect[2]),
             static_cast<std::uint32_t>(call.rect[3]),
             call.colorPointer };
}

GuestClearInputs ClearPlanInputs(const ClearPlan& plan, std::size_t index)
{
    const ClearCall& call = plan.calls.calls[index];
    GuestClearInputs inputs;
    inputs.stencil = call.stencil;
    inputs.color   = call.color;
    return inputs;
}

#if REREVVED_DEVELOPER
void NoteClearConsumed(unsigned shape) noexcept
{
    consumedCount.fetch_add(1, std::memory_order_relaxed);
    if (!ClearShapeTiled(shape))
        return;
    consumedTiledCount.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kClearShapeCount; ++i)
        if (shape & (1U << i))
            shapeCounts[i].fetch_add(1, std::memory_order_relaxed);
}

void NoteClearFallback(ClearFallback reason) noexcept
{
    fallbackCounts[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

ClearConsumeCounts TakeClearConsumeCounts() noexcept
{
    ClearConsumeCounts counts;
    counts.consumed      = consumedCount.exchange(0, std::memory_order_relaxed);
    counts.consumedTiled = consumedTiledCount.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kClearShapeCount; ++i)
        counts.shapes[i] = shapeCounts[i].exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kClearFallbackCount; ++i)
        counts.fallback[i] = fallbackCounts[i].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatClearConsumeCounts(const ClearConsumeCounts& counts)
{
    std::string fields = std::format(" clear_consume_ok={}", counts.consumed);
    if (counts.consumedTiled)
        fields += std::format(" clear_consume_ok_tiled={}", counts.consumedTiled);
    for (std::size_t i = 0; i < kClearShapeCount; ++i)
        if (counts.shapes[i])
            fields += std::format(" clear_consume_tiled_{}={}", kClearShapeNames[i], counts.shapes[i]);
    for (std::size_t i = 0; i < kClearFallbackCount; ++i)
        if (counts.fallback[i])
            fields += std::format(" clear_consume_fallback_{}={}", kClearFallbackNames[i], counts.fallback[i]);
    return fields;
}
#endif

} // namespace rerevved::gpu::guest_d3d
