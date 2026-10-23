// clear_effect: expected dirty words are literal generated stores, not derive formulas
// (g158:13166-13924; g88:13109-13389; g96:13268-13306).

#include "gpu/guest_d3d/clear_derive.h"

#include "developer_checks.hpp"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint64_t kAll = ~0ULL;

constexpr std::uint32_t kG = 0x40010000, kRect = 0x40021000, kColor = 0x40022000, kSurface = 0x40030000, kDepthSurface = 0x40031000, kSp = 0x70000000;

using Memory = std::unordered_map<std::uint32_t, std::uint32_t>;
using Rect   = std::array<std::int32_t, 4>;

struct Scene
{
    std::uint32_t                flags      = 1;
    Rect                         rect       = { 0, 0, 64, 8 };
    std::array<bool, 4>          rt         = { true, false, false, false };
    bool                         depthBound = true;
    std::uint32_t                tileByte   = 0;              // G+0x2ABC
    std::uint32_t                tileFlags  = 0;              // G+0x2ABF
    std::uint32_t                tileMask   = 0;              // G+0x31A4
    std::vector<Rect>            tiles;                       // G+0x31D0 + 16 i, G+0x31CC their count
    std::uint32_t                tileCount      = 0xFFFFFFFF; // 0xFFFFFFFF: the number of tiles
    bool                         savedFromBound = false;
    std::array<std::uint32_t, 5> saved{};
    std::uint32_t                control = 0; // G+0x2880
    std::uint32_t                format  = 0; // (surface+0x1C >> 16) & 0xF of every color target
};

Memory build(const Scene& scene)
{
    Memory memory;
    for (std::uint32_t k = 0; k < 4; ++k)
    {
        memory[kG + 0x3098 + 4 * k]         = scene.rt[k] ? kSurface + 0x100 * k : 0;
        memory[kSurface + 0x100 * k + 0x1C] = scene.format << 16;
    }
    memory[kG + 0x30A8] = scene.depthBound ? kDepthSurface : 0;
    for (std::uint32_t i = 0; i < 5; ++i)
        memory[kG + 0x31B8 + 4 * i] = scene.savedFromBound ? memory[kG + 0x3098 + 4 * i] : scene.saved[i];
    if (scene.savedFromBound)
        for (std::uint32_t i = 0; i < 5; ++i)
            if (scene.saved[i])
                memory[kG + 0x31B8 + 4 * i] = scene.saved[i];
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        memory[kG + 0x3168 + 4 * i] = std::bit_cast<std::uint32_t>(i < 2 ? 0.0f : 4096.0f);
        memory[kG + 0x3184 + 4 * i] = 0;
        memory[kColor + 4 * i]      = 0x3F800000;
        memory[kRect + 4 * i]       = static_cast<std::uint32_t>(scene.rect[i]);
    }
    memory[kG + 0x2E50] = 0;
    memory[kG + 0x2880] = scene.control;
    memory[kG + 0x2ABC] = (scene.tileByte << 24) | scene.tileFlags;
    memory[kG + 0x31A4] = scene.tileMask;
    memory[kG + 0x31CC] = scene.tileCount == 0xFFFFFFFF ? static_cast<std::uint32_t>(scene.tiles.size()) : scene.tileCount;
    for (std::size_t i = 0; i < scene.tiles.size(); ++i)
        for (std::uint32_t j = 0; j < 4; ++j)
            memory[kG + 0x31D0 + 16 * static_cast<std::uint32_t>(i) + 4 * j] = static_cast<std::uint32_t>(scene.tiles[i][j]);
    return memory;
}

GuestWordReader readerOf(const Memory& memory)
{
    return [&memory](std::uint32_t address, std::uint32_t& value)
    {
        const auto found = memory.find(address);
        if (found == memory.end())
            return false;
        value = found->second;
        return true;
    };
}

ClearEntry entryOf(const Scene& scene)
{
    ClearEntry entry;
    entry.sp      = kSp;
    entry.g       = kG;
    entry.flags   = scene.flags;
    entry.rect    = kRect;
    entry.color   = kColor;
    entry.depth   = 1.0;
    entry.stencil = 0;
    return entry;
}

struct Expected
{
    const char* name;
    Scene       scene;
    ClearWatch  entry; // dirty words and G+0x2ABD
    ClearWatch  exit;  // the words the generated sequences leave
    // reachedFlush, then calls, stencil, fast, untiled, block, loop, none, allocates
    bool        reached;
    std::size_t calls, stencil, fast, untiled, block, loop, none;
    bool        allocates;
};

ClearWatch watch(std::uint64_t d10, std::uint64_t d18, std::uint64_t d20, std::uint8_t byte)
{
    ClearWatch w;
    w.dirty     = { d10, d18, d20 };
    w.allocByte = byte;
    return w;
}

// Expected words emulate Clear wrapper 0x826A2970, body 0x826A22F8 and callees
// 0x826A1D90, 0x826A1AF0, 0x826A1800, independently of clear_derive.cpp masks.
std::vector<Expected> cases()
{
    std::vector<Expected> table;
    const auto            add = [&](const char* name, Scene scene, ClearWatch entry, ClearWatch exit, bool reached, std::size_t calls, std::size_t stencil, std::size_t fast, std::size_t untiled, std::size_t block, std::size_t loop, std::size_t none, bool allocates)
    {
        table.push_back({ name, std::move(scene), entry, exit, reached, calls, stencil, fast, untiled, block, loop, none, allocates });
    };
    constexpr std::uint64_t kPattern10 = 0xA5C30F0F5A5E3F0FULL, kPattern18 = 0xFEDCBA9876543210ULL, kPattern20 = 0x0F0F0F0F0F0F0F0FULL;

    // One color target, 64x8 (below every fast threshold), all dirty bits set at the entry.
    add("untiled_color_ones", Scene{}, watch(kAll, kAll, kAll, 0x00), watch(0xFF0013A0001E0EE8ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFD000080000ULL, 0x04), true, 1, 0, 0, 1, 0, 0, 0, true);
    add("untiled_color_zero", Scene{}, watch(0, 0, 0, 0x00), watch(0x030013A0001A0EE8ULL, 0x0000000880000000ULL, 0x0000001000080000ULL, 0x04), true, 1, 0, 0, 1, 0, 0, 0, true);
    // Bits 58..63 and 17..20 of G+0x10 and the bits outside 34..54 of G+0x18 and below 38 of G+0x20 survive.
    add("untiled_color_pattern", Scene{}, watch(kPattern10, kPattern18, kPattern20, 0x18), watch(0xA70013A0001E0EE8ULL, 0xFE800008F6543210ULL, 0x0F0F0F1000080000ULL, 0x1C), true, 1, 0, 0, 1, 0, 0, 0, true);
    {
        // Two color targets with the stencil flag: the first call keeps 0x23 and ORs
        // 0x10000000, the second has the depth bits dropped and takes the fast path.
        Scene scene;
        scene.flags = 0x23;
        scene.rect  = { 0, 0, 640, 480 };
        scene.rt    = { true, true, false, false };
        add("stencil_then_fast", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFF8013A0101E0FE8ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFD000080000ULL, 0x04), true, 2, 1, 1, 2, 0, 0, 0, true);
    }
    {
        // The stencil flag alone (no fast path to share its OR) and a color flag other than slot 0.
        Scene scene;
        scene.flags = 0x21;
        add("stencil_only", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFF0013A0101E0EE8ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFD000080000ULL, 0x04), true, 1, 1, 0, 1, 0, 0, 0, true);
        Scene slot1;
        slot1.flags = 0x2;
        slot1.rt    = { false, true, false, false };
        add("color_slot1", slot1, watch(0, 0, 0, 0x00), watch(0x030013A0001A0EE8ULL, 0x0000000880000000ULL, 0x0000001000080000ULL, 0x04), true, 1, 0, 0, 1, 0, 0, 0, true);
    }
    {
        Scene scene;
        scene.flags   = 0x1;
        scene.rect    = { 0, 0, 40, 8 };
        scene.control = 2U << 16;
        add("fast_color_40x8", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFF8013A0101E0FE8ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFD000080000ULL, 0x04), true, 1, 0, 1, 1, 0, 0, 0, true);
    }
    {
        // The depth-only call (index -1) with the flag 0x40: no color branch, format zero.
        Scene scene;
        scene.flags = 0x40;
        scene.rect  = { 0, 0, 640, 480 };
        add("depth_only_fast", scene, watch(kAll, 0, 0, 0x40), watch(0xFF801380101E09E8ULL, 0x0000000880000000ULL, 0x0000001000080000ULL, 0x44), true, 1, 0, 1, 1, 0, 0, 0, true);
        scene.rect = { 0, 0, 64, 8 };
        add("depth_only_plain", scene, watch(0x10, 0x10, 0x10, 0x00), watch(0x03001380001A08E8ULL, 0x0000000880000010ULL, 0x0000001000080000ULL, 0x04), true, 1, 0, 0, 1, 0, 0, 0, true);
    }
    {
        // Color flag with no bound slot: no call, but the flush and the ORs still happen.
        Scene scene;
        scene.rt = { false, false, false, false };
        add("flush_no_calls", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFC0013A0001E06E0ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFC000080000ULL, 0x04), true, 0, 0, 0, 0, 0, 0, 0, false);
    }
    {
        // Early exits write nothing.
        Scene scene;
        scene.flags      = 0x10;
        scene.depthBound = false;
        add("early_no_depth_surface", scene, watch(kAll, kAll, kAll, 0x21), watch(kAll, kAll, kAll, 0x21), false, 0, 0, 0, 0, 0, 0, 0, false);
        Scene empty;
        empty.rect = { 10, 10, 10, 20 };
        add("early_empty_rectangle", empty, watch(kAll, kAll, kAll, 0x21), watch(kAll, kAll, kAll, 0x21), false, 0, 0, 0, 0, 0, 0, 0, false);
    }
    // A marker already set at the entry fails every allocation: the post-allocation ORs are missing.
    add("marker_at_entry", Scene{}, watch(kAll, kAll, kAll, 0x20), watch(0xFC0013A0001E06E0ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFC000080000ULL, 0x24), true, 1, 0, 0, 1, 0, 0, 0, true);
    {
        // Tiled by the byte 0x10 with an empty tile table: the call allocates nothing.
        Scene scene;
        scene.tileByte = 0x10;
        add("tiled_none", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFC0013A0001E06E0ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFC000080000ULL, 0x04), true, 1, 0, 0, 0, 0, 0, 1, false);
    }
    {
        // Block A (G+0x2ABF & 0x20, G+0x2ABC & 0x40, G+0x31A4 & 0x15555555) calls once for flags & 0x30.
        Scene scene;
        scene.flags     = 0x11;
        scene.tileByte  = 0x50;
        scene.tileFlags = 0x20;
        scene.tileMask  = 0x1;
        add("tiled_block_reach", scene, watch(kPattern10, kPattern18, kPattern20, 0x00), watch(0xA70013A0001E0EE8ULL, 0xFE800008F6543210ULL, 0x0F0F0F1000080000ULL, 0x04), true, 1, 0, 0, 0, 1, 0, 0, true);
        // g88 reloads G+0x31A4 on each call: two targets both visit block A, but only the first keeps the depth flags and so allocates. The exit words are those of one color branch and one allocation.
        Scene two = scene;
        two.flags = 0x13;
        two.rt    = { true, true, false, false };
        add("tiled_block_two_targets", two, watch(0, 0, 0, 0x00), watch(0x030013A0001A0EE8ULL, 0x0000000880000000ULL, 0x0000001000080000ULL, 0x04), true, 2, 0, 0, 0, 2, 0, 1, true);
        // Without flags & 0x30 block A is entered and calls nothing.
        scene.flags = 0x1;
        add("tiled_block_without_depth_flags", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFC0013A0001E06E0ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFC000080000ULL, 0x04), true, 1, 0, 0, 0, 1, 0, 1, false);
        // Block A clears the mask bits it used, so the tile loop that follows sees none of them.
        scene.tiles = { { 0, 0, 64, 64 } };
        add("tiled_block_then_loop", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFC0013A0001E06E0ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFC000080000ULL, 0x04), true, 1, 0, 0, 0, 1, 0, 1, false);
        scene.tiles.clear();
        // Block A needs G+0x2ABC & 0x40 as well.
        scene.flags    = 0x11;
        scene.tileByte = 0x10;
        add("tiled_block_without_0x40", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFC0013A0001E06E0ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFC000080000ULL, 0x04), true, 1, 0, 0, 0, 0, 0, 1, false);
        scene.tileByte = 0x50;
        // Bit 30 is outside 0x15555555 (it is inside 0x55555555): block A is not entered.
        scene.flags    = 0x11;
        scene.tileMask = 0x40000000;
        add("tiled_block_mask_bit30", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFC0013A0001E06E0ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFC000080000ULL, 0x04), true, 1, 0, 0, 0, 0, 0, 1, false);
    }
    {
        // The tile loop: tile 1 (100..200) intersects the clipped rectangle and its mask bits (3 << 2) hit G+0x31A4.
        Scene scene;
        scene.rect     = { 120, 0, 180, 50 };
        scene.tileByte = 0x10;
        scene.tiles    = { { 0, 0, 100, 100 }, { 100, 0, 200, 100 } };
        scene.tileMask = 0x4;
        add("tiled_loop_reach", scene, watch(0, kAll, 0, 0x00), watch(0x030013A0001A0EE8ULL, 0xFF80000BFFFFFFFFULL, 0x0000001000080000ULL, 0x04), true, 1, 0, 0, 0, 0, 1, 0, true);
        // Tile 0's bits are not in the mask and tile 1's... are not either.
        scene.tileMask = 0x1;
        add("tiled_loop_mask_miss", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFC0013A0001E06E0ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFC000080000ULL, 0x04), true, 1, 0, 0, 0, 0, 0, 1, false);
        // The rectangle misses the only tile whose mask bits hit.
        scene.tileMask = 0x4;
        scene.rect     = { 10, 0, 80, 50 };
        add("tiled_loop_rect_miss", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFC0013A0001E06E0ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFC000080000ULL, 0x04), true, 1, 0, 0, 0, 0, 0, 1, false);
    }
    {
        // Byte 0x20 tiles when each bound slot is the saved one or unbound (here slot 1 differs and is unbound).
        Scene scene;
        scene.rect           = { 0, 0, 64, 8 };
        scene.tileByte       = 0x20;
        scene.savedFromBound = true;
        scene.saved          = { 0, 0x1234, 0, 0, 0 };
        scene.tiles          = { { 0, 0, 64, 64 } };
        scene.tileMask       = 0x1;
        add("tiled_saved_reach", scene, watch(kAll, 0, kAll, 0x08), watch(0xFF0013A0001E0EE8ULL, 0x0000000880000000ULL, 0xFFFFFFD000080000ULL, 0x0C), true, 1, 0, 0, 0, 0, 1, 0, true);
        // A bound slot that is not the saved one: untiled, one allocation.
        scene.saved = { kSurface + 4, 0, 0, 0, 0 };
        add("untiled_saved_mismatch", scene, watch(kAll, kAll, kAll, 0x00), watch(0xFF0013A0001E0EE8ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFD000080000ULL, 0x04), true, 1, 0, 0, 1, 0, 0, 0, true);
    }
    return table;
}

Expected named(const std::vector<Expected>& table, const std::string& name)
{
    for (const auto& item : table)
        if (name == item.name)
            return item;
    throw std::runtime_error("no case " + name);
}

struct Derived
{
    ClearCalls   calls;
    ClearEffects effects;
};

Derived deriveOf(const Expected& item, const Memory& memory)
{
    std::string why;
    const auto  reader = readerOf(memory);
    const auto  entry  = entryOf(item.scene);
    auto        calls  = DeriveClearCalls(reader, entry, &why);
    UNSCOPED_INFO(std::string(item.name) + ": calls: " + why);
    REQUIRE(calls.has_value());
    auto effects = DeriveClearEffects(reader, entry, *calls, &why);
    UNSCOPED_INFO(std::string(item.name) + ": effects: " + why);
    REQUIRE(effects.has_value());
    return { *calls, *effects };
}

// The dirty words and G+0x2ABD a flush leaves, from the prediction and the words at the entry.
struct PredictedExit
{
    std::array<std::uint64_t, kClearDirtyWords> dirty{};
    std::uint8_t                                allocByte = 0;
};

PredictedExit predictExit(const ClearEffects& effects, const ClearWatch& entry)
{
    const bool    allocated = effects.allocates && !(entry.allocByte & kClearAllocFailedBit);
    PredictedExit exit;
    for (std::size_t i = 0; i < kClearDirtyWords; ++i)
        exit.dirty[i] = (entry.dirty[i] & effects.keep[i]) | effects.set[i] | (allocated ? effects.setIfAllocated[i] : 0);
    exit.allocByte = static_cast<std::uint8_t>(entry.allocByte | effects.byteSet);
    return exit;
}

void testTable()
{
    for (const auto& item : cases())
    {
        const std::string name   = item.name;
        const auto        memory = build(item.scene);
        const auto        d      = deriveOf(item, memory);
        UNSCOPED_INFO(name + ": reachedFlush");
        REQUIRE((d.calls.reachedFlush == item.reached && d.effects.reachedFlush == item.reached));
        UNSCOPED_INFO(name + ": calls");
        REQUIRE((d.effects.calls == item.calls && d.calls.count == item.calls));
        UNSCOPED_INFO(name + ": stencil and fast calls");
        REQUIRE((d.effects.stencilCalls == item.stencil && d.effects.fastCalls == item.fast));
        UNSCOPED_INFO(name + ": paths");
        REQUIRE((d.effects.untiledCalls == item.untiled && d.effects.tiledBlockCalls == item.block && d.effects.tiledLoopCalls == item.loop && d.effects.tiledNoneCalls == item.none));
        UNSCOPED_INFO(name + ": allocates");
        REQUIRE(d.effects.allocates == item.allocates);
        UNSCOPED_INFO(name + ": the tiled device and the tiles its loops select");
        REQUIRE((d.effects.tiled == (item.block + item.loop + item.none != 0) && d.effects.loopTiles == item.loop));

        if (!item.reached)
        {
            UNSCOPED_INFO(name + ": an early exit with unchanged words");
            REQUIRE((item.entry.dirty == item.exit.dirty && item.entry.allocByte == item.exit.allocByte));
#if REREVVED_DEVELOPER
            const auto early = CompareClearEffects(d.effects, item.entry, item.exit);
            REQUIRE((early.early && !early.earlyChanged && !early.compared && !early.mismatch));
#endif
            continue;
        }
        UNSCOPED_INFO(name + ": predicted words");
        const auto predicted = predictExit(d.effects, item.entry);
        REQUIRE((predicted.dirty == item.exit.dirty && predicted.allocByte == item.exit.allocByte));
#if REREVVED_DEVELOPER
        const auto result = CompareClearEffects(d.effects, item.entry, item.exit);
        UNSCOPED_INFO(name + ": compared");
        REQUIRE((result.compared && !result.early && !result.allocFailed));
        UNSCOPED_INFO(name + ": exact match, mismatch mask " + std::to_string(result.mismatch));
        REQUIRE(result.mismatch == 0);
        UNSCOPED_INFO(name + ": predicted words");
        REQUIRE((result.expect == item.exit.dirty && result.expectByte == item.exit.allocByte));

        // One flipped bit of the exit state is exactly its field.
        for (std::size_t word = 0; word < kClearDirtyWords; ++word)
            for (unsigned bit : { 0U, 17U, 36U, 63U })
            {
                auto exit = item.exit;
                exit.dirty[word] ^= 1ULL << bit;
                const auto flipped = CompareClearEffects(d.effects, item.entry, exit);
                UNSCOPED_INFO(name + ": a flipped bit " + std::to_string(bit) + " of word " + std::to_string(word));
                REQUIRE((flipped.compared && flipped.mismatch == 1U << word));
            }
        for (std::uint8_t bit : { 0x01, 0x04, 0x08, 0x80 })
        {
            auto exit = item.exit;
            exit.allocByte ^= bit;
            UNSCOPED_INFO(name + ": a flipped byte bit");
            REQUIRE(CompareClearEffects(d.effects, item.entry, exit).mismatch == 8U);
        }
#endif
    }
}

void testFastBoundaries()
{
    // (G+0x2880 >> 16) & 3 picks 80x16, 80x8 or 40x8 as the least rectangle.
    struct Boundary
    {
        std::uint32_t mode;
        Rect          rect;
        bool          fast;
    };

    const std::vector<Boundary> boundaries{
        { 0, { 0, 0, 80, 16 }, true },
        { 0, { 0, 0, 79, 16 }, false },
        { 0, { 0, 0, 80, 15 }, false },
        { 0, { -10, -4, 80, 16 }, true }, // clipped to the viewport at 0,0
        { 0, { -10, -4, 79, 16 }, false },
        { 1, { 0, 0, 80, 8 }, true },
        { 1, { 0, 0, 80, 7 }, false },
        { 1, { 0, 0, 79, 8 }, false },
        { 2, { 0, 0, 40, 8 }, true },
        { 2, { 0, 0, 39, 8 }, false },
        { 3, { 0, 0, 40, 8 }, true },
        { 3, { 0, 0, 40, 7 }, false },
    };
    for (const auto& boundary : boundaries)
    {
        Expected item{ "fast_boundary", Scene{}, {}, {}, true, 1, 0, 0, 1, 0, 0, 0, true };
        item.scene.control = boundary.mode << 16;
        item.scene.rect    = boundary.rect;
        const auto memory  = build(item.scene);
        const auto d       = deriveOf(item, memory);
        UNSCOPED_INFO("fast boundary mode " + std::to_string(boundary.mode));
        REQUIRE((d.effects.fastCalls == 1) == boundary.fast);
    }
    // Formats 5, 7 and 15 of the color surface never take the fast path; others do.
    for (std::uint32_t format = 0; format < 16; ++format)
    {
        Expected item{ "format", Scene{}, {}, {}, true, 1, 0, 0, 1, 0, 0, 0, true };
        item.scene.rect   = { 0, 0, 640, 480 };
        item.scene.format = format;
        const auto d      = deriveOf(item, build(item.scene));
        UNSCOPED_INFO("fast format " + std::to_string(format));
        REQUIRE((d.effects.fastCalls == 1) == (format != 5 && format != 7 && format != 15));
    }
    // Flags 0x10 or 0x20 (any of 0x30) forbid the fast path.
    for (std::uint32_t flags : { 0x11U, 0x21U, 0x31U })
    {
        Expected item{ "flags", Scene{}, {}, {}, true, 1, 0, 0, 1, 0, 0, 0, true };
        item.scene.rect  = { 0, 0, 640, 480 };
        item.scene.flags = flags;
        const auto d     = deriveOf(item, build(item.scene));
        UNSCOPED_INFO("no fast path with flags " + std::to_string(flags));
        REQUIRE(d.effects.fastCalls == 0);
        UNSCOPED_INFO("stencil call with flags " + std::to_string(flags));
        REQUIRE(d.effects.stencilCalls == ((flags & 0x20) ? 1U : 0U));
    }
}

#if REREVVED_DEVELOPER
void testCompare()
{
    const auto table = cases();
    const auto base  = named(table, "untiled_color_pattern");
    const auto d     = deriveOf(base, build(base.scene));

    // The bit 0x02 of G+0x2ABD added by the ring-segment switch is noted, not a mismatch.
    {
        auto exit = base.exit;
        exit.allocByte |= 0x02;
        const auto result = CompareClearEffects(d.effects, base.entry, exit);
        UNSCOPED_INFO("the segment switch bit is noted");
        REQUIRE((result.compared && result.switchBit && result.mismatch == 0));
        // A different extra bit is a byte mismatch.
        exit = base.exit;
        exit.allocByte |= 0x40;
        UNSCOPED_INFO("another extra byte bit is a mismatch");
        REQUIRE(CompareClearEffects(d.effects, base.entry, exit).mismatch == 8U);
        // Bit 0x02 set at the entry is predicted and must stay.
        auto entry = base.entry;
        entry.allocByte |= 0x02;
        exit = base.exit;
        exit.allocByte |= 0x02;
        auto ok = CompareClearEffects(d.effects, entry, exit);
        UNSCOPED_INFO("a bit 0x02 kept from the entry is predicted");
        REQUIRE((ok.mismatch == 0 && !ok.switchBit));
        UNSCOPED_INFO("a bit 0x02 cleared by the call is a mismatch");
        REQUIRE(CompareClearEffects(d.effects, entry, base.exit).mismatch == 8U);
    }
    // A marker that appears during the call is an allocation failure and compares nothing.
    {
        auto exit = base.exit;
        exit.allocByte |= 0x20;
        exit.dirty[0]     = 0;
        const auto result = CompareClearEffects(d.effects, base.entry, exit);
        UNSCOPED_INFO("an allocation failure is not compared");
        REQUIRE((result.allocFailed && !result.compared && !result.mismatch));
    }
    // A marker set at the entry is compared, without the post-allocation ORs.
    {
        const auto marked = named(table, "marker_at_entry");
        const auto md     = deriveOf(marked, build(marked.scene));
        const auto result = CompareClearEffects(md.effects, marked.entry, marked.exit);
        UNSCOPED_INFO("a marker at the entry");
        REQUIRE((result.compared && result.markerAtEntry && !result.allocFailed && result.mismatch == 0));
        auto exit = marked.exit;
        exit.dirty[0] |= 0x0300000000000808ULL;
        UNSCOPED_INFO("the allocation ORs are not predicted after a marker at the entry");
        REQUIRE(CompareClearEffects(md.effects, marked.entry, exit).mismatch == 1U);
        exit = marked.exit;
        exit.allocByte &= ~0x20;
        UNSCOPED_INFO("a marker cleared by the call is a mismatch");
        REQUIRE(CompareClearEffects(md.effects, marked.entry, exit).mismatch == 8U);
    }
    // Early exits: unchanged words are ok, any changed word or byte is changed.
    {
        const auto early = named(table, "early_no_depth_surface");
        const auto ed    = deriveOf(early, build(early.scene));
        auto       r     = CompareClearEffects(ed.effects, early.entry, early.exit);
        UNSCOPED_INFO("early ok");
        REQUIRE((r.early && !r.earlyChanged));
        for (std::size_t word = 0; word < kClearDirtyWords; ++word)
        {
            auto exit = early.exit;
            exit.dirty[word] ^= 1;
            r = CompareClearEffects(ed.effects, early.entry, exit);
            UNSCOPED_INFO("early changed in a dirty word");
            REQUIRE((r.early && r.earlyChanged && !r.compared));
        }
        auto exit = early.exit;
        exit.allocByte |= 4;
        UNSCOPED_INFO("early changed in the byte");
        REQUIRE(CompareClearEffects(ed.effects, early.entry, exit).earlyChanged);
        // G+0x2ABE and the deltas are record-only, also in an early exit.
        exit           = early.exit;
        exit.stateByte = 7;
        exit.ring      = 0x100;
        exit.allocated = 0x54;
        exit.cursor    = 0x20;
        r              = CompareClearEffects(ed.effects, early.entry, exit);
        UNSCOPED_INFO("record-only words");
        REQUIRE((r.early && !r.earlyChanged && r.stateChanged && r.ringDelta == 0x100 && r.allocDelta == 0x54 && r.cursorDelta == 0x20));
    }
    // Deltas are signed 32-bit differences.
    {
        auto entry      = base.entry;
        entry.ring      = 0x1000;
        entry.allocated = 0x400;
        entry.cursor    = 0x10;
        auto exit       = base.exit;
        exit.ring       = 0x1054;
        exit.allocated  = 0x454;
        exit.cursor     = 0x0;
        const auto r    = CompareClearEffects(d.effects, entry, exit);
        UNSCOPED_INFO("deltas");
        REQUIRE((r.ringDelta == 0x54 && r.allocDelta == 0x54 && r.cursorDelta == -0x10 && !r.stateChanged && r.mismatch == 0));
    }
}

#endif

void testRefusals()
{
    const auto table = cases();
    const auto base  = named(table, "untiled_color_zero");
    {
        auto memory = build(base.scene);
        memory.erase(kG + 0x2ABC);
        std::string why;
        const auto  reader = readerOf(memory);
        const auto  calls  = DeriveClearCalls(reader, entryOf(base.scene), &why);
        UNSCOPED_INFO("calls do not read G+0x2ABC with a rectangle");
        REQUIRE(calls.has_value());
        UNSCOPED_INFO("unreadable tiling byte: " + why);
        REQUIRE((!DeriveClearEffects(reader, entryOf(base.scene), *calls, &why) && why.find("unreadable") == 0));
    }
    {
        auto memory = build(base.scene);
        memory.erase(kSurface + 0x1C);
        const auto  reader = readerOf(memory);
        std::string why;
        const auto  calls = DeriveClearCalls(reader, entryOf(base.scene), &why);
        UNSCOPED_INFO("an unreadable format word is unsupported");
        REQUIRE((calls.has_value() && !DeriveClearEffects(reader, entryOf(base.scene), *calls, &why)));
    }
    {
        Scene scene        = base.scene;
        scene.tileByte     = 0x10;
        scene.tileCount    = kClearMaxTiles + 1;
        auto        memory = build(scene);
        std::string why;
        const auto  reader = readerOf(memory);
        const auto  calls  = DeriveClearCalls(reader, entryOf(scene), &why);
        UNSCOPED_INFO("a tile table beyond the limit: " + why);
        REQUIRE((calls.has_value() && !DeriveClearEffects(reader, entryOf(scene), *calls, &why) && why.find("tile_count") == 0));
        scene.tileCount = kClearMaxTiles;
        for (std::uint32_t i = 0; i < kClearMaxTiles; ++i)
            scene.tiles.push_back({ 0, 0, 0, 0 });
        scene.tileCount    = 0xFFFFFFFF;
        memory             = build(scene);
        const auto reader2 = readerOf(memory);
        UNSCOPED_INFO("a full tile table is read");
        REQUIRE(DeriveClearEffects(reader2, entryOf(scene), *DeriveClearCalls(reader2, entryOf(scene)), &why).has_value());
    }
    {
        // An unreadable tile word.
        Scene scene    = base.scene;
        scene.tileByte = 0x10;
        scene.tiles    = { { 0, 0, 10, 10 } };
        auto memory    = build(scene);
        memory.erase(kG + 0x31D0 + 8);
        std::string why;
        const auto  reader = readerOf(memory);
        const auto  calls  = DeriveClearCalls(reader, entryOf(scene), &why);
        UNSCOPED_INFO("an unreadable tile word is unsupported");
        REQUIRE((calls.has_value() && !DeriveClearEffects(reader, entryOf(scene), *calls, &why)));
    }
    {
        // A list that overflowed is not modeled; an early exit needs no reads at all.
        const auto memory = build(base.scene);
        const auto reader = readerOf(memory);
        ClearCalls overflowed;
        overflowed.reachedFlush = true;
        overflowed.overflow     = 1;
        std::string why;
        UNSCOPED_INFO("an overflowed list");
        REQUIRE((!DeriveClearEffects(reader, entryOf(base.scene), overflowed, &why) && why == "call_overflow"));
        const auto early = DeriveClearEffects([](std::uint32_t, std::uint32_t&)
                                              {
                                                  return false;
                                              },
                                              entryOf(base.scene),
                                              ClearCalls{},
                                              &why);
        UNSCOPED_INFO("an early exit reads nothing");
        REQUIRE((early.has_value() && !early->reachedFlush));
    }
    {
        // The watch fails on an unreadable word and an address that would wrap.
        const auto memory = build(base.scene);
        ClearWatch w;
        UNSCOPED_INFO("watch words are absent from the scene");
        REQUIRE(!ReadClearWatch(readerOf(memory), kG, w));
        UNSCOPED_INFO("an address that wraps");
        REQUIRE(!ReadClearWatch(readerOf(memory), 0xFFFFFFF0, w));
    }
}

void testWatch()
{
    Memory                             memory;
    constexpr std::uint32_t            g = kG;
    const std::array<std::uint32_t, 6> dirty{ 0x01234567, 0x89ABCDEF, 0x0F1E2D3C, 0x4B5A6978, 0xFEDCBA98, 0x76543210 };
    for (std::uint32_t i = 0; i < 6; ++i)
        memory[g + 0x10 + 4 * i] = dirty[i];
    memory[g + 0x30]   = 0x11111111;
    memory[g + 0x2ABC] = 0x40A5C3E1; // G+0x2ABD = 0xA5, G+0x2ABE = 0xC3
    memory[g + 0x3A44] = 0x22222222;
    memory[g + 0x3A54] = 0x33333333;
    memory[g + 0x3A4C] = 0x12121212;
    // The ring and pool words the consume model reads.
    constexpr std::uint32_t pool = 0x40040000;
    memory[g + 0x34]             = 0x44444444;
    memory[g + 0x38]             = 0x55555555;
    memory[g + 0x3A58]           = 0x66666666;
    memory[g + 0x31CC]           = 4;
    memory[g + 0x33B0]           = 0x77777777;
    memory[g + 0x31AC]           = 0x13131313;
    memory[g + 0x34CC]           = pool;
    memory[pool + 152]           = 0x88888888;
    memory[pool + 156]           = 0x99999999;
    ClearWatch w;
    UNSCOPED_INFO("watch read");
    REQUIRE(ReadClearWatch(readerOf(memory), g, w));
    UNSCOPED_INFO("dirty words are big-endian pairs");
    REQUIRE((w.dirty[0] == 0x0123456789ABCDEFULL && w.dirty[1] == 0x0F1E2D3C4B5A6978ULL && w.dirty[2] == 0xFEDCBA9876543210ULL));
    UNSCOPED_INFO("bytes, the tiling byte and the predicate");
    REQUIRE((w.allocByte == 0xA5 && w.stateByte == 0xC3 && w.tilingByte == 0x40 && w.predicate == 0x13131313));
    UNSCOPED_INFO("record-only words");
    REQUIRE((w.ring == 0x11111111 && w.cursor == 0x22222222 && w.allocated == 0x33333333));
    UNSCOPED_INFO("ring limits, threshold, target count and bracket");
    REQUIRE((w.limit34 == 0x44444444 && w.limit38 == 0x55555555 && w.poolThreshold == 0x66666666 && w.targetCount == 4 && w.bracket == 0x77777777));
    UNSCOPED_INFO("the pool object and its fixed buffer words");
    REQUIRE((w.poolObject == pool && w.poolBase == 0x88888888 && w.poolSize == 0x99999999));
    UNSCOPED_INFO("the wrap word is always read, the null pool area only without a pool object");
    REQUIRE((w.wrapPos == 0x12121212 && w.sideEnd == 0));
    // Without a pool object the buffer words are not read; the null pool allocator words are, through the
    // progress block G+0x2A90.
    memory[g + 0x34CC] = 0;
    memory.erase(pool + 152);
    memory.erase(pool + 156);
    UNSCOPED_INFO("a null pool object needs the allocator words");
    REQUIRE(!ReadClearWatch(readerOf(memory), g, w));
    constexpr std::uint32_t block = 0x40050000;
    memory[g + 0x3A3C]            = 0xA1A1A1A1;
    memory[g + 0x3A50]            = 0xB2B2B2B2;
    memory[g + 0x3A48]            = 0xC3C3C3C3;
    memory[g + 0x33B4]            = 0xD4D4D4D4;
    memory[g + 0x2A90]            = block;
    memory[block + 4]             = 0xE5E5E5E5;
    UNSCOPED_INFO("a null pool object reads no buffer word");
    REQUIRE((ReadClearWatch(readerOf(memory), g, w) && w.poolObject == 0 && w.poolBase == 0 && w.poolSize == 0));
    UNSCOPED_INFO("the null pool allocator words");
    REQUIRE((w.sideEnd == 0xA1A1A1A1 && w.wrapLimit == 0xB2B2B2B2 && w.segment == 0xC3C3C3C3 && w.bracketNext == 0xD4D4D4D4 && w.progress == 0xE5E5E5E5));
    // Words exist at addresses 4 and 2, so only the block check refuses a null or wrapping block.
    memory[4]          = 0x01010101;
    memory[2]          = 0x02020202;
    memory[g + 0x2A90] = 0;
    UNSCOPED_INFO("a null progress block is unreadable");
    REQUIRE(!ReadClearWatch(readerOf(memory), g, w));
    memory[g + 0x2A90] = 0xFFFFFFFE;
    UNSCOPED_INFO("a progress block whose word would wrap");
    REQUIRE(!ReadClearWatch(readerOf(memory), g, w));
    memory[g + 0x2A90] = block;
    // With one, an unreadable buffer word fails the read.
    memory[g + 0x34CC] = pool;
    UNSCOPED_INFO("an unreadable pool word");
    REQUIRE(!ReadClearWatch(readerOf(memory), g, w));
    memory[g + 0x34CC] = 0xFFFFFFF0U;
    UNSCOPED_INFO("a pool object whose words would wrap");
    REQUIRE(!ReadClearWatch(readerOf(memory), g, w));
}

#if REREVVED_DEVELOPER
void testCounters()
{
    TakeClearEffectCounts();
    const auto table = cases();
    const auto note  = [&](const char* name, const ClearWatch* exitOverride = nullptr)
    {
        const auto item = named(table, name);
        const auto d    = deriveOf(item, build(item.scene));
        NoteClearEffect(d.effects, CompareClearEffects(d.effects, item.entry, exitOverride ? *exitOverride : item.exit));
    };
    note("untiled_color_ones");
    note("stencil_then_fast");
    note("depth_only_fast");
    note("flush_no_calls");
    note("tiled_block_reach");
    note("tiled_loop_reach");
    note("tiled_none");
    note("early_no_depth_surface");
    {
        auto exit     = named(table, "early_empty_rectangle").exit;
        exit.dirty[1] = 0;
        note("early_empty_rectangle", &exit);
    }
    {
        auto exit = named(table, "untiled_color_zero").exit;
        exit.dirty[2] ^= 1;
        exit.allocByte ^= 1;
        note("untiled_color_zero", &exit);
    }
    {
        auto exit = named(table, "untiled_color_zero").exit;
        exit.allocByte |= 0x20;
        note("untiled_color_zero", &exit);
    }
    note("marker_at_entry");
    {
        auto exit = named(table, "untiled_color_pattern").exit;
        exit.allocByte |= 0x02;
        exit.stateByte = 1;
        exit.ring      = 0x54;
        exit.allocated = 0x54;
        exit.cursor    = 0x40;
        note("untiled_color_pattern", &exit);
    }
    NoteClearEffectUnsupported();
    NoteClearEffectUnsupported();
    NoteClearEffectUnpaired(3);
    NoteClearEffectUnpaired(0);

    const auto counts = TakeClearEffectCounts();

    UNSCOPED_INFO("exact counts");
    REQUIRE((counts.exactOk == 9 && counts.mismatch[0] == 0 && counts.mismatch[1] == 0 && counts.mismatch[2] == 1 && counts.mismatch[3] == 1));
    UNSCOPED_INFO("early and allocation counts");
    REQUIRE((counts.earlyOk == 1 && counts.earlyChanged == 1 && counts.allocFailed == 1));
    UNSCOPED_INFO("unsupported and unpaired");
    REQUIRE((counts.unsupported == 2 && counts.unpaired == 3));
    UNSCOPED_INFO("color, depth-only and no-call counts");
    REQUIRE((counts.color == 10 && counts.depthOnly == 1 && counts.flushNoCalls == 1));
    UNSCOPED_INFO("stencil and fast counts");
    REQUIRE((counts.stencil == 1 && counts.fast == 2));
    UNSCOPED_INFO("path counts");
    REQUIRE((counts.pathUntiled == 8 && counts.pathTiledBlock == 1 && counts.pathTiledLoop == 1 && counts.pathTiledNone == 1));
    UNSCOPED_INFO("marker, switch and state counts");
    REQUIRE((counts.markerAtEntry == 1 && counts.switchBit == 1 && counts.stateChanged == 1));
    UNSCOPED_INFO("delta sums");
    REQUIRE((counts.allocBytesSum == 0x54 && counts.ringDeltaSum == 0x54 && counts.cursorDeltaSum == 0x40));
    const std::string expected =
        " clear_effect_exact_ok=9 clear_effect_exact_mismatch_dirty10=0 clear_effect_exact_mismatch_dirty18=0 clear_effect_exact_mismatch_dirty20=1"
        " clear_effect_exact_mismatch_byte2abd=1 clear_effect_early_ok=1 clear_effect_early_changed=1 clear_effect_alloc_failed=1"
        " clear_effect_unsupported=2 clear_effect_unpaired=3 clear_effect_path_untiled=8 clear_effect_path_tiled_block=1"
        " clear_effect_path_tiled_loop=1 clear_effect_path_tiled_none=1 clear_effect_color=10 clear_effect_depth_only=1 clear_effect_stencil=1"
        " clear_effect_fast=2 clear_effect_flush_no_calls=1 clear_effect_2abe_changed=1 clear_effect_alloc_bytes_sum=84"
        " clear_effect_ring_delta_sum=84 clear_effect_cursor_delta_sum=64 clear_effect_alloc_marker_entry=1 clear_effect_2abd_switch_bit=1";
    UNSCOPED_INFO("formatted counts: " + FormatClearEffectCounts(counts));
    REQUIRE(FormatClearEffectCounts(counts) == expected);
    const auto again = TakeClearEffectCounts();
    UNSCOPED_INFO("take resets");
    REQUIRE((again.exactOk == 0 && again.unsupported == 0 && again.unpaired == 0 && again.allocBytesSum == 0 && again.mismatch == std::array<std::uint64_t, kClearEffectFieldCount>{}));
}

#endif

ClearProbe probeOf([[maybe_unused]] std::uint32_t g, bool derive, bool effects)
{
    ClearProbe probe;
    probe.deriveCalls = derive;
    probe.effects     = effects;
#if REREVVED_DEVELOPER
    probe.entry.g = g;
#endif
    return probe;
}

void testDropCounts()
{
    constexpr std::uint32_t outer = 0x70000000, inner = 0x6FFFF000, innermost = 0x6FFFE000;
    const auto              frameOf = [](std::uint32_t sp)
    {
        return sp - kClearFrameSize;
    };
    unsigned dropped = 0;
    {
        // The outer exit drops two probes above it: one derive-only, one with both checks.
        ClearProbeStack stack;
        ClearDropCounts drops;
        stack.Push(outer, probeOf(1, true, true), &drops);
        stack.Push(inner, probeOf(2, true, false), &drops);
        stack.Push(innermost, probeOf(3, true, true), &drops);
        UNSCOPED_INFO("pushes drop nothing");
        REQUIRE((drops.derive == 0 && drops.effects == 0));
        auto popped = stack.Pop(frameOf(outer), dropped, &drops);
        UNSCOPED_INFO("skipped exits are counted per check");
        REQUIRE((popped && dropped == 2 && drops.derive == 2 && drops.effects == 1));
    }
    {
        // A shallower entry drops the stale probes; effect-only probes count for effects alone.
        ClearProbeStack     stack;
        ClearDropCounts     drops;
        const std::uint32_t second = frameOf(innermost) - 0x1000, third = frameOf(second) - 0x1000;
        stack.Push(innermost, probeOf(1, false, true), &drops);
        stack.Push(second, probeOf(2, false, true), &drops);
        stack.Push(third, probeOf(3, true, false), &drops);
        UNSCOPED_INFO("nested pushes drop nothing");
        REQUIRE((drops.derive == 0 && drops.effects == 0 && stack.Size() == 3));
        stack.Push(outer, probeOf(4, true, true), &drops);
        UNSCOPED_INFO("two stale effect-only and a stale derive-only probe");
        REQUIRE((drops.derive == 1 && drops.effects == 2 && stack.Size() == 1));
    }
    {
        // A full stack drops the new probe, counted by its own checks.
        ClearProbeStack stack;
        ClearDropCounts drops;
        for (unsigned i = 0; i < ClearProbeStack::kDepth; ++i)
            stack.Push(outer - 0x1000 * i, probeOf(i, false, false), &drops);
        const auto count = stack.Push(outer - 0x1000 * ClearProbeStack::kDepth, probeOf(9, false, true), &drops);
        UNSCOPED_INFO("a full stack drops an effect-only probe");
        REQUIRE((count == 1 && drops.derive == 0 && drops.effects == 1));
    }
    {
        // A call hook finds the probe and drops the stale ones above it.
        ClearProbeStack stack;
        ClearDropCounts drops;
        stack.Push(outer, probeOf(1, true, true), &drops);
        stack.Push(inner, probeOf(2, false, true), &drops);
        auto* found = stack.Find(frameOf(outer), dropped, &drops);
        UNSCOPED_INFO("a call hook drops the stale effect-only probe");
        REQUIRE((found && dropped == 1 && drops.derive == 0 && drops.effects == 1 && stack.Size() == 1));
    }
    {
        // A probe a hook already invalidated was counted unpaired there: skipping its exit counts nothing more.
        ClearProbeStack stack;
        ClearDropCounts drops;
        stack.Push(outer, probeOf(1, true, true), &drops);
        stack.Push(inner, probeOf(2, true, true), &drops);
        stack.Push(innermost, probeOf(3, false, true), &drops);
        auto* stale = stack.Find(frameOf(inner), dropped, &drops);
        UNSCOPED_INFO("the live probe above is counted by a call hook");
        REQUIRE((stale && dropped == 1 && drops.derive == 0 && drops.effects == 1));
        stale->live = false;
        auto popped = stack.Pop(frameOf(outer), dropped, &drops);
        UNSCOPED_INFO("an invalidated probe is not counted when its exit is skipped");
        REQUIRE((popped && dropped == 1 && drops.derive == 0 && drops.effects == 1));
        ClearProbeStack other;
        ClearDropCounts pushDrops;
        other.Push(inner, probeOf(1, true, true), &pushDrops);
        other.Push(innermost, probeOf(2, true, true), &pushDrops);
        other.Find(frameOf(innermost), dropped, &pushDrops)->live = false;
        other.Push(outer, probeOf(3, true, true), &pushDrops);
        UNSCOPED_INFO("a shallower entry does not count an invalidated probe");
        REQUIRE((other.Size() == 1 && pushDrops.derive == 1 && pushDrops.effects == 1));
    }
}

// Tiles count once only when rectangle and mask select them.
void testLoopTiles()
{
    Scene scene;
    scene.rect           = { 50, 0, 150, 50 };
    scene.tileByte       = 0x10;
    scene.tiles          = { { 0, 0, 100, 100 }, { 100, 0, 200, 100 }, { 200, 0, 300, 100 }, { 0, 100, 100, 200 } };
    const auto effectsOf = [&](std::uint32_t mask)
    {
        scene.tileMask     = mask;
        const auto  memory = build(scene);
        std::string why;
        const auto  calls = DeriveClearCalls(readerOf(memory), entryOf(scene), &why);
        UNSCOPED_INFO("calls: " + why);
        REQUIRE(calls.has_value());
        const auto effects = DeriveClearEffects(readerOf(memory), entryOf(scene), *calls, &why);
        UNSCOPED_INFO("effects: " + why);
        REQUIRE(effects.has_value());
        return *effects;
    };
    auto both = effectsOf(0xFF);
    UNSCOPED_INFO("tiles 0 and 1 meet the rectangle; 2 and 3 do not");
    REQUIRE((both.tiled && both.tiledLoopCalls == 1 && both.loopTiles == 2 && both.allocates));
    auto first = effectsOf(0x3);
    UNSCOPED_INFO("bits 0 and 1 are tile 0");
    REQUIRE((first.tiledLoopCalls == 1 && first.loopTiles == 1));
    auto offscreen = effectsOf(0xC0);
    UNSCOPED_INFO("the mask names only tiles the rectangle misses");
    REQUIRE((offscreen.tiledNoneCalls == 1 && offscreen.loopTiles == 0 && !offscreen.allocates));
    // slw gives zero for a shift of 32 and more, so tile 16 and beyond are never selected: tile 16 lies under the
    // rectangle but its mask bits are bits 32 and 33.
    scene.tiles.assign(17, Rect{ 1000, 1000, 1001, 1001 });
    scene.tiles[16] = { 0, 0, 100, 100 };
    UNSCOPED_INFO("a tile beyond 15 is never selected");
    REQUIRE(effectsOf(0xFFFFFFFF).loopTiles == 0);
    scene.tiles[15] = { 0, 0, 100, 100 };
    UNSCOPED_INFO("tile 15 uses bits 30 and 31");
    REQUIRE(effectsOf(0xC0000000).loopTiles == 1);
}

} // namespace

TEST_CASE("clear_effect", "[ctest][clear-effect][timeout-60]")
{
    testTable();
    testFastBoundaries();
#if REREVVED_DEVELOPER
    testCompare();
#endif
    testRefusals();
    testWatch();
#if REREVVED_DEVELOPER
    testCounters();
#endif
    testDropCounts();
    testLoopTiles();
}
