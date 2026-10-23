// stack_guard: KPCR at r13 supplies [limit,r1+above); overlap or unknown bounds
// refuse.

#include "gpu/guest_d3d/stack_guard.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>

using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint32_t kLimit = 0x70000000, kBase = 0x70100000, kSp = 0x700F0000, kPcr = 0x40020000, kG = 0x40010000;

// Guest words by address; a missing address is unreadable.
struct Words
{
    std::map<std::uint32_t, std::uint32_t> map;

    bool operator()(std::uint32_t address, std::uint32_t& value) const
    {
        const auto at = map.find(address);
        if (at == map.end())
            return false;
        value = at->second;
        return true;
    }
};

Words withPcr(std::uint32_t limit = kLimit, std::uint32_t base = kBase)
{
    Words words;
    words.map[kPcr + 0x70] = base;
    words.map[kPcr + 0x74] = limit;
    return words;
}

void testConstants()
{
    UNSCOPED_INFO("the KPCR stack words");
    REQUIRE((kPcrStackBaseOffset == 0x70 && kPcrStackLimitOffset == 0x74 && kPcrStackBytes == 0x80));
    UNSCOPED_INFO("the allocator objects");
    REQUIRE((kStackPoolObjectWord == 0x34CC && kStackPoolObjectBytes == 160 && kStackProgressWord == 0x2A90 && kStackProgressOffset == 4));
}

void testBetween()
{
    UNSCOPED_INFO("a stack pointer inside the range is known");
    REQUIRE(StackReach::Between(kLimit, kBase, kSp, 0).Known());
    UNSCOPED_INFO("the limit and the base are inside the range");
    REQUIRE((StackReach::Between(kLimit, kBase, kLimit, 0).Known() && StackReach::Between(kLimit, kBase, kBase, 0).Known()));
    UNSCOPED_INFO("a stack pointer below the limit is unknown");
    REQUIRE(!StackReach::Between(kLimit, kBase, kLimit - 1, 0).Known());
    UNSCOPED_INFO("a stack pointer above the base is unknown");
    REQUIRE(!StackReach::Between(kLimit, kBase, kBase + 1, 0).Known());
    UNSCOPED_INFO("a zero limit is unknown");
    REQUIRE(!StackReach::Between(0, kBase, kSp, 0).Known());
    UNSCOPED_INFO("an empty or inverted range is unknown");
    REQUIRE((!StackReach::Between(kBase, kBase, kBase, 0).Known() && !StackReach::Between(kBase, kLimit, kSp, 0).Known()));
    const auto reach = StackReach::Between(kLimit, kBase, kSp, 0x58);
    UNSCOPED_INFO("the reach is [limit, sp + above)");
    REQUIRE((reach.Low() == kLimit && reach.High() == std::uint64_t{ kSp } + 0x58));
    // The sum is taken in 64 bits.
    UNSCOPED_INFO("sp + above is not truncated");
    REQUIRE(StackReach::Between(0xFFFF0000U, 0xFFFFFFF0U, 0xFFFFFFF0U, 0x100).High() == 0x1000000F0ULL);
}

void testEdges()
{
    const auto reach = StackReach::Between(kLimit, kBase, kSp, 0x58);
    // The lower end: an object ending exactly at the limit is clear (below the limit lie guard pages), one byte more overlaps.
    UNSCOPED_INFO("an object ending exactly at the limit is clear");
    REQUIRE(!reach.Overlaps(kLimit - 16, 16));
    UNSCOPED_INFO("an object one byte over the limit overlaps");
    REQUIRE(reach.Overlaps(kLimit - 15, 16));
    UNSCOPED_INFO("the limit byte is in the reach");
    REQUIRE(reach.Overlaps(kLimit, 1));
    // The upper end: an object starting exactly at sp + above is clear, one byte lower overlaps.
    UNSCOPED_INFO("an object starting exactly at sp + above is clear");
    REQUIRE(!reach.Overlaps(kSp + 0x58, 16));
    UNSCOPED_INFO("an object starting one byte below sp + above overlaps");
    REQUIRE(reach.Overlaps(kSp + 0x58 - 1, 16));
    // The part above r1 is in the reach: the parameter words.
    UNSCOPED_INFO("the caller's words below sp + above overlap");
    REQUIRE((reach.Overlaps(kSp + 0x54, 4) && reach.Overlaps(kSp, 4)));
    UNSCOPED_INFO("the frames below r1 and an object that covers them overlap");
    REQUIRE((reach.Overlaps(kSp - 8, 8) && reach.Overlaps(kLimit + 0x1000, 0x20000)));
    // With nothing above r1 the byte at r1 is clear.
    const auto bare = StackReach::Between(kLimit, kBase, kSp, 0);
    UNSCOPED_INFO("a reach with nothing above r1 ends at r1");
    REQUIRE((bare.Overlaps(kSp - 1, 1) && !bare.Overlaps(kSp, 4)));
    // A far object is clear.
    UNSCOPED_INFO("an object away from the stack is clear");
    REQUIRE((!reach.Overlaps(kG, 0x3A5C) && !reach.Overlaps(kBase, 0x100)));
}

void testEmpty()
{
    const auto reach = StackReach::Between(kLimit, kBase, kSp, 0x58);
    UNSCOPED_INFO("an empty object inside the reach overlaps nothing");
    REQUIRE((!reach.Overlaps(kSp - 0x100, 0) && !reach.Overlaps(kLimit, 0) && !reach.Overlaps(0, 0)));
    UNSCOPED_INFO("an empty StackObject overlaps nothing");
    REQUIRE(!reach.Overlaps(StackObject{ kSp, 0 }));
    // A reach whose r1 is the limit and has nothing above is empty and overlaps nothing, whatever the object.
    const auto none = StackReach::Between(kLimit, kBase, kLimit, 0);
    UNSCOPED_INFO("r1 at the limit with nothing above is an empty reach");
    REQUIRE((none.Known() && none.Low() == none.High()));
    UNSCOPED_INFO("an empty reach overlaps nothing");
    REQUIRE((!none.Overlaps(kLimit - 4, 8) && !none.Overlaps(kLimit, 4) && !none.Overlaps(0, 0x100000000ULL - 1)));
    // An unknown reach refuses every nonempty object, and an empty one still names nothing.
    const StackReach unknown;
    UNSCOPED_INFO("an unknown reach overlaps every nonempty object");
    REQUIRE((!unknown.Known() && unknown.Overlaps(kG, 4) && !unknown.Overlaps(kG, 0)));
}

void testWrap()
{
    const auto reach = StackReach::Between(kLimit, kBase, kSp, 0x58);
    UNSCOPED_INFO("an object that runs past 2^32 cannot be placed and is refused");
    REQUIRE(reach.Overlaps(0xFFFFFFF0U, 0x20));
    UNSCOPED_INFO("an object ending exactly at 2^32 is placed");
    REQUIRE((reach.Overlaps(0xFFFFFFFFU, 2) && !reach.Overlaps(0xFFFFFFF0U, 0x10) && !reach.Overlaps(0x100, 0x100)));
    UNSCOPED_INFO("a device whose block runs past 2^32 is refused");
    REQUIRE(reach.Overlaps(0xFFFFF000ULL, 0x2000));
    UNSCOPED_INFO("an address past 2^32 is refused");
    REQUIRE(reach.Overlaps(0x100000000ULL, 4));
    // A stack at the top of memory: the end of the reach is not truncated.
    const auto top = StackReach::Between(0xFFFF0000U, 0xFFFFFFF0U, 0xFFFFFFE0U, 0x40);
    UNSCOPED_INFO("a reach past 2^32 keeps its bytes");
    REQUIRE((top.Overlaps(0xFFFFFFF8U, 4) && !top.Overlaps(0xFFFEFFF0U, 0x10)));
    UNSCOPED_INFO("the last byte of memory is in a reach that ends past 2^32");
    REQUIRE(top.Overlaps(0xFFFFFFFFU, 1));
}

void testOf()
{
    auto       words = withPcr();
    const auto reach = StackReach::Of(words, kPcr, kSp, 0x58);
    UNSCOPED_INFO("the reach comes from the KPCR words");
    REQUIRE((reach.Known() && reach.Low() == kLimit && reach.High() == std::uint64_t{ kSp } + 0x58));
    // The base is at +0x70 and the limit at +0x74: swapped words are an inverted range.
    UNSCOPED_INFO("swapped base and limit are unknown");
    REQUIRE(!StackReach::Of(withPcr(kBase, kLimit), kPcr, kSp, 0).Known());
    UNSCOPED_INFO("r13 zero is unknown");
    REQUIRE(!StackReach::Of(words, 0, kSp, 0).Known());
    UNSCOPED_INFO("a KPCR whose words wrap is unknown");
    REQUIRE(!StackReach::Of(words, 0xFFFFFFF0U, kSp, 0).Known());
    Words noLimit = withPcr();
    noLimit.map.erase(kPcr + 0x74);
    UNSCOPED_INFO("an unreadable limit is unknown");
    REQUIRE(!StackReach::Of(noLimit, kPcr, kSp, 0).Known());
    Words noBase = withPcr();
    noBase.map.erase(kPcr + 0x70);
    UNSCOPED_INFO("an unreadable base is unknown");
    REQUIRE(!StackReach::Of(noBase, kPcr, kSp, 0).Known());
    UNSCOPED_INFO("a zero limit is unknown");
    REQUIRE(!StackReach::Of(withPcr(0, kBase), kPcr, kSp, 0).Known());
    UNSCOPED_INFO("an r1 outside the thread's range is unknown");
    REQUIRE((!StackReach::Of(words, kPcr, kLimit - 1, 0).Known() && !StackReach::Of(words, kPcr, kBase + 4, 0).Known()));
    UNSCOPED_INFO("an r1 at either end of the range is known");
    REQUIRE((StackReach::Of(words, kPcr, kLimit, 0).Known() && StackReach::Of(words, kPcr, kBase, 0).Known()));
    // The KPCR itself must stay out of the reach: [pcr, pcr + 0x80).
    const auto at = [](std::uint32_t pcr)
    {
        Words placed;
        placed.map[pcr + 0x70] = kBase;
        placed.map[pcr + 0x74] = kLimit;
        return placed;
    };
    UNSCOPED_INFO("a KPCR inside the stack reach is unknown");
    REQUIRE(!StackReach::Of(at(kSp - 0x40), kSp - 0x40, kSp, 0).Known());
    UNSCOPED_INFO("a KPCR above r1 is clear when nothing above r1 is reached");
    REQUIRE(StackReach::Of(at(kSp + 0x10), kSp + 0x10, kSp, 0).Known());
    UNSCOPED_INFO("a KPCR inside the reach above r1 is unknown");
    REQUIRE(!StackReach::Of(at(kSp + 0x10), kSp + 0x10, kSp, 0x58).Known());
    UNSCOPED_INFO("a KPCR starting exactly at the end of the reach is clear");
    REQUIRE(StackReach::Of(at(kSp + 0x58), kSp + 0x58, kSp, 0x58).Known());
    UNSCOPED_INFO("a KPCR starting one byte inside the reach is unknown");
    REQUIRE(!StackReach::Of(at(kSp + 0x57), kSp + 0x57, kSp, 0x58).Known());
    UNSCOPED_INFO("a KPCR ending exactly at the limit is clear");
    REQUIRE(StackReach::Of(at(kLimit - 0x80), kLimit - 0x80, kSp, 0).Known());
    UNSCOPED_INFO("a KPCR one byte into the stack is unknown");
    REQUIRE(!StackReach::Of(at(kLimit - 0x7F), kLimit - 0x7F, kSp, 0).Known());
}

// Entry frame must fit [limit,r1); otherwise bounds are unknown.
void testFrame()
{
    constexpr std::uint32_t kFrame = 112;
    const auto              edge   = [](std::uint32_t sp)
    {
        return StackReach::Between(kLimit, kBase, sp, 0, kFrame);
    };
    UNSCOPED_INFO("exactly the frame between the limit and r1 is known");
    REQUIRE(edge(kLimit + kFrame).Known());
    UNSCOPED_INFO("one byte short of the frame is unknown");
    REQUIRE(!edge(kLimit + kFrame - 1).Known());
    UNSCOPED_INFO("r1 at the limit is unknown with a frame (the saves would store below it)");
    REQUIRE(!edge(kLimit).Known());
    UNSCOPED_INFO("a word above the limit is unknown with a frame");
    REQUIRE(!edge(kLimit + 4).Known());
    UNSCOPED_INFO("the reach starts at the limit, not at the frame");
    REQUIRE((edge(kLimit + kFrame).Low() == kLimit && edge(kLimit + kFrame).High() == std::uint64_t{ kLimit } + kFrame));
    UNSCOPED_INFO("without a frame r1 at the limit is the empty reach");
    REQUIRE(StackReach::Between(kLimit, kBase, kLimit, 0).Known());
    // The same through the KPCR words and the footprint.
    auto                 words = withPcr();
    StackReach           reach;
    const StackFootprint footprint{ 0, 0x100, kFrame };
    UNSCOPED_INFO("Of takes the frame");
    REQUIRE((StackReach::Of(words, kPcr, kLimit + kFrame, 0, kFrame).Known() && !StackReach::Of(words, kPcr, kLimit + kFrame - 1, 0, kFrame).Known()));
    UNSCOPED_INFO("a footprint's frame that fits is admitted");
    REQUIRE((!StackReachRefuses(words, { kPcr, kLimit + kFrame }, footprint, kG, reach) && reach.Known()));
    UNSCOPED_INFO("a footprint's frame that does not fit is refused");
    REQUIRE((StackReachRefuses(words, { kPcr, kLimit + kFrame - 1 }, footprint, kG, reach) && !reach.Known()));
    UNSCOPED_INFO("r1 at the limit is refused far from every word");
    REQUIRE(StackReachRefuses(words, { kPcr, kLimit }, footprint, kG, reach));
}

void testRefuses()
{
    auto                     words = withPcr();
    StackReach               reach;
    constexpr StackFootprint footprint{ 0x58, 0x3A5C };
    // The device block is [G, G + deviceSpan): a block ending exactly at the limit is clear.
    UNSCOPED_INFO("a block ending exactly at the limit is clear");
    REQUIRE((!StackReachRefuses(words, { kPcr, kSp }, footprint, kLimit - 0x3A5C, reach) && reach.Known()));
    UNSCOPED_INFO("a block one byte over the limit is refused");
    REQUIRE(StackReachRefuses(words, { kPcr, kSp }, footprint, kLimit - 0x3A5B, reach));
    UNSCOPED_INFO("a block starting exactly at sp + above is clear");
    REQUIRE(!StackReachRefuses(words, { kPcr, kSp }, footprint, kSp + 0x58, reach));
    UNSCOPED_INFO("a block starting one byte below sp + above is refused");
    REQUIRE(StackReachRefuses(words, { kPcr, kSp }, footprint, kSp + 0x57, reach));
    UNSCOPED_INFO("a block away from the stack is clear");
    REQUIRE(!StackReachRefuses(words, { kPcr, kSp }, footprint, kG, reach));
    // Unknown bounds refuse, whatever the device.
    UNSCOPED_INFO("r13 zero refuses");
    REQUIRE((StackReachRefuses(words, { 0, kSp }, footprint, kG, reach) && !reach.Known()));
    UNSCOPED_INFO("an r1 outside the range refuses");
    REQUIRE(StackReachRefuses(words, { kPcr, kBase + 1 }, footprint, kG, reach));
    // The footprint decides the bytes of the block: a block that stops short of the stack is clear.
    UNSCOPED_INFO("the device span is the footprint's");
    REQUIRE((!StackReachRefuses(words, { kPcr, kSp }, StackFootprint{ 0, 0x100 }, kLimit - 0x100, reach) && StackReachRefuses(words, { kPcr, kSp }, StackFootprint{ 0, 0x101 }, kLimit - 0x100, reach)));
    // An empty device block overlaps nothing, but the bounds are still required.
    UNSCOPED_INFO("an empty block is clear, an unknown stack is not");
    REQUIRE((!StackReachRefuses(words, { kPcr, kSp }, StackFootprint{ 0, 0 }, kLimit, reach) && StackReachRefuses(words, { 0, kSp }, StackFootprint{ 0, 0 }, kLimit, reach)));
    // A device that runs past 2^32 is refused.
    UNSCOPED_INFO("a block past 2^32 is refused");
    REQUIRE(StackReachRefuses(words, { kPcr, kSp }, footprint, 0xFFFFF000U, reach));
}

void testPointee()
{
    auto       words     = withPcr();
    const auto reach     = StackReach::Between(kLimit, kBase, kSp, 0x58);
    words.map[kG + 0x10] = kSp - 0x40;
    UNSCOPED_INFO("a pointer into the frames overlaps");
    REQUIRE(StackReachesPointee(words, reach, kG + 0x10, 0, 4));
    UNSCOPED_INFO("the offset moves the object");
    REQUIRE(StackReachesPointee(words, reach, kG + 0x10, 0x30, 0x20));
    UNSCOPED_INFO("an object past sp + above is clear");
    REQUIRE(!StackReachesPointee(words, reach, kG + 0x10, 0x40 + 0x58, 4));
    words.map[kG + 0x14] = kLimit - 4;
    UNSCOPED_INFO("an object ending at the limit is clear, one byte more overlaps");
    REQUIRE((!StackReachesPointee(words, reach, kG + 0x14, 0, 4) && StackReachesPointee(words, reach, kG + 0x14, 0, 5)));
    words.map[kG + 0x18] = 0;
    UNSCOPED_INFO("a null pointer names no object");
    REQUIRE(!StackReachesPointee(words, reach, kG + 0x18, 0, 4));
    UNSCOPED_INFO("an unreadable pointer names no object here");
    REQUIRE(!StackReachesPointee(words, reach, kG + 0x1C, 0, 4));
    UNSCOPED_INFO("a pointer word that cannot be placed is not read");
    REQUIRE((!StackReachesPointee(words, reach, 0xFFFFFFFFULL, 0, 4) && !StackReachesPointee(words, reach, 0x100000000ULL, 0, 4)));
    words.map[kG + 0x20] = 0xFFFFFFF0U;
    UNSCOPED_INFO("an object that would run past 2^32 names no readable object");
    REQUIRE(!StackReachesPointee(words, reach, kG + 0x20, 0x10, 4));
    UNSCOPED_INFO("an unknown reach refuses it all the same");
    REQUIRE(StackReachesPointee(words, StackReach{}, kG + 0x20, 0x10, 4));
    UNSCOPED_INFO("an empty object overlaps nothing");
    REQUIRE(!StackReachesPointee(words, reach, kG + 0x10, 0, 0));
    UNSCOPED_INFO("an unknown reach refuses a named object");
    REQUIRE(StackReachesPointee(words, StackReach{}, kG + 0x10, 0, 4));
}

void testAllocator()
{
    auto       words       = withPcr();
    const auto reach       = StackReach::Between(kLimit, kBase, kSp, 0);
    words.map[kG + 0x34CC] = 0x50000000;
    words.map[kG + 0x2A90] = 0x50001000;
    UNSCOPED_INFO("an allocator away from the stack is clear");
    REQUIRE(!StackReachesAllocator(words, reach, kG));
    // The pool object is 160 bytes: its base and size words are +152 and +156.
    words.map[kG + 0x34CC] = kSp - 160;
    UNSCOPED_INFO("a pool object ending at r1 overlaps");
    REQUIRE(StackReachesAllocator(words, reach, kG));
    words.map[kG + 0x34CC] = kLimit - 160;
    UNSCOPED_INFO("a pool object ending at the limit is clear");
    REQUIRE(!StackReachesAllocator(words, reach, kG));
    words.map[kG + 0x34CC] = kLimit - 159;
    UNSCOPED_INFO("a pool object one byte over the limit overlaps");
    REQUIRE(StackReachesAllocator(words, reach, kG));
    words.map[kG + 0x34CC] = 0x50000000;
    // The progress word is at block + 4.
    words.map[kG + 0x2A90] = kSp - 4;
    UNSCOPED_INFO("a progress word starting at r1 is clear with nothing above r1");
    REQUIRE(!StackReachesAllocator(words, reach, kG));
    words.map[kG + 0x2A90] = kSp - 8;
    UNSCOPED_INFO("a progress word ending at r1 overlaps");
    REQUIRE(StackReachesAllocator(words, reach, kG));
    words.map[kG + 0x2A90] = kLimit - 4;
    UNSCOPED_INFO("a progress word at the limit overlaps (block + 4)");
    REQUIRE(StackReachesAllocator(words, reach, kG));
    words.map[kG + 0x2A90] = kLimit - 8;
    UNSCOPED_INFO("a progress word ending at the limit is clear");
    REQUIRE(!StackReachesAllocator(words, reach, kG));
    words.map[kG + 0x2A90] = 0;
    words.map[kG + 0x34CC] = 0;
    UNSCOPED_INFO("null allocator pointers name nothing");
    REQUIRE(!StackReachesAllocator(words, reach, kG));
}

} // namespace

TEST_CASE("stack_guard", "[ctest][stack-guard][timeout-60]")
{
    testConstants();
    testBetween();
    testEdges();
    testEmpty();
    testWrap();
    testOf();
    testFrame();
    testRefuses();
    testPointee();
    testAllocator();
}
