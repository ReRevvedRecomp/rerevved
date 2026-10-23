#include "indexed_buffer_consume.h"

#include "indexed_buffer_gate.h"
#include "lowerer_consume.h"
#include "native_draw_dispatch.h"
#include "ring_stubs.h"
#include "user_pointer_consume.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <format>

REXCVAR_DEFINE_BOOL(native_consume_indexed_buffer,
                    true,
                    "ReRevved",
                    "On by default for renderer=native with native_ring_stubs: skip the IndexedBuffer draw entry's Original on untiled roots whose "
                    "dirty low word has no bit of 0x1E0000 and no diagnostic needs its body")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_consume_indexed_buffer_tiled,
                    true,
                    "ReRevved",
                    "On by default, with native_consume_indexed_buffer: also skip the IndexedBuffer draw entry's Original on roots whose tiling byte is "
                    "exactly 0x21 when the bracket tripwires hold")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_consume_indexed_buffer_dirty,
                    true,
                    "ReRevved",
                    "On by default, with native_consume_indexed_buffer: also skip the IndexedBuffer draw entry's Original on roots with a dirty bit "
                    "of 0x1E0000 when the shader-state guards hold")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_consume_dirty_vertex_fetch,
                    false,
                    "ReRevved",
                    "Off by default, with the dirty cvars of the IndexedBuffer entry and the user-pointer wrappers: also skip Original on dirty roots whose "
                    "Original would patch the VS vertex fetch microcode, which the host re-derives from the shader's templates; while off, such roots fall back as dirty_vertex_fetch")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

namespace
{

#if REREVVED_DEVELOPER
// Disjoint by (tiled, dirty, vertex fetch), so one drain never shows a subset above its total.
std::array<std::atomic<std::uint64_t>, 8>                           consumedCounts{};
std::array<std::atomic<std::uint64_t>, kIndexedBufferFallbackCount> fallbackCounts{};
std::atomic<std::uint64_t>                                          listRecords{ 0 }, listEmpty{ 0 }, listMisaligned{ 0 };
#endif
std::atomic<VertexFetchConsumeObserver> vertexFetchObserver{ nullptr };

void noteConsumed([[maybe_unused]] bool tiled, [[maybe_unused]] bool dirty, [[maybe_unused]] bool vertexFetch) noexcept
{
#if REREVVED_DEVELOPER
    consumedCounts[(tiled ? 1 : 0) | (dirty ? 2 : 0) | (vertexFetch ? 4 : 0)].fetch_add(1, std::memory_order_relaxed);
#endif
}

void noteFallback([[maybe_unused]] IndexedBufferFallback reason) noexcept
{
#if REREVVED_DEVELOPER
    fallbackCounts[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
#endif
}

} // namespace

void SetVertexFetchConsumeObserver(VertexFetchConsumeObserver observer) noexcept
{
    vertexFetchObserver.store(observer, std::memory_order_release);
}

void NoteVertexFetchConsume(const VertexFetchConsume& consume) noexcept
{
    if (const auto observer = vertexFetchObserver.load(std::memory_order_acquire)) [[unlikely]]
        observer(consume);
}

void NoteVertexFetchConsume(std::uint32_t graphics, const EntryStride* stride) noexcept
{
    // The root is built only for an observer.
    if (vertexFetchObserver.load(std::memory_order_acquire)) [[unlikely]]
    {
        const auto root = CurrentNativeDrawRoot();
        NoteVertexFetchConsume({ &root, graphics, stride });
    }
}

bool IndexedBufferConsumeRequested() noexcept
{
    return RingStubsPublished() && REXCVAR_GET(native_consume_indexed_buffer);
}

bool DirtyVertexFetchConsumeRequested() noexcept
{
    return REXCVAR_GET(native_consume_dirty_vertex_fetch);
}

bool IndexedBufferTiledConsumeRequested() noexcept
{
    return REXCVAR_GET(native_consume_indexed_buffer_tiled);
}

bool IndexedBufferDirtyConsumeRequested() noexcept
{
    return REXCVAR_GET(native_consume_indexed_buffer_dirty);
}

IndexedBufferTiledState CheckIndexedBufferTiledState(IndexedBufferWordReader read, const std::uint8_t* base, std::uint32_t graphics) noexcept
{
    constexpr std::uint32_t kHighest = kIndexedBufferBracketOffset;
    static_assert(kHighest > kIndexedBufferPendingOffset && kHighest > kIndexedBufferModeWord);
    std::uint32_t bracket = 0, pending = 0, mode = 0;
    if (!read || !graphics || graphics > 0xFFFFFFFFU - (kHighest + 3) || !read(base, graphics + kIndexedBufferBracketOffset, bracket) ||
        !read(base, graphics + kIndexedBufferPendingOffset, pending) || !read(base, graphics + kIndexedBufferModeWord, mode))
        return IndexedBufferTiledState::Unreadable;
    return IndexedBufferTiledStateHolds(bracket, pending, mode) ? IndexedBufferTiledState::Holds : IndexedBufferTiledState::Tripped;
}

namespace
{

// Whether 0x826ADD88 would patch the VS's vertex fetch microcode in place
// (see kIndexedBufferDeclTagOffset), for a VS and PS already read.
IndexedBufferDirtyState CheckVertexFetchPatch(IndexedBufferWordReader read, const std::uint8_t* base, std::uint32_t graphics, std::uint32_t vertex, std::uint32_t pixel, std::uint32_t modes, std::optional<std::uint8_t> stream0) noexcept
{
    if (modes & (kIndexedBufferFetchCommandBuffer | kIndexedBufferFetchNoPatch))
        return IndexedBufferDirtyState::Holds;
    const auto word = [&](std::uint64_t address, std::uint32_t& out)
    {
        return address <= 0xFFFFFFFFULL - 3 && read(base, static_cast<std::uint32_t>(address), out);
    };
    const auto pair = [&](std::uint64_t address, std::uint64_t& out)
    {
        std::uint32_t high = 0, low = 0;
        if (!word(address, high) || !word(address + 4, low))
            return false;
        out = static_cast<std::uint64_t>(high) << 32 | low;
        return true;
    };
    std::uint32_t decl = 0, flags = 0;
    if (!word(static_cast<std::uint64_t>(graphics) + kIndexedBufferVertexDeclOffset, decl))
        return IndexedBufferDirtyState::Unreadable;
    if (!decl)
        return IndexedBufferDirtyState::Holds;
    if (!word(static_cast<std::uint64_t>(vertex) + kIndexedBufferVertexFlagsOffset, flags))
        return IndexedBufferDirtyState::Unreadable;
    if (flags & kIndexedBufferVertexNoPatch)
        return IndexedBufferDirtyState::Holds;
    const std::uint64_t record    = static_cast<std::uint64_t>(vertex) + kIndexedBufferFetchCacheOffset + (!pixel && (flags & kIndexedBufferVertexAlternate) ? kIndexedBufferFetchCacheStride : 0);
    std::uint32_t       recordTag = 0, declTag = 0;
    std::uint64_t       recorded0 = 0, recorded1 = 0, mask0 = 0, mask1 = 0, stream[2]{};
    if (!word(record, recordTag) || !word(static_cast<std::uint64_t>(decl) + kIndexedBufferDeclTagOffset, declTag) || !pair(record + 8, recorded0) || !pair(record + 16, recorded1) ||
        !pair(static_cast<std::uint64_t>(decl) + kIndexedBufferDeclMaskOffset, mask0) || !pair(static_cast<std::uint64_t>(decl) + kIndexedBufferDeclMaskOffset + 8, mask1) ||
        !pair(static_cast<std::uint64_t>(graphics) + kIndexedBufferStreamOffset, stream[0]) || !pair(static_cast<std::uint64_t>(graphics) + kIndexedBufferStreamOffset + 8, stream[1]))
        return IndexedBufferDirtyState::Unreadable;
    if (stream0)
        stream[0] = (stream[0] & 0x00FFFFFFFFFFFFFFULL) | static_cast<std::uint64_t>(*stream0) << 56;
    const bool hit = recordTag == declTag && (((recorded0 ^ stream[0]) & mask0) | ((recorded1 ^ stream[1]) & mask1)) == 0;
    return hit ? IndexedBufferDirtyState::Holds : IndexedBufferDirtyState::VertexFetch;
}

} // namespace

IndexedBufferDirtyState CheckIndexedBufferDirtyState(IndexedBufferWordReader read, const std::uint8_t* base, std::uint32_t graphics, std::uint32_t dirtyLow, std::optional<std::uint8_t> stream0) noexcept
{
    constexpr std::uint32_t kHighest = kIndexedBufferVertexShaderOffset;
    static_assert(kHighest > kIndexedBufferPixelShaderOffset && kHighest > kIndexedBufferModeControlOffset && kHighest > kIndexedBufferModeWord);
    std::uint32_t modes = 0, vertex = 0;
    if (!read || !graphics || graphics > 0xFFFFFFFFU - (kHighest + 3) || !read(base, graphics + kIndexedBufferModeWord, modes))
        return IndexedBufferDirtyState::Unreadable;
    if (modes & kIndexedBufferRecordedShader)
        return IndexedBufferDirtyState::Recorded;
    if (!read(base, graphics + kIndexedBufferVertexShaderOffset, vertex))
        return IndexedBufferDirtyState::Unreadable;
    if (!vertex)
        return IndexedBufferDirtyState::Holds;
    std::uint32_t pixel = 0, control = 0;
    if (!read(base, graphics + kIndexedBufferPixelShaderOffset, pixel) || !read(base, graphics + kIndexedBufferModeControlOffset, control))
        return IndexedBufferDirtyState::Unreadable;
    const auto mode = control & 7;
    if (pixel ? (dirtyLow & kIndexedBufferDirtyPixel) && mode != 4 : mode != 5)
        return IndexedBufferDirtyState::ModeToggle;
    if (modes & kIndexedBufferZPrepass)
    {
        std::uint32_t flags = 0;
        if (vertex > 0xFFFFFFFFU - (kIndexedBufferVertexFlagsOffset + 3) || !read(base, vertex + kIndexedBufferVertexFlagsOffset, flags))
            return IndexedBufferDirtyState::Unreadable;
        if (flags & 0x20)
            return IndexedBufferDirtyState::ZPrepass;
    }
    if (dirtyLow & kIndexedBufferStrideDirty)
        return CheckVertexFetchPatch(read, base, graphics, vertex, pixel, modes, stream0);
    return IndexedBufferDirtyState::Holds;
}

std::optional<IndexedBufferFallback> ConsumeStateVerdict(const NativeDrawScope& scope,
                                                         std::uint32_t          graphics,
                                                         bool                   tiledCvar,
                                                         bool                   dirtyCvar,
                                                         bool                   vertexFetchCvar,
                                                         const EntryStride*     stride,
                                                         const StackFrame&      frame,
                                                         const StackFootprint&  footprint,
                                                         const StackObjects&    objects,
                                                         NativeDrawRootView&    root,
                                                         ConsumeShape&          shape) noexcept
{
    shape = {};
    if (!scope.Observed())
        return IndexedBufferFallback::Dispatcher;
    if (!scope.Root())
        return IndexedBufferFallback::Nested;
    root = CurrentNativeDrawRoot();
    if (!root.read)
        return IndexedBufferFallback::Dispatcher;
    if (!RingStubsReady(graphics))
        return IndexedBufferFallback::Stubs;
    // Before any other guest word is read: a skipped Original's saves would change the words below. A device
    // whose block would wrap past 2^32 cannot be placed (its words are unreadable), as for the other consumes.
    if (graphics > 0xFFFFFFFFU - footprint.deviceSpan)
        return IndexedBufferFallback::Unreadable;
    {
        const auto word = [&root](std::uint32_t address, std::uint32_t& value)
        {
            return root.read(root.base, address, value);
        };
        StackReach reach;
        if (StackReachRefuses(word, frame, footprint, graphics, reach) || IndexedBufferStackObjectsOverlap(word, reach, graphics) ||
            (objects.overlaps && objects.overlaps(reach, root, objects.context)))
            return IndexedBufferFallback::StackOverlap;
    }
    bool tiledRecord = false;
    switch (IndexedBufferConsumeGate(root.read, root.base, graphics))
    {
        case IndexedBufferGate::Admit:
            break;
        case IndexedBufferGate::TiledRecord:
            // Without the tiled cvar the record branch is as any set tiling byte.
            if (!tiledCvar)
                return IndexedBufferFallback::Tiling;
            tiledRecord = true;
            break;
        case IndexedBufferGate::TilingSet:
            return IndexedBufferFallback::Tiling;
        case IndexedBufferGate::Unreadable:
            return IndexedBufferFallback::Unreadable;
    }
    std::uint32_t dirtyLow = 0;
    if (graphics > 0xFFFFFFFFU - (kIndexedBufferDirtyLowOffset + 3) || !root.read(root.base, graphics + kIndexedBufferDirtyLowOffset, dirtyLow))
        return IndexedBufferFallback::Unreadable;
    std::optional<std::uint8_t> stream0;
    if (stride)
    {
        constexpr std::uint32_t kHighest = kIndexedBufferVertexShaderOffset;
        static_assert(kHighest > kIndexedBufferStrideByteOffset);
        std::uint32_t word = stride->value, recorded = 0, vertex = 0;
        if (stride->inStack && (word > 0xFFFFFFFFU - 3 || !root.read(root.base, word, word)))
            return IndexedBufferFallback::Unreadable;
        if (graphics > 0xFFFFFFFFU - (kHighest + 3) || !root.read(root.base, graphics + kIndexedBufferStrideByteOffset, recorded) ||
            !root.read(root.base, graphics + kIndexedBufferVertexShaderOffset, vertex))
            return IndexedBufferFallback::Unreadable;
        if (((word >> 2) & 0xFF) != (recorded >> 24) && vertex)
            dirtyLow |= kIndexedBufferStrideDirty;
        stream0 = static_cast<std::uint8_t>(word >> 2);
    }
    const bool stateDirty  = dirtyLow & kIndexedBufferDirtyLowMask;
    bool       vertexFetch = false;
    if (stateDirty)
    {
        if (!dirtyCvar)
            return IndexedBufferFallback::StateDirty;
        switch (CheckIndexedBufferDirtyState(root.read, root.base, graphics, dirtyLow, stream0))
        {
            case IndexedBufferDirtyState::Holds:
                break;
            case IndexedBufferDirtyState::Recorded:
                return IndexedBufferFallback::DirtyRecorded;
            case IndexedBufferDirtyState::ModeToggle:
                return IndexedBufferFallback::DirtyMode;
            case IndexedBufferDirtyState::ZPrepass:
                return IndexedBufferFallback::DirtyZPrepass;
            case IndexedBufferDirtyState::VertexFetch:
                // The host derives the patched VS from the shader's templates, so the
                // words the skipped patch would write are not read from guest memory.
                if (!vertexFetchCvar)
                    return IndexedBufferFallback::DirtyVertexFetch;
                vertexFetch = true;
                break;
            case IndexedBufferDirtyState::Unreadable:
                return IndexedBufferFallback::Unreadable;
        }
    }
    if (tiledRecord)
    {
        switch (CheckIndexedBufferTiledState(root.read, root.base, graphics))
        {
            case IndexedBufferTiledState::Holds:
                break;
            case IndexedBufferTiledState::Tripped:
                return IndexedBufferFallback::TiledState;
            case IndexedBufferTiledState::Unreadable:
                return IndexedBufferFallback::Unreadable;
        }
    }
    shape = { tiledRecord, stateDirty, vertexFetch };
    return std::nullopt;
}

std::optional<IndexedBufferFallback> IndexedBufferConsumeVerdict(const NativeDrawScope& scope, std::uint32_t graphics, bool diagnostics, bool* tiled, bool* dirty, bool* vertexFetch) noexcept
{
    if (tiled)
        *tiled = false;
    if (dirty)
        *dirty = false;
    if (vertexFetch)
        *vertexFetch = false;
    NativeDrawRootView root;
    ConsumeShape       shape;
    if (const auto fallback = ConsumeStateVerdict(scope, graphics, IndexedBufferTiledConsumeRequested(), IndexedBufferDirtyConsumeRequested(), DirtyVertexFetchConsumeRequested(), nullptr, { scope.EntryPcr(), scope.EntryStack() }, kIndexedBufferStackFootprint, {}, root, shape))
        return fallback;
    if (diagnostics || root.effectSample)
        return IndexedBufferFallback::Diagnostics;
    if (tiled)
        *tiled = shape.tiled;
    if (dirty)
        *dirty = shape.dirty;
    if (vertexFetch)
        *vertexFetch = shape.vertexFetch;
    return std::nullopt;
}

bool PreflightDrawDirtyClear(const NativeDrawRootView& root, std::uint32_t graphics) noexcept
{
    if (!root.read || !root.write || !root.writable || !graphics || (graphics & 7) || graphics > UINT32_MAX - 0x27)
        return false;
    for (std::uint32_t offset = 0; offset < 0x28; offset += 4)
    {
        std::uint32_t value = 0;
        if (!root.read(root.base, graphics + offset, value) || !root.writable(root.base, graphics + offset, 4))
            return false;
    }
    return true;
}

void ReplayDrawDirtyClear(const NativeDrawRootView& root, std::uint32_t graphics, bool rearm) noexcept
{
    // Zero std sites: .91.cpp:13400/13418/13543/13588/13675 (IndexedBuffer),
    // .150.cpp:12658/12676/12801/12846/12933 (IndexedUp lowerer), and
    // .79.cpp:13056/13074/13199/13244/13331 (NonindexedUp sink).
    for (std::uint32_t offset = 0; offset < 0x28; offset += 8)
    {
        root.write(root.base, graphics + offset, 0);
        root.write(root.base, graphics + offset + 4, 0);
    }
    if (rearm)
    {
        // std at 826A3520 (IndexedUp) / 826A2FD8 (NonindexedUp).
        root.write(root.base, graphics + 0x18, 0);
        root.write(root.base, graphics + 0x1C, 1);
    }
}

bool ConsumeIndexedBuffer(const NativeDrawScope& scope, const PPCContext& ctx, bool diagnostics) noexcept
{
    if (!IndexedBufferConsumeRequested()) [[likely]]
        return false;
    // Held until the decision (and any guest store) is done; a withdrawal waits for it.
    const RingStubLease lease;
    if (!lease) [[unlikely]]
        return false;
    bool       tiled = false, dirty = false, vertexFetch = false;
    auto       verdict = IndexedBufferConsumeVerdict(scope, ctx.r3.u32, diagnostics, &tiled, &dirty, &vertexFetch);
    const auto root    = CurrentNativeDrawRoot();
    if (!verdict && !PreflightDrawDirtyClear(root, ctx.r3.u32))
        verdict = IndexedBufferFallback::Unreadable;
    if (!verdict)
    {
        noteConsumed(tiled, dirty, vertexFetch);
        if (vertexFetch)
            NoteVertexFetchConsume(ctx.r3.u32, nullptr);
        ReplayDrawDirtyClear(root, ctx.r3.u32, false);
        return true;
    }
    noteFallback(*verdict);
    return false;
}

#if REREVVED_DEVELOPER
IndexedBufferConsumeCounts TakeIndexedBufferConsumeCounts() noexcept
{
    IndexedBufferConsumeCounts counts;
    for (std::size_t subset = 0; subset < consumedCounts.size(); ++subset)
    {
        const auto taken = consumedCounts[subset].exchange(0, std::memory_order_relaxed);
        counts.consumed += taken;
        if (subset & 1)
            counts.consumedTiled += taken;
        if (subset & 2)
            counts.consumedDirty += taken;
        if (subset & 4)
            counts.consumedDirtyVertexFetch += taken;
    }
    for (std::size_t i = 0; i < kIndexedBufferFallbackCount; ++i)
        counts.fallback[i] = fallbackCounts[i].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatIndexedBufferConsumeCounts(const IndexedBufferConsumeCounts& counts)
{
    std::string fields = std::format(" draw_consumed_indexed_buffer={} draw_consumed_indexed_buffer_tiled={} draw_consumed_indexed_buffer_dirty={} draw_consumed_indexed_buffer_dirty_vertex_fetch={}",
                                     counts.consumed,
                                     counts.consumedTiled,
                                     counts.consumedDirty,
                                     counts.consumedDirtyVertexFetch);
    for (std::size_t i = 0; i < kIndexedBufferFallbackCount; ++i)
        if (counts.fallback[i])
            fields += std::format(" draw_fallback_indexed_buffer_{}={}", kIndexedBufferFallbackNames[i], counts.fallback[i]);
    return fields;
}

#endif

#if REREVVED_DEVELOPER
bool TileListObservationRequested() noexcept
{
    return (IndexedBufferConsumeRequested() && IndexedBufferTiledConsumeRequested()) || (UserPointerConsumeRequested() && UserPointerTiledConsumeRequested()) ||
           LowererTileListObservationRequested();
}

void ObserveTileList(IRingStubMemory& memory, std::uint32_t graphics) noexcept
{
    std::optional<std::uint32_t> chunk, cursor;
    if (graphics && graphics <= 0xFFFFFFFFU - (kTileListCursorOffset + 3))
    {
        chunk  = memory.Read32(graphics + kTileListChunkOffset);
        cursor = memory.Read32(graphics + kTileListCursorOffset);
    }
    // An unreadable header is counted as a list that cannot be walked.
    NoteTileList(chunk && cursor ? ClassifyTileList(*chunk, *cursor) : TileListShape{ 0, false, true });
}

void NoteTileList(const TileListShape& shape) noexcept
{
    if (shape.misaligned)
        listMisaligned.fetch_add(1, std::memory_order_relaxed);
    else if (shape.empty)
        listEmpty.fetch_add(1, std::memory_order_relaxed);
    listRecords.fetch_add(shape.records, std::memory_order_relaxed);
}

TileListCounts TakeTileListCounts() noexcept
{
    return { listRecords.exchange(0, std::memory_order_relaxed), listEmpty.exchange(0, std::memory_order_relaxed), listMisaligned.exchange(0, std::memory_order_relaxed) };
}

std::string FormatTileListCounts(const TileListCounts& counts)
{
    return std::format(" tiling_list_records={} tiling_list_empty={} tiling_list_misaligned={}", counts.records, counts.empty, counts.misaligned);
}
#endif

} // namespace rerevved::gpu::guest_d3d
