#include "lowerer_consume.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <exception>
#include <format>
#include <new>

REXCVAR_DEFINE_BOOL(native_consume_lowerer,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native with native_ring_stubs: skip the held direct draw lowerer's (0x826A3048) Original on untiled roots "
                    "whose exit the host derives exactly (allocator state, staging words, out words), when it cannot flush, wait, fail an allocation or run "
                    "0x826ADD88 and no diagnostic needs its body")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_consume_lowerer_tiled,
                    false,
                    "ReRevved",
                    "Bring-up only, with native_consume_lowerer: also skip the lowerer's Original on roots whose tiling byte is exactly 0x21 when the "
                    "bracket tripwires hold and the tile list has room for the record")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

namespace
{

#if REREVVED_DEVELOPER
// Disjoint by tiled, so one drain never shows the tiled subset above its total.
std::array<std::atomic<std::uint64_t>, 2>                     consumedCounts{};
std::array<std::atomic<std::uint64_t>, kLowererFallbackCount> fallbackCounts{};

void noteFallback(LowererFallback reason) noexcept
{
    fallbackCounts[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void noteConsumed(bool tiled) noexcept
{
    consumedCounts[tiled ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
}
#else
void noteFallback(LowererFallback) noexcept
{
}

void noteConsumed(bool) noexcept
{
}
#endif

// The reasons of the shared state gates, for a verdict whose dirty cvar is off.
LowererFallback FromGate(IndexedBufferFallback gate) noexcept
{
    switch (gate)
    {
        case IndexedBufferFallback::Dispatcher:
            return LowererFallback::Dispatcher;
        case IndexedBufferFallback::Nested:
            return LowererFallback::Nested;
        case IndexedBufferFallback::Stubs:
            return LowererFallback::Stubs;
        case IndexedBufferFallback::StackOverlap:
            return LowererFallback::StackOverlap;
        case IndexedBufferFallback::Tiling:
            return LowererFallback::Tiling;
        case IndexedBufferFallback::Unreadable:
            return LowererFallback::Unreadable;
        case IndexedBufferFallback::TiledState:
            return LowererFallback::TiledState;
        case IndexedBufferFallback::Diagnostics:
            return LowererFallback::Diagnostics;
        default:
            // StateDirty, and the guards that only a dirty cvar reaches.
            return LowererFallback::StateDirty;
    }
}

LowererFallback FromRefusal(LowererExitRefusal refusal) noexcept
{
    switch (refusal)
    {
        case LowererExitRefusal::Tiling:
            return LowererFallback::Tiling;
        case LowererExitRefusal::StateDirty:
            return LowererFallback::StateDirty;
        case LowererExitRefusal::AllocMarker:
            return LowererFallback::AllocMarker;
        case LowererExitRefusal::TileGrowth:
            return LowererFallback::TileGrowth;
        case LowererExitRefusal::AllocPath:
            return LowererFallback::AllocPath;
        case LowererExitRefusal::RingSpace:
            return LowererFallback::RingSpace;
        case LowererExitRefusal::AllocCapacity:
            return LowererFallback::AllocCapacity;
        case LowererExitRefusal::AllocWait:
            return LowererFallback::AllocWait;
        default:
            return LowererFallback::Unreadable;
    }
}

} // namespace

bool LowererConsumeRequested() noexcept
{
    return REXCVAR_GET(native_consume_lowerer);
}

bool LowererTiledConsumeRequested() noexcept
{
    return REXCVAR_GET(native_consume_lowerer_tiled);
}

bool LowererTileListObservationRequested() noexcept
{
    return LowererConsumeRequested() && LowererTiledConsumeRequested();
}

LowererEntry LowererEntryOf(const PPCContext& ctx) noexcept
{
    return { ctx.r3.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, ctx.r1.u32, ctx.r13.u32 };
}

std::optional<LowererFallback> LowererConsumeVerdict(const NativeDrawScope& scope, const LowererEntry& entry, std::uint32_t caller, bool diagnostics, LowererPlan* plan) noexcept
{
    try
    {
        // The IndexedUp wrapper's static child call is nested even when it is the
        // first observed draw of its thread, and is covered by the wrapper's consume.
        if (scope.Observed() && scope.Root() && caller == kIndexedUpLowererReturn)
            return LowererFallback::Nested;
        NativeDrawRootView root;
        ConsumeShape       shape;
        const EntryStride  stride{ false, entry.stride };
        // The pool and out-word objects are tested with the reach, among the stack tests of the gates.
        const StackObjects objects{ [](const StackReach& reach, const NativeDrawRootView& view, const void* context) noexcept
                                    {
                                        return LowererStackObjectsOverlap(
                                            [&view](std::uint32_t address, std::uint32_t& value)
                                            {
                                                return view.read(view.base, address, value);
                                            },
                                            reach,
                                            *static_cast<const LowererEntry*>(context));
                                    },
                                    &entry };
        if (const auto gate = ConsumeStateVerdict(scope, entry.g, LowererTiledConsumeRequested(), false, false, &stride, { entry.pcr, entry.stack }, kLowererStackFootprint, objects, root, shape))
            return FromGate(*gate);
        // The stores go through the dispatcher's writer, which a fixture may not provide.
        if (!root.write || !root.writable)
            return LowererFallback::Unreadable;
        const GuestWordReader read = [&root](std::uint32_t address, std::uint32_t& value)
        {
            return root.read(root.base, address, value);
        };
        LowererWatch watch;
        if (!ReadLowererWatch(read, entry, watch))
            return LowererFallback::Unreadable;
        LowererExitRefusal refusal = LowererExitRefusal::None;
        const auto         exit    = DeriveLowererExitState(entry, watch, &refusal);
        if (!exit)
            return FromRefusal(refusal);
        if (diagnostics || root.effectSample)
            return LowererFallback::Diagnostics;
        auto writes = LowererExitWrites(entry, watch, *exit);
        // A store that cannot land would leave a half-replayed call, so check them all first.
        for (const auto& write : writes)
            if (!root.writable(root.base, write.address, 4))
                return LowererFallback::Unreadable;
        if (plan)
        {
            plan->entry  = entry;
            plan->watch  = watch;
            plan->exit   = *exit;
            plan->writes = std::move(writes);
            plan->tiled  = shape.tiled;
        }
        return std::nullopt;
    }
    catch (...)
    {
        return LowererFallback::Unreadable;
    }
}

bool ConsumeLowerer(const NativeDrawScope& scope, PPCContext& ctx, bool diagnostics) noexcept
{
    if (!LowererConsumeRequested()) [[likely]]
        return false;
    LowererPlan plan;
    // The probe reads the body's exit state, which a skipped Original never produces.
    const auto verdict = LowererConsumeVerdict(scope, LowererEntryOf(ctx), static_cast<std::uint32_t>(ctx.lr), diagnostics || LowererExitCheckRequested(), &plan);
    if (verdict)
    {
        noteFallback(*verdict);
        return false;
    }
    // The verdict held the thread's open root and checked every store.
    const auto root = CurrentNativeDrawRoot();
    for (const auto& write : plan.writes)
        root.write(root.base, write.address, write.value);
    ctx.r3.u64 = 0;
    noteConsumed(plan.tiled);
    return true;
}

#if REREVVED_DEVELOPER
LowererConsumeCounts TakeLowererConsumeCounts() noexcept
{
    LowererConsumeCounts counts;
    const auto           untiled = consumedCounts[0].exchange(0, std::memory_order_relaxed);
    const auto           tiled   = consumedCounts[1].exchange(0, std::memory_order_relaxed);
    counts.consumed              = untiled + tiled;
    counts.consumedTiled         = tiled;
    for (std::size_t i = 0; i < kLowererFallbackCount; ++i)
        counts.fallback[i] = fallbackCounts[i].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatLowererConsumeCounts(const LowererConsumeCounts& counts)
{
    std::string fields = std::format(" draw_consumed_lowerer={} draw_consumed_lowerer_tiled={}", counts.consumed, counts.consumedTiled);
    for (std::size_t i = 0; i < kLowererFallbackCount; ++i)
        if (counts.fallback[i])
            fields += std::format(" draw_fallback_lowerer_{}={}", kLowererFallbackNames[i], counts.fallback[i]);
    return fields;
}

struct LowererExitProbe::State
{
    LowererEntry       entry;
    LowererWatch       before;
    LowererExitState   predicted;
    NativeDrawRootView root;
};

LowererExitProbe::LowererExitProbe(const PPCContext& ctx) noexcept
{
    if (!LowererExitCheckRequested()) [[likely]]
        return;
    try
    {
        // A held direct root only: the thread's open root is this call, called from outside the IndexedUp wrapper.
        const auto root = CurrentNativeDrawRoot();
        if (!root.read || static_cast<std::uint32_t>(ctx.lr) == kIndexedUpLowererReturn)
            return;
        auto armed                 = std::make_unique<State>();
        armed->entry               = LowererEntryOf(ctx);
        armed->root                = root;
        const GuestWordReader read = [&root](std::uint32_t address, std::uint32_t& value)
        {
            return root.read(root.base, address, value);
        };
        // As in the consume verdict, before any other guest word is read.
        StackReach reach;
        if (StackReachRefuses(read, { armed->entry.pcr, armed->entry.stack }, kLowererStackFootprint, armed->entry.g, reach) || IndexedBufferStackObjectsOverlap(read, reach, armed->entry.g) ||
            LowererStackObjectsOverlap(read, reach, armed->entry))
        {
            NoteLowererExitUnmodeled(LowererExitRefusal::StackOverlap);
            return;
        }
        if (!ReadLowererWatch(read, armed->entry, armed->before))
        {
            NoteLowererExitUnreadable();
            return;
        }
        LowererExitRefusal refusal = LowererExitRefusal::None;
        const auto         exit    = DeriveLowererExitState(armed->entry, armed->before, &refusal);
        if (!exit)
        {
            NoteLowererExitUnmodeled(refusal);
            return;
        }
        armed->predicted = *exit;
        state            = std::move(armed);
        context          = &ctx;
        exceptions       = std::uncaught_exceptions();
    }
    catch (...)
    {
        state.reset();
    }
}

LowererExitProbe::~LowererExitProbe()
{
    if (!state)
        return;
    // A call that unwound has no exit to compare.
    if (std::uncaught_exceptions() > exceptions)
        return;
    try
    {
        const auto&           root = state->root;
        const GuestWordReader read = [&root](std::uint32_t address, std::uint32_t& value)
        {
            return root.read(root.base, address, value);
        };
        LowererWatch after;
        if (!ReadLowererWatch(read, state->entry, after))
        {
            NoteLowererExitUnreadable();
            return;
        }
        const auto mismatch = LowererExitMismatches(state->predicted, state->before, after, context->r3.u32);
        NoteLowererExit(mismatch, state->predicted.tiled, LowererObservedFlushWords(state->before, after));
    }
    catch (...)
    {
    }
}
#endif

} // namespace rerevved::gpu::guest_d3d
