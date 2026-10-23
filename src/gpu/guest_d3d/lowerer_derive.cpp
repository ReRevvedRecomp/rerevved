#include "lowerer_derive.h"

#include "ring_words.h"

#include <rex/cvar.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <format>

#if REREVVED_DEVELOPER
REXCVAR_DEFINE_BOOL(native_lowerer_exit_check,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native: compare the exit state the lowerer consume (native_consume_lowerer) would leave with the guest "
                    "words after every lowerer call whose Original runs, on a root draw under a dispatcher. Observes only; lowerer_exit_* fields on the "
                    "native_frame_timing line")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#endif
namespace rerevved::gpu::guest_d3d
{

namespace
{

// Alignment of both side buffers: the r5 the lowerer passes to 0x8269B818 (g63:13430, 13441).
constexpr std::uint32_t kSideBufferAlignment = 16;
// The pool's failure test adds this to the cursor (g100:13270-13282).
constexpr std::uint32_t kPoolCursorMargin = 164;

constexpr std::uint64_t Runs(std::uint64_t bits) noexcept
{
    return static_cast<std::uint64_t>(std::popcount(bits & ~(bits << 1)));
}

// A payload of `perBit` words per set bit and `perRun` words of header and filler per run of set bits.
constexpr std::uint64_t RunWords(std::uint64_t bits, std::uint64_t perBit, std::uint64_t perRun) noexcept
{
    return perBit * static_cast<std::uint64_t>(std::popcount(bits)) + perRun * Runs(bits);
}

} // namespace

LowererDraw DeriveLowererDraw(const LowererEntry& entry) noexcept
{
    LowererDraw out;
    // g63:13055: the signed product of r6 and r9; g63:13070: its low 32 bits shifted right two.
    const auto product = static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(entry.vertexCount)) * static_cast<std::int32_t>(entry.stride));
    out.vertexDwords   = product >> 2;
    // g63:13067-13081: r8 bit 2 keeps the count, else two 16-bit indices share a dword.
    out.indices32   = (entry.flags & 4) != 0;
    out.indexDwords = out.indices32 ? entry.indexCount : (entry.indexCount + 1) >> 1;
    out.vertexBytes = out.vertexDwords << 2;
    out.indexBytes  = out.indexDwords << 2;
    out.strideByte  = static_cast<std::uint8_t>(entry.stride >> 2);
    return out;
}

bool ReadLowererWatch(const GuestWordReader& read, const LowererEntry& entry, LowererWatch& out)
{
    const auto g = entry.g;
    if (!g || g > UINT32_MAX - kLowererDeviceSpan || !entry.indexOut || entry.stack > UINT32_MAX - kLowererVertexOutSlot - 3)
        return false;
    std::array<std::uint32_t, 10> dirty{};
    std::uint32_t                 maskHigh = 0, maskLow = 0, state = 0;
    bool                          ok = true;
    for (std::uint32_t i = 0; i < dirty.size(); ++i)
        ok = ok && read(g + 4 * i, dirty[i]);
    ok = ok && read(g + 0x28, maskHigh) && read(g + 0x2C, maskLow);
    ok = ok && read(g + 0x30, out.cursor) && read(g + 0x34, out.limit34) && read(g + 0x38, out.limit38) && read(g + 0x2A9C, out.fence) && read(g + 0x2ABC, state);
    ok = ok && read(g + 0x2E38, out.strideWord) && read(g + 0x3198, out.vertexShader) && read(g + 0x31CC, out.targetCount);
    ok = ok && read(g + 0x33B0, out.bracket) && read(g + 0x33B4, out.bracketNext);
    ok = ok && read(g + 0x33C4, out.tileChunk) && read(g + 0x33C8, out.tileCursor) && read(g + 0x33CC, out.tileLimit);
    ok = ok && read(g + 0x3484, out.pendingEnd) && read(g + 0x3488, out.vertexDest) && read(g + 0x348C, out.indexDest) && read(g + 0x3490, out.vertexDwords) &&
         read(g + 0x3494, out.indexDwords);
    ok = ok && read(g + 0x34CC, out.poolObject) && read(g + 0x3A44, out.poolPos) && read(g + 0x3A4C, out.wrapPos) && read(g + 0x3A54, out.poolUsed) && read(g + 0x3A58, out.poolThreshold);
    // The out words: the slot in the caller's frame names the vertex out address.
    ok = ok && read(entry.stack + kLowererVertexOutSlot, out.vertexOutAddress) && out.vertexOutAddress != 0 && read(out.vertexOutAddress, out.vertexOut) && read(entry.indexOut, out.indexOut);
    // The pool's fixed buffer words, read only through a pool object.
    out.poolBase = out.poolSize = 0;
    if (ok && out.poolObject)
        ok = out.poolObject <= UINT32_MAX - 160 && read(out.poolObject + 152, out.poolBase) && read(out.poolObject + 156, out.poolSize);
    // The null pool allocator's words, read only without a pool object. 0x8269B158
    // loads the progress word through the block pointer G+0x2A90; a null
    // block is not a word the host can tell.
    out.sideEnd = out.wrapLimit = out.segment = out.progress = 0;
    if (ok && !out.poolObject)
    {
        std::uint32_t block = 0;
        ok                  = read(g + 0x3A3C, out.sideEnd) && read(g + 0x3A50, out.wrapLimit) && read(g + 0x3A48, out.segment) && read(g + 0x2A90, block) && block != 0 &&
                              block <= UINT32_MAX - 4 && read(block + 4, out.progress);
    }
    if (!ok)
        return false;
    for (std::size_t i = 0; i < out.dirty.size(); ++i)
        out.dirty[i] = (std::uint64_t{ dirty[2 * i] } << 32) | dirty[2 * i + 1];
    out.mask       = (std::uint64_t{ maskHigh } << 32) | maskLow;
    out.tilingByte = static_cast<std::uint8_t>(state >> 24);
    out.allocByte  = static_cast<std::uint8_t>(state >> 16);
    out.stateByte  = static_cast<std::uint8_t>(state >> 8);
    return true;
}

std::uint64_t LowererFlushWords(const LowererWatch& entry) noexcept
{
    const auto& d = entry.dirty;
    // 0x826ADD88 (g63:13159-13170): its words follow shader records read from guest memory.
    if (static_cast<std::uint32_t>(d[2]) & kLowererDirtyShaderMask)
        return UINT64_MAX;
    // 0x826ACFF8 on G+0x00 and G+0x08 (g63:13132, 13150).
    std::uint64_t words = RunWords(d[0], 16, 4) + RunWords(d[1], 16, 4);
    // 0x826AC8C8 when the mask meets G+0x10 (g63:13175-13190).
    if (entry.mask & d[2])
    {
        if (entry.targetCount > kRingMaxTargetCount)
            return UINT64_MAX;
        words += RingWordsAc8c8(entry.targetCount);
    }
    // 0x826ACC60 on G+0x10 bits 0..11, 12..16, 42..57 and 21..41 (g63:13195-13274).
    words += RingGroupWords(d[2], 0, 12) + RingGroupWords(d[2], 12, 5) + RingGroupWords(d[2], 42, 16) + RingGroupWords(d[2], 21, 21);
    // 0x826ACEA0 on the G+0x18 low word, then 0x826ACC60 on bits 34..54 (g63:13284-13319).
    if (const auto low = d[3] & 0xFFFFFFFFULL)
        words += RunWords(low, 6, 2) + 4;
    words += RingGroupWords(d[3], 34, 21);
    // 0x826ACC60 on G+0x20 bits 0..37 and 38..45, 0x826ACD20 on 49..54, and bit 56 (g63:13329-13406).
    words += RingGroupWords(d[4], 0, 38) + RingGroupWords(d[4], 38, 8);
    if (const auto bits = (d[4] >> 49) & 0x3F)
        words += 13 + RunWords(bits, 4, 2);
    if ((d[4] >> 56) & 1)
        words += 1 + 40;
    return words;
}

std::optional<LowererExitState> DeriveLowererExitState(const LowererEntry& entry, const LowererWatch& watch, LowererExitRefusal* refusal, std::string* why)
{
    const auto refuse = [&](LowererExitRefusal reason, const char* text) -> std::optional<LowererExitState>
    {
        if (refusal)
            *refusal = reason;
        if (why)
            *why = text;
        return std::nullopt;
    };
    if (refusal)
        *refusal = LowererExitRefusal::None;
    if (watch.tilingByte != 0 && watch.tilingByte != kLowererTiledByte)
        return refuse(LowererExitRefusal::Tiling, "tiling_byte");
    const bool tiled = watch.tilingByte == kLowererTiledByte;
    const auto draw  = DeriveLowererDraw(entry);
    // g63:13084-13106: the stride byte against the recorded one sets the 0x80000 that runs 0x826ADD88.
    const bool strideDirty = draw.strideByte != (watch.strideWord >> 24) && watch.vertexShader != 0;
    if ((static_cast<std::uint32_t>(watch.dirty[2]) & kLowererDirtyShaderMask) || strideDirty)
        return refuse(LowererExitRefusal::StateDirty, strideDirty ? "stride_dirty" : "shader_dirty");
    if (watch.allocByte & 0x20)
        return refuse(LowererExitRefusal::AllocMarker, "alloc_marker");
    // g63:13643-13658: the record needs 16 bytes of the list, else 0x8269C170 grows it.
    if (tiled && std::uint64_t{ watch.tileCursor } + kLowererTileRecordBytes > watch.tileLimit)
        return refuse(LowererExitRefusal::TileGrowth, "tile_growth");
    if (watch.poolObject && !watch.poolBase)
        return refuse(LowererExitRefusal::AllocPath, "alloc_path");

    // The flush groups run first; every ring check precedes the allocations, so
    // it sees the limits as they stand (g63:13414-13425).
    const std::uint64_t flush = LowererFlushWords(watch);
    if (flush == UINT64_MAX)
        return refuse(LowererExitRefusal::RingSpace, "flush_unbounded");
    const std::uint64_t end = std::uint64_t{ watch.cursor } + 4 * flush;
    if (end > UINT32_MAX - kPoolCursorMargin)
        return refuse(LowererExitRefusal::RingSpace, "ring_space");
    if (end > watch.limit38 || end >= watch.limit34)
        return refuse(LowererExitRefusal::RingSpace, "ring_space");

    const std::array<std::uint32_t, 2> bytes{ draw.vertexBytes, draw.indexBytes };
    std::array<std::uint32_t, 2>       buffers{};
    std::uint32_t                      used = watch.poolUsed, limit34 = watch.limit34, limit38 = watch.limit38, poolPos = watch.poolPos, wrapPos = watch.wrapPos;
    if (watch.poolObject)
    {
        // Pool takes 0x826A6848 (g100:13237-13311); 0x8269B818 adds bytes to
        // G+0x3A54 and may move limits to cursor (g93:12827-12865).
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            const std::uint32_t top    = poolPos ? poolPos : (watch.poolSize & ~3U) + watch.poolBase;
            const std::uint32_t buffer = (top - (bytes[i] & ~3U)) & ~(kSideBufferAlignment - 1);
            const std::uint32_t delta  = (top - buffer) & ~3U;
            if (limit38 < delta || limit34 < delta)
                return refuse(LowererExitRefusal::RingSpace, "ring_limit");
            limit34 -= delta;
            limit38 -= delta;
            used += bytes[i];
            if (used > watch.poolThreshold && watch.bracket == 0)
                return refuse(LowererExitRefusal::RingSpace, "pool_threshold");
            // The take fails below the cursor plus 164, and the cursor is no higher than the end of the flush words.
            if (std::uint64_t{ buffer } < end + kPoolCursorMargin)
                return refuse(LowererExitRefusal::AllocCapacity, "alloc_capacity");
            poolPos    = buffer;
            buffers[i] = buffer;
        }
    }
    else
    {
        // 0x8269B618 moves neither limit; the takes see each other's stores.
        ResolveWatch view;
        view.limit34     = watch.limit34;
        view.sideEnd     = watch.sideEnd;
        view.wrapLimit   = watch.wrapLimit;
        view.segment     = watch.segment;
        view.bracket     = watch.bracket;
        view.bracketNext = watch.bracketNext;
        view.progress    = watch.progress;
        view.poolPos     = watch.poolPos;
        view.wrapPos     = watch.wrapPos;
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            const auto take = ModelNullPoolAllocator(view, bytes[i], kSideBufferAlignment);
            if (!take.ok)
                return refuse(LowererExitRefusal::AllocCapacity, "alloc_fail");
            if (take.waits)
                return refuse(LowererExitRefusal::AllocWait, "alloc_wait");
            // Checked per take, as 0x8269B818 does (g93:12827-12865): a 32-bit
            // sum of both takes could wrap back under the threshold.
            used += bytes[i];
            if (used > watch.poolThreshold && watch.bracket == 0)
                return refuse(LowererExitRefusal::RingSpace, "pool_threshold");
            buffers[i]                                = take.end - bytes[i];
            (take.wrap ? view.wrapPos : view.poolPos) = take.end;
        }
        poolPos = view.poolPos;
        wrapPos = view.wrapPos;
    }

    LowererExitState out;
    out.dirty        = watch.dirty;
    out.dirty[3]     = watch.dirty[3] | 1;
    out.limit34      = limit34;
    out.limit38      = limit38;
    out.poolPos      = poolPos;
    out.poolUsed     = used;
    out.wrapPos      = wrapPos;
    out.pendingEnd   = watch.cursor;
    out.vertexDest   = buffers[0];
    out.indexDest    = buffers[1];
    out.vertexDwords = draw.vertexDwords;
    out.indexDwords  = draw.indexDwords;
    out.flushWords   = flush;
    out.pm4Words     = tiled ? kLowererPm4TiledWords : kLowererPm4Words;
    out.tiled        = tiled;
    return out;
}

std::vector<LowererWrite> LowererExitWrites(const LowererEntry& entry, const LowererWatch& watch, const LowererExitState& exit)
{
    std::vector<LowererWrite> writes;
    const auto                word = [&](std::uint32_t address, std::uint32_t before, std::uint32_t after)
    {
        if (before != after)
            writes.push_back({ address, after });
    };
    const auto g = entry.g;
    word(g + 0x1C, static_cast<std::uint32_t>(watch.dirty[3]), static_cast<std::uint32_t>(exit.dirty[3]));
    word(g + 0x34, watch.limit34, exit.limit34);
    word(g + 0x38, watch.limit38, exit.limit38);
    word(g + 0x3A44, watch.poolPos, exit.poolPos);
    word(g + 0x3A4C, watch.wrapPos, exit.wrapPos);
    word(g + 0x3A54, watch.poolUsed, exit.poolUsed);
    word(g + 0x3484, watch.pendingEnd, exit.pendingEnd);
    word(g + 0x3488, watch.vertexDest, exit.vertexDest);
    word(g + 0x348C, watch.indexDest, exit.indexDest);
    word(g + 0x3490, watch.vertexDwords, exit.vertexDwords);
    word(g + 0x3494, watch.indexDwords, exit.indexDwords);
    // The two out words are stored whatever they held.
    writes.push_back({ watch.vertexOutAddress, exit.vertexDest });
    writes.push_back({ entry.indexOut, exit.indexDest });
    return writes;
}

#if REREVVED_DEVELOPER
std::uint64_t LowererObservedFlushWords(const LowererWatch& entry, const LowererWatch& exit) noexcept
{
    const std::uint32_t delta = exit.cursor - entry.cursor;
    return exit.cursor < entry.cursor || delta % 4 ? 0 : delta / 4;
}

unsigned LowererExitMismatches(const LowererExitState& predicted, const LowererWatch& entry, const LowererWatch& exit, std::uint32_t result) noexcept
{
    unsigned   mask = 0;
    const auto flag = [&](std::size_t word, bool differs)
    {
        mask |= differs ? 1U << word : 0U;
    };
    flag(0, result != 0);
    // Every flush group stores zero; the body then ORs 1 into G+0x18.
    flag(1, exit.dirty[0] != 0);
    flag(2, exit.dirty[1] != 0);
    flag(3, exit.dirty[2] != 0);
    flag(4, exit.dirty[3] != 1);
    flag(5, exit.dirty[4] != 0);
    flag(6, predicted.limit34 != exit.limit34);
    flag(7, predicted.limit38 != exit.limit38);
    flag(8, predicted.poolPos != exit.poolPos);
    flag(9, predicted.poolUsed != exit.poolUsed);
    flag(10, predicted.wrapPos != exit.wrapPos);
    flag(11, predicted.vertexDest != exit.vertexDest);
    flag(12, predicted.indexDest != exit.indexDest);
    flag(13, predicted.vertexDwords != exit.vertexDwords);
    flag(14, predicted.indexDwords != exit.indexDwords);
    flag(15, predicted.vertexDest != exit.vertexOut);
    flag(16, predicted.indexDest != exit.indexOut);
    flag(17, entry.allocByte != exit.allocByte);
    // G+0x3484 is the cursor the PM4 words end at; the flush groups moved G+0x30 before them.
    flag(18, exit.pendingEnd - exit.cursor != 4 * predicted.pm4Words);
    const std::uint32_t moved = exit.cursor - entry.cursor;
    flag(19, exit.cursor < entry.cursor || moved % 4 || moved / 4 > predicted.flushWords);
    flag(20, exit.tileChunk != entry.tileChunk || exit.tileCursor != entry.tileCursor + (predicted.tiled ? kLowererTileRecordBytes : 0));
    flag(21, entry.fence != exit.fence);
    return mask;
}

namespace
{

struct ExitTallies
{
    std::atomic<std::uint64_t>                                       ok{ 0 }, tiled{ 0 }, tiledOk{ 0 }, unreadable{ 0 };
    std::array<std::atomic<std::uint64_t>, kLowererExitWordCount>    mismatch{};
    std::array<std::atomic<std::uint64_t>, kLowererExitRefusalCount> unmodeled{};
    std::atomic<std::uint64_t>                                       flushMax{ 0 }, flushSum{ 0 }, flushCalls{ 0 };
};

ExitTallies exitTallies;

} // namespace

bool LowererExitCheckRequested() noexcept
{
    return REXCVAR_GET(native_lowerer_exit_check);
}

void NoteLowererExit(unsigned mismatch, bool tiled, std::uint64_t flushWords) noexcept
{
    if (!mismatch)
        exitTallies.ok.fetch_add(1, std::memory_order_relaxed);
    if (tiled)
    {
        exitTallies.tiled.fetch_add(1, std::memory_order_relaxed);
        if (!mismatch)
            exitTallies.tiledOk.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < kLowererExitWordCount; ++i)
        if (mismatch & (1U << i))
            exitTallies.mismatch[i].fetch_add(1, std::memory_order_relaxed);
    exitTallies.flushCalls.fetch_add(1, std::memory_order_relaxed);
    exitTallies.flushSum.fetch_add(flushWords, std::memory_order_relaxed);
    auto seen = exitTallies.flushMax.load(std::memory_order_relaxed);
    while (flushWords > seen && !exitTallies.flushMax.compare_exchange_weak(seen, flushWords, std::memory_order_relaxed))
    {
    }
}

void NoteLowererExitUnmodeled(LowererExitRefusal refusal) noexcept
{
    exitTallies.unmodeled[static_cast<std::size_t>(refusal)].fetch_add(1, std::memory_order_relaxed);
}

void NoteLowererExitUnreadable() noexcept
{
    exitTallies.unreadable.fetch_add(1, std::memory_order_relaxed);
}

LowererExitCounts TakeLowererExitCounts() noexcept
{
    LowererExitCounts counts;
    counts.ok              = exitTallies.ok.exchange(0, std::memory_order_relaxed);
    counts.tiled           = exitTallies.tiled.exchange(0, std::memory_order_relaxed);
    counts.tiledOk         = exitTallies.tiledOk.exchange(0, std::memory_order_relaxed);
    counts.unreadable      = exitTallies.unreadable.exchange(0, std::memory_order_relaxed);
    counts.flushWordsMax   = exitTallies.flushMax.exchange(0, std::memory_order_relaxed);
    counts.flushWordsSum   = exitTallies.flushSum.exchange(0, std::memory_order_relaxed);
    counts.flushWordsCalls = exitTallies.flushCalls.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kLowererExitWordCount; ++i)
        counts.mismatch[i] = exitTallies.mismatch[i].exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kLowererExitRefusalCount; ++i)
        counts.unmodeled[i] = exitTallies.unmodeled[i].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatLowererExitCounts(const LowererExitCounts& counts)
{
    std::string fields = std::format(" lowerer_exit_ok={} lowerer_exit_tiled={} lowerer_exit_tiled_ok={} lowerer_exit_unreadable={} lowerer_exit_flush_words_max={} lowerer_exit_flush_words_sum={} lowerer_exit_flush_calls={}",
                                     counts.ok,
                                     counts.tiled,
                                     counts.tiledOk,
                                     counts.unreadable,
                                     counts.flushWordsMax,
                                     counts.flushWordsSum,
                                     counts.flushWordsCalls);
    for (std::size_t i = 0; i < kLowererExitWordCount; ++i)
        fields += std::format(" lowerer_exit_mismatch_{}={}", kLowererExitWordNames[i], counts.mismatch[i]);
    for (std::size_t i = 1; i < kLowererExitRefusalCount; ++i)
        if (counts.unmodeled[i])
            fields += std::format(" lowerer_exit_unmodeled_{}={}", kLowererExitRefusalNames[i], counts.unmodeled[i]);
    return fields;
}
#endif

} // namespace rerevved::gpu::guest_d3d
