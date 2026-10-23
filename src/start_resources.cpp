#include "start_resources.h"

#include "resource_identity_overrides.h"

#include <algorithm>
#include <utility>

namespace rerevved::start_resources
{
namespace
{

namespace overrides = resource_identity_overrides;

// Default city scan: sub_82D00C78, 0x82D00C84..0x82D00CD0 and
// 0x82D014EC..0x82D01784; tables 0x82F7A0B8/0x82F7A168, entries 0..7.
constexpr std::array<Coord, 8> kWorkableOffsets = {
    Coord{ 0, 1 }, { 1, 1 }, { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 }
};

uint32_t next(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

struct Ring
{
    std::array<Coord, 8> coordinates;
    size_t               count = 0;
    TerrainMix           mix;
};

Ring ring(const Snapshot& snapshot, size_t player)
{
    Ring result;
    if (player >= kPlayers || !snapshot.bounds.Contains(snapshot.players[player].anchor))
    {
        return result;
    }
    const Coord anchor = snapshot.players[player].anchor;
    for (Coord offset : kWorkableOffsets)
    {
        // The city scan wraps a at 0x82D01534..0x82D01550 and clips b.
        const Coord coordinate{ (anchor.a + offset.a + snapshot.bounds.a) % snapshot.bounds.a, anchor.b + offset.b };
        if (snapshot.bounds.Contains(coordinate) &&
            std::find(result.coordinates.begin(), result.coordinates.begin() + result.count, coordinate) == result.coordinates.begin() + result.count)
        {
            result.coordinates[result.count++] = coordinate;
            const int terrain                  = snapshot.tiles[Index(coordinate)].terrain;
            if (terrain >= 0 && terrain <= 6)
            {
                ++result.mix.terrain[terrain];
            }
            else
            {
                ++result.mix.other;
            }
            ++result.mix.count;
        }
    }
    return result;
}

int effectiveIdentity(Tile tile, Coord coordinate, const overrides::Snapshot& snapshot)
{
    const uint64_t sum = overrides::CoordinateSum(snapshot, static_cast<uint32_t>(Index(coordinate)), tile.terrain, static_cast<uint64_t>(coordinate.a + coordinate.b));
    return Identity(tile, { 0, static_cast<int>(sum) });
}

} // namespace

bool Bounds::Valid() const
{
    return a == 32 && b == 32;
}

bool Bounds::Contains(Coord coordinate) const
{
    return Valid() && coordinate.a >= 0 && coordinate.a < a && coordinate.b >= 0 && coordinate.b < b;
}

int Identity(Tile tile, Coord coordinate)
{
    if ((tile.flags & 0x20) == 0 || (tile.flags & 0x1001) != 0 || tile.terrain < 0 || tile.terrain > 6 ||
        coordinate.a < 0 || coordinate.b < 0)
    {
        return kAbsent;
    }
    return overrides::kIdentities[tile.terrain][(coordinate.a + coordinate.b) % (tile.terrain == 1 ? 4 : 3)];
}

bool IsLiveSettler(uint32_t player, uint8_t owner, uint8_t type, uint32_t flags, Coord unit, Coord anchor)
{
    return player < kPlayers && owner == player && type == 0 && (flags & 0x80000000u) == 0 && unit == anchor;
}

bool TerrainMix::CompleteRing() const
{
    return count == 8;
}

Trio SelectTrio(const Snapshot& snapshot, size_t player)
{
    Trio result;
    Ring area  = ring(snapshot, player);
    result.mix = area.mix;
    if (!result.mix.CompleteRing())
    {
        return result;
    }
    result.valid   = true;
    uint32_t state = snapshot.seed ^ (0x9E3779B9u * static_cast<uint32_t>(player + 1));
    for (size_t remaining = area.count; remaining > 1; --remaining)
    {
        std::swap(area.coordinates[remaining - 1], area.coordinates[next(state) % remaining]);
    }
    constexpr std::array<int8_t, 3> terrains = { 1, 3, 0 };
    for (size_t resource = 0; resource < kBalancedResources.size(); ++resource)
    {
        // Native matches win even when an override candidate appears first.
        for (bool nativeOnly : { true, false })
        {
            for (Coord coordinate : area.coordinates)
            {
                const Tile tile = snapshot.tiles[Index(coordinate)];
                if (coordinate == snapshot.players[player].anchor || tile.terrain != terrains[resource] || (tile.flags & 0x1021) != 0)
                {
                    continue;
                }
                Tile present = tile;
                present.flags |= 0x20;
                const bool nativeMatch = Identity(present, coordinate) == kBalancedResources[resource];
                if (nativeOnly && !nativeMatch)
                {
                    continue;
                }
                result.placements[resource] = { coordinate, !nativeMatch };
                break;
            }
            if (snapshot.bounds.Contains(result.placements[resource].tile))
            {
                break;
            }
        }
    }
    return result;
}

Result Apply(resource_settings::FrozenSelection& selection, World& world)
{
    Result     result;
    const auto mode = selection.Consume();
    if (!mode)
    {
        return result;
    }
    overrides::Clear();
    result.mode = *mode;
    if (*mode != resource_settings::Mode::Balanced && *mode != resource_settings::Mode::Legendary)
    {
        return result;
    }

    Snapshot original;
    if (!world.Capture(original) || !original.bounds.Valid())
    {
        return result;
    }
    result.captured                        = true;
    Snapshot                       planned = original;
    overrides::Snapshot            records;
    std::array<int, kTileCapacity> chosen;
    chosen.fill(kAbsent);
    for (size_t player = 0; player < kPlayers; ++player)
    {
        auto& report    = result.players[player];
        report.anchor   = original.players[player].anchor;
        report.trio.mix = ring(original, player).mix;
        if (!original.bounds.Contains(report.anchor))
        {
            report.status = Status::InvalidAnchor;
            continue;
        }
        if (!original.players[player].liveSettler)
        {
            report.status = Status::NoSettler;
            continue;
        }
        if (*mode == resource_settings::Mode::Legendary)
        {
            const Ring area = ring(original, player);
            if (!area.mix.CompleteRing())
            {
                report.status = Status::IncompleteRing;
                continue;
            }
            report.status  = Status::Verified;
            uint32_t state = original.seed ^ (0x9E3779B9u * static_cast<uint32_t>(player + 1));
            // Draw once per ring tile so exclusions do not shift later choices.
            for (size_t i = 0; i < area.count; ++i)
            {
                const Coord coordinate = area.coordinates[i];
                const auto  index      = Index(coordinate);
                const Tile  tile       = original.tiles[index];
                auto&       placement  = report.legendary[i];
                placement.tile         = coordinate;
                placement.terrain      = tile.terrain;
                const uint32_t draw    = next(state);
                if ((tile.flags & 0x1001) != 0)
                {
                    placement.skip = SkipReason::ForbiddenFlags;
                }
                else if (tile.terrain < 0 || tile.terrain > 6)
                {
                    placement.skip = SkipReason::UnsupportedTerrain;
                }
                else if ((tile.flags & 0x20) != 0)
                {
                    placement.skip    = SkipReason::ExistingResource;
                    placement.ordinal = Identity(tile, coordinate);
                }
                else if (chosen[index] != kAbsent)
                {
                    placement.skip    = SkipReason::Overlap;
                    placement.ordinal = chosen[index];
                }
                else
                {
                    // Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/docs/reference/resource-placement.md#legendary-identity-choices
                    constexpr int pools[7][3] = { { 18, -1, -1 }, { 0, 19, 20 }, { 8, 13, -1 }, { 9, -1, -1 }, { 1, -1, -1 }, { 12, -1, -1 }, { 15, 21, -1 } };
                    constexpr int counts[7]   = { 1, 3, 2, 1, 1, 1, 2 };
                    placement.ordinal         = pools[tile.terrain][draw % counts[tile.terrain]];
                    Tile& present             = planned.tiles[index];
                    present.flags |= 0x20;
                    placement.needsOverride = Identity(present, coordinate) != placement.ordinal;
                    chosen[index]           = placement.ordinal;
                    if (placement.needsOverride)
                    {
                        records.push_back({ static_cast<uint16_t>(index), static_cast<uint8_t>(placement.ordinal) });
                    }
                }
            }
            continue;
        }
        report.trio = SelectTrio(original, player);
        if (!report.trio.valid)
        {
            report.status = Status::IncompleteRing;
            continue;
        }
        report.status = Status::Verified;
        for (size_t resource = 0; resource < kBalancedResources.size(); ++resource)
        {
            const Placement& placement = report.trio.placements[resource];
            if (!original.bounds.Contains(placement.tile))
            {
                continue;
            }
            planned.tiles[Index(placement.tile)].flags |= 0x20;
            if (placement.needsOverride)
            {
                records.push_back({ static_cast<uint16_t>(Index(placement.tile)), static_cast<uint8_t>(kBalancedResources[resource]) });
            }
        }
    }
    // Readers must see the chosen identity before presence and fresh-object refresh.
    if (!overrides::Publish(records))
    {
        result.hardFailure = true;
        return result;
    }
    const auto                      published = overrides::Current();
    std::array<Coord, kPlayers * 8> changed;
    size_t                          changedCount = 0;
    for (int a = 0; a < original.bounds.a; ++a)
    {
        for (int b = 0; b < original.bounds.b; ++b)
        {
            const Coord coordinate{ a, b };
            if (original.tiles[Index(coordinate)].flags != planned.tiles[Index(coordinate)].flags)
            {
                changed[changedCount++] = coordinate;
                world.WriteFlags(coordinate, planned.tiles[Index(coordinate)].flags);
            }
        }
    }
    bool valid = true;
    for (size_t i = 0; i < changedCount; ++i)
    {
        valid = world.Refresh(changed[i]) && valid;
    }
    Tiles finalTiles = original.tiles;
    for (int a = 0; a < original.bounds.a; ++a)
    {
        for (int b = 0; b < original.bounds.b; ++b)
        {
            const Coord coordinate{ a, b };
            finalTiles[Index(coordinate)] = world.Read(coordinate);
            valid                         = finalTiles[Index(coordinate)] == planned.tiles[Index(coordinate)] && valid;
        }
    }
    for (auto& report : result.players)
    {
        if (report.status != Status::Verified)
        {
            continue;
        }
        if (*mode == resource_settings::Mode::Legendary)
        {
            for (auto& placement : report.legendary)
            {
                placement.finalIdentity = effectiveIdentity(finalTiles[Index(placement.tile)], placement.tile, *published);
                if (placement.skip == SkipReason::None || placement.skip == SkipReason::Overlap || placement.skip == SkipReason::ExistingResource)
                {
                    valid = placement.finalIdentity == placement.ordinal && valid;
                }
            }
            continue;
        }
        for (size_t resource = 0; resource < kBalancedResources.size(); ++resource)
        {
            const Coord coordinate = report.trio.placements[resource].tile;
            if (original.bounds.Contains(coordinate))
            {
                report.finalIdentities[resource] = effectiveIdentity(finalTiles[Index(coordinate)], coordinate, *published);
                valid                            = report.finalIdentities[resource] == kBalancedResources[resource] && valid;
            }
        }
    }
    if (valid)
    {
        return result;
    }

    result.rolledBack = true;
    overrides::Clear();
    std::array<Coord, kTileCapacity> restored;
    size_t                           restoredCount = 0;
    for (int a = 0; a < original.bounds.a; ++a)
    {
        for (int b = 0; b < original.bounds.b; ++b)
        {
            const Coord coordinate{ a, b };
            if (finalTiles[Index(coordinate)].flags != original.tiles[Index(coordinate)].flags)
            {
                restored[restoredCount++] = coordinate;
                world.WriteFlags(coordinate, original.tiles[Index(coordinate)].flags);
            }
        }
    }
    bool restoredOk = true;
    for (size_t i = 0; i < restoredCount; ++i)
    {
        restoredOk = world.Refresh(restored[i]) && restoredOk;
    }
    for (int a = 0; a < original.bounds.a; ++a)
    {
        for (int b = 0; b < original.bounds.b; ++b)
        {
            const Coord coordinate{ a, b };
            finalTiles[Index(coordinate)] = world.Read(coordinate);
            restoredOk                    = finalTiles[Index(coordinate)] == original.tiles[Index(coordinate)] && restoredOk;
        }
    }
    result.rollbackVerified = restoredOk;
    result.hardFailure      = !restoredOk;
    if (result.hardFailure)
    {
        // A failed callback cannot justify more callbacks or any terrain store.
        for (int a = 0; a < original.bounds.a; ++a)
        {
            for (int b = 0; b < original.bounds.b; ++b)
            {
                const Coord coordinate{ a, b };
                if (finalTiles[Index(coordinate)].flags != original.tiles[Index(coordinate)].flags)
                {
                    world.WriteFlags(coordinate, original.tiles[Index(coordinate)].flags);
                }
            }
        }
        for (int a = 0; a < original.bounds.a; ++a)
        {
            for (int b = 0; b < original.bounds.b; ++b)
            {
                const Coord coordinate{ a, b };
                finalTiles[Index(coordinate)] = world.Read(coordinate);
            }
        }
    }
    for (auto& report : result.players)
    {
        if (report.status == Status::Verified)
        {
            report.status = Status::ReadbackFailed;
            if (*mode == resource_settings::Mode::Legendary)
            {
                for (auto& placement : report.legendary)
                {
                    placement.finalIdentity = Identity(finalTiles[Index(placement.tile)], placement.tile);
                }
                continue;
            }
            for (size_t resource = 0; resource < kBalancedResources.size(); ++resource)
            {
                const Coord coordinate = report.trio.placements[resource].tile;
                if (original.bounds.Contains(coordinate))
                {
                    report.finalIdentities[resource] = Identity(finalTiles[Index(coordinate)], coordinate);
                }
            }
        }
    }
    return result;
}

const char* Name(Status status)
{
    switch (status)
    {
        case Status::NoSettler:
            return "no live Settler at anchor";
        case Status::InvalidAnchor:
            return "invalid anchor";
        case Status::IncompleteRing:
            return "expected eight ring tiles";
        case Status::Verified:
            return "verified";
        case Status::ReadbackFailed:
            return "final readback failed";
        default:
            return "not run";
    }
}

const char* Name(SkipReason reason)
{
    switch (reason)
    {
        case SkipReason::ExistingResource:
            return "existing resource";
        case SkipReason::ForbiddenFlags:
            return "forbidden flags 0x1001";
        case SkipReason::UnsupportedTerrain:
            return "unsupported terrain";
        case SkipReason::Overlap:
            return "assigned by earlier player";
        default:
            return "none";
    }
}

} // namespace rerevved::start_resources
