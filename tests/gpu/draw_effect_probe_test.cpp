#include "gpu/guest_d3d/draw_effect_probe.h"
#include "gpu/guest_d3d/native_draw_dispatch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <latch>
#include <semaphore>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/hook.h>

using namespace rerevved::gpu::guest_d3d;
using namespace std::chrono_literals;

namespace
{

constexpr std::size_t   kGuestBytes = 0x8000;
constexpr std::uint32_t kGraphics   = 0x100;
constexpr std::uint32_t kStack      = 0x7000;
constexpr std::uint32_t kTiling     = kGraphics + kIndexedBufferTilingOffset;

std::atomic<bool> failBlock{ false }, failWords{ false };
// In-flight draw-root counts a probed root sees: itself alone, or a peer too.
std::atomic<std::uint64_t> flightOne{ 1 }, flightTwo{ 2 };

bool readWord(const std::uint8_t* base, std::uint32_t address, std::uint32_t& value)
{
    if (failWords || !base || address > kGuestBytes - 4)
        return false;
    value = std::uint32_t(base[address]) << 24 | std::uint32_t(base[address + 1]) << 16 | std::uint32_t(base[address + 2]) << 8 | base[address + 3];
    return true;
}

bool readBlock(const std::uint8_t* base, std::uint32_t address, std::span<std::uint8_t> bytes)
{
    if (failBlock || !base || address > kGuestBytes || bytes.size() > kGuestBytes - address)
        return false;
    std::memcpy(bytes.data(), base + address, bytes.size());
    return true;
}

const DrawEffectReaders kBlockReaders{ readWord, readBlock };
const DrawEffectReaders kWordReaders{ readWord, nullptr };

std::vector<std::uint8_t> memory()
{
    return std::vector<std::uint8_t>(kGuestBytes);
}

// Selects every interval-th root, so counts are exact.
bool byInterval(std::uint64_t index, std::uint32_t interval)
{
    return interval <= 1 || index % interval == 0;
}

DrawEffectConfig config(std::uint32_t interval, std::uint64_t limit, std::uint32_t perSwap = 0, DrawEffectSelector selector = byInterval)
{
    DrawEffectConfig value;
    value.interval = interval;
    value.limit    = limit;
    value.perSwap  = perSwap;
    value.selector = selector;
    return value;
}

// One probed root: Begin, the fake Original, End. `inFlight` is the draw-root
// count the dispatcher passes at both ends.
template <typename Original>
bool root(DrawEffectProbe& probe, const DrawEffectReaders& readers, std::vector<std::uint8_t>& guest, Original&& original, std::uint64_t inFlight = 1)
{
    const std::atomic<std::uint64_t> flight{ inFlight };
    const bool                       armed = probe.BeginRoot(readers, guest.data(), kGraphics, flight);
    original(guest);
    if (armed)
        probe.EndRoot(readers, guest.data(), flight);
    return armed;
}

void testGate()
{
    auto guest = memory();

    struct Case
    {
        std::uint8_t      tile;
        IndexedBufferGate verdict;
    };

    constexpr Case cases[]{
        { 0x00, IndexedBufferGate::Admit },
        { 0x40, IndexedBufferGate::Admit },
        { 0xDE, IndexedBufferGate::Admit },
        { 0x01, IndexedBufferGate::TilingSet },
        { 0x20, IndexedBufferGate::TilingSet },
        { 0x21, IndexedBufferGate::TiledRecord },
        { 0x31, IndexedBufferGate::TilingSet },
        { 0x11, IndexedBufferGate::TilingSet },
        { 0x61, IndexedBufferGate::TilingSet },
        { 0xA1, IndexedBufferGate::TilingSet },
        { 0xFF, IndexedBufferGate::TilingSet },
    };
    for (const auto& [tile, expected] : cases)
    {
        // Only the top byte of the word is the tiling byte.
        guest[kTiling]     = tile;
        guest[kTiling + 1] = 0xFF;
        guest[kTiling + 2] = 0x21;
        guest[kTiling + 3] = 0x21;
        UNSCOPED_INFO("gate verdict for the tiling byte");
        REQUIRE(IndexedBufferConsumeGate(readWord, guest.data(), kGraphics) == expected);
        UNSCOPED_INFO("constexpr gate agrees");
        REQUIRE(IndexedBufferTilingAdmits(tile) == (expected == IndexedBufferGate::Admit));
        // The tiled-record byte is exactly 0x21, whatever the other bytes of the word hold.
        UNSCOPED_INFO("only the exact byte is a tiled record");
        REQUIRE((expected == IndexedBufferGate::TiledRecord) == (tile == kIndexedBufferTiledByte));
    }
    guest[kTiling] = 0;
    UNSCOPED_INFO("no reader");
    REQUIRE(IndexedBufferConsumeGate(nullptr, guest.data(), kGraphics) == IndexedBufferGate::Unreadable);
    UNSCOPED_INFO("null device");
    REQUIRE(IndexedBufferConsumeGate(readWord, guest.data(), 0) == IndexedBufferGate::Unreadable);
    UNSCOPED_INFO("device past the address space");
    REQUIRE(IndexedBufferConsumeGate(readWord, guest.data(), 0xFFFFFFF0U) == IndexedBufferGate::Unreadable);
    UNSCOPED_INFO("tiling word outside readable memory");
    REQUIRE(IndexedBufferConsumeGate(readWord, guest.data(), 0x7F00) == IndexedBufferGate::Unreadable);
    failWords = true;
    UNSCOPED_INFO("reader failure");
    REQUIRE(IndexedBufferConsumeGate(readWord, guest.data(), kGraphics) == IndexedBufferGate::Unreadable);
    failWords = false;
}

void testClassification()
{
    struct Pin
    {
        std::uint32_t   first, last;
        DrawEffectClass kind;
    };

    constexpr Pin pins[]{
        { 0x00, 0x27, DrawEffectClass::ExpectedDirty },
        { 0x30, 0x33, DrawEffectClass::ExpectedCursor },
        { 0x2884, 0x2887, DrawEffectClass::ExpectedCache },
        { 0x2920, 0x292B, DrawEffectClass::ExpectedCache },
        { 0x2954, 0x2957, DrawEffectClass::ExpectedCache },
        { 0x2ABE, 0x2ABE, DrawEffectClass::ExpectedCache },
        { 0x2E38, 0x2E47, DrawEffectClass::ExpectedCache },
        { 0x319C, 0x319F, DrawEffectClass::ExpectedCache },
    };
    std::size_t expected = 0;
    for (std::uint32_t offset = 0; offset < kDrawEffectGlobalSize; ++offset)
    {
        auto kind = DrawEffectClass::Unexpected;
        for (const auto& pin : pins)
            if (offset >= pin.first && offset <= pin.last)
                kind = pin.kind;
        UNSCOPED_INFO("offset classification matches the pinned set");
        REQUIRE(ClassifyDrawEffectOffset(offset) == kind);
        expected += kind != DrawEffectClass::Unexpected;
    }
    // 40 dirty, 4 cursor, 4 + 12 + 4 + 1 + 16 + 4 cache.
    UNSCOPED_INFO("expected set holds exactly 85 byte offsets");
    REQUIRE(expected == 85);
    // Neighbours of every range are outside it, as are the tiling flags and the ring.
    for (const std::uint32_t outside : { 0x28U, 0x2FU, 0x34U, 0x38U, 0x2883U, 0x2888U, 0x291FU, 0x292CU, 0x2953U, 0x2958U, 0x2ABCU, 0x2ABDU, 0x2ABFU, 0x2E37U, 0x2E48U, 0x319BU, 0x31A0U, 0x33C8U, 0x5B10U })
    {
        UNSCOPED_INFO("neighbour is unexpected");
        REQUIRE(ClassifyDrawEffectOffset(outside) == DrawEffectClass::Unexpected);
    }
    UNSCOPED_INFO("class names");
    REQUIRE((std::string(DrawEffectClassName(DrawEffectClass::ExpectedDirty)) == "expected_dirty" &&
             std::string(DrawEffectClassName(DrawEffectClass::ExpectedCursor)) == "expected_cursor" &&
             std::string(DrawEffectClassName(DrawEffectClass::ExpectedCache)) == "expected_cache" &&
             std::string(DrawEffectClassName(DrawEffectClass::ExpectedList)) == "expected_list" &&
             std::string(DrawEffectClassName(DrawEffectClass::ExpectedAllocator)) == "expected_allocator" &&
             std::string(DrawEffectClassName(DrawEffectClass::Unexpected)) == "unexpected"));

    // The tiled set is the untiled one plus the list header and the growth
    // allocator words; the untiled classifier never reports them.
    constexpr Pin tiledPins[]{
        { 0x33C0, 0x33CF, DrawEffectClass::ExpectedList },
        { 0x3A44, 0x3A4F, DrawEffectClass::ExpectedAllocator },
        { 0x3A54, 0x3A57, DrawEffectClass::ExpectedAllocator },
    };
    std::size_t tiledExtra = 0;
    for (std::uint32_t offset = 0; offset < kDrawEffectGlobalSize; ++offset)
    {
        auto kind = ClassifyDrawEffectOffset(offset);
        for (const auto& pin : tiledPins)
            if (offset >= pin.first && offset <= pin.last)
                kind = pin.kind;
        UNSCOPED_INFO("tiled classification is the untiled set plus the pinned list and allocator ranges");
        REQUIRE(ClassifyTiledDrawEffectOffset(offset) == kind);
        tiledExtra += kind != ClassifyDrawEffectOffset(offset);
    }
    // 16 list header bytes, 12 + 4 allocator bytes.
    UNSCOPED_INFO("the tiled set adds exactly 32 byte offsets");
    REQUIRE(tiledExtra == 32);
    for (const std::uint32_t outside : { 0x33BFU, 0x33D0U, 0x3A43U, 0x3A50U, 0x3A53U, 0x3A58U, 0x34U, 0x38U, 0x2ABDU })
    {
        UNSCOPED_INFO("tiled neighbour is unexpected");
        REQUIRE(ClassifyTiledDrawEffectOffset(outside) == DrawEffectClass::Unexpected);
    }
    for (const std::uint32_t listed : { 0x33C0U, 0x33CFU, 0x3A44U, 0x3A57U })
    {
        UNSCOPED_INFO("the untiled classifier does not know the tiled ranges");
        REQUIRE(ClassifyDrawEffectOffset(listed) == DrawEffectClass::Unexpected);
    }
}

// A TiledRecord root is sampled into its own tallies; the untiled keys do not move.
void testTiledSamples()
{
    auto            guest = memory();
    DrawEffectProbe probe(config(1, 1000));
    // Untiled root: the list header is not an effect of an untiled Original.
    root(probe, kBlockReaders, guest, [](auto& g)
         {
             g[kGraphics + 0x33C8] = 0x11;
             g[kGraphics + 0x30]   = 0x11;
         });
    guest[kTiling] = 0x21;
    // Tiled roots, isolated and concurrent.
    root(probe, kBlockReaders, guest, [](auto& g)
         {
             g[kGraphics + 0x33C8] = 0x24;
             g[kGraphics + 0x3A57] = 0x40;
             g[kGraphics + 0x30]   = 0x01;
             g[kGraphics + 0x2ABE] = 0x04;
             g[kGraphics + 0x4000] = 0x7F;
         });
    root(probe, kBlockReaders, guest, [](auto& g)
         {
             g[kGraphics + 0x33C8] = 0x34;
             g[kGraphics + 0x3A48] = 0x01;
             g[kGraphics + 0x5000] = 0x01;
         },
         2);
    // A tiled root that changes nothing.
    root(probe, kBlockReaders, guest, [](auto&)
         {
         });
    // Another tiling byte is skipped as before.
    guest[kTiling] = 0x31;
    root(probe, kBlockReaders, guest, [](auto&)
         {
         });
    const auto  view = probe.Snapshot();
    const auto& c    = view.counts;
    UNSCOPED_INFO("the gate separates tiled roots from skipped ones");
    REQUIRE((c.roots == 5 && c.selected == 5 && c.gateSkipped == 1 && c.tiledSelected == 3));
    UNSCOPED_INFO("the untiled tallies hold the untiled sample only");
    REQUIRE((c.sampled == 1 && c.sampledIsolated == 1 && c.sampledWithChange == 1));
    UNSCOPED_INFO("three tiled samples, two with a change");
    REQUIRE((c.tiledSampled == 3 && c.tiledSampledIsolated == 2 && c.tiledSampledConcurrent == 1 && c.tiledSampledWithChange == 2));
    UNSCOPED_INFO("the untiled rows are the untiled sample's");
    REQUIRE((c.changedOffsets == 2 && c.unexpectedOffsets == 1 && view.offsets.size() == 2));
    UNSCOPED_INFO("an untiled change of the list cursor is unexpected");
    REQUIRE((view.offsets[1].offset == 0x33C8 && view.offsets[1].kind == DrawEffectClass::Unexpected));
    UNSCOPED_INFO("tiled offset totals");
    REQUIRE((c.tiledChangedOffsets == 7 && c.tiledIsolatedOffsets == 5 && c.tiledUnexpectedOffsets == 1 && c.tiledUnexpectedConcurrent == 1));
    const auto classOffsets = [&](DrawEffectClass kind)
    {
        return c.tiledClassOffsets[static_cast<std::size_t>(kind)];
    };
    UNSCOPED_INFO("tiled offsets per class");
    REQUIRE((classOffsets(DrawEffectClass::ExpectedCursor) == 1 && classOffsets(DrawEffectClass::ExpectedCache) == 1 && classOffsets(DrawEffectClass::ExpectedList) == 1 &&
             classOffsets(DrawEffectClass::ExpectedAllocator) == 2 && classOffsets(DrawEffectClass::Unexpected) == 2 && classOffsets(DrawEffectClass::ExpectedDirty) == 0));
    const auto& rows = view.tiledOffsets;
    UNSCOPED_INFO("tiled rows start with the cursor");
    REQUIRE((rows.size() == 7 && rows[0].offset == 0x30 && rows[0].kind == DrawEffectClass::ExpectedCursor));
    UNSCOPED_INFO("tiled cache row");
    REQUIRE((rows[1].offset == 0x2ABE && rows[1].kind == DrawEffectClass::ExpectedCache));
    UNSCOPED_INFO("tiled list cursor row with per-kind first changes");
    REQUIRE((rows[2].offset == 0x33C8 && rows[2].isolated == 1 && rows[2].concurrent == 1 && rows[2].kind == DrawEffectClass::ExpectedList &&
             rows[2].isolatedAfter == 0x24 && rows[2].concurrentBefore == 0x24 && rows[2].concurrentAfter == 0x34));
    UNSCOPED_INFO("tiled allocator rows");
    REQUIRE((rows[3].offset == 0x3A48 && rows[3].kind == DrawEffectClass::ExpectedAllocator && rows[4].offset == 0x3A57 && rows[4].kind == DrawEffectClass::ExpectedAllocator));
    UNSCOPED_INFO("tiled unexpected rows");
    REQUIRE((rows[5].offset == 0x4000 && rows[5].kind == DrawEffectClass::Unexpected && rows[6].offset == 0x5000 && rows[6].kind == DrawEffectClass::Unexpected));
    UNSCOPED_INFO("the sample-swap range follows untiled samples");
    REQUIRE((c.firstSampleSwap == 0 && c.lastSampleSwap == 0));
}

void testSnapshotReaders()
{
    auto guest = memory();
    for (std::size_t i = 0; i < guest.size(); ++i)
        guest[i] = std::uint8_t(i * 7 + 1);
    std::array<std::uint8_t, kDrawEffectGlobalSize> viaBlock{}, viaWords{};
    UNSCOPED_INFO("block copy");
    REQUIRE(ReadDrawEffectGlobal(kBlockReaders, guest.data(), kGraphics, viaBlock));
    UNSCOPED_INFO("word copy");
    REQUIRE(ReadDrawEffectGlobal(kWordReaders, guest.data(), kGraphics, viaWords));
    UNSCOPED_INFO("both readers copy the bytes as stored");
    REQUIRE((viaBlock == viaWords && !std::memcmp(viaBlock.data(), guest.data() + kGraphics, kDrawEffectGlobalSize)));
    failBlock = true;
    UNSCOPED_INFO("block failure");
    REQUIRE(!ReadDrawEffectGlobal(kBlockReaders, guest.data(), kGraphics, viaBlock));
    failBlock = false;
    UNSCOPED_INFO("range past memory");
    REQUIRE(!ReadDrawEffectGlobal(kBlockReaders, guest.data(), 0x7000, viaBlock));
    UNSCOPED_INFO("no reader");
    REQUIRE(!ReadDrawEffectGlobal({}, guest.data(), kGraphics, viaBlock));
}

void testSampling()
{
    auto guest = memory();
    {
        DrawEffectProbe probe(config(4, 1000));
        std::uint64_t   armed = 0;
        for (int i = 0; i < 100; ++i)
            armed += root(probe, kBlockReaders, guest, [](auto&)
                          {
                          });
        const auto counts = probe.Counts();
        UNSCOPED_INFO("the selector picks 25 of 100");
        REQUIRE((armed == 25 && counts.roots == 100 && counts.selected == 25 && counts.sampled == 25));
        UNSCOPED_INFO("no change, no skip");
        REQUIRE((!counts.sampledWithChange && !counts.changedOffsets && !counts.gateSkipped && !counts.limited));
    }

    // Tile byte set on every odd root: the gate rejects those roots that are selected.
    DrawEffectProbe gated(config(1, 1000));
    for (int i = 0; i < 10; ++i)
    {
        guest[kTiling] = (i & 1) ? 0x01 : 0x00;
        root(gated, kBlockReaders, guest, [](auto&)
             {
             });
    }
    auto counts = gated.Counts();
    UNSCOPED_INFO("the gate skips tiled roots");
    REQUIRE((counts.roots == 10 && counts.selected == 10 && counts.gateSkipped == 5 && counts.sampled == 5));
    guest[kTiling] = 0;

    // A selected root with an unreadable tiling word is counted apart from a skip.
    failWords = true;
    root(gated, kBlockReaders, guest, [](auto&)
         {
         });
    failWords = false;
    counts    = gated.Counts();
    UNSCOPED_INFO("unreadable tiling word");
    REQUIRE((counts.gateUnreadable == 1 && counts.gateSkipped == 5 && counts.sampled == 5));

    DrawEffectProbe limited(config(1, 5));
    limited.NoteSwap(9);
    for (int i = 0; i < 8; ++i)
        root(limited, kBlockReaders, guest, [](auto&)
             {
             });
    counts = limited.Counts();
    UNSCOPED_INFO("sampling stops at the total cap");
    REQUIRE((counts.selected == 8 && counts.sampled == 5 && counts.limited == 3));
    UNSCOPED_INFO("the cap records the swap it was first hit in");
    REQUIRE(counts.limitHitSwap == 9);

    // A zero interval never divides by zero.
    DrawEffectProbe zero(config(0, 10));
    UNSCOPED_INFO("interval zero samples every root");
    REQUIRE(root(zero, kBlockReaders, guest, [](auto&)
                 {
                 }));
}

bool never(std::uint64_t, std::uint32_t)
{
    return false;
}

void testRandomSelection()
{
    // Interval 16 over many frames of a stable draw count that is a multiple of
    // the interval: every call position is reached, which a modulus never does.
    constexpr std::uint64_t    frame = 1600, frames = 2000;
    std::vector<std::uint32_t> perPosition(frame, 0);
    std::uint64_t              selected = 0;
    std::array<bool, 16>       residue{};
    for (std::uint64_t index = 0; index < frame * frames; ++index)
        if (DrawEffectSelectRandom(index, 16))
        {
            ++perPosition[index % frame];
            ++selected;
            residue[index % 16] = true;
        }
    UNSCOPED_INFO("selection averages one root in sixteen");
    REQUIRE((selected > frame * frames / 16 * 9 / 10 && selected < frame * frames / 16 * 11 / 10));
    for (const auto hits : perPosition)
    {
        UNSCOPED_INFO("every call position is sampled somewhere");
        REQUIRE(hits > 0);
    }
    for (const auto seen : residue)
    {
        UNSCOPED_INFO("every residue modulo the interval is sampled");
        REQUIRE(seen);
    }
    // The default selector backs a probe built without one.
    DrawEffectProbe probe(config(16, 100000, 0, nullptr));
    auto            guest = memory();
    for (int i = 0; i < 1600; ++i)
        root(probe, kBlockReaders, guest, [](auto&)
             {
             });
    std::uint64_t expected = 0;
    for (std::uint64_t i = 0; i < 1600; ++i)
        expected += DrawEffectSelectRandom(i, 16);
    UNSCOPED_INFO("the probe uses the default selector");
    REQUIRE((expected > 0 && probe.Counts().selected == expected));
    UNSCOPED_INFO("intervals 0 and 1 select every root");
    REQUIRE((DrawEffectSelectRandom(12345, 1) && DrawEffectSelectRandom(12345, 0)));
}

void testSwapBudget()
{
    auto guest = memory();
    {
        // Two sampled roots a swap, however many are selected.
        DrawEffectProbe probe(config(1, 1000, 2));
        probe.NoteSwap(10);
        for (int i = 0; i < 5; ++i)
            root(probe, kBlockReaders, guest, [](auto&)
                 {
                 });
        probe.NoteSwap(11);
        for (int i = 0; i < 4; ++i)
            root(probe, kBlockReaders, guest, [](auto&)
                 {
                 });
        const auto counts = probe.Counts();
        UNSCOPED_INFO("budget caps a swap's samples");
        REQUIRE((counts.selected == 6 && counts.sampled == 3 && counts.budgetSkipped == 3));
        UNSCOPED_INFO("one swap spent its budget");
        REQUIRE((counts.budgetSwaps == 1 && counts.swapsSeen == 2));
        UNSCOPED_INFO("first and last swap that produced a sample");
        REQUIRE((counts.firstSampleSwap == 10 && counts.lastSampleSwap == 11));
        UNSCOPED_INFO("the total cap was not hit");
        REQUIRE((counts.limited == 0 && counts.limitHitSwap == kDrawEffectNoSwap));
    }
    {
        // The interval is fitted to the previous swap's root count: 1600 roots
        // and a budget of 8 give ceil(1600 * 4 / 24) = 267.
        DrawEffectProbe probe(config(16, 100000, 8));
        probe.NoteSwap(1);
        for (int i = 0; i < 1600; ++i)
            root(probe, kBlockReaders, guest, [](auto&)
                 {
                 });
        auto counts = probe.Counts();
        UNSCOPED_INFO("the first swap runs at the base interval");
        REQUIRE((counts.selected == 100 && counts.sampled == 8 && counts.budgetSkipped == 92));
        probe.NoteSwap(2);
        for (int i = 0; i < 2670; ++i)
            root(probe, kBlockReaders, guest, [](auto&)
                 {
                 });
        counts = probe.Counts();
        UNSCOPED_INFO("the next swap is spread by the fitted interval");
        REQUIRE((counts.selected == 110 && counts.sampled == 16 && counts.budgetSkipped == 94));
        UNSCOPED_INFO("swap range of the samples");
        REQUIRE((counts.lastSampleSwap == 2 && counts.firstSampleSwap == 1 && counts.budgetSwaps == 2));
    }
    {
        // A swap just past the base interval's share fits a slightly larger interval:
        // 7 roots in swap 1, then ceil(100 * 4 / 24) = 17 for the next 160.
        DrawEffectProbe probe(config(16, 100000, 8));
        probe.NoteSwap(1);
        for (int i = 0; i < 100; ++i)
            root(probe, kBlockReaders, guest, [](auto&)
                 {
                 });
        probe.NoteSwap(2);
        for (int i = 0; i < 160; ++i)
            root(probe, kBlockReaders, guest, [](auto&)
                 {
                 });
        UNSCOPED_INFO("the interval follows the previous swap");
        REQUIRE(probe.Counts().selected == 7 + 10);
    }
}

void testDiff()
{
    DrawEffectProbe probe(config(1, 100));
    auto            guest = memory();
    root(probe, kBlockReaders, guest, [](auto& g)
         {
             g[kGraphics + 0x33]   = 0x10;
             g[kGraphics + 0x2ABE] = 0x08;
         });
    root(probe, kBlockReaders, guest, [](auto& g)
         {
             g[kGraphics + 0x33]   = 0x20;
             g[kGraphics + 0x1234] = 0xAA;
         });
    root(probe, kBlockReaders, guest, [](auto&)
         {
         });
    root(probe, kWordReaders, guest, [](auto& g)
         {
             g[kGraphics + 0x33] = 0x30;
         });
    const auto view = probe.Snapshot();
    UNSCOPED_INFO("four isolated samples, three with a change");
    REQUIRE((view.counts.roots == 4 && view.counts.sampled == 4 && view.counts.sampledIsolated == 4 && view.counts.sampledWithChange == 3));
    UNSCOPED_INFO("three offsets changed, one unexpected");
    REQUIRE((view.counts.changedOffsets == 3 && view.counts.unexpectedOffsets == 1 && view.counts.unexpectedConcurrent == 0));
    const auto& rows = view.offsets;
    UNSCOPED_INFO("three rows");
    REQUIRE(rows.size() == 3);
    UNSCOPED_INFO("cursor byte: three changes, first 00 to 10");
    REQUIRE((rows[0].offset == 0x33 && rows[0].isolated == 3 && rows[0].concurrent == 0 && rows[0].isolatedBefore == 0x00 && rows[0].isolatedAfter == 0x10 && rows[0].kind == DrawEffectClass::ExpectedCursor));
    UNSCOPED_INFO("unexpected byte");
    REQUIRE((rows[1].offset == 0x1234 && rows[1].isolated == 1 && rows[1].isolatedBefore == 0x00 && rows[1].isolatedAfter == 0xAA && rows[1].kind == DrawEffectClass::Unexpected));
    UNSCOPED_INFO("cache byte");
    REQUIRE((rows[2].offset == 0x2ABE && rows[2].isolated == 1 && rows[2].isolatedBefore == 0x00 && rows[2].isolatedAfter == 0x08 && rows[2].kind == DrawEffectClass::ExpectedCache));
    UNSCOPED_INFO("the accessors agree with the snapshot");
    REQUIRE((probe.Counts().changedOffsets == view.counts.changedOffsets && probe.Offsets().size() == rows.size()));
}

void testConcurrentSamples()
{
    auto guest = memory();
    {
        // Another root in flight at either end, or one that began meanwhile.
        DrawEffectProbe probe(config(1, 100));
        UNSCOPED_INFO("armed with another root in flight");
        REQUIRE(probe.BeginRoot(kBlockReaders, guest.data(), kGraphics, flightTwo));
        probe.EndRoot(kBlockReaders, guest.data(), flightOne);
        UNSCOPED_INFO("armed");
        REQUIRE(probe.BeginRoot(kBlockReaders, guest.data(), kGraphics, flightOne));
        probe.EndRoot(kBlockReaders, guest.data(), flightTwo);
        UNSCOPED_INFO("armed");
        REQUIRE(probe.BeginRoot(kBlockReaders, guest.data(), kGraphics, flightOne));
        probe.NoteRoot();
        probe.EndRoot(kBlockReaders, guest.data(), flightOne);
        auto counts = probe.Counts();
        UNSCOPED_INFO("three concurrent samples");
        REQUIRE((counts.sampledConcurrent == 3 && counts.sampledIsolated == 0));
        // A root that began before the sample does not make it concurrent.
        probe.NoteRoot();
        UNSCOPED_INFO("armed");
        REQUIRE(probe.BeginRoot(kBlockReaders, guest.data(), kGraphics, flightOne));
        probe.EndRoot(kBlockReaders, guest.data(), flightOne);
        counts = probe.Counts();
        UNSCOPED_INFO("an earlier root is not a writer");
        REQUIRE((counts.sampledConcurrent == 3 && counts.sampledIsolated == 1));
    }
    {
        // Concurrent and isolated changes are counted apart, and only isolated
        // ones feed unexpected_offsets.
        DrawEffectProbe probe(config(1, 100));
        root(probe, kBlockReaders, guest, [](auto& g)
             {
                 g[kGraphics + 0x1000] = 0x7F;
             });
        root(probe, kBlockReaders, guest, [](auto& g)
             {
                 g[kGraphics + 0x1000] = 0x80;
                 g[kGraphics + 0x2000] = 0x01;
                 g[kGraphics + 0x30]   = 0x01;
             },
             2);
        const auto view = probe.Snapshot();
        UNSCOPED_INFO("one of each kind");
        REQUIRE((view.counts.sampledIsolated == 1 && view.counts.sampledConcurrent == 1));
        UNSCOPED_INFO("offsets changed in an isolated sample");
        REQUIRE((view.counts.changedOffsets == 3 && view.counts.isolatedOffsets == 1));
        UNSCOPED_INFO("unexpected offsets by kind");
        REQUIRE((view.counts.unexpectedOffsets == 1 && view.counts.unexpectedConcurrent == 2));
        UNSCOPED_INFO("per-kind counts and first changes");
        REQUIRE((view.offsets[1].offset == 0x1000 && view.offsets[1].isolated == 1 && view.offsets[1].concurrent == 1 &&
                 view.offsets[1].isolatedAfter == 0x7F && view.offsets[1].concurrentBefore == 0x7F && view.offsets[1].concurrentAfter == 0x80));
    }
}

void testPairing()
{
    auto guest = memory();
    {
        // An end without a begin does nothing.
        DrawEffectProbe probe(config(1, 100));
        probe.EndRoot(kBlockReaders, guest.data(), flightOne);
        UNSCOPED_INFO("an end without a begin does nothing");
        REQUIRE(probe.Counts().sampled == 0);
    }
    {
        // An unreadable copy at either end is an unreadable snapshot.
        DrawEffectProbe probe(config(1, 100));
        failBlock = true;
        UNSCOPED_INFO("before copy unreadable");
        REQUIRE(!probe.BeginRoot(kBlockReaders, guest.data(), kGraphics, flightOne));
        failBlock = false;
        UNSCOPED_INFO("armed");
        REQUIRE(probe.BeginRoot(kBlockReaders, guest.data(), kGraphics, flightOne));
        failBlock = true;
        probe.EndRoot(kBlockReaders, guest.data(), flightOne);
        failBlock         = false;
        const auto counts = probe.Counts();
        UNSCOPED_INFO("two unreadable snapshots");
        REQUIRE((counts.snapshotUnreadable == 2 && counts.sampled == 0 && counts.gateUnreadable == 0));
    }
    {
        DrawEffectProbe probe(config(1, 100));
        UNSCOPED_INFO("armed");
        REQUIRE(probe.BeginRoot(kBlockReaders, guest.data(), kGraphics, flightOne));
        probe.AbandonRoot();
        // A second begin while one is armed abandons the first.
        UNSCOPED_INFO("armed again");
        REQUIRE(probe.BeginRoot(kBlockReaders, guest.data(), kGraphics, flightOne));
        UNSCOPED_INFO("armed over a stale sample");
        REQUIRE(probe.BeginRoot(kBlockReaders, guest.data(), kGraphics, flightOne));
        probe.EndRoot(kBlockReaders, guest.data(), flightOne);
        const auto counts = probe.Counts();
        UNSCOPED_INFO("abandoned samples");
        REQUIRE((counts.abandoned == 2 && counts.sampled == 1));
    }
}

void testThreads()
{
    DrawEffectProbe probe(config(1, 100000));
    auto            work = [&probe]
    {
        auto guest = memory();
        for (int i = 0; i < 200; ++i)
            root(probe, kBlockReaders, guest, [](auto& g)
                 {
                     g[kGraphics + 0x33] ^= 0xFF;
                 });
    };
    std::thread a(work), b(work);
    a.join();
    b.join();
    const auto counts = probe.Counts();
    const auto rows   = probe.Offsets();
    UNSCOPED_INFO("both threads sampled every root");
    REQUIRE((counts.roots == 400 && counts.sampled == 400 && counts.abandoned == 0));
    UNSCOPED_INFO("per-thread copies never mix");
    REQUIRE((rows.size() == 1 && rows[0].offset == 0x33 && rows[0].isolated == 400));
}

void testLazyBuffers()
{
    bool before = true, afterSkipped = true, afterSampled = false;
    std::thread([&]
                {
                    auto guest                = memory();
                    before                    = DrawEffectThreadBuffersAllocated();
                    DrawEffectConfig skipping = config(1, 100);
                    skipping.selector         = never;
                    DrawEffectProbe skip(skipping);
                    root(skip, kBlockReaders, guest, [](auto&)
                         {
                         });
                    afterSkipped = DrawEffectThreadBuffersAllocated();
                    DrawEffectProbe sample(config(1, 100));
                    root(sample, kBlockReaders, guest, [](auto&)
                         {
                         });
                    afterSampled = DrawEffectThreadBuffersAllocated();
                })
        .join();
    UNSCOPED_INFO("a thread allocates its buffers on its first sampled root only");
    REQUIRE((!before && !afterSkipped && afterSampled));
}

// ---- interleavings driven through the probe's test hook ----

struct HookScope
{
    explicit HookScope(void (*hook)(DrawEffectHookPoint, std::uint64_t))
    {
        detail::drawEffectHook = hook;
    }

    ~HookScope()
    {
        detail::drawEffectHook = nullptr;
    }
};

DrawEffectProbe*                             hookProbe = nullptr;
DrawEffectHookPoint                          peerAt    = DrawEffectHookPoint::BeginAfterMarker;
std::binary_semaphore                        reachedCommit{ 0 }, resumeCommit{ 0 };
std::array<std::atomic<std::uint32_t>, 1024> sequenceSamples{};

// A peer root starts, and finishes, at one chosen point of the sample.
void peerStarts(DrawEffectHookPoint point, std::uint64_t)
{
    if (point == peerAt)
        hookProbe->NoteRoot();
}

void pauseBeforeCommit(DrawEffectHookPoint point, std::uint64_t)
{
    if (point != DrawEffectHookPoint::EndBeforeCommit)
        return;
    reachedCommit.release();
    resumeCommit.acquire();
}

void tallySequence(DrawEffectHookPoint point, std::uint64_t sequence)
{
    if (point == DrawEffectHookPoint::Reserved && sequence < sequenceSamples.size())
        sequenceSamples[sequence].fetch_add(1);
}

void testCommitIsAtomicWithClose()
{
    {
        // Close arrives while a finished sample waits to commit: the report has
        // neither its counters nor its rows, now or after the commit is refused.
        DrawEffectProbe probe(config(1, 100));
        HookScope       scope(pauseBeforeCommit);
        std::thread     worker([&]
                               {
                               auto guest = memory();
                               root(probe, kBlockReaders, guest, [](auto& g)
                                    {
                                        g[kGraphics + 0x33] = 0x10;
                                    });
                               });
        reachedCommit.acquire();
        probe.Seal();
        auto view = probe.Snapshot();
        UNSCOPED_INFO("a sample not yet committed is not reported");
        REQUIRE((view.counts.sampled == 0 && view.counts.sampledWithChange == 0 && view.offsets.empty()));
        resumeCommit.release();
        worker.join();
        view = probe.Snapshot();
        UNSCOPED_INFO("a commit after Seal is refused whole");
        REQUIRE((view.counts.sampled == 0 && view.counts.sampledWithChange == 0 && view.offsets.empty()));
    }
    {
        // A snapshot taken while samples commit never shows a counter without its rows:
        // every sample changes exactly one byte.
        DrawEffectProbe  probe(config(1, 1000000));
        std::atomic<int> running{ 2 };
        auto             work = [&]
        {
            auto guest = memory();
            for (int i = 0; i < 4000; ++i)
                root(probe, kBlockReaders, guest, [](auto& g)
                     {
                         g[kGraphics + 0x33] ^= 0xFF;
                     });
            --running;
        };
        std::thread   a(work), b(work);
        std::uint64_t seen = 0;
        while (running.load())
        {
            const auto    view = probe.Snapshot();
            std::uint64_t rows = 0;
            for (const auto& row : view.offsets)
                rows += row.isolated + row.concurrent;
            UNSCOPED_INFO("a snapshot holds whole samples");
            REQUIRE((view.counts.sampled == view.counts.sampledWithChange && rows == view.counts.sampled));
            seen += view.counts.sampled;
        }
        a.join();
        b.join();
        const auto view = probe.Snapshot();
        UNSCOPED_INFO("every sample committed");
        REQUIRE((view.counts.sampled == 8000 && view.offsets.size() == 1 && view.offsets[0].isolated + view.offsets[0].concurrent == 8000));
    }
}

void testOverlapOrdering()
{
    auto            guest = memory();
    DrawEffectProbe probe(config(1, 100));
    hookProbe = &probe;
    {
        // A peer that starts after the begin marker, writes G and finishes before
        // the in-flight count is read, is still seen: the marker moved.
        peerAt = DrawEffectHookPoint::BeginAfterMarker;
        HookScope scope(peerStarts);
        root(probe, kBlockReaders, guest, [](auto& g)
             {
                 g[kGraphics + 0x1000] = 0x01;
             });
        auto counts = probe.Counts();
        UNSCOPED_INFO("a peer after the begin marker is concurrent");
        REQUIRE((counts.sampledConcurrent == 1 && counts.sampledIsolated == 0));
    }
    {
        // A peer that starts between the end's in-flight read and its marker read.
        peerAt = DrawEffectHookPoint::EndAfterInFlight;
        HookScope scope(peerStarts);
        root(probe, kBlockReaders, guest, [](auto& g)
             {
                 g[kGraphics + 0x1001] = 0x01;
             });
        auto counts = probe.Counts();
        UNSCOPED_INFO("a peer between the end's two reads is concurrent");
        REQUIRE((counts.sampledConcurrent == 2 && counts.sampledIsolated == 0));
    }
    {
        // No peer: isolated.
        root(probe, kBlockReaders, guest, [](auto&)
             {
             });
        UNSCOPED_INFO("no peer, isolated");
        REQUIRE(probe.Counts().sampledIsolated == 1);
    }
    hookProbe = nullptr;
}

bool always(std::uint64_t, std::uint32_t)
{
    return true;
}

void testSwapBudgetIsAtomic()
{
    // Workers sample every root while swaps advance: no sequence gets more
    // samples than the budget.
    constexpr std::uint32_t    budget = 3;
    DrawEffectProbe            probe(config(1, 10000000, budget, always));
    HookScope                  scope(tallySequence);
    std::atomic<bool>          stop{ false };
    std::atomic<std::uint64_t> ran{ 0 };
    auto                       work = [&]
    {
        auto guest = memory();
        while (!stop.load())
        {
            root(probe, kBlockReaders, guest, [](auto& g)
                 {
                     g[kGraphics + 0x33] ^= 0xFF;
                 });
            ran.fetch_add(1);
        }
    };
    std::thread a(work), b(work), c(work);
    while (ran.load() < 100)
        std::this_thread::yield();
    for (std::uint64_t swap = 1; swap < 600; ++swap)
    {
        probe.NoteSwap(swap);
        const auto seen = ran.load();
        while (ran.load() < seen + 3)
            std::this_thread::yield();
    }
    stop = true;
    a.join();
    b.join();
    c.join();
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < 600; ++i)
    {
        UNSCOPED_INFO("no swap sequence exceeds the budget");
        REQUIRE(sequenceSamples[i].load() <= budget);
        total += sequenceSamples[i].load();
    }
    UNSCOPED_INFO("every reserved sample was committed under its sequence");
    REQUIRE((total > 0 && probe.Counts().sampled == total));
}

void testThreadExitCountsArmedSample()
{
    {
        // A thread that exits holding a sample accounts for it.
        DrawEffectProbe probe(config(1, 100));
        std::thread([&]
                    {
                        auto guest = memory();
                        UNSCOPED_INFO("armed");
                        REQUIRE(probe.BeginRoot(kBlockReaders, guest.data(), kGraphics, flightOne));
                    })
            .join();
        const auto counts = probe.Counts();
        UNSCOPED_INFO("a thread exit abandons its armed sample");
        REQUIRE((counts.abandoned == 1 && counts.sampled == 0));
    }
    {
        // The probe is gone before the thread exits: nothing is touched.
        auto*       dead = new DrawEffectProbe(config(1, 100));
        std::latch  armed(1), go(1);
        std::thread thread([&]
                           {
                               auto guest = memory();
                               UNSCOPED_INFO("armed");
                               REQUIRE(dead->BeginRoot(kBlockReaders, guest.data(), kGraphics, flightOne));
                               armed.count_down();
                               go.wait();
                           });
        armed.wait();
        delete dead;
        go.count_down();
        thread.join();
    }
}

void testReport()
{
    DrawEffectProbe probe(config(1, 100));
    auto            guest = memory();
    root(probe, kBlockReaders, guest, [](auto& g)
         {
             g[kGraphics + 0x33]   = 0x10;
             g[kGraphics + 0x2ABE] = 0x08;
             g[kGraphics + 0x1000] = 0x7F;
         });
    guest[kTiling] = 0x01;
    root(probe, kBlockReaders, guest, [](auto&)
         {
         });
    guest[kTiling] = 0x00;
    root(probe, kBlockReaders, guest, [](auto& g)
         {
             g[kGraphics + 0x1000] = 0x80;
             g[kGraphics + 0x34]   = 0x01;
         },
         2);
    std::ostringstream out;
    probe.Write(out);
    const std::string expected =
        "[draw_effect_probe]\n"
        "scope = 'G only: [G, G+0x5B10); ring words and writes outside G are not observed'\n"
        "interval = 1\n"
        "sample_limit = 100\n"
        "samples_per_swap = 0\n"
        "global_size = 0x5b10\n"
        "roots = 3\n"
        "selected = 3\n"
        "gate_skipped = 1\n"
        "gate_unreadable = 0\n"
        "snapshot_unreadable = 0\n"
        "sampled = 2\n"
        "sampled_isolated = 1\n"
        "sampled_concurrent = 1\n"
        "sampled_with_change = 2\n"
        "abandoned = 0\n"
        "swaps_seen = 0\n"
        "first_sample_swap = 0\n"
        "last_sample_swap = 0\n"
        "swap_budget_skipped = 0\n"
        "swap_budget_swaps = 0\n"
        "total_limit_refused = 0\n"
        "total_limit_first_swap = 'none'\n"
        "changed_offsets = 4\n"
        "isolated_offsets = 3\n"
        "unexpected_offsets = 1\n"
        "unexpected_offsets_concurrent = 2\n"
        "tiled_selected = 0\n"
        "tiled_sampled = 0\n"
        "tiled_sampled_isolated = 0\n"
        "tiled_sampled_concurrent = 0\n"
        "tiled_sampled_with_change = 0\n"
        "tiled_changed_offsets = 0\n"
        "tiled_isolated_offsets = 0\n"
        "tiled_unexpected_offsets = 0\n"
        "tiled_unexpected_offsets_concurrent = 0\n"
        "tiled_offsets_expected_dirty = 0\n"
        "tiled_offsets_expected_cursor = 0\n"
        "tiled_offsets_expected_cache = 0\n"
        "tiled_offsets_expected_list = 0\n"
        "tiled_offsets_expected_allocator = 0\n"
        "tiled_offsets_unexpected = 0\n"
        "\n"
        "[[changed_offset]]\n"
        "offset = 0x0033\n"
        "isolated_count = 1\n"
        "isolated_first_before = 0x00\n"
        "isolated_first_after = 0x10\n"
        "concurrent_count = 0\n"
        "class = 'expected_cursor'\n"
        "\n"
        "[[changed_offset]]\n"
        "offset = 0x0034\n"
        "isolated_count = 0\n"
        "concurrent_count = 1\n"
        "concurrent_first_before = 0x00\n"
        "concurrent_first_after = 0x01\n"
        "class = 'unexpected'\n"
        "\n"
        "[[changed_offset]]\n"
        "offset = 0x1000\n"
        "isolated_count = 1\n"
        "isolated_first_before = 0x00\n"
        "isolated_first_after = 0x7f\n"
        "concurrent_count = 1\n"
        "concurrent_first_before = 0x7f\n"
        "concurrent_first_after = 0x80\n"
        "class = 'unexpected'\n"
        "\n"
        "[[changed_offset]]\n"
        "offset = 0x2abe\n"
        "isolated_count = 1\n"
        "isolated_first_before = 0x00\n"
        "isolated_first_after = 0x08\n"
        "concurrent_count = 0\n"
        "class = 'expected_cache'\n";
    if (out.str() != expected)
    {
        std::cerr << "report differs:\n"
                  << out.str() << '\n';
        std::exit(1);
    }
}

// The tiled tallies and rows are written apart from the untiled ones.
void testTiledReport()
{
    DrawEffectProbe probe(config(1, 100));
    auto            guest = memory();
    guest[kTiling]        = 0x21;
    root(probe, kBlockReaders, guest, [](auto& g)
         {
             g[kGraphics + 0x33C8] = 0x24;
             g[kGraphics + 0x3A54] = 0x01;
         });
    std::ostringstream out;
    probe.Write(out);
    const auto text = out.str();
    const auto has  = [&](const std::string& part)
    {
        return text.find(part) != std::string::npos;
    };
    UNSCOPED_INFO("the untiled keys stay empty");
    REQUIRE((has("\nsampled = 0\n") && has("\nchanged_offsets = 0\n") && !has("[[changed_offset]]")));
    UNSCOPED_INFO("the tiled keys count the sample by class");
    REQUIRE((has("\ntiled_selected = 1\n") && has("\ntiled_sampled = 1\n") && has("\ntiled_sampled_isolated = 1\n") && has("\ntiled_sampled_with_change = 1\n") &&
             has("\ntiled_changed_offsets = 2\n") && has("\ntiled_unexpected_offsets = 0\n") && has("\ntiled_offsets_expected_list = 1\n") &&
             has("\ntiled_offsets_expected_allocator = 1\n") && has("\ntiled_offsets_unexpected = 0\n")));
    UNSCOPED_INFO("the tiled rows carry their classes");
    REQUIRE((has("[[changed_offset_tiled]]\noffset = 0x33c8\nisolated_count = 1\nisolated_first_before = 0x00\nisolated_first_after = 0x24\nconcurrent_count = 0\nclass = 'expected_list'\n") &&
             has("[[changed_offset_tiled]]\noffset = 0x3a54\n") && has("class = 'expected_allocator'\n")));
}

// The production path: the draw scope the generated-code wrapper builds, a
// dispatcher with fixture readers, and the probe the draw hooks find.
std::atomic<std::uint64_t> originalCalls{ 0 };
// What the dispatcher's `began` callback writes into G before Original.
thread_local std::uint8_t* beganGuest = nullptr;
std::atomic<std::uint8_t>  beganValue{ 0 };

PPCContext entry(std::uint32_t stack)
{
    PPCContext ctx{};
    ctx.r1.u64 = stack;
    ctx.r3.u64 = kGraphics;
    ctx.lr     = 0x82306000;
    return ctx;
}

void toggleCursor(std::vector<std::uint8_t>& guest)
{
    guest[kGraphics + 0x33] ^= 0xFF;
}

void testLiveWrapper()
{
    const auto path = std::filesystem::temp_directory_path() / "rerevved_draw_effect_probe_test.toml";
    std::filesystem::remove(path);
    UNSCOPED_INFO("probe is off until opened");
    REQUIRE(drawEffectProbe.load() == nullptr);

    NativeDrawDispatchConfig dispatch;
    dispatch.mode      = NativeDrawMode::Skip;
    dispatch.read      = readWord;
    dispatch.readBlock = readBlock;
    // A callback that writes G before Original: the before copy must follow it.
    dispatch.began = [](const NativeDrawSubmission&)
    {
        if (beganGuest)
            beganGuest[kGraphics + 0x40] = ++beganValue;
    };
    NativeDrawDispatcher dispatcher(dispatch);
    UNSCOPED_INFO("dispatcher installs");
    REQUIRE(InstallNativeDrawDispatcher(dispatcher));

    // Off: the root reaches Original and the dispatcher, and the thread takes no sample buffers.
    {
        bool allocated = true;
        auto guest     = memory();
        std::thread([&]
                    {
                        beganGuest          = guest.data();
                        auto            ctx = entry(kStack);
                        NativeDrawScope scope(NativeDrawHook::IndexedBuffer, ctx, guest.data());
                        ++originalCalls;
                        toggleCursor(guest);
                        ctx.r3.u64 = 0;
                        allocated  = DrawEffectThreadBuffersAllocated();
                    })
            .join();
        UNSCOPED_INFO("Original and the began callback ran");
        REQUIRE((originalCalls.load() == 1 && guest[kGraphics + 0x33] == 0xFF && guest[kGraphics + 0x40] == 1));
        UNSCOPED_INFO("the dispatcher numbered the root");
        REQUIRE(dispatcher.Totals().attempted == 1);
        UNSCOPED_INFO("an unsampled thread holds no sample buffers");
        REQUIRE(!allocated);
    }
    originalCalls = 0;
    beganGuest    = nullptr;

    // The probe a draw hook finds is published through one pointer.
    DrawEffectProbe local(config(1, 1000));
    drawEffectProbe.store(&local);
    auto guest = memory();
    beganGuest = guest.data();

    // A nested draw's effect belongs to the root; the sample ends after the root.
    {
        auto            ctx = entry(kStack);
        NativeDrawScope scope(NativeDrawHook::IndexedBuffer, ctx, guest.data());
        ++originalCalls;
        toggleCursor(guest);
        {
            auto            inner = entry(kStack - 0x100);
            NativeDrawScope nested(NativeDrawHook::IndexedBuffer, inner, guest.data());
            guest[kGraphics + 0x2ABE] = 0x08;
        }
        guest[kGraphics + 0x1000] = 0x7F;
        ctx.r3.u64                = 0;
    }
    {
        const auto view = local.Snapshot();
        UNSCOPED_INFO("one isolated sample");
        REQUIRE((originalCalls.load() == 1 && view.counts.roots == 1 && view.counts.sampledIsolated == 1 && view.counts.abandoned == 0));
        UNSCOPED_INFO("the nested draw's and the later effects are in the root's diff");
        REQUIRE((view.offsets.size() == 3 && view.offsets[0].offset == 0x33 && view.offsets[1].offset == 0x1000 && view.offsets[2].offset == 0x2ABE));
        // The began callback wrote 0x40 before the before copy, so it is not a change.
        UNSCOPED_INFO("a write by the began callback is not an effect of Original");
        REQUIRE((view.counts.unexpectedOffsets == 1 && guest[kGraphics + 0x40] != 0));
    }

    // An unwinding root drops its sample.
    {
        DrawEffectProbe unwound(config(1, 1000));
        drawEffectProbe.store(&unwound);
        try
        {
            auto            ctx = entry(kStack);
            NativeDrawScope scope(NativeDrawHook::IndexedBuffer, ctx, guest.data());
            toggleCursor(guest);
            throw std::runtime_error("fixture Original unwind");
        }
        catch (const std::runtime_error&)
        {
        }
        const auto counts = unwound.Counts();
        UNSCOPED_INFO("an unwound root abandons its sample");
        REQUIRE((counts.roots == 1 && counts.abandoned == 1 && counts.sampled == 0));
        drawEffectProbe.store(&local);
    }

    // A later entry at or above the root's frame finishes the stale root through
    // finishRoot: its sample is abandoned and the new root samples.
    {
        DrawEffectProbe stale(config(1, 1000));
        drawEffectProbe.store(&stale);
        {
            auto            first = entry(kStack);
            NativeDrawScope skipped(NativeDrawHook::IndexedBuffer, first, guest.data());
            auto            second = entry(kStack);
            NativeDrawScope scope(NativeDrawHook::IndexedBuffer, second, guest.data());
            toggleCursor(guest);
        }
        const auto counts = stale.Counts();
        UNSCOPED_INFO("a stale root abandons its sample");
        REQUIRE((counts.roots == 2 && counts.abandoned == 1 && counts.sampled == 1 && counts.sampledIsolated == 1));
        drawEffectProbe.store(&local);
    }

    // A thread that exits with a draw scope that never ended: the dispatcher
    // unwinds the root and the probe counts the sample as abandoned.
    {
        DrawEffectProbe exiting(config(1, 1000));
        drawEffectProbe.store(&exiting);
        std::thread([&]
                    {
                        auto guest = memory();
                        beganGuest = guest.data();
                        // Leaked on purpose: the scope is never destroyed.
                        auto* ctx = new PPCContext(entry(kStack));
                        new NativeDrawScope(NativeDrawHook::IndexedBuffer, *ctx, guest.data());
                    })
            .join();
        const auto counts = exiting.Counts();
        UNSCOPED_INFO("a thread exit with a skipped scope abandons the sample");
        REQUIRE((counts.roots == 1 && counts.abandoned == 1 && counts.sampled == 0));
        drawEffectProbe.store(&local);
    }

    // Two roots in flight on different threads are both concurrent.
    {
        DrawEffectProbe overlap(config(1, 1000));
        drawEffectProbe.store(&overlap);
        auto        other = memory();
        std::latch  entered(1), release(1);
        std::thread peer([&]
                         {
                             beganGuest          = other.data();
                             auto            ctx = entry(kStack);
                             NativeDrawScope scope(NativeDrawHook::IndexedBuffer, ctx, other.data());
                             entered.count_down();
                             release.wait();
                             toggleCursor(other);
                         });
        entered.wait();
        {
            auto            ctx = entry(kStack);
            NativeDrawScope scope(NativeDrawHook::IndexedBuffer, ctx, guest.data());
            guest[kGraphics + 0x1000] = 0x55;
        }
        release.count_down();
        peer.join();
        const auto counts = overlap.Counts();
        UNSCOPED_INFO("overlapping roots are concurrent");
        REQUIRE((counts.sampled == 2 && counts.sampledConcurrent == 2 && counts.sampledIsolated == 0));
        UNSCOPED_INFO("the overlap's unexpected change is not an isolated one");
        REQUIRE((counts.unexpectedOffsets == 0 && counts.unexpectedConcurrent == 1));
        drawEffectProbe.store(&local);
    }
    drawEffectProbe.store(nullptr);

    // The process probe: opened by the cvar, sampled one root in sixteen at random,
    // and written at close.
    std::string error;
    UNSCOPED_INFO("probe opens a new file");
    REQUIRE(OpenDrawEffectProbe(path, error));
    UNSCOPED_INFO("a second open is refused");
    REQUIRE((!OpenDrawEffectProbe(path, error) && !error.empty()));
    UNSCOPED_INFO("probe is live");
    REQUIRE(drawEffectProbe.load() != nullptr);
    NoteDrawEffectSwap(7);
    guest                  = memory();
    beganGuest             = guest.data();
    std::uint64_t selected = 0;
    for (int call = 0; call < 200; ++call)
    {
        selected += DrawEffectSelectRandom(call, 16);
        auto            ctx = entry(kStack);
        NativeDrawScope scope(NativeDrawHook::IndexedBuffer, ctx, guest.data());
        toggleCursor(guest);
        ctx.r3.u64 = 0;
    }
    UNSCOPED_INFO("the selector picks some of the 200 roots");
    REQUIRE(selected >= 4);
    const auto sampled = std::min<std::uint64_t>(selected, kDrawEffectSamplesPerSwap);

    UNSCOPED_INFO("probe closes");
    REQUIRE(CloseDrawEffectProbe(error));
    UNSCOPED_INFO("probe is off after close");
    REQUIRE(drawEffectProbe.load() == nullptr);
    UNSCOPED_INFO("dispatcher removes");
    REQUIRE(RemoveNativeDrawDispatcher(2000ms));

    std::ifstream     in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const auto has = [&text](const std::string& line)
    {
        return text.find("\n" + line + "\n") != std::string::npos;
    };
    UNSCOPED_INFO("report header names its scope");
    REQUIRE(text.starts_with("[draw_effect_probe]\nscope = 'G only: [G, G+0x5B10); ring words and writes outside G are not observed'\n"));
    UNSCOPED_INFO("live counts");
    REQUIRE((has("roots = 200") && has("selected = " + std::to_string(selected)) && has("sampled = " + std::to_string(sampled))));
    UNSCOPED_INFO("live samples are isolated");
    REQUIRE((has("sampled_isolated = " + std::to_string(sampled)) && has("sampled_concurrent = 0")));
    UNSCOPED_INFO("the swap budget cut the rest");
    REQUIRE(has("swap_budget_skipped = " + std::to_string(selected - sampled)));
    UNSCOPED_INFO("swap range");
    REQUIRE((has("swaps_seen = 1") && has("first_sample_swap = 7") && has("last_sample_swap = 7") && has("total_limit_first_swap = 'none'")));
    UNSCOPED_INFO("only the cursor byte changed");
    REQUIRE((has("changed_offsets = 1") && has("unexpected_offsets = 0") && has("unexpected_offsets_concurrent = 0") && has("offset = 0x0033") &&
             has("isolated_count = " + std::to_string(sampled)) && has("class = 'expected_cursor'")));
    UNSCOPED_INFO("a second close does nothing");
    REQUIRE(CloseDrawEffectProbe(error));
    std::filesystem::remove(path);
}

} // namespace

TEST_CASE("draw_effect_probe", "[ctest][draw-effect-probe][timeout-60]")
{
    testGate();
    testClassification();
    testSnapshotReaders();
    testSampling();
    testTiledSamples();
    testRandomSelection();
    testSwapBudget();
    testDiff();
    testConcurrentSamples();
    testPairing();
    testThreads();
    testLazyBuffers();
    testCommitIsAtomicWithClose();
    testOverlapOrdering();
    testSwapBudgetIsAtomic();
    testThreadExitCountsArmedSample();
    testReport();
    testTiledReport();
    testLiveWrapper();
}
