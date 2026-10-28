#include "start_resources.h"

#include "resource_identity_overrides.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <iterator>
#include <string_view>
#include <vector>

namespace
{

using namespace rerevved::start_resources;
using rerevved::resource_settings::FrozenSelection;
namespace overrides = rerevved::resource_identity_overrides;

constexpr std::array<Coord, 8> kOffsets = {
    Coord{ 0, 1 }, { 1, 1 }, { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 }
};

Coord neighbor(Coord anchor, size_t index)
{
    return { (anchor.a + kOffsets[index].a + 32) % 32, anchor.b + kOffsets[index].b };
}

void setRing(Snapshot& snapshot, Coord anchor, const std::array<int8_t, 8>& terrains)
{
    for (size_t i = 0; i < kOffsets.size(); ++i)
    {
        const Coord coordinate = neighbor(anchor, i);
        if (snapshot.bounds.Contains(coordinate))
        {
            snapshot.tiles[Index(coordinate)] = { 0, terrains[i] };
        }
    }
}

Snapshot nativeStart(bool needOverrides = true, Coord anchor = { 10, 10 })
{
    Snapshot snapshot;
    snapshot.bounds = { 32, 32 };
    snapshot.seed   = 123456u;
    snapshot.tiles.fill({ 0, 2 });
    snapshot.players[0] = { anchor, 10, true };
    setRing(snapshot, anchor, needOverrides ? std::array<int8_t, 8>{ 1, 0, 0, 3, 1, 3, 2, 2 } : std::array<int8_t, 8>{ 1, 3, 0, 1, 3, 2, 2, 0 });
    return snapshot;
}

int effectiveIdentity(Tile tile, Coord coordinate)
{
    const auto snapshot = overrides::Current();
    const auto sum      = overrides::CoordinateSum(*snapshot, static_cast<uint32_t>(Index(coordinate)), tile.terrain, static_cast<uint64_t>(coordinate.a + coordinate.b));
    return Identity(tile, { 0, static_cast<int>(sum) });
}

class FakeWorld final : public World
{
public:
    Snapshot           state                 = nativeStart();
    int                captures              = 0;
    int                reads                 = 0;
    int                writes                = 0;
    int                refreshes             = 0;
    int                corruptAtRefresh      = 0;
    int                corruptAgainAtRefresh = 0;
    int                failAtRefresh         = 0;
    uint16_t           corruptMask           = 0x20;
    uint16_t           corruptAgainMask      = 0x2;
    Coord              corruptTile;
    Coord              corruptAgainTile;
    bool               corruptTerrain          = false;
    bool               invalidRead             = false;
    size_t             recordsBeforeFirstWrite = 0;
    std::vector<Coord> written;
    std::vector<Coord> refreshed;
    std::vector<char>  events;

    bool Capture(Snapshot& snapshot) override
    {
        ++captures;
        snapshot = state;
        return true;
    }

    Tile Read(Coord coordinate) override
    {
        ++reads;
        events.push_back('r');
        if (!state.bounds.Contains(coordinate))
        {
            invalidRead = true;
            return {};
        }
        return state.tiles[Index(coordinate)];
    }

    void WriteFlags(Coord coordinate, uint16_t flags) override
    {
        if (writes == 0)
        {
            recordsBeforeFirstWrite = overrides::Current()->size();
        }
        ++writes;
        events.push_back('w');
        written.push_back(coordinate);
        state.tiles[Index(coordinate)].flags = flags;
    }

    bool Refresh(Coord coordinate) override
    {
        ++refreshes;
        events.push_back('f');
        refreshed.push_back(coordinate);
        if (refreshes == corruptAtRefresh)
        {
            state.tiles[Index(corruptTile)].flags ^= corruptMask;
            if (corruptTerrain)
            {
                state.tiles[Index(corruptTile)].terrain = 6;
            }
        }
        if (refreshes == corruptAgainAtRefresh)
        {
            state.tiles[Index(corruptAgainTile)].flags ^= corruptAgainMask;
        }
        return refreshes != failAtRefresh;
    }
};

void testIdentityAndSettler()
{
    constexpr int identities[7][4] = {
        { 10, 16, 18, -1 }, { 0, 20, 19, 11 }, { 8, 13, 14, -1 }, { 5, 9, 17, -1 }, { 1, 3, 6, -1 }, { 2, 4, 12, -1 }, { 7, 15, 21, -1 }
    };
    for (int8_t terrain = 0; terrain <= 6; ++terrain)
    {
        for (int remainder = 0; remainder < (terrain == 1 ? 4 : 3); ++remainder)
        {
            UNSCOPED_INFO("all native terrain/remainder identities match the matrix");
            CHECK(Identity({ 0x20, terrain }, { 0, remainder }) == identities[terrain][remainder]);
        }
    }
    const overrides::Snapshot forced = { { 410, 0 } };
    UNSCOPED_INFO("Oxen fixture publishes");
    CHECK(overrides::Publish(forced));
    for (uint16_t flags : { 0, 0x21, 0x1020 })
    {
        UNSCOPED_INFO("overrides preserve native presence and forbidden-flag gates");
        CHECK(effectiveIdentity({ flags, 1 }, { 10, 10 }) == kAbsent);
    }
    UNSCOPED_INFO("Oxen ordinal zero remains valid");
    CHECK(effectiveIdentity({ 0x20, 1 }, { 10, 10 }) == 0);
    overrides::Clear();
    const Coord anchor{ 10, 10 };
    UNSCOPED_INFO("slot zero can hold a live Settler");
    CHECK(IsLiveSettler(0, 0, 0, 0, anchor, anchor));
    CHECK(IsLiveSettler(5, 5, 0, 0, anchor, anchor));
    CHECK(IsLiveSettler(6, 6, 0, 0, anchor, anchor));
    CHECK_FALSE(IsLiveSettler(7, 7, 0, 0, anchor, anchor));
    UNSCOPED_INFO("owner must match the player pool");
    CHECK(!IsLiveSettler(0, 1, 0, 0, anchor, anchor));
    UNSCOPED_INFO("a Militia is not a Settler");
    CHECK(!IsLiveSettler(0, 0, 7, 0, anchor, anchor));
    UNSCOPED_INFO("killed Settlers are rejected");
    CHECK(!IsLiveSettler(0, 0, 0, 0x80000000, anchor, anchor));
    UNSCOPED_INFO("unit coordinates must match the final arrays");
    CHECK(!IsLiveSettler(0, 0, 0, 0, { 11, 10 }, anchor));
}

void testNoOpAndOrdering()
{
    for (int mode : { -1, 0, 2, 4 })
    {
        UNSCOPED_INFO("stale snapshot fixture publishes");
        CHECK(overrides::Publish(overrides::Snapshot{ { 410, 0 } }));
        FakeWorld       world;
        const auto      before = world.state.tiles;
        FrozenSelection selection(mode);
        Apply(selection, world);
        UNSCOPED_INFO("inactive modes perform no guest operations");
        CHECK((world.captures == 0 && world.reads == 0 && world.writes == 0 && world.refreshes == 0 && world.state.tiles == before));
        UNSCOPED_INFO("inactive modes publish an empty snapshot");
        CHECK(overrides::Current()->empty());
    }
    FakeWorld       world;
    FrozenSelection selection(1);
    const auto      result = Apply(selection, world);
    UNSCOPED_INFO("all three overridden identities verify");
    CHECK((result.players[0].status == Status::Verified && result.players[0].finalIdentities == kBalancedResources));
    UNSCOPED_INFO("all overrides publish before the first presence store");
    CHECK(world.recordsBeforeFirstWrite == 3);
    UNSCOPED_INFO("each addition refreshes once");
    CHECK((world.writes == 3 && world.refreshes == 3 && world.written == world.refreshed));
    UNSCOPED_INFO("all presence writes and refreshes precede readback");
    CHECK((world.events.size() > 6 && std::vector<char>(world.events.begin(), world.events.begin() + 7) == std::vector<char>{ 'w', 'w', 'w', 'f', 'f', 'f', 'r' }));
    const auto retained   = overrides::Current();
    const int  operations = world.captures + world.reads + world.writes + world.refreshes;
    Apply(selection, world);
    UNSCOPED_INFO("consumed selection neither rewrites the map nor clears its snapshot");
    CHECK((operations == world.captures + world.reads + world.writes + world.refreshes && overrides::Current() == retained));
}

void testSelectionAndTerrain()
{
    for (bool needOverrides : { false, true })
    {
        for (uint32_t seed = 0; seed < 32; ++seed)
        {
            FakeWorld world;
            world.state           = nativeStart(needOverrides);
            world.state.seed      = seed;
            const Snapshot before = world.state;
            const auto     chosen = SelectTrio(before, 0);
            UNSCOPED_INFO("choices are deterministic for each seed");
            CHECK((chosen.valid && chosen == SelectTrio(before, 0)));
            for (size_t resource = 0; resource < kBalancedResources.size(); ++resource)
            {
                const auto placement = chosen.placements[resource];
                UNSCOPED_INFO("native matches are preferred before override candidates");
                CHECK(placement.needsOverride == needOverrides);
                bool inRing = false;
                for (size_t i = 0; i < kOffsets.size(); ++i)
                {
                    inRing = inRing || placement.tile == neighbor(before.players[0].anchor, i);
                }
                UNSCOPED_INFO("all selected tiles are in the eight-tile ring and never its center");
                CHECK((inRing && placement.tile != before.players[0].anchor));
            }
            FrozenSelection selection(1);
            const auto      result = Apply(selection, world);
            UNSCOPED_INFO("both native and overridden trios verify");
            CHECK((result.players[0].status == Status::Verified && result.players[0].finalIdentities == kBalancedResources));
            UNSCOPED_INFO("native matches do not need override records");
            CHECK(overrides::Current()->size() == (needOverrides ? 3 : 0));
            for (size_t tile = 0; tile < kTileCapacity; ++tile)
            {
                UNSCOPED_INFO("every terrain byte stays unchanged");
                CHECK(before.tiles[tile].terrain == world.state.tiles[tile].terrain);
            }
            UNSCOPED_INFO("center flags stay unchanged");
            CHECK(world.state.tiles[Index(before.players[0].anchor)] == before.tiles[Index(before.players[0].anchor)]);
            FakeWorld repeat;
            repeat.state            = before;
            const auto      records = *overrides::Current();
            FrozenSelection sameSeed(1);
            Apply(sameSeed, repeat);
            UNSCOPED_INFO("the same seed and map produce identical writes and records");
            CHECK((world.written == repeat.written && world.state.tiles == repeat.state.tiles && records == *overrides::Current()));
        }
    }
}

void testMissingKindsAndExclusions()
{
    for (size_t missing = 0; missing < 3; ++missing)
    {
        FakeWorld                       world;
        constexpr std::array<int8_t, 3> terrains = { 1, 3, 0 };
        for (auto& tile : world.state.tiles)
        {
            if (tile.terrain == terrains[missing])
            {
                tile.terrain = 2;
            }
        }
        FrozenSelection selection(1);
        const auto      result = Apply(selection, world);
        UNSCOPED_INFO("missing terrain skips only its resource");
        CHECK((result.players[0].status == Status::Verified && world.writes == 2 && result.players[0].finalIdentities[missing] == kAbsent));
        for (size_t resource = 0; resource < 3; ++resource)
        {
            if (resource != missing)
            {
                UNSCOPED_INFO("other resource kinds still verify");
                CHECK(result.players[0].finalIdentities[resource] == kBalancedResources[resource]);
            }
        }
    }
    for (uint16_t flags : { 0x20, 0x21, 0x1020, 0x1000, 1 })
    {
        FakeWorld world;
        // Both Grassland candidates are protected, even if one is a native Oxen.
        for (Coord coordinate : { Coord{ 10, 11 }, Coord{ 10, 9 } })
        {
            world.state.tiles[Index(coordinate)].flags = flags;
        }
        const auto      before = world.state.tiles;
        FrozenSelection selection(1);
        const auto      result = Apply(selection, world);
        UNSCOPED_INFO("present resources and forbidden flags cannot be selected");
        CHECK((result.players[0].finalIdentities[0] == kAbsent && world.writes == 2));
        for (Coord coordinate : { Coord{ 10, 11 }, Coord{ 10, 9 } })
        {
            UNSCOPED_INFO("existing tile bytes are preserved");
            CHECK(world.state.tiles[Index(coordinate)] == before[Index(coordinate)]);
        }
    }
    FakeWorld existing;
    existing.state = nativeStart(false);
    const Coord oxen{ 11, 9 };
    existing.state.tiles[Index(oxen)].flags = 0x20;
    const auto      before                  = existing.state.tiles[Index(oxen)];
    FrozenSelection selection(1);
    const auto      result = Apply(selection, existing);
    UNSCOPED_INFO("a native matching resource is preserved and never selected as the new grant");
    CHECK((result.players[0].trio.placements[0].tile != oxen && existing.writes == 3 && existing.state.tiles[Index(oxen)] == before));
}

void testEdgesAndPlayers()
{
    FakeWorld missing;
    missing.state.players[0].liveSettler = false;
    FrozenSelection noSettler(1);
    UNSCOPED_INFO("a live Settler is required");
    CHECK((Apply(noSettler, missing).players[0].status == Status::NoSettler && missing.writes == 0));
    for (const Bounds bounds : { Bounds{ 0, 32 }, { 31, 32 }, { 32, 31 }, { 40, 40 } })
    {
        FakeWorld invalid;
        invalid.state.bounds = bounds;
        FrozenSelection unsupported(1);
        UNSCOPED_INFO("unsupported bounds fail before tile access with no overrides");
        CHECK((!Apply(unsupported, invalid).captured && invalid.reads == 0 && invalid.writes == 0 && overrides::Current()->empty()));
    }
    FakeWorld seam;
    seam.state = nativeStart(true, { 0, 10 });
    FrozenSelection wrap(1);
    UNSCOPED_INFO("the first coordinate wraps to physical row 31");
    CHECK((Apply(wrap, seam).players[0].status == Status::Verified && !seam.invalidRead));
    FakeWorld clipped;
    clipped.state = nativeStart(true, { 10, 0 });
    FrozenSelection clip(1);
    const auto      edge = Apply(clip, clipped);
    UNSCOPED_INFO("the second coordinate clips and rejects an incomplete ring");
    CHECK((edge.players[0].status == Status::IncompleteRing && edge.players[0].trio.mix.count == 5 && clipped.writes == 0 && !clipped.invalidRead));
    FakeWorld players;
    players.state.players[1] = { { 20, 20 }, 2, true };
    setRing(players.state, { 20, 20 }, { 1, 3, 0, 1, 3, 2, 2, 0 });
    FrozenSelection all(1);
    const auto      result = Apply(all, players);
    UNSCOPED_INFO("independent live starts each receive their chosen resources");
    CHECK((result.players[0].status == Status::Verified && result.players[1].status == Status::Verified && players.writes == 6));
    UNSCOPED_INFO("unassigned anchors are rejected");
    CHECK(result.players[2].status == Status::InvalidAnchor);
}

void testRollback()
{
    for (bool unrelatedDamage : { false, true })
    {
        FakeWorld  world;
        const auto before      = world.state.tiles;
        const auto chosen      = SelectTrio(world.state, 0);
        world.corruptAtRefresh = 3;
        world.corruptTile      = unrelatedDamage ? Coord{ 1, 1 } : chosen.placements[0].tile;
        world.corruptMask      = unrelatedDamage ? 0x2 : 0x20;
        FrozenSelection selection(1);
        const auto      result = Apply(selection, world);
        UNSCOPED_INFO("callback flag damage triggers exact verified rollback");
        CHECK((result.rolledBack && result.rollbackVerified && !result.hardFailure && result.players[0].status == Status::ReadbackFailed));
        UNSCOPED_INFO("rollback restores all flags and clears overrides");
        CHECK((world.state.tiles == before && overrides::Current()->empty()));
    }
    FakeWorld  again;
    const auto before           = again.state.tiles;
    again.corruptAtRefresh      = 3;
    again.corruptTile           = SelectTrio(again.state, 0).placements[0].tile;
    again.corruptAgainAtRefresh = 4;
    again.corruptAgainTile      = { 1, 1 };
    FrozenSelection selection(1);
    const auto      result = Apply(selection, again);
    UNSCOPED_INFO("rollback callback damage triggers raw flag restoration and final readback");
    CHECK((result.hardFailure && !result.rollbackVerified && again.state.tiles == before && again.events.back() == 'r' && overrides::Current()->empty()));
    FakeWorld failed;
    failed.failAtRefresh     = 3;
    const auto      original = failed.state.tiles;
    FrozenSelection refreshFailure(1);
    const auto      failure = Apply(refreshFailure, failed);
    UNSCOPED_INFO("refresh failure rolls back even when identities match");
    CHECK((failure.rolledBack && failure.rollbackVerified && failed.state.tiles == original));
    FakeWorld terrainDamage;
    terrainDamage.corruptAtRefresh = 3;
    terrainDamage.corruptTile      = SelectTrio(terrainDamage.state, 0).placements[0].tile;
    terrainDamage.corruptTerrain   = true;
    FrozenSelection damaged(1);
    const auto      hard = Apply(damaged, terrainDamage);
    UNSCOPED_INFO("unexpected callback terrain damage is reported without a compensating terrain store");
    CHECK((hard.hardFailure && !hard.rollbackVerified && terrainDamage.state.tiles[Index(terrainDamage.corruptTile)].terrain == 6));
}

void testLegendary()
{
    constexpr std::array<int8_t, 8>   terrains    = { 0, 1, 2, 3, 4, 5, 6, 0 };
    constexpr int                     pools[7][3] = { { 18, -1, -1 }, { 0, 19, 20 }, { 8, 13, -1 }, { 9, -1, -1 }, { 1, -1, -1 }, { 12, -1, -1 }, { 15, 21, -1 } };
    std::array<bool, 22>              seen{};
    bool                              choicesChanged = false;
    std::array<LegendaryPlacement, 8> first;
    for (uint32_t seed = 0; seed < 64; ++seed)
    {
        FakeWorld world;
        world.state.seed = seed;
        setRing(world.state, world.state.players[0].anchor, terrains);
        const auto      before = world.state;
        FrozenSelection selection(3);
        const auto      result = Apply(selection, world);
        UNSCOPED_INFO("Legendary fills all eight tiles without a terrain-mix gate");
        CHECK((result.players[0].status == Status::Verified && world.writes == 8 && world.refreshes == 8));
        UNSCOPED_INFO("Legendary publishes every override before writes");
        CHECK(world.recordsBeforeFirstWrite == overrides::Current()->size());
        size_t expectedRecords = 0;
        for (size_t i = 0; i < 8; ++i)
        {
            const auto& placement = result.players[0].legendary[i];
            UNSCOPED_INFO("Legendary uses the Balanced ring in scan order");
            REQUIRE((placement.tile == neighbor(before.players[0].anchor, i) && placement.terrain == terrains[i]));
            UNSCOPED_INFO("Legendary selects only its permitted terrain pool");
            CHECK(std::find(std::begin(pools[terrains[i]]), std::end(pools[terrains[i]]), placement.ordinal) != std::end(pools[terrains[i]]));
            UNSCOPED_INFO("every Legendary identity reads back through the hook math");
            CHECK((placement.finalIdentity == placement.ordinal && effectiveIdentity(world.state.tiles[Index(placement.tile)], placement.tile) == placement.ordinal));
            Tile present = before.tiles[Index(placement.tile)];
            present.flags |= 0x20;
            UNSCOPED_INFO("native remainder matches need no record");
            CHECK(placement.needsOverride == (Identity(present, placement.tile) != placement.ordinal));
            expectedRecords += placement.needsOverride;
            seen[placement.ordinal] = true;
        }
        UNSCOPED_INFO("only nonnative Legendary choices are recorded");
        CHECK(overrides::Current()->size() == expectedRecords);
        for (size_t tile = 0; tile < kTileCapacity; ++tile)
        {
            UNSCOPED_INFO("Legendary preserves every terrain byte");
            CHECK(world.state.tiles[tile].terrain == before.tiles[tile].terrain);
        }
        UNSCOPED_INFO("Legendary excludes the center");
        CHECK(world.state.tiles[Index(before.players[0].anchor)] == before.tiles[Index(before.players[0].anchor)]);
        const auto records = *overrides::Current();
        FakeWorld  repeat;
        repeat.state = before;
        FrozenSelection same(3);
        const auto      repeated = Apply(same, repeat);
        UNSCOPED_INFO("Legendary choices and writes repeat for the same seed and player");
        CHECK((result.players[0].legendary == repeated.players[0].legendary && records == *overrides::Current() && world.state.tiles == repeat.state.tiles));
        if (seed == 0)
        {
            first = result.players[0].legendary;
        }
        choicesChanged = choicesChanged || first != result.players[0].legendary;
    }
    UNSCOPED_INFO("different setup seeds can change Legendary choices");
    CHECK(choicesChanged);
    for (int ordinal : { 18, 0, 19, 20, 8, 13, 9, 1, 12, 15, 21 })
    {
        UNSCOPED_INFO("seed fixtures exercise every Legendary pool entry");
        CHECK(seen[ordinal]);
    }

    FakeWorld   excluded;
    const Coord anchor                                       = excluded.state.players[0].anchor;
    excluded.state.tiles[Index(neighbor(anchor, 0))]         = { 0x20, 0 };
    excluded.state.tiles[Index(neighbor(anchor, 1))].flags   = 0x1000;
    excluded.state.tiles[Index(neighbor(anchor, 2))].flags   = 1;
    excluded.state.tiles[Index(neighbor(anchor, 3))].terrain = 7;
    const auto      original                                 = excluded.state;
    FrozenSelection skip(3);
    const auto      skipped    = Apply(skip, excluded);
    const auto&     placements = skipped.players[0].legendary;
    UNSCOPED_INFO("Legendary records each exclusion reason");
    CHECK((placements[0].skip == SkipReason::ExistingResource && placements[1].skip == SkipReason::ForbiddenFlags && placements[2].skip == SkipReason::ForbiddenFlags && placements[3].skip == SkipReason::UnsupportedTerrain));
    UNSCOPED_INFO("existing native Dye is preserved even outside the Legendary Sea pool");
    CHECK((placements[0].ordinal == 10 && placements[0].finalIdentity == 10 && !placements[0].needsOverride));
    UNSCOPED_INFO("exclusions skip just their own tiles");
    CHECK((excluded.writes == 4 && skipped.players[0].status == Status::Verified));
    for (size_t i = 0; i < 4; ++i)
    {
        const auto index = Index(neighbor(anchor, i));
        UNSCOPED_INFO("existing and forbidden resources retain exact bytes");
        CHECK(original.tiles[index] == excluded.state.tiles[index]);
    }

    FakeWorld overlap;
    overlap.state.players[1] = overlap.state.players[0];
    FrozenSelection shared(3);
    const auto      sharedResult = Apply(shared, overlap);
    UNSCOPED_INFO("overlapping rings assign each physical tile once");
    CHECK((overlap.writes == 8 && overlap.refreshes == 8 && sharedResult.players[1].status == Status::Verified));
    for (size_t i = 0; i < 8; ++i)
    {
        UNSCOPED_INFO("the first deciding player's choice wins the overlap");
        CHECK((sharedResult.players[1].legendary[i].skip == SkipReason::Overlap && sharedResult.players[1].legendary[i].ordinal == sharedResult.players[0].legendary[i].ordinal));
    }
    FakeWorld playerOnly;
    playerOnly.state                        = overlap.state;
    playerOnly.state.tiles                  = nativeStart().tiles;
    playerOnly.state.players[0].liveSettler = false;
    FrozenSelection second(3);
    const auto      secondResult = Apply(second, playerOnly);
    UNSCOPED_INFO("Legendary requires each deciding player's live Settler");
    CHECK((secondResult.players[0].status == Status::NoSettler && secondResult.players[1].status == Status::Verified));
    FakeWorld secondRepeat;
    secondRepeat.state                        = nativeStart();
    secondRepeat.state.players[1]             = secondRepeat.state.players[0];
    secondRepeat.state.players[0].liveSettler = false;
    FrozenSelection samePlayer(3);
    const auto      secondRepeated = Apply(samePlayer, secondRepeat);
    UNSCOPED_INFO("the same player seed repeats independently of an earlier skipped player");
    CHECK(secondResult.players[1].legendary == secondRepeated.players[1].legendary);

    FakeWorld allPlayers;
    for (size_t player = 0; player < allPlayers.state.civilizationCount; ++player)
    {
        const int   position = 5 * static_cast<int>(player + 1);
        const Coord start{ position, position };
        allPlayers.state.players[player] = { start, static_cast<int>(player), true };
        setRing(allPlayers.state, start, terrains);
    }
    FrozenSelection all(3);
    const auto      allResult = Apply(all, allPlayers);
    UNSCOPED_INFO("all five players can receive eight Legendary additions through the shared path");
    CHECK((allPlayers.writes == 40 && allPlayers.refreshes == 40 && !allResult.rolledBack));
    for (size_t player = 0; player < allPlayers.state.civilizationCount; ++player)
    {
        UNSCOPED_INFO("all five Legendary player results verify");
        CHECK(allResult.players[player].status == Status::Verified);
    }
    FakeWorld water;
    setRing(water.state, water.state.players[0].anchor, { 0, 0, 0, 0, 0, 0, 0, 0 });
    FrozenSelection fish(3);
    const auto      waterResult = Apply(fish, water);
    UNSCOPED_INFO("extra water tiles all receive Fish without a land quota");
    CHECK((water.writes == 8 && std::all_of(waterResult.players[0].legendary.begin(), waterResult.players[0].legendary.end(), [](const auto& placement)
                                            {
                                                return placement.finalIdentity == kFish;
                                            })));
    FakeWorld clipped;
    clipped.state = nativeStart(true, { 10, 0 });
    FrozenSelection edge(3);
    UNSCOPED_INFO("Legendary retains the complete-ring anchor gate");
    CHECK((Apply(edge, clipped).players[0].status == Status::IncompleteRing && clipped.writes == 0));

    FakeWorld  failed;
    const auto before       = failed.state.tiles;
    failed.corruptAtRefresh = 8;
    failed.corruptTile      = neighbor(failed.state.players[0].anchor, 0);
    FrozenSelection broken(3);
    const auto      rollback = Apply(broken, failed);
    UNSCOPED_INFO("Legendary readback failure uses verified rollback and clears records");
    CHECK((rollback.rolledBack && rollback.rollbackVerified && !rollback.hardFailure && rollback.players[0].status == Status::ReadbackFailed && failed.state.tiles == before && overrides::Current()->empty()));
}

void testExpandedPlayers()
{
    for (const size_t count : { size_t{ 5 }, size_t{ 6 }, size_t{ 7 } })
    {
        for (const int mode : { 1, 3 })
        {
            FakeWorld world;
            world.state.civilizationCount = count;
            for (size_t player = 0; player < kPlayers; ++player)
            {
                const Coord anchor{ 4 + 7 * static_cast<int>(player % 4), 6 + 12 * static_cast<int>(player / 4) };
                world.state.players[player] = { anchor, static_cast<int>(player), true };
                setRing(world.state, anchor, { 1, 3, 0, 1, 3, 2, 2, 0 });
            }
            const auto      before = world.state;
            FrozenSelection selection(mode);
            const auto      result = Apply(selection, world);
            CHECK_FALSE(result.rolledBack);
            CHECK(world.writes == static_cast<int>(count) * (mode == 1 ? 3 : 8));
            for (size_t player = 0; player < count; ++player)
            {
                CHECK(result.players[player].status == Status::Verified);
                if (mode == 1)
                {
                    CHECK(result.players[player].finalIdentities == kBalancedResources);
                }
            }
            for (size_t player = count; player < kPlayers; ++player)
            {
                CHECK(result.players[player].status == Status::NotRun);
                CHECK_FALSE(SelectTrio(before, player).valid);
                for (size_t tile = 0; tile < kOffsets.size(); ++tile)
                {
                    const auto coordinate = neighbor(before.players[player].anchor, tile);
                    CHECK(world.state.tiles[Index(coordinate)] == before.tiles[Index(coordinate)]);
                }
            }
            const auto records = *overrides::Current();
            FakeWorld  repeated;
            repeated.state = before;
            FrozenSelection same(mode);
            const auto      second = Apply(same, repeated);
            CHECK(second.players[count - 1].trio == result.players[count - 1].trio);
            CHECK(second.players[count - 1].legendary == result.players[count - 1].legendary);
            CHECK(repeated.state.tiles == world.state.tiles);
            CHECK(*overrides::Current() == records);
        }
    }
}

} // namespace

TEST_CASE("start_resources", "[ctest][start-resources][local-fast]")
{
    testIdentityAndSettler();
    testNoOpAndOrdering();
    testSelectionAndTerrain();
    testMissingKindsAndExclusions();
    testEdgesAndPlayers();
    testRollback();
    testLegendary();
    testExpandedPlayers();
}
