#include "gpu/guest_d3d/resolve_surface_hooks.h"
#include "gpu/guest_d3d/resolve_surface_registry.h"
#include "gpu/guest_d3d/resource_mirror_registry.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace rerevved::gpu;
using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint32_t kBase = 0x1E3B0000;

// A whole-surface copy of `width` x `height` into kBase, with its fetch format.
GuestResolveCopy wholeCopy(std::uint32_t width, std::uint32_t height, GuestResolvePacking packing = GuestResolvePacking::HostRgba8, std::uint32_t base = kBase)
{
    std::uint32_t format = 6, copyFormat = 6, number = 0;
    if (packing == GuestResolvePacking::Color2101010Words)
        format = 54, copyFormat = 7;
    else if (packing == GuestResolvePacking::Float16x2Words)
        format = 31, copyFormat = 31, number = 7U << 13;
    else if (packing == GuestResolvePacking::Depth24Stencil8Words)
        format = 22, copyFormat = 6;
    GuestResolveCopy copy;
    copy.copyControl = packing == GuestResolvePacking::Depth24Stencil8Words ? 0x00100004 : 0x00100000;
    copy.destBase    = base;
    copy.destPitch   = (height << 16) | width;
    copy.destInfo    = (packing == GuestResolvePacking::Float16x2Words ? 0 : 1U << 24) | number | (copyFormat << 7);
    copy.destFetch   = { 0x80000002U | ((width / 32) << 22), (base & 0x1FFFF000U) | format, (width - 1) | ((height - 1) << 13), 0xC14, 0, 0x200 };
    copy.rectWidth   = width;
    copy.rectHeight  = height;
    copy.packing     = packing;
    return copy;
}

NativeGuestWordPacking wordsOf(GuestResolvePacking packing)
{
    switch (packing)
    {
        case GuestResolvePacking::Color8888Words:
            return NativeGuestWordPacking::Color8888;
        case GuestResolvePacking::Color2101010Words:
            return NativeGuestWordPacking::Color2101010;
        case GuestResolvePacking::Depth24Stencil8Words:
            return NativeGuestWordPacking::Depth24Stencil8;
        case GuestResolvePacking::Float16x2Words:
            return NativeGuestWordPacking::Float16x2;
        default:
            return NativeGuestWordPacking::None;
    }
}

ResolveSurfaceRecord record(std::uint64_t mark, const GuestResolveCopy& copy, bool staged = false)
{
    ResolveSurfaceRecord value;
    value.mark    = mark;
    value.copy    = copy;
    value.destEnd = std::uint64_t(copy.destBase) + std::uint64_t((copy.destFetch[0] >> 22) & 0x1FF) * 32 * 4 * (((copy.destFetch[2] >> 13) & 8191) + 32);
    value.packing = wordsOf(copy.packing);
    value.staged  = staged;
    return value;
}

NativeRenderFrameTexture stored(std::uint64_t mark, const GuestResolveCopy& copy)
{
    const auto               words = wordsOf(copy.packing);
    NativeRenderFrameTexture texture{ ResolveSurfaceTexture(mark), copy.rectWidth, copy.rectHeight, words == NativeGuestWordPacking::None ? NativeDrawReplayTextureFormat::Rgba8 : NativeDrawReplayTextureFormat::R32Uint, ResolveSurfaceKey(mark), true };
    texture.importedWords = words;
    return texture;
}

// Records and admits a resolve as the assembler does once its frame was submitted.
void produce(ResolveSurfaceRegistry& registry, std::uint64_t mark, const GuestResolveCopy& copy, bool staged = false)
{
    registry.Record(record(mark, copy, staged));
    registry.Admit(mark, stored(mark, copy));
}

std::optional<ResolveSurfaceBinding> bound(const ResolveSurfaceRegistry& registry, const std::array<std::uint32_t, 6>& fetch)
{
    const auto lookup = registry.Find(fetch, true);
    if (!lookup || !std::holds_alternative<ResolveSurfaceBinding>(*lookup))
        return std::nullopt;
    return std::get<ResolveSurfaceBinding>(*lookup);
}

std::optional<NativeResolvedTextureMiss> missed(const ResolveSurfaceRegistry& registry, const std::array<std::uint32_t, 6>& fetch, bool castable = true, std::size_t* form = nullptr)
{
    const auto lookup = registry.Find(fetch, castable, form);
    if (!lookup || !std::holds_alternative<NativeResolvedTextureMiss>(*lookup))
        return std::nullopt;
    return std::get<NativeResolvedTextureMiss>(*lookup);
}

std::array<std::uint32_t, 6> fetchOf(const GuestResolveCopy& copy)
{
    auto fetch = copy.destFetch;
    // Through a virtual view, as draws name destinations.
    fetch[1] |= 0xE0000000U;
    return fetch;
}

// Same-range resolves supersede versions; old keys release after last binding.
void versioning()
{
    ResolveSurfaceRegistry registry;
    const auto             copy = wholeCopy(256, 128);
    produce(registry, 10, copy);
    auto first = bound(registry, fetchOf(copy));
    UNSCOPED_INFO("the first resolve binds as version 1");
    REQUIRE((first && first->mark == 10 && first->version == 1 && first->texture.frameTextureId == ResolveSurfaceTexture(10)));
    UNSCOPED_INFO("the binding has the fetch's layout");
    REQUIRE((first->texture.layout.width == 256 && first->texture.layout.height == 128 && first->texture.layout.format == NativeDrawReplayTextureFormat::Rgba8));
    produce(registry, 20, copy);
    auto second = bound(registry, fetchOf(copy));
    UNSCOPED_INFO("a covering resolve of the same range is version 2");
    REQUIRE((second && second->mark == 20 && second->version == 2));
    UNSCOPED_INFO("the superseded entry ended");
    REQUIRE((!registry.View(10) && registry.View(20) && registry.Size() == 1));
    UNSCOPED_INFO("an ended entry's import stays until its key is released");
    REQUIRE(registry.Import(ResolveSurfaceTexture(10)).has_value());
    auto held = registry.TakeReleases([](std::uint64_t id)
                                      {
                                          return id == ResolveSurfaceTexture(10);
                                      });
    UNSCOPED_INFO("a key a source draw binds is held back");
    REQUIRE(held.empty());
    const auto keys = registry.TakeReleases({});
    UNSCOPED_INFO("the released key leaves with its import");
    REQUIRE((keys == std::vector<std::uint64_t>{ ResolveSurfaceKey(10) } && !registry.Import(ResolveSurfaceTexture(10))));
    registry.Landed(20);
    UNSCOPED_INFO("a landed image makes the range Both");
    REQUIRE(registry.View(20)->state == ResolveSurfaceState::Both);
    std::cout << "A covering resolve writes a new version and releases the old key\n";
}

// Overlap ends old entries; changed fetch bytes still take the bytes path.
void overlap()
{
    ResolveSurfaceRegistry registry;
    const auto             wide   = wholeCopy(256, 256);
    const auto             nested = wholeCopy(64, 64, GuestResolvePacking::HostRgba8, kBase + 0x8000);
    produce(registry, 1, wide);
    produce(registry, 2, nested);
    UNSCOPED_INFO("the nested resolve ends the older entry and counts once");
    REQUIRE((registry.Size() == 1 && !registry.View(1) && registry.TakeCounts().overlaps == 1));
    UNSCOPED_INFO("the older fetch binds nothing; the nested one binds its own entry");
    REQUIRE((!registry.Find(fetchOf(wide), true) && bound(registry, fetchOf(nested))->mark == 2));
    // A same-range resolve that does not cover the older rectangle ends it too.
    produce(registry, 3, wide);
    auto strip      = wide;
    strip.rectWidth = 128;
    strip.destX     = 128;
    strip.rectX     = 128;
    produce(registry, 4, strip);
    const auto counts = registry.TakeCounts();
    UNSCOPED_INFO("a partial strip ends the whole entry, which dropped the nested one");
    REQUIRE((!registry.View(3) && registry.View(4) && counts.overlaps == 1 && counts.droppedCovered == 1));
    UNSCOPED_INFO("the strip alone does not cover the fetch");
    REQUIRE(missed(registry, fetchOf(wide)) == NativeResolvedTextureMiss::Coverage);
    std::cout << "A newer intersecting resolve ends the older entry\n";
}

// Whole cover drops old ranges; partial cover leaves old images owed.
void coveredDrops()
{
    using Marks       = std::vector<std::uint64_t>;
    const auto target = wholeCopy(1024, 1024);
    const auto fog    = wholeCopy(1280, 720, GuestResolvePacking::Float16x2Words);
    // A registry with an owner, so its endings are named.
    const auto owned = []
    {
        auto registry = std::make_unique<ResolveSurfaceRegistry>();
        registry->SetSettle([](const ResolveSurfaceLockRanges*)
                            {
                            });
        return registry;
    };
    // `owed` are the marks left owed on the host, none owed now.
    const auto ended = [](ResolveSurfaceRegistry& registry, const Marks& owed, const Marks& dropped, std::uint64_t overlaps, std::uint64_t covered)
    {
        const auto endings = registry.TakeEndings();
        const auto counts  = registry.TakeCounts();
        return endings.owed.empty() && counts.owed == owed.size() && std::ranges::all_of(owed, [&](std::uint64_t mark)
                                                                                         {
                                                                                             return registry.Holds(mark) == ResolveSurfaceHold::Hold;
                                                                                         }) &&
               endings.dropped == dropped && counts.overlaps == overlaps && counts.droppedCovered == covered;
    };
    {
        auto registry = owned();
        produce(*registry, 1, target);
        produce(*registry, 2, fog);
        UNSCOPED_INFO("the fog, 720 rows, is not whole and leaves the target under it owed");
        REQUIRE(ended(*registry, { 1 }, {}, 1, 0));
        produce(*registry, 3, target);
        const auto endings = registry->TakeEndings();
        const auto counts  = registry->TakeCounts();
        UNSCOPED_INFO("the whole target drops the owed target it supersedes and the fog inside it");
        REQUIRE((endings.owed.empty() && endings.dropped == Marks{ 1, 2 } && counts.droppedCovered == 1 && counts.owedDropped == 1 && counts.owed == 0 &&
                 registry->Holds(1) == ResolveSurfaceHold::Write && registry->Size() == 1 && registry->View(3)));
    }
    {
        // A whole resolve's bytes end at its last row: half the target's
        // rows do not reach the fog's end.
        auto half       = target;
        half.rectHeight = 512;
        auto registry   = owned();
        produce(*registry, 4, fog);
        static_cast<void>(registry->TakeEndings());
        static_cast<void>(registry->TakeCounts());
        produce(*registry, 5, half);
        UNSCOPED_INFO("a whole resolve shorter than the older range leaves it owed");
        REQUIRE(ended(*registry, { 4 }, {}, 1, 0));
    }
    // Rows that end inside a tile row or start past the origin, columns short
    // of the pitch, and an array destination the encoder refuses leave bytes
    // of the range unwritten.
    auto rows          = target;
    rows.rectHeight    = 1000;
    auto columns       = target;
    columns.rectWidth  = 1016;
    auto shifted       = target;
    shifted.destY      = 32;
    shifted.rectHeight = 992;
    auto array         = target;
    array.destInfo |= 8;
    for (const auto& partial : { rows, columns, shifted, array })
    {
        auto registry = owned();
        produce(*registry, 6, fog);
        static_cast<void>(registry->TakeEndings());
        static_cast<void>(registry->TakeCounts());
        produce(*registry, 7, partial);
        UNSCOPED_INFO(std::format("a {}x{} resolve at row {}, info {:X}, is not whole and leaves the fog owed", partial.rectWidth, partial.rectHeight, partial.destY, partial.destInfo));
        REQUIRE(ended(*registry, { 6 }, {}, 1, 0));
    }
    {
        // An older range that starts before the whole resolve's base is not inside it.
        auto       registry = owned();
        const auto before   = wholeCopy(64, 64, GuestResolvePacking::HostRgba8, kBase - 0x1000);
        produce(*registry, 8, before);
        static_cast<void>(registry->TakeEndings());
        static_cast<void>(registry->TakeCounts());
        produce(*registry, 9, target);
        UNSCOPED_INFO("an older range starting before the base is owed");
        REQUIRE(ended(*registry, { 8 }, {}, 1, 0));
    }
    {
        // Only a later resolve covers: one recorded with an earlier mark owes.
        auto registry = owned();
        registry->Record(record(20, fog));
        static_cast<void>(registry->TakeEndings());
        static_cast<void>(registry->TakeCounts());
        registry->Record(record(15, target));
        UNSCOPED_INFO("an entry with a later mark is not covered");
        REQUIRE(ended(*registry, { 20 }, {}, 1, 0));
    }
    std::cout << "A later whole resolve drops the older entries inside it\n";
}

// Owed tiles wait for lock, land, cover, free/unlock or age resolution.
void owedTiles()
{
    using Marks       = std::vector<std::uint64_t>;
    const auto target = wholeCopy(1024, 1024);
    const auto fog    = wholeCopy(1280, 720, GuestResolvePacking::Float16x2Words);
    // Inside the target's range, past the fog's.
    const auto tail  = ResolveSurfaceLockRanges{ { { kBase + 0x3F0000, kBase + 0x3F0010 }, {} } };
    const auto quiet = [](ResolveSurfaceRegistry& registry)
    {
        static_cast<void>(registry.TakeEndings());
        static_cast<void>(registry.TakeCounts());
    };
    {
        ResolveSurfaceRegistry registry;
        produce(registry, 1, target);
        produce(registry, 2, fog);
        UNSCOPED_INFO("without an owner nothing is owed");
        REQUIRE((registry.Holds(1) == ResolveSurfaceHold::Write && registry.TakeCounts().owed == 0));
    }
    ResolveSurfaceRegistry registry;
    registry.SetSettle([](const ResolveSurfaceLockRanges*)
                       {
                       });
    produce(registry, 1, target);
    registry.Landed(1);
    produce(registry, 2, fog);
    UNSCOPED_INFO("a landed tile is not owed");
    REQUIRE((registry.Holds(1) == ResolveSurfaceHold::Write && registry.TakeCounts().owed == 0 && registry.TakeEndings().owed.empty()));

    produce(registry, 3, target);
    quiet(registry);
    produce(registry, 4, fog);
    {
        const auto endings = registry.TakeEndings();
        const auto counts  = registry.TakeCounts();
        UNSCOPED_INFO("the fog leaves the target owed, out of the lookups");
        REQUIRE((endings.owed.empty() && endings.dropped.empty() && counts.owed == 1 && counts.overlaps == 1 && registry.Holds(3) == ResolveSurfaceHold::Hold && !registry.View(3)));
        const auto line = FormatResolveSurfaceCounts(counts);
        UNSCOPED_INFO("the line carries the owed counters: " + line);
        REQUIRE(line.find(" surface_dropped_covered=0 surface_owed_dropped=0 surface_owed=1 surface_unlocked_writes=0") != std::string::npos);
    }
    registry.Locked(0x40001000, tail, false);
    UNSCOPED_INFO("a lock of the owed tail names it");
    REQUIRE((registry.Holds(3) == ResolveSurfaceHold::Locked && registry.LockedMarks(tail) == Marks{ 3 }));
    registry.Unlocked(0x40001000);
    UNSCOPED_INFO("a read lock leaves it owed");
    REQUIRE((registry.Holds(3) == ResolveSurfaceHold::Hold && registry.TakeEndings().owed.empty()));
    registry.Landed(3);
    UNSCOPED_INFO("a landed image is no longer owed");
    REQUIRE((registry.Holds(3) == ResolveSurfaceHold::Write && registry.TakeCounts().owed == 0));

    produce(registry, 5, target);
    quiet(registry);
    produce(registry, 6, fog);
    registry.Skipped(5);
    UNSCOPED_INFO("a skipped image is no longer owed");
    REQUIRE((registry.Holds(5) == ResolveSurfaceHold::Write && registry.TakeCounts().owed == 0 && registry.TakeEndings().owed.empty()));

    produce(registry, 7, target);
    quiet(registry);
    produce(registry, 8, fog);
    registry.Freed(kBase + 0x3F0000, kBase + 0x3F1000);
    {
        const auto endings = registry.TakeEndings();
        const auto counts  = registry.TakeCounts();
        UNSCOPED_INFO("a free of the tail drops the owed target, not the fog");
        REQUIRE((endings.dropped == Marks{ 7 } && endings.owed.empty() && counts.owedDropped == 1 && counts.owed == 0 && registry.View(8)));
    }

    produce(registry, 9, target);
    quiet(registry);
    produce(registry, 10, fog);
    registry.Written(kBase + 0x3F0000, kBase + 0x3F0004, 0);
    UNSCOPED_INFO("a store the record already saw leaves it owed");
    REQUIRE((registry.Holds(9) == ResolveSurfaceHold::Hold && registry.TakeEndings().dropped.empty()));
    registry.Locked(0x40002000, tail, true);
    registry.Written(kBase + 0x3F0000, kBase + 0x3F0004, 1);
    UNSCOPED_INFO("a store inside a write lock, which landed it, leaves it");
    REQUIRE((registry.Holds(9) == ResolveSurfaceHold::Locked && registry.TakeEndings().dropped.empty()));
    registry.Unlocked(0x40002000);
    registry.Written(kBase + 0x3F0000, kBase + 0x3F0004, 2);
    {
        const auto endings = registry.TakeEndings();
        UNSCOPED_INFO("an unlocked store after the record drops it");
        REQUIRE((endings.dropped == Marks{ 9 } && endings.owed.empty() && registry.Holds(9) == ResolveSurfaceHold::Write));
    }

    produce(registry, 11, target);
    quiet(registry);
    produce(registry, 12, fog);
    registry.Overwritten(kBase + 0x3F0000, kBase + 0x3F0004);
    {
        const auto endings = registry.TakeEndings();
        UNSCOPED_INFO("an unmodeled write makes it owed now");
        REQUIRE((endings.owed == Marks{ 11 } && endings.dropped.empty() && registry.Holds(11) == ResolveSurfaceHold::Write && registry.View(12)));
    }

    // A partial newer resolve leaves both owed; the next whole target drops them.
    produce(registry, 13, target);
    quiet(registry);
    produce(registry, 14, fog);
    auto half       = target;
    half.rectHeight = 512;
    produce(registry, 15, half);
    UNSCOPED_INFO("a partial resolve leaves the target and the fog owed");
    REQUIRE((registry.Holds(13) == ResolveSurfaceHold::Hold && registry.Holds(14) == ResolveSurfaceHold::Hold && registry.TakeEndings().owed.empty()));
    quiet(registry);
    produce(registry, 16, target);
    {
        const auto endings = registry.TakeEndings();
        const auto counts  = registry.TakeCounts();
        UNSCOPED_INFO("the whole target drops the owed tiles and supersedes the half");
        REQUIRE((endings.dropped == Marks{ 13, 14, 15 } && endings.owed.empty() && counts.owedDropped == 2 && counts.owed == 0));
    }

    // Past kOwed the oldest owed tiles land now.
    for (std::uint64_t i = 0; i <= ResolveSurfaceRegistry::kOwed; ++i)
    {
        const auto base = 0x10000000U + std::uint32_t(i) * 0x10000U;
        produce(registry, 100 + 2 * i, wholeCopy(64, 64, GuestResolvePacking::HostRgba8, base));
        produce(registry, 101 + 2 * i, wholeCopy(32, 32, GuestResolvePacking::HostRgba8, base));
        registry.Skipped(101 + 2 * i);
    }
    {
        const auto endings = registry.TakeEndings();
        const auto counts  = registry.TakeCounts();
        UNSCOPED_INFO("the oldest owed tiles past kOwed land now");
        REQUIRE((endings.owed == Marks{ 100 } && counts.owed == ResolveSurfaceRegistry::kOwed && registry.Holds(100) == ResolveSurfaceHold::Write && registry.Holds(102) == ResolveSurfaceHold::Hold));
    }
    registry.SetSettle(nullptr);
    UNSCOPED_INFO("removing the owner forgets the owed tiles");
    REQUIRE((registry.Holds(102) == ResolveSurfaceHold::Write && registry.TakeCounts().owed == 0));
    std::cout << "Owed tiles stay on the host until a lock, a land or a drop\n";
}

// Bracket tiles commit at EndTiling; in-bracket draws bind the earlier version.
void bracketCommit()
{
    {
        ResolveSurfaceRegistry registry;
        const auto             copy = wholeCopy(128, 64);
        produce(registry, 1, copy);
        produce(registry, 2, copy, true);
        auto inside = bound(registry, fetchOf(copy));
        UNSCOPED_INFO("inside the bracket the earlier version binds");
        REQUIRE((inside && inside->mark == 1 && registry.View(2) && !registry.View(2)->committed));
        registry.CommitTiles();
        auto after = bound(registry, fetchOf(copy));
        UNSCOPED_INFO("EndTiling commits the tile resolve as the next version");
        REQUIRE((after && after->mark == 2 && after->version == 2 && !registry.View(1)));
    }
    {
        // [old A, old B, staged A, staged B]: committing staged A ends old A
        // ahead of staged B, which still commits.
        ResolveSurfaceRegistry registry;
        const auto             a = wholeCopy(128, 64), b = wholeCopy(128, 64, GuestResolvePacking::HostRgba8, 0x1D000000);
        produce(registry, 1, a);
        produce(registry, 2, b);
        produce(registry, 3, a, true);
        produce(registry, 4, b, true);
        registry.CommitTiles();
        const auto boundA = bound(registry, fetchOf(a)), boundB = bound(registry, fetchOf(b));
        UNSCOPED_INFO("EndTiling commits both pending versions over both old ones");
        REQUIRE((boundA && boundA->mark == 3 && boundA->version == 2 && boundB && boundB->mark == 4 && boundB->version == 2 && registry.Size() == 2));
    }
    std::cout << "Tile resolves commit their versions at EndTiling\n";
}

// A tiled version binds only after both frames submit and the bracket ends.
void tiles()
{
    const auto copy = wholeCopy(128, 64);
    const auto tile = [&](std::uint32_t x)
    {
        auto value      = copy;
        value.rectX     = x;
        value.destX     = x;
        value.rectWidth = 64;
        return value;
    };
    {
        ResolveSurfaceRegistry registry;
        produce(registry, 1, copy);
        registry.Record(record(5, tile(0), true));
        registry.Record(record(6, tile(64), true));
        UNSCOPED_INFO("the bracket's tiles form one pending version of the destination's size");
        REQUIRE((registry.Size() == 2 && registry.View(6) && registry.View(6)->width == 128 && registry.View(6)->height == 64));
        const auto left = registry.FrameSurface(5, 0x55), right = registry.FrameSurface(6, 0x66);
        UNSCOPED_INFO("the first tile copies to the origin of the version's texture");
        REQUIRE((left && left->textureId == 0x55 && left->key == ResolveSurfaceKey(5) && left->x == 0 && left->width == 128 && left->height == 64));
        UNSCOPED_INFO("the second tile copies at its destination offset");
        REQUIRE((right && right->key == ResolveSurfaceKey(5) && right->x == 64 && right->y == 0));
        registry.Admit(5, stored(5, tile(0)));
        auto inside = bound(registry, fetchOf(copy));
        UNSCOPED_INFO("inside the bracket the committed version binds");
        REQUIRE((inside && inside->mark == 1));
        registry.CommitTiles();
        UNSCOPED_INFO("the version waits for every tile's frame");
        REQUIRE((missed(registry, fetchOf(copy)) == NativeResolvedTextureMiss::Unmodeled && !registry.View(1)));
        registry.Admit(6, stored(6, tile(64)));
        const auto whole = bound(registry, fetchOf(copy));
        UNSCOPED_INFO("the assembled version binds as the next version");
        REQUIRE((whole && whole->mark == 5 && whole->version == 2 && whole->texture.frameTextureId == ResolveSurfaceTexture(5)));
        const auto import = registry.Import(ResolveSurfaceTexture(5));
        UNSCOPED_INFO("the version imports the destination-sized texture");
        REQUIRE((import && import->width == 128 && import->height == 64 && import->persistentKey == ResolveSurfaceKey(5)));
        registry.Landed(5);
        UNSCOPED_INFO("guest memory is current only once every tile landed");
        REQUIRE(registry.View(6)->state == ResolveSurfaceState::Host);
        registry.Landed(6);
        UNSCOPED_INFO("both tiles landed");
        REQUIRE(registry.View(5)->state == ResolveSurfaceState::Both);
    }
    {
        ResolveSurfaceRegistry registry;
        registry.Record(record(5, tile(0), true));
        registry.Admit(5, stored(5, tile(0)));
        registry.CommitTiles();
        UNSCOPED_INFO("a destination missing a tile is a coverage miss");
        REQUIRE(missed(registry, fetchOf(copy)) == NativeResolvedTextureMiss::Coverage);
    }
    std::cout << "Tile resolves assemble one destination-sized version\n";
}

// A pending version admits only after every tile submits, including late joins.
void tileAdmission()
{
    const auto copy = wholeCopy(128, 64);
    const auto tile = [&](std::uint32_t x)
    {
        auto value      = copy;
        value.rectX     = x;
        value.destX     = x;
        value.rectWidth = 64;
        return value;
    };
    ResolveSurfaceRegistry registry;
    registry.Record(record(5, tile(0), true));
    registry.Admit(5, stored(5, tile(0)));
    registry.Record(record(6, tile(64), true));
    registry.CommitTiles();
    UNSCOPED_INFO("a tile joining after an admitted one holds the version back");
    REQUIRE((missed(registry, fetchOf(copy)) == NativeResolvedTextureMiss::Unmodeled && !registry.View(5)->admitted));
    registry.Admit(6, stored(6, tile(64)));
    const auto whole = bound(registry, fetchOf(copy));
    UNSCOPED_INFO("the version binds once its last tile's frame was submitted");
    REQUIRE((whole && whole->mark == 5));
    std::cout << "A pending version admits once every tile's frame was submitted\n";
}

// A newer ordinary resolve ends pending tiles; later tiles start a new version.
void pendingOrder()
{
    const auto copy = wholeCopy(128, 64);
    {
        ResolveSurfaceRegistry registry;
        produce(registry, 1, copy);
        produce(registry, 5, copy, true);
        produce(registry, 6, copy);
        UNSCOPED_INFO("the newer ordinary resolve ends the pending version and counts");
        REQUIRE((!registry.View(5) && registry.TakeCounts().overlaps == 1));
        registry.CommitTiles();
        const auto after = bound(registry, fetchOf(copy));
        UNSCOPED_INFO("EndTiling leaves the newer resolve bound");
        REQUIRE((after && after->mark == 6 && after->version == 2 && !registry.View(1)));
    }
    {
        // A staged resolve into another destination over the range ends it too.
        ResolveSurfaceRegistry registry;
        produce(registry, 5, copy, true);
        produce(registry, 6, wholeCopy(64, 64), true);
        UNSCOPED_INFO("an overlapping pending version of another destination ends the older one");
        REQUIRE((!registry.View(5) && registry.View(6) && registry.TakeCounts().overlaps == 1));
        registry.CommitTiles();
        UNSCOPED_INFO("only the newer version commits");
        REQUIRE((bound(registry, fetchOf(wholeCopy(64, 64)))->mark == 6 && missed(registry, fetchOf(copy)) == NativeResolvedTextureMiss::Form));
    }
    std::cout << "Pending versions yield to newer overlapping resolves\n";
}

// Tiles accumulate in order; later rectangles win and coverage uses union.
void tileOrder()
{
    const auto copy = wholeCopy(128, 64);
    const auto tile = [&](std::uint32_t x, std::uint32_t width)
    {
        auto value      = copy;
        value.rectX     = x;
        value.destX     = x;
        value.rectWidth = width;
        return value;
    };
    {
        ResolveSurfaceRegistry                                       registry;
        const std::array<std::pair<std::uint64_t, std::uint32_t>, 4> writes{ { { 5, 0 }, { 6, 0 }, { 7, 64 }, { 8, 64 } } };
        for (const auto& [mark, x] : writes)
            produce(registry, mark, tile(x, 64), true);
        UNSCOPED_INFO("the four tiles form one pending version");
        REQUIRE((registry.Size() == 1 && registry.TakeCounts().overlaps == 0));
        for (const auto& [mark, x] : writes)
        {
            const auto surface = registry.FrameSurface(mark, mark);
            UNSCOPED_INFO("each tile copies into the version's texture at its rectangle");
            REQUIRE((surface && surface->key == ResolveSurfaceKey(5) && surface->x == x && surface->width == 128));
        }
        registry.CommitTiles();
        const auto whole = bound(registry, fetchOf(copy));
        UNSCOPED_INFO("the version of all four tiles binds");
        REQUIRE((whole && whole->mark == 5 && whole->version == 1));
        const auto records = registry.HostCopy(copy.destBase, copy.destBase + 4);
        UNSCOPED_INFO("a lock reads the tiles back in resolve order, the later rectangle last");
        REQUIRE((records.size() == 4 && records[0].mark == 5 && records[1].mark == 6 && records[2].mark == 7 && records[3].mark == 8));
    }
    {
        ResolveSurfaceRegistry registry;
        produce(registry, 5, tile(0, 80), true);
        produce(registry, 6, tile(48, 80), true);
        registry.CommitTiles();
        UNSCOPED_INFO("overlapping rectangles that together cover the destination bind");
        REQUIRE(bound(registry, fetchOf(copy)).has_value());
    }
    {
        ResolveSurfaceRegistry registry;
        produce(registry, 5, tile(0, 64), true);
        produce(registry, 6, tile(16, 32), true);
        registry.CommitTiles();
        UNSCOPED_INFO("overlapping rectangles leaving a gap are a coverage miss");
        REQUIRE(missed(registry, fetchOf(copy)) == NativeResolvedTextureMiss::Coverage);
    }
    std::cout << "Tile writes accumulate in resolve order in one pending version\n";
}

// A skipped image ends the entry holding its resolve, pending or committed.
void skippedImage()
{
    const auto             copy = wholeCopy(128, 64);
    ResolveSurfaceRegistry registry;
    produce(registry, 1, copy);
    registry.Skipped(1);
    UNSCOPED_INFO("a skipped image ends its entry");
    REQUIRE((!registry.View(1) && !registry.Find(fetchOf(copy), true)));
    produce(registry, 5, copy, true);
    registry.Skipped(5);
    registry.CommitTiles();
    UNSCOPED_INFO("a skipped tile ends its pending version, and both keys release");
    REQUIRE((!registry.View(5) && registry.Size() == 0 && registry.TakeReleases({}).size() == 2));
    std::cout << "A skipped image ends its entry\n";
}

// Ended pending versions still overwrite guest memory and end older entries.
void pendingEndsOlder()
{
    const auto copy = wholeCopy(128, 64);
    const auto tile = [&](std::uint32_t x)
    {
        auto value      = copy;
        value.rectX     = x;
        value.destX     = x;
        value.rectWidth = 64;
        return value;
    };
    {
        ResolveSurfaceRegistry registry;
        produce(registry, 1, copy);
        for (std::uint64_t i = 0; i < ResolveSurfaceRegistry::kTiles; ++i)
            produce(registry, 10 + i, tile(0), true);
        UNSCOPED_INFO("the full pending version waits over the older entry");
        REQUIRE((registry.View(1) && registry.View(10)->committed == false));
        produce(registry, 100, tile(64), true);
        registry.CommitTiles();
        UNSCOPED_INFO("a tile refused beyond kTiles ends the version and the older entry");
        REQUIRE((registry.TakeCounts().refused == 1 && !registry.View(1) && !registry.View(10) && !registry.Find(fetchOf(copy), true)));
    }
    {
        ResolveSurfaceRegistry registry;
        produce(registry, 1, copy);
        produce(registry, 5, tile(0), true);
        produce(registry, 6, tile(64), true);
        registry.Landed(5);
        registry.Skipped(6);
        registry.CommitTiles();
        UNSCOPED_INFO("a skipped tile beside a landed one ends the version and the older entry");
        REQUIRE((!registry.View(1) && !registry.View(5) && !registry.Find(fetchOf(copy), true)));
    }
    {
        // The older entry lies past the end of the newer resolve's range.
        ResolveSurfaceRegistry registry;
        const auto             tail = wholeCopy(32, 32, GuestResolvePacking::HostRgba8, kBase + 0x8000);
        produce(registry, 1, tail);
        produce(registry, 5, copy, true);
        produce(registry, 6, wholeCopy(32, 32), true);
        registry.CommitTiles();
        UNSCOPED_INFO("a pending version a newer resolve ends takes the older entries past that resolve with it");
        REQUIRE((!registry.View(1) && !registry.View(5) && registry.View(6) && !registry.Find(fetchOf(tail), true) && registry.TakeCounts().overlaps == 2));
    }
    std::cout << "An ended pending version ends the older entries over its range\n";
}

// Capacity refuses new entries but ends older ranges the resolve overwrites.
void capacity()
{
    ResolveSurfaceRegistry registry;
    for (std::uint64_t i = 0; i < ResolveSurfaceRegistry::kEntries; ++i)
        produce(registry, i + 1, wholeCopy(32, 32, GuestResolvePacking::HostRgba8, 0x10000000U + std::uint32_t(i) * 0x10000U));
    UNSCOPED_INFO("the registry holds its capacity");
    REQUIRE(registry.Size() == ResolveSurfaceRegistry::kEntries);
    produce(registry, 1000, wholeCopy(32, 32, GuestResolvePacking::HostRgba8, 0x10000000U));
    const auto counts = registry.TakeCounts();
    UNSCOPED_INFO("the refused resolve ends only the entry it overwrites");
    REQUIRE((counts.refused == 1 && registry.Size() == ResolveSurfaceRegistry::kEntries - 1 && !registry.View(1) && !registry.View(1000) && registry.View(2)));
    UNSCOPED_INFO("a refused resolve hands nothing to the store");
    REQUIRE(!registry.FrameSurface(1000, 7));
    std::cout << "A full registry refuses new destinations and keeps live ones\n";
}

// Unlock ends a write-lock entry; flag 0x10 keeps it, outside stores end it.
void unlockEndsEntry()
{
    const auto copy  = wholeCopy(128, 128);
    const auto range = std::array<std::pair<std::uint64_t, std::uint64_t>, 2>{ { { kBase, kBase + 0x1000 }, {} } };
    {
        ResolveSurfaceRegistry registry;
        produce(registry, 1, copy);
        registry.Locked(0x40001000, range, false);
        registry.Unlocked(0x40001000);
        UNSCOPED_INFO("a lock without write intent keeps the entry");
        REQUIRE(registry.View(1).has_value());
        registry.Locked(0x40001000, range, true);
        registry.Locked(0x40001000, range, true);
        registry.Unlocked(0x40001000);
        UNSCOPED_INFO("the entry lives until the last unlock");
        REQUIRE(registry.View(1).has_value());
        registry.Unlocked(0x40001000);
        UNSCOPED_INFO("a write lock ends the entry at unlock");
        REQUIRE((!registry.View(1) && missed(registry, fetchOf(copy)) == std::nullopt));
    }
    {
        ResolveSurfaceRegistry registry;
        auto                   numbered = record(1, copy);
        numbered.watchedFrom            = 5;
        registry.Record(numbered);
        registry.Admit(1, stored(1, copy));
        registry.Written(kBase, kBase + 4, 5);
        UNSCOPED_INFO("a write numbered before the watch was armed is old");
        REQUIRE(registry.View(1).has_value());
        registry.Locked(0x40002000, range, true);
        registry.Written(kBase, kBase + 4, 6);
        UNSCOPED_INFO("a store inside a write lock waits for the unlock");
        REQUIRE((registry.View(1).has_value() && registry.TakeCounts().unlockedWrites == 0));
        registry.Unlocked(0x40002000);
        UNSCOPED_INFO("the unlock ends it");
        REQUIRE(!registry.View(1));
        produce(registry, 2, copy);
        registry.Written(kBase + 0x100, kBase + 0x104, 7);
        UNSCOPED_INFO("a store outside a lock ends the entry and counts");
        REQUIRE((!registry.View(2) && registry.TakeCounts().unlockedWrites == 1));
    }
    std::cout << "Write unlocks and unlocked stores end entries\n";
}

// The physical free drops overlapping entries; so does an unmodeled write.
void freeDrops()
{
    ResolveSurfaceRegistry registry;
    const auto             copy  = wholeCopy(128, 128);
    const auto             other = wholeCopy(64, 64, GuestResolvePacking::HostRgba8, 0x1D000000);
    produce(registry, 1, copy);
    produce(registry, 2, other);
    registry.Freed(kBase, std::uint64_t(kBase) + 0x10000);
    UNSCOPED_INFO("the free drops only its own entry");
    REQUIRE((!registry.View(1) && registry.View(2)));
    registry.Overwritten(0x1D000000, 0x1D000004);
    UNSCOPED_INFO("an unmodeled write drops the other, and both keys release");
    REQUIRE((!registry.View(2) && registry.Size() == 0 && registry.TakeReleases({}).size() == 2));
    std::cout << "The physical free drops its entries\n";
}

// Endings report owed/dropped tiles; settle uses the opened lock ranges.
void heldEndings()
{
    const auto copy   = wholeCopy(128, 128);
    const auto nested = wholeCopy(32, 32, GuestResolvePacking::HostRgba8, kBase + 0x4000);
    const auto range  = ResolveSurfaceLockRanges{ { { kBase, kBase + 0x1000 }, {} } };
    const auto away   = ResolveSurfaceLockRanges{ { { 0x1D000000, 0x1D001000 }, {} } };
    using Marks       = std::vector<std::uint64_t>;
    const auto ended  = [](ResolveSurfaceRegistry& registry, const Marks& owed, const Marks& dropped)
    {
        const auto endings = registry.TakeEndings();
        return endings.owed == owed && endings.dropped == dropped;
    };
    {
        ResolveSurfaceRegistry registry;
        produce(registry, 1, copy);
        registry.Freed(kBase, kBase + 0x10000);
        UNSCOPED_INFO("without an owner no ending is named");
        REQUIRE(ended(registry, {}, {}));
    }
    ResolveSurfaceRegistry                               registry;
    std::vector<std::optional<ResolveSurfaceLockRanges>> settles;
    registry.SetSettle([&](const ResolveSurfaceLockRanges* locked)
                       {
                           settles.push_back(locked ? std::optional(*locked) : std::nullopt);
                       });
    produce(registry, 1, copy);
    UNSCOPED_INFO("a live tile may stay on the host; an unknown mark lands");
    REQUIRE((registry.Holds(1) == ResolveSurfaceHold::Hold && registry.Holds(99) == ResolveSurfaceHold::Write));
    registry.Locked(0x40001000, range, false);
    UNSCOPED_INFO("a lock of either intent over the entry makes its images land, and names its marks");
    REQUIRE((registry.Holds(1) == ResolveSurfaceHold::Locked && registry.LockedMarks(range) == Marks{ 1 } && registry.LockedMarks(away).empty()));
    registry.Settle(&range);
    registry.Unlocked(0x40001000);
    registry.Settle();
    UNSCOPED_INFO("Settle hands the owner the lock's ranges, then none");
    REQUIRE((registry.Holds(1) == ResolveSurfaceHold::Hold && settles.size() == 2 && settles[0] == range && !settles[1]));
    produce(registry, 2, copy);
    UNSCOPED_INFO("the next covering version drops the older images");
    REQUIRE(ended(registry, {}, { 1 }));
    produce(registry, 3, nested);
    UNSCOPED_INFO("an overlap leaves the older images owed");
    REQUIRE((ended(registry, {}, {}) && registry.Holds(2) == ResolveSurfaceHold::Hold));
    registry.Overwritten(kBase + 0x4000, kBase + 0x4004);
    UNSCOPED_INFO("an unmodeled write makes them and the owed ones land now");
    REQUIRE(ended(registry, { 2, 3 }, {}));
    produce(registry, 4, copy);
    registry.Skipped(4);
    UNSCOPED_INFO("a skipped image ends its entry, owing none of it");
    REQUIRE((ended(registry, {}, {}) && registry.Holds(4) == ResolveSurfaceHold::Write));
    produce(registry, 5, copy);
    registry.Overwritten(kBase, kBase + 4, false);
    UNSCOPED_INFO("a host write about to land drops them");
    REQUIRE(ended(registry, {}, { 5 }));
    {
        // A store outside a lock ends a pending version and the committed
        // entry under it, both dropped: the store follows the notice.
        auto numbered        = record(6, copy);
        numbered.watchedFrom = 1;
        registry.Record(numbered);
        registry.Admit(6, stored(6, copy));
        produce(registry, 7, copy, true);
        registry.Written(kBase, kBase + 4, 2);
        UNSCOPED_INFO("an unlocked store drops the pending version and the entry under it");
        REQUIRE(ended(registry, {}, { 6, 7 }));
    }
    produce(registry, 8, copy);
    registry.Locked(0x40002000, range, true);
    registry.Unlocked(0x40002000);
    UNSCOPED_INFO("a write unlock leaves them owed");
    REQUIRE((ended(registry, {}, {}) && registry.Holds(8) == ResolveSurfaceHold::Hold));
    produce(registry, 9, copy);
    registry.Freed(kBase, kBase + 0x10000);
    UNSCOPED_INFO("the next version drops the owed images, and a free its own");
    REQUIRE(ended(registry, {}, { 8, 9 }));
    for (std::uint64_t i = 0; i < ResolveSurfaceRegistry::kEntries; ++i)
        produce(registry, 100 + i, wholeCopy(32, 32, GuestResolvePacking::HostRgba8, 0x10000000U + std::uint32_t(i) * 0x10000U));
    static_cast<void>(registry.TakeEndings());
    produce(registry, 1000, wholeCopy(32, 32, GuestResolvePacking::HostRgba8, 0x10000000U));
    UNSCOPED_INFO("a refused resolve leaves the entry it overwrites owed and is held by none");
    REQUIRE((ended(registry, {}, {}) && registry.Holds(100) == ResolveSurfaceHold::Hold && registry.Holds(1000) == ResolveSurfaceHold::Write));
    registry.CountLockReadbacks(2);
    registry.CountFlushes(3);
    registry.SetHeldBytes(std::uint64_t(3) << 20);
    const auto line = FormatResolveSurfaceCounts(registry.TakeCounts());
    UNSCOPED_INFO("the line carries the held counters: " + line);
    REQUIRE(line.ends_with(" lock_readbacks=2 surface_flushes=3 surface_readback_mb=3.0"));
    const auto next = registry.TakeCounts();
    UNSCOPED_INFO("the counters restart; the held bytes are a gauge");
    REQUIRE((!next.lockReadbacks && !next.flushes && next.heldBytes == std::uint64_t(3) << 20));
    registry.SetSettle(nullptr);
    registry.Settle();
    UNSCOPED_INFO("a removed owner is not called");
    REQUIRE(settles.size() == 2);
    std::cout << "Held images: holds, endings and settles" << '\n';
}

// k_16_16_FLOAT binds signed or not; k_24_8 binds converted depth.
void views()
{
    ResolveSurfaceRegistry registry;
    auto                   fog = wholeCopy(1280, 720, GuestResolvePacking::Float16x2Words);
    // The guest's fog fetch is signed (0x8A000156 sign bits 0x55).
    fog.destFetch[0] |= 0x154;
    produce(registry, 1, fog);
    auto half = bound(registry, fetchOf(fog));
    UNSCOPED_INFO("a signed k_16_16_FLOAT fetch binds Float16x2 words");
    REQUIRE((half && half->texture.layout.format == NativeDrawReplayTextureFormat::Rg16Float));
    // Selectors 0xC14 are {2, 1, 0, 3}; the RGGG host view reads blue and alpha from green.
    UNSCOPED_INFO("the k_16_16_FLOAT view maps selectors as the bytes path does");
    REQUIRE((half->texture.swizzle == std::array<std::uint8_t, 4>{ 1, 1, 0, 1 }));
    UNSCOPED_INFO("packed words need castable views");
    REQUIRE(missed(registry, fetchOf(fog), false) == NativeResolvedTextureMiss::Packing);
    auto biased      = fetchOf(fog);
    biased[0]        = (biased[0] & ~0x3FCU) | (0xAA << 2);
    std::size_t form = 0;
    UNSCOPED_INFO("biased signs are a signed form refusal");
    REQUIRE((missed(registry, biased, true, &form) == NativeResolvedTextureMiss::Form && form == 8));

    const auto depth = wholeCopy(512, 512, GuestResolvePacking::Depth24Stencil8Words, 0x1C000000);
    produce(registry, 2, depth);
    form                 = kResolveSurfaceFormConditions;
    const auto converted = bound(registry, fetchOf(depth));
    UNSCOPED_INFO("k_24_8 depth words bind their converted texture");
    REQUIRE((converted && converted->texture.layout.format == NativeDrawReplayTextureFormat::R32Float && converted->texture.frameTextureId == ResolveSurfaceDepthTexture(2)));

    const auto wide = wholeCopy(256, 256, GuestResolvePacking::Color2101010Words, 0x1B000000);
    produce(registry, 3, wide);
    auto ten = bound(registry, fetchOf(wide));
    UNSCOPED_INFO("format 54 binds Color2101010 words");
    REQUIRE((ten && ten->texture.layout.format == NativeDrawReplayTextureFormat::Rgb10a2));
    auto eight = fetchOf(wide);
    eight[1]   = (eight[1] & ~63U) | 6;
    UNSCOPED_INFO("an 8888 fetch of 2101010 words is a format refusal");
    REQUIRE((missed(registry, eight, true, &form) == NativeResolvedTextureMiss::Form && form == 0));

    registry.Record(record(4, wholeCopy(64, 64, GuestResolvePacking::HostRgba8, 0x1A000000)));
    UNSCOPED_INFO("an unadmitted entry is unmodeled");
    REQUIRE(missed(registry, fetchOf(wholeCopy(64, 64, GuestResolvePacking::HostRgba8, 0x1A000000))) == NativeResolvedTextureMiss::Unmodeled);
    std::cout << "The view rule binds k_16_16_FLOAT and converted k_24_8\n";
}

// The fetch's format and endian mode, as a draw would read another view.
std::array<std::uint32_t, 6> viewOf(const GuestResolveCopy& copy, std::uint32_t format, std::uint32_t endian)
{
    auto fetch = fetchOf(copy);
    fetch[1]   = (fetch[1] & ~0xFFU) | (endian << 6) | format;
    return fetch;
}

// The copy stores its words in endian mode `endian`.
GuestResolveCopy storedIn(GuestResolveCopy copy, std::uint32_t endian)
{
    copy.destInfo     = (copy.destInfo & ~7U) | endian;
    copy.destFetch[1] = (copy.destFetch[1] & ~0xC0U) | (endian << 6);
    return copy;
}

// Endian lanes fold into selectors; conversion or split-byte views miss.
void formatViews()
{
    using Swizzle = std::array<std::uint8_t, 4>;
    ResolveSurfaceRegistry registry;
    std::size_t            form = kResolveSurfaceFormConditions;
    // world-harvest pair: k_16_16_FLOAT words stored 8in32, read as k_8_8_8_8
    // at endian 0. The fetch reads byte k from host byte 3 - k; selectors
    // 0xC14 are {2, 1, 0, 3}.
    const auto fog = storedIn(wholeCopy(1280, 720, GuestResolvePacking::Float16x2Words), 2);
    produce(registry, 1, fog);
    const auto bytes = bound(registry, viewOf(fog, 6, 0));
    UNSCOPED_INFO("a k_8_8_8_8 fetch of 8in32 Float16x2 words reads their bytes reversed");
    REQUIRE((bytes && bytes->texture.layout.format == NativeDrawReplayTextureFormat::Rgba8 && bytes->texture.swizzle == Swizzle{ 1, 2, 3, 0 }));
    const auto halves = bound(registry, viewOf(fog, 31, 1));
    UNSCOPED_INFO("an 8in16 k_16_16_FLOAT fetch of 8in32 words reads the halves exchanged");
    REQUIRE((halves && halves->texture.layout.format == NativeDrawReplayTextureFormat::Rg16Float && halves->texture.swizzle == Swizzle{ 0, 0, 1, 0 }));
    UNSCOPED_INFO("a byte split inside a half is an endian refusal");
    REQUIRE((missed(registry, viewOf(fog, 31, 0), true, &form) == NativeResolvedTextureMiss::Form && form == 3));
    const auto wide = bound(registry, viewOf(fog, 54, 2));
    UNSCOPED_INFO("a k_2_10_10_10 fetch of unswapped words in their endian mode reads them as stored");
    REQUIRE((wide && wide->texture.layout.format == NativeDrawReplayTextureFormat::Rgb10a2 && wide->texture.swizzle == Swizzle{ 2, 1, 0, 3 }));
    UNSCOPED_INFO("a moved byte splits 10-bit fields");
    REQUIRE((missed(registry, viewOf(fog, 54, 0), true, &form) == NativeResolvedTextureMiss::Form && form == 3));

    // Swapped Color8888 words: the copy exchanged bytes 0 and 2 first.
    const auto color = wholeCopy(256, 256, GuestResolvePacking::Color8888Words, 0x1B000000);
    produce(registry, 2, color);
    const auto reversed = bound(registry, viewOf(color, 6, 2));
    UNSCOPED_INFO("an 8in32 read of swapped bytes reverses, then exchanges red and blue");
    REQUIRE((reversed && reversed->texture.swizzle == Swizzle{ 1, 0, 3, 2 }));
    UNSCOPED_INFO("a byte swap splits 10-bit fields");
    REQUIRE((missed(registry, viewOf(color, 54, 0), true, &form) == NativeResolvedTextureMiss::Form && form == 0));
    UNSCOPED_INFO("a byte swap splits float halves");
    REQUIRE((missed(registry, viewOf(color, 31, 0), true, &form) == NativeResolvedTextureMiss::Form && form == 0));

    // Depth words never swap; their bytes have a view, their depth does not.
    const auto depth = wholeCopy(512, 512, GuestResolvePacking::Depth24Stencil8Words, 0x1C000000);
    produce(registry, 3, depth);
    const auto stencil = bound(registry, viewOf(depth, 6, 0));
    UNSCOPED_INFO("a k_8_8_8_8 fetch of depth words reads their bytes unswapped");
    REQUIRE((stencil && stencil->texture.layout.format == NativeDrawReplayTextureFormat::Rgba8 && stencil->texture.swizzle == Swizzle{ 2, 1, 0, 3 }));
    const auto unorm = bound(registry, viewOf(depth, 22, 0));
    UNSCOPED_INFO("k_24_8 depth reads the converted texture's red for every data selector");
    REQUIRE((unorm && unorm->texture.layout.format == NativeDrawReplayTextureFormat::R32Float && unorm->texture.swizzle == Swizzle{ 0, 0, 0, 0 }));
    UNSCOPED_INFO("k_24_8 in another endian mode moves the depth bytes");
    REQUIRE((missed(registry, viewOf(depth, 22, 2), true, &form) == NativeResolvedTextureMiss::Form && form == 3));

    // Swapped Color2101010 words exchange fields: only format 54 in their
    // endian mode reads them.
    const auto ten = wholeCopy(256, 256, GuestResolvePacking::Color2101010Words, 0x1D000000);
    produce(registry, 4, ten);
    UNSCOPED_INFO("an 8in16 read of 2101010 words is an endian refusal");
    REQUIRE((missed(registry, viewOf(ten, 54, 1), true, &form) == NativeResolvedTextureMiss::Form && form == 3));
    UNSCOPED_INFO("swapped fields split float halves");
    REQUIRE((missed(registry, viewOf(ten, 31, 0), true, &form) == NativeResolvedTextureMiss::Form && form == 0));

    // Unpacked RGBA8 storage has only its own view.
    const auto host = storedIn(wholeCopy(64, 64, GuestResolvePacking::HostRgba8, 0x1A000000), 1);
    produce(registry, 5, host);
    const auto pairs = bound(registry, viewOf(host, 6, 0));
    UNSCOPED_INFO("an endian 0 read of 8in16 swapped bytes exchanges pairs, then red and blue");
    REQUIRE((pairs && pairs->texture.swizzle == Swizzle{ 3, 2, 1, 0 }));
    UNSCOPED_INFO("unpacked storage has no cast");
    REQUIRE((missed(registry, viewOf(host, 54, 1), true, &form) == NativeResolvedTextureMiss::Form && form == 0));
    std::cout << "The view rule binds other formats' views of stored words\n";
}

// SDK e19035b src/graphics/pipeline/texture/util.cpp:424, the generic 2D
// formula at 32 bpp, independent of the encoder's specialization.
std::uint32_t referenceOffset(std::uint32_t x, std::uint32_t y, std::uint32_t pitch)
{
    constexpr unsigned logBpp = 2;
    pitch                     = (pitch + 31) & ~31U;
    const auto macro          = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (logBpp + 7);
    const auto micro          = ((x & 7) + ((y & 14) << 2)) << logBpp;
    const auto offset         = macro + ((micro & ~15U) << 1) + (micro & 15) + ((y & 1) << 4);
    return ((offset & ~511U) << 3) + ((y & 16) << 7) + ((offset & 448) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63);
}

// Guest memory for the lock hook: object headers and the lock's stack by
// virtual address, and the destination by physical address.
struct GuestMemory
{
    std::map<std::uint32_t, std::vector<std::uint8_t>> blocks;
    std::vector<std::uint8_t>                          destination;

    void words(std::uint32_t address, std::initializer_list<std::pair<std::size_t, std::uint32_t>> values, std::size_t size)
    {
        auto& block = blocks[address];
        block.resize(std::max(block.size(), size));
        for (const auto& [offset, value] : values)
            for (unsigned c = 0; c < 4; ++c)
                block[offset + c] = static_cast<std::uint8_t>(value >> (24 - 8 * c));
    }

    bool Read(std::uint32_t address, std::span<std::uint8_t> bytes) const
    {
        for (const auto& [start, block] : blocks)
            if (address >= start && address - start + bytes.size() <= block.size())
            {
                std::copy_n(block.begin() + (address - start), bytes.size(), bytes.begin());
                return true;
            }
        return false;
    }

    // As the ledger publishes an image: whole, or one write per run.
    void Write(const GuestResolveImage& image)
    {
        const auto at = image.physical - kBase;
        UNSCOPED_INFO("the image lies inside the destination");
        REQUIRE((image.physical >= kBase && at + image.bytes.size() <= destination.size()));
        if (image.runs.empty())
            std::copy(image.bytes.begin(), image.bytes.end(), destination.begin() + at);
        for (const auto& run : image.runs)
            std::copy_n(image.bytes.begin() + run.offset, run.length, destination.begin() + at + run.offset);
    }
};

void transitionBarrier()
{
    const auto registry = std::make_shared<ResolveSurfaceRegistry>();
    const auto copy     = wholeCopy(128, 64);
    produce(*registry, 1, copy);
    produce(*registry, 2, copy, true);
    std::vector<std::uint64_t> drained;
    registry->SetBarrier([&]
                         {
                             const auto queued = bound(*registry, fetchOf(copy));
                             drained.push_back(queued ? queued->mark : 0);
                         });
    PublishResolveSurfaces(registry);
    ObserveResolveSurfaceTiling();
    const auto after = bound(*registry, fetchOf(copy));
    UNSCOPED_INFO("a draw queued inside the bracket drains before EndTiling commits the next version");
    REQUIRE((drained == std::vector<std::uint64_t>{ 1 } && after && after->mark == 2));
    ObserveResolveSurfaceTiling();
    ObserveResolveSurfaceFree(0x1C000000, 0x1000);
    UNSCOPED_INFO("a transition that changes no entry runs no barrier");
    REQUIRE(drained.size() == 1);
    ObserveResolveSurfaceFree(copy.destBase, 0x1000);
    UNSCOPED_INFO("a free over an entry drains before the entry drops");
    REQUIRE((drained == std::vector<std::uint64_t>{ 1, 2 } && !bound(*registry, fetchOf(copy))));
    PublishResolveSurfaces(nullptr);
    std::cout << "Registry transitions drain queued draws first\n";
}

// Lock bytes match SDK tiling/encoder images; write unlock ends the entry.
void lockRoundTrip()
{
    constexpr std::uint32_t kTexture = 0x40001000, kStack = 0x70000000;
    const auto              texels = [](std::uint32_t width, std::uint32_t height, std::uint32_t rowPitch)
    {
        std::vector<std::uint8_t> bytes(std::size_t(rowPitch) * height);
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::uint8_t>(i * 131 + (i >> 9) + width);
        return bytes;
    };
    // The texture header of `copy`'s destination fetch and a lock whose
    // ninth argument has no 0x10.
    const auto lock = [&](GuestMemory& memory, ResolveSurfaceRegistry& registry, const GuestResolveCopy& copy)
    {
        memory.words(kTexture, { { 0, 3 } }, kGuestTextureHeaderBytes);
        for (std::size_t i = 0; i < copy.destFetch.size(); ++i)
            memory.words(kTexture, { { 0x1C + i * 4, copy.destFetch[i] } }, kGuestTextureHeaderBytes);
        memory.words(kStack, { { 84, 0x0 } }, 128);
        ObserveResolveSurfaceLock(registry, kTexture, kStack, [&](std::uint32_t address, std::span<std::uint8_t> bytes)
                                  {
                                      return memory.Read(address, bytes);
                                  });
        return registry.HostCopy(copy.destBase, copy.destBase + 4);
    };
    const auto readBack = [&](GuestMemory& memory, const std::vector<ResolveSurfaceRecord>& records, const std::vector<std::uint8_t>& host, std::uint32_t hostPitch)
    {
        for (const auto& resolve : records)
        {
            const auto&               copy      = resolve.copy;
            const auto                tilePitch = copy.rectWidth * 4;
            std::vector<std::uint8_t> tile(std::size_t(tilePitch) * copy.rectHeight);
            for (std::uint32_t y = 0; y < copy.rectHeight; ++y)
                std::copy_n(host.begin() + (std::size_t(copy.destY) + y) * hostPitch + std::size_t(copy.destX) * 4, tilePitch, tile.begin() + std::size_t(y) * tilePitch);
            GuestResolveImage image;
            std::string       error;
            UNSCOPED_INFO("the lock's resolve encodes: " + error);
            REQUIRE(EncodeGuestResolve(copy, tile, tilePitch, image, error));
            memory.Write(image);
        }
    };
    {
        // Two RGBA8 tiles, swapped red and blue, endian 0.
        const auto  copy  = wholeCopy(128, 64);
        const auto  whole = record(0, copy);
        GuestMemory memory;
        memory.destination.assign(whole.destEnd - copy.destBase, 0xA5);
        ResolveSurfaceRegistry registry;
        for (std::uint32_t tile = 0; tile < 2; ++tile)
        {
            auto part  = copy;
            part.rectX = part.destX = tile * 64;
            part.rectWidth          = 64;
            registry.Record(record(5 + tile, part, true));
            registry.Admit(5 + tile, stored(5 + tile, part));
        }
        registry.CommitTiles();
        const auto host    = texels(128, 64, 128 * 4);
        const auto records = lock(memory, registry, copy);
        UNSCOPED_INFO("the lock names both tiles of the Host version");
        REQUIRE((records.size() == 2 && records[0].mark == 5 && records[1].mark == 6));
        readBack(memory, records, host, 128 * 4);
        std::size_t mismatches = 0;
        for (std::uint32_t y = 0; y < 64; ++y)
            for (std::uint32_t x = 0; x < 128; ++x)
            {
                std::array<std::uint8_t, 4> rgba{};
                std::copy_n(host.begin() + std::size_t(y) * 512 + x * 4, 4, rgba.begin());
                std::swap(rgba[0], rgba[2]);
                for (unsigned c = 0; c < 4; ++c)
                    mismatches += memory.destination[referenceOffset(x, y, 128) + c] != rgba[c];
            }
        UNSCOPED_INFO(std::format("the tiles' lock readback differs from the SDK tiling at {} bytes", mismatches));
        REQUIRE(mismatches == 0);
        registry.Unlocked(kTexture);
        UNSCOPED_INFO("the write lock's unlock ends the version");
        REQUIRE((!registry.View(5) && registry.HostCopy(copy.destBase, copy.destBase + 4).empty()));
    }

    struct Case
    {
        GuestResolvePacking packing;
        std::uint32_t       width, height;
    };

    const std::array<Case, 5> cases{ { { GuestResolvePacking::HostRgba8, 64, 32 },
                                       { GuestResolvePacking::Color8888Words, 96, 17 },
                                       { GuestResolvePacking::Color2101010Words, 64, 96 },
                                       { GuestResolvePacking::Float16x2Words, 64, 32 },
                                       { GuestResolvePacking::Depth24Stencil8Words, 32, 32 } } };
    std::size_t               checked = 0;
    for (const auto& form : cases)
    {
        const auto  copy = wholeCopy(form.width, form.height, form.packing);
        GuestMemory memory, ledger;
        memory.destination.assign(record(0, copy).destEnd - copy.destBase, 0x5A);
        ledger.destination = memory.destination;
        ResolveSurfaceRegistry registry;
        produce(registry, 7, copy);
        const auto        host = texels(form.width, form.height, form.width * 4);
        GuestResolveImage image;
        std::string       error;
        UNSCOPED_INFO(std::format("packing {} encodes: {}", int(form.packing), error));
        REQUIRE(EncodeGuestResolve(copy, host, form.width * 4, image, error));
        ledger.Write(image);
        const auto records = lock(memory, registry, copy);
        UNSCOPED_INFO("a lock of the destination finds its Host entry");
        REQUIRE((records.size() == 1 && records[0].mark == 7));
        readBack(memory, records, host, form.width * 4);
        UNSCOPED_INFO(std::format("packing {}: the lock readback differs from the resolve image", int(form.packing)));
        REQUIRE(memory.destination == ledger.destination);
        registry.Landed(7);
        UNSCOPED_INFO("a Both range needs no readback");
        REQUIRE(lock(memory, registry, copy).empty());
        ++checked;
    }
    std::cout << "A lock readback through the lock hook reproduces two RGBA8 tiles and the resolve image in " << checked << " encoder forms\n";
}

} // namespace

// Converted LOD 0 depth releases with its resolve version.
void depthConversion()
{
    using Swizzle = std::array<std::uint8_t, 4>;
    // census-0011: both world harvests fetch 1280x720 depth, levels 0..10; the
    // 1:1 consumer samples converted base at LOD 0 or below.
    constexpr std::array<std::uint32_t, 6> kWorld{ 0x8A004802, 0x1D0F0096, 0x0059E4FF, 0x00A81690, 0x00000283, 0x1D488A18 };
    auto                                   depth = storedIn(wholeCopy(1280, 720, GuestResolvePacking::Depth24Stencil8Words, 0x1D0F0000), 2);
    depth.destFetch[5] |= 0x800;
    ResolveSurfaceRegistry registry;
    produce(registry, 7, depth);
    const auto world = bound(registry, kWorld);
    UNSCOPED_INFO("the world k_24_8 fetch binds the converted texture");
    REQUIRE((world && world->mark == 7 && world->texture.frameTextureId == ResolveSurfaceDepthTexture(7)));
    UNSCOPED_INFO("the converted texture is the base level as R32Float");
    REQUIRE((world->texture.layout.format == NativeDrawReplayTextureFormat::R32Float && world->texture.layout.width == 1280 && world->texture.layout.height == 720 &&
             world->texture.layout.mipLevels == 1));
    // Selectors 0xB48 are {0, 1, 5, 5}: depth through red and green, then ones.
    UNSCOPED_INFO("the converted texture maps selectors to its RRRR host view");
    REQUIRE(world->texture.swizzle == Swizzle{ 0, 0, 5, 5 });

    std::size_t form   = kResolveSurfaceFormConditions;
    auto        higher = kWorld;
    higher[4]          = (higher[4] & ~(15U << 2)) | (1U << 2);
    UNSCOPED_INFO("a minimum level past the base is a mips refusal");
    REQUIRE((missed(registry, higher, true, &form) == NativeResolvedTextureMiss::Form && form == 10));
    auto unpaged = kWorld;
    unpaged[5] &= 0xFFF;
    unpaged[4] = (unpaged[4] & ~(15U << 2)) | (1U << 2);
    UNSCOPED_INFO("without a mip page the SDK clamps the minimum level to the base");
    REQUIRE(bound(registry, unpaged).has_value());
    auto huge = kWorld;
    huge[4]   = (huge[4] & ~(15U << 2)) | (15U << 2);
    UNSCOPED_INFO("a minimum level clamped to the smallest level is a mips refusal");
    REQUIRE((missed(registry, huge, true, &form) == NativeResolvedTextureMiss::Form && form == 10));
    // Other views keep the full rule: a level past the base needs it uploaded.
    auto bytes = kWorld;
    bytes[1]   = (bytes[1] & ~0xFFU) | (2U << 6) | 6;
    UNSCOPED_INFO("a k_8_8_8_8 fetch of the same chain stays a mips refusal");
    REQUIRE((missed(registry, bytes, true, &form) == NativeResolvedTextureMiss::Form && form == 10));
    bytes[5] &= 0xFFF;
    UNSCOPED_INFO("the k_8_8_8_8 base-only fetch binds the depth words");
    REQUIRE(bound(registry, bytes).has_value());

    const auto import = registry.Import(ResolveSurfaceDepthTexture(7));
    UNSCOPED_INFO("the converted import is its own R32Float texture under the depth key");
    REQUIRE((import && import->id == ResolveSurfaceDepthTexture(7) && import->format == NativeDrawReplayTextureFormat::R32Float && import->persistentKey == ResolveSurfaceDepthKey(7) &&
             import->importedWords == NativeGuestWordPacking::None && import->width == 1280 && import->height == 720 && import->imported));
    const auto words = registry.Import(ResolveSurfaceTexture(7));
    UNSCOPED_INFO("the depth words stay importable beside it");
    REQUIRE((words && words->format == NativeDrawReplayTextureFormat::R32Uint && words->persistentKey == ResolveSurfaceKey(7)));
    const auto surface = registry.FrameSurface(7, 0x77);
    UNSCOPED_INFO("the depth resolve hands the store its conversion key");
    REQUIRE((surface && surface->key == ResolveSurfaceKey(7) && surface->depthKey == ResolveSurfaceDepthKey(7)));

    const auto color = wholeCopy(64, 64, GuestResolvePacking::Color8888Words, 0x1A000000);
    produce(registry, 8, color);
    UNSCOPED_INFO("color words have no converted texture");
    REQUIRE((!registry.Import(ResolveSurfaceDepthTexture(8)) && registry.FrameSurface(8, 0x88)->depthKey == 0));
    UNSCOPED_INFO("the converted id is a surface id of its own, under its own key");
    REQUIRE((ResolveSurfaceDepthTexture(7) != ResolveSurfaceTexture(7) && IsResolveSurfaceTexture(ResolveSurfaceDepthTexture(7)) && ResolveSurfaceDepthKey(7) != ResolveSurfaceKey(7)));

    // A covering resolve is the next version: the converted texture ends
    // with its version and releases its key with the words' key.
    produce(registry, 9, depth);
    const auto next = bound(registry, kWorld);
    UNSCOPED_INFO("the next version binds its own converted texture");
    REQUIRE((next && next->mark == 9 && next->version == 2 && next->texture.frameTextureId == ResolveSurfaceDepthTexture(9)));
    UNSCOPED_INFO("the ended converted import stays until its key is released");
    REQUIRE(registry.Import(ResolveSurfaceDepthTexture(7)).has_value());
    const auto held = registry.TakeReleases([](std::uint64_t id)
                                            {
                                                return id == ResolveSurfaceDepthTexture(7);
                                            });
    UNSCOPED_INFO("a draw still binding the converted texture holds both keys back");
    REQUIRE(held.empty());
    const auto keys = registry.TakeReleases({});
    UNSCOPED_INFO("the released version takes its converted key with it");
    REQUIRE((keys == std::vector<std::uint64_t>{ ResolveSurfaceKey(7), ResolveSurfaceDepthKey(7) } && !registry.Import(ResolveSurfaceDepthTexture(7)) && !registry.Import(ResolveSurfaceTexture(7))));

    // The counters an owner run reads: binds, and each consumer shader once.
    registry.TakeCounts();
    registry.CountDepthView();
    registry.CountDepthView();
    UNSCOPED_INFO("a consumer shader is noted once");
    REQUIRE((registry.NoteDepthConsumer(0x1262103F2F06927FULL, 0x8250C768) && !registry.NoteDepthConsumer(0x1262103F2F06927FULL, 0x8250C768)));
    UNSCOPED_INFO("the binds count as depth views");
    REQUIRE(registry.TakeCounts().depthViews == 2);
    const auto consumers = registry.TakeNewDepthConsumers();
    UNSCOPED_INFO("a new consumer is handed over once");
    REQUIRE((consumers.size() == 1 && consumers[0].first == 0x1262103F2F06927FULL && consumers[0].second == 0x8250C768 && registry.TakeNewDepthConsumers().empty()));
    UNSCOPED_INFO("the timing line carries the depth views and submits");
    REQUIRE(FormatResolveSurfaceCounts({}).find(" surface_depth_views=0 surface_submits=0 ") != std::string::npos);
    std::cout << "The world k_24_8 fetch binds the converted base level, which ends and releases with its version\n";
}

TEST_CASE("resolve_surface_registry", "[ctest][resolve-surface-registry]")
{
    versioning();
    overlap();
    coveredDrops();
    owedTiles();
    bracketCommit();
    transitionBarrier();
    tiles();
    tileAdmission();
    pendingOrder();
    tileOrder();
    skippedImage();
    pendingEndsOlder();
    capacity();
    unlockEndsEntry();
    freeDrops();
    heldEndings();
    views();
    formatViews();
    depthConversion();
    lockRoundTrip();
}
