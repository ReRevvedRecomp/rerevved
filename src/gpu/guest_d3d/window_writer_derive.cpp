#include "window_writer_derive.h"

#include <rex/cvar.h>

#include <algorithm>
#include <atomic>
#include <format>

#if REREVVED_DEVELOPER
REXCVAR_DEFINE_BOOL(native_window_writer_exit_check,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native: compare the exit the window packet writer consume (native_consume_window_writer) would leave "
                    "with the guest words after every 0x8268D3A0 call whose Original runs. Observes only; window_writer_exit_* fields on the "
                    "native_frame_timing line")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

namespace rerevved::gpu::guest_d3d
{

namespace
{

// The device offsets the body reads (g153:12656-13083).
constexpr std::uint32_t kCursor = 0x30, kLimit34 = 0x34, kLimit38 = 0x38, kModeWord = 0x2ABC;
constexpr std::uint32_t kBound = 0x3098, kApplied = 0x31B8, kPredicateMask = 0x31A4, kPredicateWord = 0x31A8, kTileCount = 0x31CC, kTileRects = 0x31D0, kTileOffsets = 0x32C0;

// The constant ring words (g153:12762-13083).
constexpr std::uint32_t kPlainHeader = 0x00022080, kTiledHeader = 0xC0006100, kTileOpen = 0xC0006000, kTileMask = 0xC0032D01, kTileControl = 0x00040080;
constexpr std::uint32_t kExtraMask = 0xC0022D01, kExtraControl = 0x00040081;

// slw: a shift amount with bit 0x20 set gives zero (g153:12912).
constexpr std::uint32_t Slw(std::uint32_t value, std::uint32_t amount) noexcept
{
    return (amount & 0x20) ? 0 : value << (amount & 0x1F);
}

// rlwinm of the window words: the high field is shifted up 16 and masked to 14 bits, the low field masked to 14 bits.
constexpr std::uint32_t Pack(std::uint32_t high, std::uint32_t low) noexcept
{
    return ((high << 16) & 0x3FFF0000U) | (low & 0x3FFFU);
}

} // namespace

bool WindowWriterStackOverlaps(const GuestWordReader& read, const StackFrame& frame, std::uint32_t g, StackReach& reach)
{
    return StackReachRefuses(read, frame, kWindowWriterStackFootprint, g, reach);
}

WindowWriterForm ClassifyWindowWriterForm(const WindowWriterWatch& watch) noexcept
{
    const auto tiling = watch.TilingByte();
    if (tiling & kWindowWriterTilingForceBit)
        return WindowWriterForm::Tiled;
    if (!(tiling & kWindowWriterTilingBoundBit))
        return WindowWriterForm::Plain;
    // A pair passes when the applied surface is the bound one or none is bound (g153:12699-12751).
    for (std::size_t i = 0; i < watch.bound.size(); ++i)
        if (watch.applied[i] != watch.bound[i] && watch.bound[i] != 0)
            return WindowWriterForm::Plain;
    return WindowWriterForm::Tiled;
}

bool ReadWindowWriterWatch(const GuestWordReader& read, std::uint32_t g, bool content, WindowWriterWatch& out)
{
    out = WindowWriterWatch{};
    // Every device offset the watch reads is below G + kWindowWriterDeviceSpan.
    if (g > 0xFFFFFFFFU - kWindowWriterDeviceSpan)
        return false;
    if (!read(g + kCursor, out.cursor) || !read(g + kLimit38, out.limit38) || !read(g + kModeWord, out.mode))
        return false;
    if (content && !read(g + kLimit34, out.limit34))
        return false;
    if ((out.TilingByte() & (kWindowWriterTilingForceBit | kWindowWriterTilingBoundBit)) == kWindowWriterTilingBoundBit)
        for (std::uint32_t i = 0; i < out.bound.size(); ++i)
            if (!read(g + kBound + 4 * i, out.bound[i]) || !read(g + kApplied + 4 * i, out.applied[i]))
                return false;
    if (ClassifyWindowWriterForm(out) != WindowWriterForm::Tiled)
        return true;
    if (!read(g + kTileCount, out.tileCount))
        return false;
    if (!content)
        return true;
    if (!read(g + kPredicateMask, out.predicateMask) || !read(g + kPredicateWord, out.predicateWord))
        return false;
    if (out.tileCount > kWindowWriterMaxTiles)
        return true;
    for (std::uint32_t i = 0; i < out.tileCount; ++i)
    {
        for (std::uint32_t k = 0; k < 4; ++k)
            if (!read(g + kTileRects + 16 * i + 4 * k, out.tiles[i].rect[k]))
                return false;
        for (std::uint32_t k = 0; k < 2; ++k)
            if (!read(g + kTileOffsets + 8 * i + 4 * k, out.tiles[i].offset[k]))
                return false;
    }
    return true;
}

std::optional<WindowWriterExitState> DeriveWindowWriterExitState(const WindowWriterWatch& watch, WindowWriterRefusal* refusal, std::string* why)
{
    const auto refuse = [&](WindowWriterRefusal reason, const char* text) -> std::optional<WindowWriterExitState>
    {
        if (refusal)
            *refusal = reason;
        if (why)
            *why = text;
        return std::nullopt;
    };
    if (refusal)
        *refusal = WindowWriterRefusal::None;
    // g153:12656-12668: the cursor past the soft limit calls the flush.
    if (watch.cursor > watch.limit38)
        return refuse(WindowWriterRefusal::RingSpace, "entry_flush");

    WindowWriterExitState out;
    out.form = ClassifyWindowWriterForm(watch);
    if (out.form == WindowWriterForm::Plain)
    {
        out.words  = kWindowWriterPlainWords;
        out.result = watch.cursor + 12; // three stwu on r3 (g153:12778-12794)
    }
    else
    {
        if (watch.tileCount > kWindowWriterMaxTiles)
            return refuse(WindowWriterRefusal::TileCount, "tile_count");
        out.tiles = watch.tileCount;
        out.extra = (watch.FlagByte() & kWindowWriterFlagExtraBit) && (watch.TilingByte() & kWindowWriterTilingExtraBit);
        out.words = kWindowWriterTiledHeaderWords + kWindowWriterTileWords * out.tiles + (out.extra ? kWindowWriterExtraWords : 0) + kWindowWriterTailWords;
        if (out.tiles)
        {
            // g153:12966-12979: each tile's cursor is compared with the limit; the last is the highest.
            const std::uint64_t last = std::uint64_t{ watch.cursor } + 4ULL * (kWindowWriterTiledHeaderWords + kWindowWriterTileWords * out.tiles);
            if (last > watch.limit38)
                return refuse(WindowWriterRefusal::RingSpace, "tile_flush");
            // The last tile's li r3,3 / slw r3,r3,r29 with r29 = 2i is the result (g153:12904-12912).
            out.result = Slw(3, (2 * (out.tiles - 1)) & 0xFF);
        }
        else
        {
            out.result = watch.cursor + 8; // two stwu on r3 (g153:12815-12821)
        }
    }
    const std::uint64_t end = std::uint64_t{ watch.cursor } + 4ULL * out.words;
    if (end > 0xFFFFFFFFULL)
        return refuse(WindowWriterRefusal::RingSpace, "address_wrap");
    out.cursor = static_cast<std::uint32_t>(end);
    return out;
}

std::vector<std::uint32_t> WindowWriterRingWords(const WindowWriterEntry& entry, const WindowWriterWatch& watch, const WindowWriterExitState& exit)
{
    std::vector<std::uint32_t> words;
    words.reserve(exit.words);
    if (exit.form == WindowWriterForm::Plain)
    {
        words = { kPlainHeader, 0, Pack(entry.y0, entry.x0), Pack(entry.y1, entry.x1) };
        return words;
    }
    words.push_back(kTiledHeader);
    words.push_back(0);
    for (std::uint32_t i = 0; i < exit.tiles; ++i)
    {
        const auto& tile = watch.tiles[i];
        const auto  s32  = [](std::uint32_t value)
        {
            return static_cast<std::int32_t>(value);
        };
        // The window clipped to the tile; an empty one is all zero (g153:12839-12900).
        std::int32_t left   = std::max(s32(entry.x0), s32(tile.rect[0]));
        std::int32_t top    = std::max(s32(entry.y0), s32(tile.rect[1]));
        std::int32_t right  = std::min(s32(entry.x1), s32(tile.rect[2]));
        std::int32_t bottom = std::min(s32(entry.y1), s32(tile.rect[3]));
        if (left >= right || top >= bottom)
            left = top = right = bottom = 0;
        words.push_back(kTileOpen);
        words.push_back(Slw(3, (2 * i) & 0xFF));
        words.push_back(kTileMask);
        words.push_back(kTileControl);
        // The negated offsets: x in 15 bits, y in 15 bits above it (g153:12922-12938).
        words.push_back((((0U - tile.offset[1]) << 16) & 0x7FFF0000U) | ((0U - tile.offset[0]) & 0x7FFFU));
        words.push_back(Pack(static_cast<std::uint32_t>(top), static_cast<std::uint32_t>(left)));
        words.push_back(Pack(static_cast<std::uint32_t>(bottom), static_cast<std::uint32_t>(right)));
    }
    if (exit.extra)
    {
        words.push_back(kTileOpen);
        words.push_back(1);
        words.push_back(kExtraMask);
        words.push_back(kExtraControl);
        words.push_back(Pack(entry.y0, entry.x0));
        words.push_back(Pack(entry.y1, entry.x1));
    }
    words.push_back(kTileOpen);
    words.push_back(watch.predicateMask);
    words.push_back(kTiledHeader);
    words.push_back(watch.predicateWord);
    return words;
}
#if REREVVED_DEVELOPER

namespace
{

// The watched words other than the cursor and limits.
bool SameInputs(const WindowWriterWatch& a, const WindowWriterWatch& b) noexcept
{
    if (a.mode != b.mode || a.bound != b.bound || a.applied != b.applied || a.tileCount != b.tileCount || a.predicateMask != b.predicateMask || a.predicateWord != b.predicateWord)
        return false;
    const auto count = std::min<std::size_t>(a.tileCount, a.tiles.size());
    return std::equal(a.tiles.begin(), a.tiles.begin() + count, b.tiles.begin());
}

} // namespace

unsigned WindowWriterExitMismatches(const WindowWriterExitState&      predicted,
                                    const WindowWriterWatch&          before,
                                    const WindowWriterWatch&          after,
                                    std::uint64_t                     result,
                                    const std::vector<std::uint32_t>& expected,
                                    const std::vector<std::uint32_t>& observed) noexcept
{
    unsigned   mask = 0;
    const auto flag = [&](std::size_t word, bool differs)
    {
        mask |= differs ? 1U << word : 0U;
    };
    flag(0, result != predicted.result);
    flag(1, after.cursor != predicted.cursor);
    flag(2, before.limit34 != after.limit34);
    flag(3, before.limit38 != after.limit38);
    flag(4, !SameInputs(before, after));
    flag(5, expected != observed);
    return mask;
}

std::uint64_t WindowWriterObservedWords(const WindowWriterWatch& before, const WindowWriterWatch& after) noexcept
{
    const std::uint32_t delta = after.cursor - before.cursor;
    return after.cursor < before.cursor || delta % 4 ? 0 : delta / 4;
}

namespace
{

struct ExitTallies
{
    std::atomic<std::uint64_t>                                         ok{ 0 }, tiled{ 0 }, tiledOk{ 0 }, unreadable{ 0 };
    std::array<std::atomic<std::uint64_t>, kWindowWriterExitWordCount> mismatch{};
    std::array<std::atomic<std::uint64_t>, kWindowWriterRefusalCount>  unmodeled{};
    std::atomic<std::uint64_t>                                         ringMax{ 0 }, ringSum{ 0 }, calls{ 0 };
};

ExitTallies exitTallies;

} // namespace

bool WindowWriterExitCheckRequested() noexcept
{
    return REXCVAR_GET(native_window_writer_exit_check);
}

void NoteWindowWriterExit(unsigned mismatch, bool tiled, std::uint64_t ringWords) noexcept
{
    if (!mismatch)
        exitTallies.ok.fetch_add(1, std::memory_order_relaxed);
    if (tiled)
    {
        exitTallies.tiled.fetch_add(1, std::memory_order_relaxed);
        if (!mismatch)
            exitTallies.tiledOk.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < kWindowWriterExitWordCount; ++i)
        if (mismatch & (1U << i))
            exitTallies.mismatch[i].fetch_add(1, std::memory_order_relaxed);
    exitTallies.calls.fetch_add(1, std::memory_order_relaxed);
    exitTallies.ringSum.fetch_add(ringWords, std::memory_order_relaxed);
    auto seen = exitTallies.ringMax.load(std::memory_order_relaxed);
    while (ringWords > seen && !exitTallies.ringMax.compare_exchange_weak(seen, ringWords, std::memory_order_relaxed))
    {
    }
}

void NoteWindowWriterExitUnmodeled(WindowWriterRefusal refusal) noexcept
{
    exitTallies.unmodeled[static_cast<std::size_t>(refusal)].fetch_add(1, std::memory_order_relaxed);
}

void NoteWindowWriterExitUnreadable() noexcept
{
    exitTallies.unreadable.fetch_add(1, std::memory_order_relaxed);
}

WindowWriterExitCounts TakeWindowWriterExitCounts() noexcept
{
    WindowWriterExitCounts counts;
    counts.ok           = exitTallies.ok.exchange(0, std::memory_order_relaxed);
    counts.tiled        = exitTallies.tiled.exchange(0, std::memory_order_relaxed);
    counts.tiledOk      = exitTallies.tiledOk.exchange(0, std::memory_order_relaxed);
    counts.unreadable   = exitTallies.unreadable.exchange(0, std::memory_order_relaxed);
    counts.ringWordsMax = exitTallies.ringMax.exchange(0, std::memory_order_relaxed);
    counts.ringWordsSum = exitTallies.ringSum.exchange(0, std::memory_order_relaxed);
    counts.calls        = exitTallies.calls.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kWindowWriterExitWordCount; ++i)
        counts.mismatch[i] = exitTallies.mismatch[i].exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kWindowWriterRefusalCount; ++i)
        counts.unmodeled[i] = exitTallies.unmodeled[i].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatWindowWriterExitCounts(const WindowWriterExitCounts& counts)
{
    std::string fields = std::format(
        " window_writer_exit_ok={} window_writer_exit_tiled={} window_writer_exit_tiled_ok={} window_writer_exit_unreadable={} window_writer_exit_ring_words_max={} "
        "window_writer_exit_ring_words_sum={} window_writer_exit_calls={}",
        counts.ok,
        counts.tiled,
        counts.tiledOk,
        counts.unreadable,
        counts.ringWordsMax,
        counts.ringWordsSum,
        counts.calls);
    for (std::size_t i = 0; i < kWindowWriterExitWordCount; ++i)
        fields += std::format(" window_writer_exit_mismatch_{}={}", kWindowWriterExitWordNames[i], counts.mismatch[i]);
    for (std::size_t i = 1; i < kWindowWriterRefusalCount; ++i)
        if (counts.unmodeled[i])
            fields += std::format(" window_writer_exit_unmodeled_{}={}", kWindowWriterRefusalNames[i], counts.unmodeled[i]);
    return fields;
}
#endif

} // namespace rerevved::gpu::guest_d3d
