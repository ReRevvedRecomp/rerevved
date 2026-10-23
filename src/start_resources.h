#pragma once

#include "resource_settings.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rerevved::start_resources
{

constexpr int                kMapStride         = 40;
constexpr size_t             kTileCapacity      = kMapStride * kMapStride;
constexpr size_t             kPlayers           = 5;
constexpr int                kAbsent            = -1;
constexpr int                kOxen              = 0;
constexpr int                kOak               = 9;
constexpr int                kFish              = 18;
constexpr std::array<int, 3> kBalancedResources = { kOxen, kOak, kFish };

struct Coord
{
    int  a                              = -1;
    int  b                              = -1;
    bool operator==(const Coord&) const = default;
};

struct Bounds
{
    int  a = 0;
    int  b = 0;
    bool Valid() const;
    bool Contains(Coord coordinate) const;
};

struct Tile
{
    uint16_t flags                         = 0;
    int8_t   terrain                       = 0;
    bool     operator==(const Tile&) const = default;
};

using Tiles = std::array<Tile, kTileCapacity>;

struct Player
{
    Coord anchor;
    int   civilization = -1;
    bool  liveSettler  = false;
};

struct Snapshot
{
    Bounds                       bounds;
    Tiles                        tiles;
    std::array<Player, kPlayers> players;
    uint32_t                     seed = 0;
};

constexpr size_t Index(Coord coordinate)
{
    return static_cast<size_t>(kMapStride * coordinate.a + coordinate.b);
}

// sub_82CF1558, context -1: absence is -1; ordinal zero is Oxen.
int  Identity(Tile tile, Coord coordinate);
bool IsLiveSettler(uint32_t player, uint8_t owner, uint8_t type, uint32_t flags, Coord unit, Coord anchor);

struct TerrainMix
{
    std::array<int, 7> terrain{};
    int                other = 0;
    int                count = 0;
    bool               CompleteRing() const;
    bool               operator==(const TerrainMix&) const = default;
};

struct Placement
{
    Coord tile;
    bool  needsOverride                      = false;
    bool  operator==(const Placement&) const = default;
};

struct Trio
{
    bool                     valid = false;
    TerrainMix               mix;
    std::array<Placement, 3> placements; // Oxen, Oak, Fish.
    bool                     operator==(const Trio&) const = default;
};

// Each resource selects an absent tile of its existing terrain, or skips it.
Trio SelectTrio(const Snapshot& snapshot, size_t player);

enum class Status
{
    NotRun,
    NoSettler,
    InvalidAnchor,
    IncompleteRing,
    Verified,
    ReadbackFailed,
};

enum class SkipReason
{
    None,
    ExistingResource,
    ForbiddenFlags,
    UnsupportedTerrain,
    Overlap,
};

struct LegendaryPlacement
{
    Coord      tile;
    int8_t     terrain                                     = -1;
    int        ordinal                                     = kAbsent;
    int        finalIdentity                               = kAbsent;
    bool       needsOverride                               = false;
    SkipReason skip                                        = SkipReason::None;
    bool       operator==(const LegendaryPlacement&) const = default;
};

struct PlayerResult
{
    Status                            status = Status::NotRun;
    Coord                             anchor;
    Trio                              trio;
    std::array<int, 3>                finalIdentities = { kAbsent, kAbsent, kAbsent };
    std::array<LegendaryPlacement, 8> legendary;
};

struct Result
{
    resource_settings::Mode            mode             = resource_settings::Mode::Normal;
    bool                               captured         = false;
    bool                               rolledBack       = false;
    bool                               rollbackVerified = false;
    bool                               hardFailure      = false; // Report the failure; derived state is untrusted.
    std::array<PlayerResult, kPlayers> players;
};

// The only guest boundary. Normal and Abundant never invoke it.
class World
{
public:
    virtual ~World()                                          = default;
    virtual bool Capture(Snapshot& snapshot)                  = 0;
    virtual Tile Read(Coord coordinate)                       = 0;
    virtual void WriteFlags(Coord coordinate, uint16_t flags) = 0;
    virtual bool Refresh(Coord coordinate)                    = 0;
};

Result      Apply(resource_settings::FrozenSelection& selection, World& world);
const char* Name(Status status);
const char* Name(SkipReason reason);

} // namespace rerevved::start_resources
