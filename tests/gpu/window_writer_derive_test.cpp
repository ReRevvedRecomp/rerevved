// window_writer_derive: expected values follow literal g153:12634-13092
// emulation, not formulas.

#include "gpu/guest_d3d/window_writer_derive.h"

#include "window_writer_fixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <vector>

using namespace rerevved::gpu::guest_d3d;
using namespace window_writer_fixture;

namespace
{

WindowWriterEntry entryOf(std::int32_t x0 = 0, std::int32_t y0 = 0, std::int32_t x1 = 1280, std::int32_t y1 = 720)
{
    return { kG, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), static_cast<std::uint32_t>(x1), static_cast<std::uint32_t>(y1) };
}

// A tiled device of `count` tiles stacked side by side, 160 wide each.
Device tiledDevice(std::uint32_t count)
{
    Device device;
    device.tiling    = 0x10;
    device.tileCount = count;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        Tile tile;
        tile.rect   = { static_cast<std::int32_t>(160 * i), 0, static_cast<std::int32_t>(160 * (i + 1)), 720 };
        tile.offset = { static_cast<std::int32_t>(160 * i), 0 };
        device.tiles.push_back(tile);
    }
    return device;
}

std::string describe(const std::string& label, const Device& device, const WindowWriterEntry& entry)
{
    return std::format("{} (cursor {:#x} limit {:#x} tiling {:#x} flags {:#x} tiles {} window {},{} {},{})",
                       label,
                       device.cursor,
                       device.limit38,
                       device.tiling,
                       device.flags,
                       device.tileCount,
                       static_cast<std::int32_t>(entry.x0),
                       static_cast<std::int32_t>(entry.y0),
                       static_cast<std::int32_t>(entry.x1),
                       static_cast<std::int32_t>(entry.y1));
}

// Derives from the content watch and compares with the emulation of Original on the same words.
void agrees(const std::string& label, const Device& device, const WindowWriterEntry& entry)
{
    // Expected window words use literal window_writer_fixture.h emulation, not
    // window_writer_derive.cpp.
    const auto text   = describe(label, device, entry);
    const auto memory = build(device);
    const auto emu    = emulateOriginal(memory, entry);

    WindowWriterWatch watch;
    UNSCOPED_INFO("content watch reads: " + text);
    REQUIRE(ReadWindowWriterWatch(readerOf(memory), kG, true, watch));
    WindowWriterRefusal refusal = WindowWriterRefusal::None;
    std::string         why;
    const auto          exit = DeriveWindowWriterExitState(watch, &refusal, &why);

    // The consume's minimal watch must reach the same verdict.
    WindowWriterWatch minimal;
    UNSCOPED_INFO("minimal watch reads: " + text);
    REQUIRE(ReadWindowWriterWatch(readerOf(memory), kG, false, minimal));
    WindowWriterRefusal minimalRefusal = WindowWriterRefusal::None;
    const auto          minimalExit    = DeriveWindowWriterExitState(minimal, &minimalRefusal);
    UNSCOPED_INFO("the minimal watch decides as the content watch: " + text);
    REQUIRE((static_cast<bool>(exit) == static_cast<bool>(minimalExit) && refusal == minimalRefusal));
    if (exit)
    {
        UNSCOPED_INFO("the minimal watch derives the same exit: " + text);
        REQUIRE((exit->words == minimalExit->words && exit->result == minimalExit->result && exit->cursor == minimalExit->cursor));
    }

    if (emu.flushed)
    {
        UNSCOPED_INFO("a call whose Original flushes is refused as ring_space: " + text + " why " + why);
        REQUIRE((!exit && refusal == WindowWriterRefusal::RingSpace));
        return;
    }
    UNSCOPED_INFO("a call whose Original does not flush is derived: " + text + " why " + why + " refusal " + std::to_string(static_cast<int>(refusal)));
    REQUIRE(static_cast<bool>(exit));
    UNSCOPED_INFO(std::format("r3 {:#x} vs {:#x}: ", exit->result, emu.r3) + text);
    REQUIRE(exit->result == emu.r3);
    const auto stored = emu.memory.at(kG + 0x30);
    UNSCOPED_INFO(std::format("cursor {:#x} vs {:#x}: ", exit->cursor, stored) + text);
    REQUIRE(exit->cursor == stored);
    UNSCOPED_INFO(std::format("words {} vs {}: ", exit->words, emu.ring.size()) + text);
    REQUIRE(exit->words == emu.ring.size());
    UNSCOPED_INFO("the emulation advanced the cursor by its words: " + text);
    REQUIRE(stored == device.cursor + 4 * emu.ring.size());
    const auto words = WindowWriterRingWords(entry, watch, *exit);
    UNSCOPED_INFO("ring words: " + text);
    REQUIRE(words == emu.ring);
    UNSCOPED_INFO("form: " + text);
    REQUIRE(exit->form == (emu.ring.size() == 4 ? WindowWriterForm::Plain : WindowWriterForm::Tiled));

    // Nothing else is stored: the memory differs from the entry only at G+0x30 and the ring words.
    for (const auto& [address, value] : emu.memory)
    {
        const auto found = memory.find(address);
        if (found == memory.end())
        {
            UNSCOPED_INFO(std::format("a store outside the ring words at {:#x}: ", address) + text);
            REQUIRE((address > device.cursor && address <= device.cursor + 4 * emu.ring.size() && address % 4 == device.cursor % 4));
            continue;
        }
        if (found->second != value)
        {
            UNSCOPED_INFO(std::format("a store to a device word at {:#x}: ", address) + text);
            REQUIRE(address == kG + 0x30);
        }
    }
    for (std::size_t i = 0; i < emu.ring.size(); ++i)
    {
        UNSCOPED_INFO("ring word address: " + text);
        REQUIRE(emu.memory.at(device.cursor + 4 * (i + 1)) == words[i]);
    }
}

void testGolden()
{
    // A plain 1280x720 window: 0x22080, 0, y0 << 16 | x0, y1 << 16 | x1; r3 = cursor + 12.
    Device device;
    device.cursor            = kRing + 0x200;
    const auto        memory = build(device);
    WindowWriterWatch watch;
    UNSCOPED_INFO("read");
    REQUIRE(ReadWindowWriterWatch(readerOf(memory), kG, true, watch));
    const auto exit = DeriveWindowWriterExitState(watch);
    UNSCOPED_INFO("plain exit");
    REQUIRE((exit && exit->form == WindowWriterForm::Plain && exit->words == 4 && exit->tiles == 0 && !exit->extra));
    UNSCOPED_INFO("plain result and cursor");
    REQUIRE((exit->result == kRing + 0x200 + 12 && exit->cursor == kRing + 0x200 + 16));
    const std::vector<std::uint32_t> plain{ 0x00022080, 0, 0x00000000, 0x02D00500 };
    UNSCOPED_INFO("plain words, by hand");
    REQUIRE(WindowWriterRingWords(entryOf(), watch, *exit) == plain);
    const std::vector<std::uint32_t> offset{ 0x00022080, 0, (10U << 16) | 20U, (700U << 16) | 1270U };
    UNSCOPED_INFO("plain words with an offset window, by hand");
    REQUIRE(WindowWriterRingWords(entryOf(20, 10, 1270, 700), watch, *exit) == offset);
    // Only the low 14 bits of each field are packed.
    const std::vector<std::uint32_t> wide{ 0x00022080, 0, (0x2345U << 16) | 0x1234U, (0x3FFFU << 16) | 0x3FFFU };
    UNSCOPED_INFO("plain words are masked to 14 bits");
    REQUIRE(WindowWriterRingWords(entryOf(0x5234, 0x6345, 0x7FFF, -1), watch, *exit) == wide);

    // One tile of 640 columns, no offset, no extra block: 13 words, r3 = 3.
    Device tiled;
    tiled.cursor                  = kRing + 0x300;
    tiled.tiling                  = 0x10;
    tiled.tileCount               = 1;
    tiled.tiles                   = { Tile{ { 0, 0, 640, 720 }, { 0, 0 } } };
    tiled.predicateMask           = 0xAAAA;
    tiled.predicateWord           = 0xBBBB;
    const auto        tiledMemory = build(tiled);
    WindowWriterWatch tiledWatch;
    UNSCOPED_INFO("tiled read");
    REQUIRE(ReadWindowWriterWatch(readerOf(tiledMemory), kG, true, tiledWatch));
    const auto tiledExit = DeriveWindowWriterExitState(tiledWatch);
    UNSCOPED_INFO("tiled exit");
    REQUIRE((tiledExit && tiledExit->form == WindowWriterForm::Tiled && tiledExit->tiles == 1 && !tiledExit->extra && tiledExit->words == 13));
    UNSCOPED_INFO("tiled result and cursor");
    REQUIRE((tiledExit->result == 3 && tiledExit->cursor == kRing + 0x300 + 52));
    const std::vector<std::uint32_t> expected{ 0xC0006100, 0, 0xC0006000, 3, 0xC0032D01, 0x00040080, 0, 0x00000000, (720U << 16) | 640U, 0xC0006000, 0xAAAA, 0xC0006100, 0xBBBB };
    UNSCOPED_INFO("tiled words, by hand");
    REQUIRE(WindowWriterRingWords(entryOf(), tiledWatch, *tiledExit) == expected);

    // A tile the window misses is all zero; the offsets are negated into 15 bits.
    tiled.tiles                  = { Tile{ { 700, 0, 800, 720 }, { 5, 6 } } };
    const auto        missMemory = build(tiled);
    WindowWriterWatch missWatch;
    UNSCOPED_INFO("miss read");
    REQUIRE(ReadWindowWriterWatch(readerOf(missMemory), kG, true, missWatch));
    const auto missExit = DeriveWindowWriterExitState(missWatch);
    const auto miss     = WindowWriterRingWords(entryOf(0, 0, 640, 720), missWatch, *missExit);
    UNSCOPED_INFO("negated offsets, by hand");
    REQUIRE((miss[6] == (((0U - 6U) << 16) & 0x7FFF0000U) + ((0U - 5U) & 0x7FFFU) && miss[6] == 0x7FFA7FFB));
    UNSCOPED_INFO("an empty tile is all zero");
    REQUIRE((miss[7] == 0 && miss[8] == 0));
}

void testPlainSweep()
{
    const std::array<std::int32_t, 6> values{ 0, 1, 640, 1280, -5, 40000 };
    Device                            device;
    for (const auto x0 : values)
        for (const auto y1 : values)
            agrees("plain window", device, entryOf(x0, 3, 1279, y1));
    // The form bits that keep a call plain: no bits, or the bound bit with a differing surface.
    device.tiling     = 0x20;
    device.applied[1] = 0x40100200;
    agrees("bound bit, surface differs", device, entryOf());
    device.tiling = 0x40;
    device.flags  = 0x20;
    agrees("extra bits alone are plain", device, entryOf());
    device.tiling = 0xCF;
    agrees("other tiling bits are plain", device, entryOf());
}

void testTiledSweep()
{
    for (std::uint32_t count = 0; count <= kWindowWriterMaxTiles; ++count)
        for (const auto extra : { false, true })
        {
            auto device = tiledDevice(count);
            if (extra)
            {
                device.tiling |= 0x40;
                device.flags = 0x20;
            }
            agrees("tiled, full window", device, entryOf());
            agrees("tiled, partial window", device, entryOf(100, 50, 100 + 160 * count / 2 + 7, 500));
            agrees("tiled, negative window", device, entryOf(-30, -40, 200, 90));
        }
    // The extra block needs both bits.
    for (const auto tiling : { 0x10, 0x50 })
        for (const auto flags : { 0x00, 0x20, 0x1F, 0xDF })
        {
            auto device   = tiledDevice(2);
            device.tiling = static_cast<std::uint8_t>(tiling);
            device.flags  = static_cast<std::uint8_t>(flags);
            agrees("extra block bits", device, entryOf());
            const auto        memory = build(device);
            WindowWriterWatch watch;
            UNSCOPED_INFO("read");
            REQUIRE(ReadWindowWriterWatch(readerOf(memory), kG, false, watch));
            const auto exit = DeriveWindowWriterExitState(watch);
            UNSCOPED_INFO("extra block only with both bits");
            REQUIRE((exit && exit->extra == (tiling == 0x50 && (flags & 0x20) != 0)));
        }
    // The result register: the last tile's 3 << 2(n-1).
    for (std::uint32_t count = 1; count <= kWindowWriterMaxTiles; ++count)
    {
        const auto        memory = build(tiledDevice(count));
        WindowWriterWatch watch;
        UNSCOPED_INFO("read");
        REQUIRE(ReadWindowWriterWatch(readerOf(memory), kG, false, watch));
        UNSCOPED_INFO(std::format("result for {} tiles", count));
        REQUIRE(DeriveWindowWriterExitState(watch)->result == 3U << (2 * (count - 1)));
    }
    // Tiles that overlap, miss and touch.
    auto device          = tiledDevice(3);
    device.tiles[0].rect = { 0, 0, 300, 100 };
    device.tiles[1].rect = { 100, 50, 400, 400 };
    device.tiles[2].rect = { 400, 400, 500, 500 };
    agrees("overlapping tiles", device, entryOf(50, 50, 400, 400));
    agrees("touching window", device, entryOf(300, 100, 400, 400));
    agrees("empty window", device, entryOf(100, 100, 100, 100));
    agrees("inverted window", device, entryOf(300, 300, 200, 200));
}

void testForms()
{
    // The bound form: every pair equal, or its bound surface zero.
    Device device;
    device.tiling    = 0x20;
    device.tileCount = 2;
    device.tiles     = { Tile{}, Tile{} };
    agrees("bound form, all equal", device, entryOf());
    for (std::uint32_t pair = 0; pair < 5; ++pair)
    {
        auto changed          = device;
        changed.applied[pair] = 0x40200000 + pair;
        agrees("bound form, applied differs", changed, entryOf()); // bound is nonzero for pairs 0 and 1 only
        changed.bound[pair] = 0;
        agrees("bound form, nothing bound", changed, entryOf());
        changed.bound[pair]   = 0x40300000;
        changed.applied[pair] = 0;
        agrees("bound form, nothing applied", changed, entryOf());
    }
    // The force bit ignores the pairs.
    device.tiling     = 0x30;
    device.applied[0] = 0x40200000;
    agrees("force bit with differing pairs", device, entryOf());
    // The model's form is the emulation's for every pair state: check the classification itself.
    for (std::uint32_t mask = 0; mask < 32; ++mask)
    {
        Device probe;
        probe.tiling = 0x20;
        for (std::uint32_t i = 0; i < 5; ++i)
        {
            probe.bound[i]   = 0x40100000 + 0x100 * i;
            probe.applied[i] = (mask >> i) & 1 ? 0x40500000 : probe.bound[i];
        }
        const auto        memory = build(probe);
        WindowWriterWatch watch;
        UNSCOPED_INFO("read");
        REQUIRE(ReadWindowWriterWatch(readerOf(memory), kG, false, watch));
        UNSCOPED_INFO("a bound-form call is tiled only when every pair matches");
        REQUIRE(ClassifyWindowWriterForm(watch) == (mask == 0 ? WindowWriterForm::Tiled : WindowWriterForm::Plain));
    }
}

void testRingChecks()
{
    // Plain: only the entry check. The cursor at the limit is fine, one word past is a flush.
    Device device;
    device.cursor = device.limit38;
    agrees("plain, cursor at the limit", device, entryOf());
    device.cursor = device.limit38 + 4;
    agrees("plain, cursor one word past the limit", device, entryOf());
    device.cursor = device.limit38 + 1;
    agrees("plain, cursor one byte past the limit", device, entryOf());
    // The plain tail stores past the limit without a check.
    device.cursor = device.limit38 - 4;
    agrees("plain, words pass the limit", device, entryOf());
    device.cursor = device.limit38 - 4;
    agrees("plain, words pass the limit by one", device, entryOf());

    // Tiled: the check after each tile, none after the tail.
    for (std::uint32_t count = 0; count <= 4; ++count)
        for (const auto extra : { false, true })
        {
            auto base = tiledDevice(count);
            if (extra)
            {
                base.tiling |= 0x40;
                base.flags = 0x20;
            }
            const std::uint32_t afterTiles = 4 * (2 + 7 * count);
            const std::uint32_t total      = 4 * (2 + 7 * count + (extra ? 6 : 0) + 4);
            for (const std::int32_t delta : { -8, -4, -1, 0, 1, 4, 8 })
            {
                for (const auto edge : { afterTiles, total, std::uint32_t{ 0 } })
                {
                    auto device = base;
                    // The limit sits `delta` bytes from the edge above the cursor.
                    device.limit38 = device.cursor + edge + delta;
                    agrees(std::format("tiled, limit {} bytes from {}", delta, edge), device, entryOf());
                }
            }
        }
    // Wider tests of the exact edge: with tiles the limit just at the last tile's cursor is fine.
    {
        auto device              = tiledDevice(3);
        device.limit38           = device.cursor + 4 * (2 + 21);
        const auto        memory = build(device);
        WindowWriterWatch watch;
        UNSCOPED_INFO("read");
        REQUIRE(ReadWindowWriterWatch(readerOf(memory), kG, false, watch));
        UNSCOPED_INFO("a limit at the last tile's cursor is consumed");
        REQUIRE(DeriveWindowWriterExitState(watch).has_value());
        device.limit38 -= 4;
        const auto short_ = build(device);
        UNSCOPED_INFO("read");
        REQUIRE(ReadWindowWriterWatch(readerOf(short_), kG, false, watch));
        WindowWriterRefusal refusal = WindowWriterRefusal::None;
        std::string         why;
        UNSCOPED_INFO("one word short of the last tile's cursor is a flush: " + why);
        REQUIRE((!DeriveWindowWriterExitState(watch, &refusal, &why) && refusal == WindowWriterRefusal::RingSpace && why == "tile_flush"));
    }
    // The entry check names itself.
    {
        Device late;
        late.cursor              = late.limit38 + 4;
        const auto        memory = build(late);
        WindowWriterWatch watch;
        UNSCOPED_INFO("read");
        REQUIRE(ReadWindowWriterWatch(readerOf(memory), kG, false, watch));
        WindowWriterRefusal refusal = WindowWriterRefusal::None;
        std::string         why;
        UNSCOPED_INFO("the entry check: " + why);
        REQUIRE((!DeriveWindowWriterExitState(watch, &refusal, &why) && refusal == WindowWriterRefusal::RingSpace && why == "entry_flush"));
    }
    // An address that would wrap 32 bits is refused with the ring space reason.
    {
        Device wrap;
        wrap.cursor       = 0xFFFFFFF0;
        wrap.limit38      = 0xFFFFFFFC;
        const auto memory = build(wrap);
        const auto emu    = emulateOriginal(memory, entryOf());
        UNSCOPED_INFO("the wrapping call does not flush");
        REQUIRE(!emu.flushed);
        WindowWriterWatch watch;
        UNSCOPED_INFO("read");
        REQUIRE(ReadWindowWriterWatch(readerOf(memory), kG, false, watch));
        WindowWriterRefusal refusal = WindowWriterRefusal::None;
        std::string         why;
        UNSCOPED_INFO("a wrap is refused: " + why);
        REQUIRE((!DeriveWindowWriterExitState(watch, &refusal, &why) && refusal == WindowWriterRefusal::RingSpace && why == "address_wrap"));
        wrap.cursor     = 0xFFFFFFEC; // the last word ends at 0xFFFFFFFC
        const auto fits = build(wrap);
        UNSCOPED_INFO("read");
        REQUIRE(ReadWindowWriterWatch(readerOf(fits), kG, false, watch));
        UNSCOPED_INFO("the last address that fits is consumed");
        REQUIRE(DeriveWindowWriterExitState(watch).has_value());
    }
}

void testTileCount()
{
    // Sixteen tiles are modeled, seventeen are not.
    agrees("sixteen tiles", tiledDevice(16), entryOf());
    auto device              = tiledDevice(16);
    device.tileCount         = 17;
    const auto        memory = build(device);
    WindowWriterWatch watch;
    UNSCOPED_INFO("a content read of too many tiles reads");
    REQUIRE(ReadWindowWriterWatch(readerOf(memory), kG, true, watch));
    WindowWriterRefusal refusal = WindowWriterRefusal::None;
    std::string         why;
    UNSCOPED_INFO("tile count refusal");
    REQUIRE((!DeriveWindowWriterExitState(watch, &refusal, &why) && refusal == WindowWriterRefusal::TileCount && why == "tile_count"));
    device.tileCount = 0xFFFFFFFF;
    const auto huge  = build(device);
    UNSCOPED_INFO("read");
    REQUIRE(ReadWindowWriterWatch(readerOf(huge), kG, false, watch));
    UNSCOPED_INFO("an unbounded count is refused");
    REQUIRE((!DeriveWindowWriterExitState(watch, &refusal) && refusal == WindowWriterRefusal::TileCount));
    // The entry flush is tested before the tile count.
    device.cursor   = device.limit38 + 4;
    const auto both = build(device);
    UNSCOPED_INFO("read");
    REQUIRE(ReadWindowWriterWatch(readerOf(both), kG, false, watch));
    UNSCOPED_INFO("the entry check precedes the tile count");
    REQUIRE((!DeriveWindowWriterExitState(watch, &refusal) && refusal == WindowWriterRefusal::RingSpace));
    // A plain call ignores the tile count.
    Device plain;
    plain.tileCount = 0xFFFFFFFF;
    agrees("plain with a wild tile count", plain, entryOf());
}

class Lcg
{
public:
    std::uint32_t next()
    {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    }

    std::int32_t window()
    {
        const auto kind = next() % 4;
        if (kind == 0)
            return static_cast<std::int32_t>(next() % 1400);
        if (kind == 1)
            return -static_cast<std::int32_t>(next() % 200);
        if (kind == 2)
            return static_cast<std::int32_t>(next() % 70000) - 35000;
        return static_cast<std::int32_t>(next() % 16);
    }

private:
    std::uint32_t state = 12345;
};

void testRandom()
{
    Lcg rng;
    for (int i = 0; i < 4000; ++i)
    {
        Device device;
        device.cursor   = kRing + 4 * (rng.next() % 0x400);
        device.limit38  = device.cursor + (rng.next() % 3 == 0 ? 4 * (rng.next() % 200) : 0x10000);
        const auto kind = rng.next() % 5;
        device.tiling   = kind == 0 ? 0x00 : kind == 1 ? 0x10
                                         : kind == 2   ? 0x20
                                         : kind == 3   ? 0x30
                                                       : static_cast<std::uint8_t>(rng.next());
        device.flags    = static_cast<std::uint8_t>(rng.next() % 2 ? 0x20U : rng.next());
        if (device.tiling & 0x20)
            for (std::uint32_t k = 0; k < 5; ++k)
            {
                device.bound[k]   = rng.next() % 3 == 0 ? 0 : 0x40100000 + 0x100 * k;
                device.applied[k] = rng.next() % 4 == 0 ? 0x40900000 : device.bound[k];
            }
        device.tileCount     = rng.next() % 5 == 0 ? 0 : 1 + rng.next() % kWindowWriterMaxTiles;
        device.predicateMask = rng.next();
        device.predicateWord = rng.next();
        for (std::uint32_t t = 0; t < std::max<std::uint32_t>(device.tileCount, 1); ++t)
        {
            Tile tile;
            tile.rect   = { rng.window(), rng.window(), rng.window(), rng.window() };
            tile.offset = { rng.window(), rng.window() };
            device.tiles.push_back(tile);
        }
        agrees(std::format("random {}", i), device, { kG, static_cast<std::uint32_t>(rng.window()), static_cast<std::uint32_t>(rng.window()), static_cast<std::uint32_t>(rng.window()), static_cast<std::uint32_t>(rng.window()) });
    }
}

// A reader that records the addresses it was asked for and refuses some.
struct Probe
{
    const Memory&           memory;
    std::set<std::uint32_t> asked;
    std::set<std::uint32_t> refused;

    GuestWordReader reader()
    {
        return [this](std::uint32_t address, std::uint32_t& value)
        {
            asked.insert(address);
            if (refused.contains(address))
                return false;
            const auto found = memory.find(address);
            if (found == memory.end())
                return false;
            value = found->second;
            return true;
        };
    }
};

void testRead()
{
    // A plain call reads the cursor, the limit and the mode word, nothing else.
    {
        Device device;
        device.tileCount         = 3;
        const auto        memory = build(device);
        Probe             probe{ memory };
        WindowWriterWatch watch;
        UNSCOPED_INFO("plain read");
        REQUIRE(ReadWindowWriterWatch(probe.reader(), kG, false, watch));
        UNSCOPED_INFO("a plain call reads three words");
        REQUIRE(probe.asked == std::set<std::uint32_t>({ kG + 0x30, kG + 0x38, kG + 0x2ABC }));
        UNSCOPED_INFO("plain watch");
        REQUIRE((watch.cursor == device.cursor && watch.limit38 == device.limit38 && watch.TilingByte() == 0 && watch.limit34 == 0));
        Probe content{ memory };
        UNSCOPED_INFO("plain content read");
        REQUIRE(ReadWindowWriterWatch(content.reader(), kG, true, watch));
        UNSCOPED_INFO("a plain content read adds the hard limit");
        REQUIRE(content.asked == std::set<std::uint32_t>({ kG + 0x30, kG + 0x34, kG + 0x38, kG + 0x2ABC }));
        UNSCOPED_INFO("hard limit");
        REQUIRE(watch.limit34 == device.limit34);
    }
    // A bound-form call reads the ten pair words; the force bit does not.
    {
        Device device;
        device.tiling            = 0x20;
        const auto        memory = build(device);
        Probe             probe{ memory };
        WindowWriterWatch watch;
        UNSCOPED_INFO("bound read");
        REQUIRE(ReadWindowWriterWatch(probe.reader(), kG, false, watch));
        UNSCOPED_INFO("bound form: pairs and the tile count");
        REQUIRE((probe.asked.size() == 3 + 10 + 1 && probe.asked.contains(kG + 0x31CC) && probe.asked.contains(kG + 0x3098) && probe.asked.contains(kG + 0x31C8)));
        device.tiling     = 0x30;
        const auto forced = build(device);
        Probe      forcedProbe{ forced };
        UNSCOPED_INFO("forced read");
        REQUIRE(ReadWindowWriterWatch(forcedProbe.reader(), kG, false, watch));
        UNSCOPED_INFO("the force bit skips the pairs");
        REQUIRE(forcedProbe.asked == std::set<std::uint32_t>({ kG + 0x30, kG + 0x38, kG + 0x2ABC, kG + 0x31CC }));
    }
    // A content read of a tiled call reads the predicate words and the tile arrays.
    {
        auto              device = tiledDevice(3);
        const auto        memory = build(device);
        Probe             probe{ memory };
        WindowWriterWatch watch;
        UNSCOPED_INFO("tiled content read");
        REQUIRE(ReadWindowWriterWatch(probe.reader(), kG, true, watch));
        UNSCOPED_INFO("tiles are read");
        REQUIRE((probe.asked.contains(kG + 0x31A4) && probe.asked.contains(kG + 0x31A8) && probe.asked.contains(kG + 0x31D0 + 16 * 2 + 12) && probe.asked.contains(kG + 0x32C0 + 8 * 2 + 4)));
        UNSCOPED_INFO("the tile after the last is not");
        REQUIRE(!probe.asked.contains(kG + 0x31D0 + 16 * 3));
        UNSCOPED_INFO("tile values");
        REQUIRE((watch.tiles[2].rect[2] == 480 && watch.tiles[2].offset[0] == 320));
        Probe minimal{ memory };
        UNSCOPED_INFO("tiled minimal read");
        REQUIRE(ReadWindowWriterWatch(minimal.reader(), kG, false, watch));
        UNSCOPED_INFO("the consume's read skips the content");
        REQUIRE((!minimal.asked.contains(kG + 0x31A4) && !minimal.asked.contains(kG + 0x31D0)));
    }
    // Each word the control flow reads is required; the others are not.
    {
        auto device       = tiledDevice(2);
        device.flags      = 0x20;
        device.tiling     = 0x50;
        const auto memory = build(device);
        for (const auto address : { kG + 0x30, kG + 0x38, kG + 0x2ABC, kG + 0x31CC })
        {
            Probe probe{ memory };
            probe.refused.insert(address);
            WindowWriterWatch watch;
            UNSCOPED_INFO(std::format("unreadable {:#x} fails", address));
            REQUIRE(!ReadWindowWriterWatch(probe.reader(), kG, false, watch));
        }
        for (const auto address : { kG + 0x34, kG + 0x31A4, kG + 0x31A8, kG + 0x31D0, kG + 0x32C4 })
        {
            Probe probe{ memory };
            probe.refused.insert(address);
            WindowWriterWatch watch;
            UNSCOPED_INFO(std::format("unreadable {:#x} does not matter to the consume", address));
            REQUIRE(ReadWindowWriterWatch(probe.reader(), kG, false, watch));
            Probe content{ memory };
            content.refused.insert(address);
            UNSCOPED_INFO(std::format("unreadable {:#x} fails a content read", address));
            REQUIRE(!ReadWindowWriterWatch(content.reader(), kG, true, watch));
        }
        Device bound;
        bound.tiling           = 0x20;
        const auto boundMemory = build(bound);
        for (const auto address : { kG + 0x3098, kG + 0x31B8, kG + 0x30A8, kG + 0x31C8 })
        {
            Probe probe{ boundMemory };
            probe.refused.insert(address);
            WindowWriterWatch watch;
            UNSCOPED_INFO(std::format("unreadable pair word {:#x} fails", address));
            REQUIRE(!ReadWindowWriterWatch(probe.reader(), kG, false, watch));
        }
        // A plain call does not need the pair words or the count.
        Device plain;
        auto   plainMemory = build(plain);
        plainMemory.erase(kG + 0x3098);
        plainMemory.erase(kG + 0x31CC);
        WindowWriterWatch watch;
        UNSCOPED_INFO("a plain call needs three words");
        REQUIRE(ReadWindowWriterWatch(readerOf(plainMemory), kG, false, watch));
    }
    // The device span must not wrap.
    {
        Device            device;
        const auto        memory = build(device);
        WindowWriterWatch watch;
        UNSCOPED_INFO("a device that wraps its span is unreadable");
        REQUIRE(!ReadWindowWriterWatch(readerOf(memory), 0xFFFFFFFF - kWindowWriterDeviceSpan + 1, false, watch));
        UNSCOPED_INFO("a device at the top of memory is unreadable");
        REQUIRE(!ReadWindowWriterWatch(readerOf(memory), 0xFFFFFFF0, false, watch));
    }
}

#if REREVVED_DEVELOPER
void testMismatches()
{
    auto device              = tiledDevice(2);
    device.tiling            = 0x50;
    device.flags             = 0x20;
    const auto        memory = build(device);
    const auto        entry  = entryOf();
    WindowWriterWatch before;
    UNSCOPED_INFO("read");
    REQUIRE(ReadWindowWriterWatch(readerOf(memory), kG, true, before));
    const auto predicted = *DeriveWindowWriterExitState(before);
    const auto expected  = WindowWriterRingWords(entry, before, predicted);
    auto       after     = before;
    after.cursor         = predicted.cursor;
    const auto flag      = [](std::size_t word)
    {
        return 1U << word;
    };
    UNSCOPED_INFO("an agreeing exit has no mismatch");
    REQUIRE(WindowWriterExitMismatches(predicted, before, after, predicted.result, expected, expected) == 0);
    UNSCOPED_INFO("result");
    REQUIRE(WindowWriterExitMismatches(predicted, before, after, predicted.result + 1, expected, expected) == flag(0));
    UNSCOPED_INFO("a high bit of r3 is a result mismatch");
    REQUIRE(WindowWriterExitMismatches(predicted, before, after, predicted.result | (1ULL << 32), expected, expected) == flag(0));
    auto moved   = after;
    moved.cursor = predicted.cursor + 4;
    UNSCOPED_INFO("cursor");
    REQUIRE(WindowWriterExitMismatches(predicted, before, moved, predicted.result, expected, expected) == flag(1));
    moved = after;
    moved.limit34 += 4;
    UNSCOPED_INFO("limit 34");
    REQUIRE(WindowWriterExitMismatches(predicted, before, moved, predicted.result, expected, expected) == flag(2));
    moved = after;
    moved.limit38 += 4;
    UNSCOPED_INFO("limit 38");
    REQUIRE(WindowWriterExitMismatches(predicted, before, moved, predicted.result, expected, expected) == flag(3));
    // Any other watched word is the state word.
    const auto stateChange = [&](auto&& edit)
    {
        auto changed = after;
        edit(changed);
        return WindowWriterExitMismatches(predicted, before, changed, predicted.result, expected, expected);
    };
    UNSCOPED_INFO("mode");
    REQUIRE(stateChange([](WindowWriterWatch& w)
                        {
                            w.mode ^= 0x01;
                        }) == flag(4));
    UNSCOPED_INFO("bound");
    REQUIRE(stateChange([](WindowWriterWatch& w)
                        {
                            w.bound[2] ^= 1;
                        }) == flag(4));
    UNSCOPED_INFO("applied");
    REQUIRE(stateChange([](WindowWriterWatch& w)
                        {
                            w.applied[4] ^= 1;
                        }) == flag(4));
    UNSCOPED_INFO("tile count");
    REQUIRE(stateChange([](WindowWriterWatch& w)
                        {
                            w.tileCount += 1;
                        }) == flag(4));
    UNSCOPED_INFO("predicate mask");
    REQUIRE(stateChange([](WindowWriterWatch& w)
                        {
                            w.predicateMask ^= 1;
                        }) == flag(4));
    UNSCOPED_INFO("predicate word");
    REQUIRE(stateChange([](WindowWriterWatch& w)
                        {
                            w.predicateWord ^= 1;
                        }) == flag(4));
    UNSCOPED_INFO("a tile word");
    REQUIRE(stateChange([](WindowWriterWatch& w)
                        {
                            w.tiles[1].offset[1] ^= 1;
                        }) == flag(4));
    UNSCOPED_INFO("a tile past the count is not compared");
    REQUIRE(stateChange([](WindowWriterWatch& w)
                        {
                            w.tiles[2].offset[1] ^= 1;
                        }) == 0);
    auto observed = expected;
    observed[5] ^= 1;
    UNSCOPED_INFO("ring content");
    REQUIRE(WindowWriterExitMismatches(predicted, before, after, predicted.result, expected, observed) == flag(5));
    observed = expected;
    observed.pop_back();
    UNSCOPED_INFO("ring content of a different length");
    REQUIRE(WindowWriterExitMismatches(predicted, before, after, predicted.result, expected, observed) == flag(5));
    UNSCOPED_INFO("several words");
    REQUIRE(WindowWriterExitMismatches(predicted, before, moved, predicted.result + 1, expected, observed) == (flag(0) | flag(3) | flag(5)));
    UNSCOPED_INFO("word names");
    REQUIRE((kWindowWriterExitWordNames.size() == kWindowWriterExitWordCount && kWindowWriterExitWordCount == 6));

    WindowWriterWatch entryWatch, exitWatch;
    entryWatch.cursor = 0x1000;
    exitWatch.cursor  = 0x1030;
    UNSCOPED_INFO("observed words");
    REQUIRE(WindowWriterObservedWords(entryWatch, exitWatch) == 12);
    exitWatch.cursor = 0x0FFC;
    UNSCOPED_INFO("a cursor that moved back counts nothing");
    REQUIRE(WindowWriterObservedWords(entryWatch, exitWatch) == 0);
    exitWatch.cursor = 0x1006;
    UNSCOPED_INFO("a partial word counts nothing");
    REQUIRE(WindowWriterObservedWords(entryWatch, exitWatch) == 0);
}

void testCounters()
{
    (void)TakeWindowWriterExitCounts();
    NoteWindowWriterExit(0, false, 4);
    NoteWindowWriterExit(0, true, 40);
    NoteWindowWriterExit((1U << 1) | (1U << 5), true, 3);
    NoteWindowWriterExitUnmodeled(WindowWriterRefusal::RingSpace);
    NoteWindowWriterExitUnmodeled(WindowWriterRefusal::RingSpace);
    NoteWindowWriterExitUnmodeled(WindowWriterRefusal::TileCount);
    NoteWindowWriterExitUnreadable();
    const auto counts = TakeWindowWriterExitCounts();
    UNSCOPED_INFO("tallies");
    REQUIRE((counts.ok == 2 && counts.tiled == 2 && counts.tiledOk == 1 && counts.unreadable == 1 && counts.calls == 3));
    UNSCOPED_INFO("mismatch words");
    REQUIRE((counts.mismatch[1] == 1 && counts.mismatch[5] == 1 && counts.mismatch[0] == 0));
    UNSCOPED_INFO("ring words");
    REQUIRE((counts.ringWordsMax == 40 && counts.ringWordsSum == 47));
    UNSCOPED_INFO("unmodeled");
    REQUIRE((counts.unmodeled[static_cast<std::size_t>(WindowWriterRefusal::RingSpace)] == 2 && counts.unmodeled[static_cast<std::size_t>(WindowWriterRefusal::TileCount)] == 1));
    const auto line = FormatWindowWriterExitCounts(counts);
    UNSCOPED_INFO("ok fields: " + line);
    REQUIRE((line.find(" window_writer_exit_ok=2 ") != std::string::npos && line.find(" window_writer_exit_tiled_ok=1 ") != std::string::npos));
    UNSCOPED_INFO("ring word fields: " + line);
    REQUIRE(line.find(" window_writer_exit_ring_words_max=40 window_writer_exit_ring_words_sum=47 window_writer_exit_calls=3") != std::string::npos);
    UNSCOPED_INFO("mismatch fields: " + line);
    REQUIRE((line.find(" window_writer_exit_mismatch_cursor=1") != std::string::npos && line.find(" window_writer_exit_mismatch_ring_content=1") != std::string::npos));
    UNSCOPED_INFO("every mismatch field is printed: " + line);
    REQUIRE(line.find(" window_writer_exit_mismatch_result=0") != std::string::npos);
    UNSCOPED_INFO("unmodeled fields: " + line);
    REQUIRE((line.find(" window_writer_exit_unmodeled_ring_space=2") != std::string::npos && line.find(" window_writer_exit_unmodeled_tile_count=1") != std::string::npos));
    UNSCOPED_INFO("the none reason is not printed: " + line);
    REQUIRE(line.find("window_writer_exit_unmodeled_none") == std::string::npos);
    const auto empty = TakeWindowWriterExitCounts();
    UNSCOPED_INFO("a take resets the counts");
    REQUIRE((empty.ok == 0 && empty.calls == 0 && empty.ringWordsSum == 0));
    UNSCOPED_INFO("a zero unmodeled count is not printed");
    REQUIRE(FormatWindowWriterExitCounts(empty).find("unmodeled") == std::string::npos);
    UNSCOPED_INFO("refusal names");
    REQUIRE(kWindowWriterRefusalNames.size() == kWindowWriterRefusalCount);
}
#endif

void testEntry()
{
    UNSCOPED_INFO("the probe is off by default");
    REQUIRE(!WindowWriterExitCheckRequested());
    UNSCOPED_INFO("the entry address");
    REQUIRE((WindowWriterConsumeEntry(0x8268D3A0) && !WindowWriterConsumeEntry(0x8268D3A4)));
}

} // namespace

TEST_CASE("window_writer_derive", "[ctest][window-writer-derive][timeout-60]")
{
    testGolden();
    testPlainSweep();
    testTiledSweep();
    testForms();
    testRingChecks();
    testTileCount();
    testRandom();
    testRead();
#if REREVVED_DEVELOPER
    testMismatches();
    testCounters();
#endif
    testEntry();
}
