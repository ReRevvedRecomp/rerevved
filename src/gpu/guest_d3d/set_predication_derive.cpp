#include "set_predication_derive.h"

namespace rerevved::gpu::guest_d3d
{

bool ReadSetPredicationWatch(const GuestWordReader& read, std::uint32_t g, SetPredicationWatch& out)
{
    if (g > 0xFFFFFFFFU - kSetPredicationDeviceSpan)
        return false;
    SetPredicationWatch watch;
    std::uint32_t       tilingWord = 0, flagWord = 0;
    if (!read(g + kSetPredicationCursorOffset, watch.cursor) || !read(g + kSetPredicationLimitOffset, watch.limit) ||
        !read(g + kSetPredicationTilingOffset, tilingWord) || !read(g + (kSetPredicationFlagOffset & ~3U), flagWord) ||
        !read(g + kSetPredicationPredicateOffset, watch.predicate) || !read(g + kSetPredicationMaskOffset, watch.mask) ||
        !read(g + kSetPredicationWord31A8Offset, watch.word31a8) || !read(g + kSetPredicationWord2AB4Offset, watch.word2ab4) ||
        !read(g + kSetPredicationWord2AB8Offset, watch.word2ab8))
        return false;
    for (std::size_t i = 0; i < kSetPredicationSlots; ++i)
        if (!read(g + kSetPredicationBoundOffset + 4 * static_cast<std::uint32_t>(i), watch.bound[i]) ||
            !read(g + kSetPredicationSnapshotOffset + 4 * static_cast<std::uint32_t>(i), watch.snapshot[i]))
            return false;
    // Big-endian words: the byte at the lowest address is the most significant,
    // so the byte G+0x2ABC is the top byte of its word and the byte G+0x2F9B
    // (the last byte of the word at G+0x2F98) the bottom byte of its word.
    watch.tiling = static_cast<std::uint8_t>(tilingWord >> 24);
    watch.flag   = static_cast<std::uint8_t>(flagWord);
    out          = watch;
    return true;
}

bool SetPredicationStackOverlaps(const GuestWordReader& read, const StackFrame& frame, std::uint32_t g, StackReach& reach)
{
    return StackReachRefuses(read, frame, kSetPredicationStackFootprint, g, reach);
}

namespace
{

// True when every bound surface is empty or the one BeginTiling saved
// (g171:12616-12680): a slot passes when the two words are equal, or when the
// bound word is zero.
bool boundSurfacesMatchSnapshot(const SetPredicationWatch& entry) noexcept
{
    for (std::size_t i = 0; i < kSetPredicationSlots; ++i)
        if (entry.snapshot[i] != entry.bound[i] && entry.bound[i] != 0)
            return false;
    return true;
}

} // namespace

SetPredicationExit DeriveSetPredicationExit(std::uint32_t r4, const SetPredicationWatch& entry) noexcept
{
    SetPredicationExit exit;
    exit.flush    = entry.cursor > entry.limit;
    exit.word2ab4 = r4;
    if (r4 != 0)
    {
        exit.mask      = r4;
        exit.predicate = 1;
        exit.tiling    = static_cast<std::uint8_t>(entry.tiling & 0xFE);
        return exit;
    }
    exit.mask      = 0xFFFFFFFF;
    exit.predicate = 0;
    // The bit the guest recomputes (g171:12581-12704): clear for the 8 and 4
    // states and a nonzero G+0x2F9B; set for 0x10; for 0x20 only while the
    // bound surfaces still match the snapshot.
    bool bit = false;
    if (!(entry.tiling & 0x08) && !(entry.tiling & 0x04) && entry.flag == 0)
        bit = (entry.tiling & 0x10) ? true : ((entry.tiling & 0x20) && boundSurfacesMatchSnapshot(entry));
    exit.tiling = static_cast<std::uint8_t>((entry.tiling & 0xFE) | (bit ? 1 : 0));
    return exit;
}

SetPredicationWrites SetPredicationExitWrites(std::uint32_t g, const SetPredicationWatch& entry, const SetPredicationExit& exit, bool preSwap) noexcept
{
    SetPredicationWrites writes;
    const auto           word = [&](std::uint32_t offset, std::uint32_t before, std::uint32_t after)
    {
        if (before != after)
            writes.list[writes.count++] = { g + offset, after, false };
    };
    word(kSetPredicationPredicateOffset, entry.predicate, exit.predicate);
    word(kSetPredicationMaskOffset, entry.mask, exit.mask);
    if (entry.tiling != exit.tiling)
        writes.list[writes.count++] = { g + kSetPredicationTilingOffset, exit.tiling, true };
    word(kSetPredicationWord31A8Offset, entry.word31a8, exit.word31a8);
    word(kSetPredicationWord2AB4Offset, entry.word2ab4, exit.word2ab4);
    word(kSetPredicationWord2AB8Offset, entry.word2ab8, exit.word2ab8);
    if (preSwap)
    {
        writes.list[writes.count++] = { entry.cursor + 4, kSetPredicationRingHeader, false };
        writes.list[writes.count++] = { entry.cursor + 8, exit.mask, false };
        writes.list[writes.count++] = { g + kSetPredicationCursorOffset, entry.cursor + 8, false };
    }
    return writes;
}

#if REREVVED_DEVELOPER
unsigned SetPredicationExitMismatches(const SetPredicationExit& predicted, const SetPredicationWatch& entry, const SetPredicationObserved& observed) noexcept
{
    unsigned   mismatch = 0;
    const auto differs  = [&](std::size_t index, bool value)
    {
        if (value)
            mismatch |= 1U << index;
    };
    const auto& after = observed.watch;
    differs(0, after.predicate != predicted.predicate);
    differs(1, after.mask != predicted.mask);
    differs(2, after.word31a8 != predicted.word31a8);
    differs(3, after.word2ab4 != predicted.word2ab4);
    differs(4, after.word2ab8 != predicted.word2ab8);
    differs(5, after.tiling != predicted.tiling);
    if (!predicted.flush)
    {
        const std::uint32_t cursor = entry.cursor + 8;
        differs(6, after.cursor != cursor);
        differs(7, observed.ringHeader != kSetPredicationRingHeader);
        differs(8, observed.ringMask != predicted.mask);
        differs(9, observed.result != cursor);
    }
    return mismatch;
}
#endif

} // namespace rerevved::gpu::guest_d3d
