#include "user_pointer_consume.h"

#include "lowerer_derive.h"
#include "native_draw_dispatch.h"
#include "ring_stubs.h"
#include "ring_words.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <format>

REXCVAR_DEFINE_BOOL(native_consume_user_pointer,
                    true,
                    "ReRevved",
                    "On by default for renderer=native with native_ring_stubs: skip the IndexedUp and NonindexedUp draw wrappers' Original on untiled "
                    "roots whose dirty low word, with the bit 0x80000 their nested call sets for a changed stride, has no bit of 0x1E0000 and no "
                    "diagnostic needs their body")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_consume_user_pointer_tiled,
                    true,
                    "ReRevved",
                    "On by default, with native_consume_user_pointer: also skip the wrappers' Original on roots whose tiling byte is exactly 0x21 "
                    "when the bracket tripwires hold")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_consume_user_pointer_dirty,
                    true,
                    "ReRevved",
                    "On by default, with native_consume_user_pointer: also skip the wrappers' Original on roots with a dirty bit of 0x1E0000 when "
                    "the shader-state guards hold")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

namespace
{

#if REREVVED_DEVELOPER
// Per wrapper, disjoint by (tiled, dirty, vertex fetch), so one drain never shows a subset above its total.
std::array<std::atomic<std::uint64_t>, kUserPointerWrapperCount * 8>                                      consumedCounts{};
std::array<std::array<std::atomic<std::uint64_t>, kIndexedBufferFallbackCount>, kUserPointerWrapperCount> fallbackCounts{};
#endif

void noteConsumed([[maybe_unused]] UserPointerWrapper wrapper, [[maybe_unused]] bool tiled, [[maybe_unused]] bool dirty, [[maybe_unused]] bool vertexFetch) noexcept
{
#if REREVVED_DEVELOPER
    consumedCounts[static_cast<std::size_t>(wrapper) * 8 + ((tiled ? 1 : 0) | (dirty ? 2 : 0) | (vertexFetch ? 4 : 0))].fetch_add(1, std::memory_order_relaxed);
#endif
}

void noteFallback([[maybe_unused]] UserPointerWrapper wrapper, [[maybe_unused]] UserPointerFallback reason) noexcept
{
#if REREVVED_DEVELOPER
    fallbackCounts[static_cast<std::size_t>(wrapper)][static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
#endif
}

std::optional<UserPointerFallback> ReservationVerdict(UserPointerWrapper wrapper, const PPCContext& ctx, const EntryStride& stride, const NativeDrawRootView& root) noexcept
{
    const auto g    = ctx.r3.u32;
    const auto read = [&root](std::uint32_t address, std::uint32_t& value)
    {
        return root.read(root.base, address, value);
    };
    const auto& footprint = kUserPointerStackFootprints[static_cast<std::size_t>(wrapper)];
    StackReach  reach;
    if (g > UINT32_MAX - footprint.deviceSpan)
        return UserPointerFallback::Unreadable;
    if (StackReachRefuses(read, { ctx.r13.u32, ctx.r1.u32 }, footprint, g, reach) ||
        StackReachesPointee(read, reach, std::uint64_t{ g } + 0x34CC, 152, 8) ||
        StackReachesPointee(read, reach, std::uint64_t{ g } + 0x2A90, 4, 4))
        return UserPointerFallback::StackOverlap;
    LowererWatch watch;
    for (std::uint32_t i = 0; i < 5; ++i)
    {
        std::uint32_t high = 0, low = 0;
        if (!read(g + i * 8, high) || !read(g + i * 8 + 4, low))
            return UserPointerFallback::Unreadable;
        watch.dirty[i] = (std::uint64_t{ high } << 32) | low;
    }
    std::uint32_t high = 0, low = 0, modes = 0, passedStride = stride.value;
    if ((stride.inStack && !read(stride.value, passedStride)) ||
        !read(g + 0x28, high) || !read(g + 0x2C, low) || !read(g + 0x30, watch.cursor) ||
        !read(g + 0x34, watch.limit34) || !read(g + 0x38, watch.limit38) || !read(g + 0x2ABC, modes) ||
        !read(g + 0x2E38, watch.strideWord) || !read(g + 0x3198, watch.vertexShader) || !read(g + 0x31CC, watch.targetCount) ||
        !read(g + 0x33B0, watch.bracket) || !read(g + 0x34CC, watch.poolObject) ||
        !read(g + 0x3A44, watch.poolPos) || !read(g + 0x3A4C, watch.wrapPos) ||
        !read(g + 0x3A54, watch.poolUsed) || !read(g + 0x3A58, watch.poolThreshold))
        return UserPointerFallback::Unreadable;
    watch.mask = (std::uint64_t{ high } << 32) | low;
    if (watch.vertexShader && static_cast<std::uint8_t>(passedStride >> 2) != (watch.strideWord >> 24))
        watch.dirty[2] |= kIndexedBufferStrideDirty;
    // NonindexedUp clears G18 bit 0 before its flush groups (0x826A2BA8, g151).
    if (wrapper == UserPointerWrapper::NonindexedUp)
        watch.dirty[3] &= ~std::uint64_t{ 1 };
    // With only bit 0x80000 and no VS, ADD88 returns through 0x826AE5B8
    // without allocating (g111:13039-13074). Keep AC8C8's original mask test.
    const auto maskedDirty = watch.mask & watch.dirty[2];
    if (!watch.vertexShader && (watch.dirty[2] & kLowererDirtyShaderMask) == kIndexedBufferStrideDirty)
        watch.dirty[2] &= ~std::uint64_t{ kIndexedBufferStrideDirty };
    watch.mask = 0;
    auto flush = LowererFlushWords(watch);
    if (maskedDirty && flush != UINT64_MAX)
        flush = watch.targetCount > kRingMaxTargetCount ? UINT64_MAX : flush + RingWordsAc8c8(watch.targetCount);
    const auto end = std::uint64_t{ watch.cursor } + 4 * (flush == UINT64_MAX ? 0 : flush);
    if ((modes & 0x00200000) || flush == UINT64_MAX || end > UINT32_MAX - 164 || end > watch.limit38 || end >= watch.limit34)
        return UserPointerFallback::Reservation;
    if (modes & 0x01000000)
    {
        std::uint32_t cursor = 0, limit = 0;
        if (!read(g + 0x33C8, cursor) || !read(g + 0x33CC, limit))
            return UserPointerFallback::Unreadable;
        if (std::uint64_t{ cursor } + 16 > limit)
            return UserPointerFallback::Reservation;
    }
    const auto product = [](std::uint32_t count, std::uint32_t bytes)
    {
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(count)) * static_cast<std::int32_t>(bytes)) & ~3U;
    };
    // 3568 forwards r6/r7/r9 as count/count/flags; 3000 forwards r5 as count.
    const std::array<std::uint32_t, 2> sizes{
        product(wrapper == UserPointerWrapper::IndexedUp ? ctx.r6.u32 : ctx.r5.u32, passedStride),
        ((ctx.r9.u32 & 4) ? ctx.r7.u32 : (ctx.r7.u32 + 1) >> 1) << 2
    };
    ResolveWatch view;
    if (watch.poolObject)
    {
        const auto fields = std::uint64_t{ watch.poolObject } + 152;
        if (fields + 8 > (std::uint64_t{ 1 } << 32) || (fields < std::uint64_t{ g } + 0x28 && fields + 8 > g))
            return UserPointerFallback::Reservation;
        if (!read(static_cast<std::uint32_t>(fields), watch.poolBase) || !read(static_cast<std::uint32_t>(fields + 4), watch.poolSize))
            return UserPointerFallback::Unreadable;
        if (!watch.poolBase || std::uint64_t{ watch.poolBase } + (watch.poolSize & ~3U) > UINT32_MAX)
            return UserPointerFallback::Reservation;
    }
    else
    {
        std::uint32_t block = 0;
        if (!read(g + 0x3A3C, view.sideEnd) || !read(g + 0x3A50, view.wrapLimit) || !read(g + 0x3A48, view.segment) ||
            !read(g + 0x33B4, view.bracketNext) || !read(g + 0x2A90, block) || !block || block > UINT32_MAX - 7 || !read(block + 4, view.progress))
            return UserPointerFallback::Unreadable;
        if (block + 4 < std::uint64_t{ g } + 0x28 && std::uint64_t{ block } + 8 > g)
            return UserPointerFallback::Reservation;
        view.limit34 = watch.limit34;
        view.bracket = watch.bracket;
        view.poolPos = watch.poolPos;
        view.wrapPos = watch.wrapPos;
    }
    const unsigned takes = wrapper == UserPointerWrapper::IndexedUp ? 2 : 1;
    for (unsigned i = 0; i < takes; ++i)
    {
        const auto bytes = sizes[i];
        if (std::uint64_t{ watch.poolUsed } + bytes > UINT32_MAX ||
            (watch.poolUsed + bytes > watch.poolThreshold && !watch.bracket))
            return UserPointerFallback::Reservation;
        watch.poolUsed += bytes;
        if (watch.poolObject)
        {
            const auto top = watch.poolPos ? watch.poolPos : watch.poolBase + (watch.poolSize & ~3U);
            if (top < bytes)
                return UserPointerFallback::Reservation;
            const auto buffer = (top - bytes) & ~15U;
            const auto delta  = top - buffer;
            if (!buffer || buffer < end + 164 || watch.limit34 < delta || watch.limit38 < delta)
                return UserPointerFallback::Reservation;
            watch.limit34 -= delta;
            watch.limit38 -= delta;
            watch.poolPos = buffer;
        }
        else
        {
            const auto start    = view.poolPos ? std::uint64_t{ view.poolPos } : std::uint64_t{ view.limit34 } + 60;
            const auto firstEnd = ((start + 15) & ~std::uint64_t{ 15 }) + bytes;
            const auto wrapEnd  = ((std::uint64_t{ view.wrapPos } + 15) & ~std::uint64_t{ 15 }) + bytes;
            if (firstEnd > UINT32_MAX || (firstEnd > view.sideEnd && wrapEnd > UINT32_MAX))
                return UserPointerFallback::Reservation;
            const auto take = ModelNullPoolAllocator(view, bytes, 16);
            if (!take.ok || take.waits || take.end == bytes)
                return UserPointerFallback::Reservation;
            (take.wrap ? view.wrapPos : view.poolPos) = take.end;
        }
    }
    return std::nullopt;
}

} // namespace

bool UserPointerConsumeRequested() noexcept
{
    return RingStubsPublished() && REXCVAR_GET(native_consume_user_pointer);
}

bool UserPointerTiledConsumeRequested() noexcept
{
    return REXCVAR_GET(native_consume_user_pointer_tiled);
}

bool UserPointerDirtyConsumeRequested() noexcept
{
    return REXCVAR_GET(native_consume_user_pointer_dirty);
}

EntryStride UserPointerEntryStride(UserPointerWrapper wrapper, const PPCContext& ctx) noexcept
{
    if (wrapper == UserPointerWrapper::IndexedUp)
        return { true, ctx.r1.u32 + kIndexedUpStrideSlot };
    return { false, ctx.r7.u32 };
}

std::optional<UserPointerFallback> UserPointerConsumeVerdict(UserPointerWrapper wrapper, const NativeDrawScope& scope, std::uint32_t graphics, const EntryStride& stride, bool diagnostics, bool* tiled, bool* dirty, bool* vertexFetch) noexcept
{
    if (tiled)
        *tiled = false;
    if (dirty)
        *dirty = false;
    if (vertexFetch)
        *vertexFetch = false;
    NativeDrawRootView root;
    ConsumeShape       shape;
    if (const auto fallback = ConsumeStateVerdict(scope, graphics, UserPointerTiledConsumeRequested(), UserPointerDirtyConsumeRequested(), DirtyVertexFetchConsumeRequested(), &stride, { scope.EntryPcr(), scope.EntryStack() }, kUserPointerStackFootprints[static_cast<std::size_t>(wrapper)], {}, root, shape))
        return fallback;
    if (diagnostics)
        return UserPointerFallback::Diagnostics;
    if (tiled)
        *tiled = shape.tiled;
    if (dirty)
        *dirty = shape.dirty;
    if (vertexFetch)
        *vertexFetch = shape.vertexFetch;
    return std::nullopt;
}

bool ConsumeUserPointer(UserPointerWrapper wrapper, const NativeDrawScope& scope, const PPCContext& ctx, bool diagnostics) noexcept
{
    if (!UserPointerConsumeRequested()) [[likely]]
        return false;
    // Held until the decision (and any guest store) is done; a withdrawal waits for it.
    const RingStubLease lease;
    if (!lease) [[unlikely]]
        return false;
    bool       tiled = false, dirty = false, vertexFetch = false;
    const auto stride  = UserPointerEntryStride(wrapper, ctx);
    auto       verdict = UserPointerConsumeVerdict(wrapper, scope, ctx.r3.u32, stride, diagnostics, &tiled, &dirty, &vertexFetch);
    const auto root    = CurrentNativeDrawRoot();
    if (!verdict && !PreflightDrawDirtyClear(root, ctx.r3.u32))
        verdict = UserPointerFallback::Unreadable;
    if (!verdict)
        verdict = ReservationVerdict(wrapper, ctx, stride, root);
    if (!verdict)
    {
        // Original would have made the wrapper's one nested call.
        NoteNativeDrawConsumedChild(scope);
        noteConsumed(wrapper, tiled, dirty, vertexFetch);
        if (vertexFetch)
            NoteVertexFetchConsume(ctx.r3.u32, &stride);
        ReplayDrawDirtyClear(root, ctx.r3.u32, true);
        return true;
    }
    noteFallback(wrapper, *verdict);
    return false;
}

#if REREVVED_DEVELOPER
UserPointerConsumeCounts TakeUserPointerConsumeCounts() noexcept
{
    UserPointerConsumeCounts counts;
    for (std::size_t wrapper = 0; wrapper < kUserPointerWrapperCount; ++wrapper)
    {
        for (std::size_t subset = 0; subset < 8; ++subset)
        {
            const auto taken = consumedCounts[wrapper * 8 + subset].exchange(0, std::memory_order_relaxed);
            counts.consumed[wrapper] += taken;
            if (subset & 1)
                counts.consumedTiled += taken;
            if (subset & 2)
                counts.consumedDirty += taken;
            if (subset & 4)
                counts.consumedDirtyVertexFetch += taken;
        }
        for (std::size_t i = 0; i < kIndexedBufferFallbackCount; ++i)
            counts.fallback[wrapper][i] = fallbackCounts[wrapper][i].exchange(0, std::memory_order_relaxed);
    }
    return counts;
}

std::string FormatUserPointerConsumeCounts(const UserPointerConsumeCounts& counts)
{
    std::string fields;
    for (std::size_t wrapper = 0; wrapper < kUserPointerWrapperCount; ++wrapper)
        fields += std::format(" draw_consumed_{}={}", kUserPointerWrapperNames[wrapper], counts.consumed[wrapper]);
    fields += std::format(" draw_consumed_user_pointer_tiled={} draw_consumed_user_pointer_dirty={} draw_consumed_user_pointer_dirty_vertex_fetch={}",
                          counts.consumedTiled,
                          counts.consumedDirty,
                          counts.consumedDirtyVertexFetch);
    for (std::size_t wrapper = 0; wrapper < kUserPointerWrapperCount; ++wrapper)
        for (std::size_t i = 0; i < kIndexedBufferFallbackCount; ++i)
            if (counts.fallback[wrapper][i])
                fields += std::format(" draw_fallback_{}_{}={}", kUserPointerWrapperNames[wrapper], kIndexedBufferFallbackNames[i], counts.fallback[wrapper][i]);
    return fields;
}
#endif

} // namespace rerevved::gpu::guest_d3d
