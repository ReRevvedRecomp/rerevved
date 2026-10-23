#pragma once

#include "resource_identity_overrides.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>

namespace rerevved::resource_identity_trace
{

constexpr uint64_t kEventCap = 4096;

class EventBudget
{
public:
    // cap+1 emits the single truncation marker; zero suppresses later events.
    uint64_t Next() noexcept
    {
        return sequence_ <= kEventCap ? ++sequence_ : 0;
    }

private:
    uint64_t sequence_ = 0;
};

inline bool Matches(const resource_identity_overrides::Snapshot& snapshot, uint64_t index) noexcept
{
    for (const auto record : snapshot)
    {
        if (index == record.tileIndex)
        {
            return true;
        }
    }
    return false;
}

enum class EventKind
{
    Decision,
    Return,
};

struct CoordinateMatch
{
    uint64_t index        = 0;
    uint64_t wrappedIndex = 0;
    int32_t  boundsA      = 0;
    int32_t  boundsB      = 0;
    bool     boundsRead   = false;
    bool     outOfBounds  = false;
    bool     exact        = false;
    bool     wrapped      = false;

    bool ShouldTrace() const noexcept
    {
        return exact || wrapped || outOfBounds;
    }
};

inline CoordinateMatch MatchCoordinates(const resource_identity_overrides::Snapshot& snapshot, uint64_t a, uint64_t b, int32_t boundsA, int32_t boundsB) noexcept
{
    CoordinateMatch result;
    result.index      = uint64_t{ static_cast<uint32_t>(a) * uint32_t{ 40 } } + b;
    result.exact      = Matches(snapshot, result.index);
    result.boundsA    = boundsA;
    result.boundsB    = boundsB;
    result.boundsRead = boundsA > 0 && boundsA <= 40 && boundsB > 0 && boundsB <= 40;
    if (result.boundsRead)
    {
        const auto signedA = std::bit_cast<int32_t>(static_cast<uint32_t>(a));
        const auto signedB = std::bit_cast<int32_t>(static_cast<uint32_t>(b));
        result.outOfBounds = signedA < 0 || signedA >= boundsA || signedB < 0 || signedB >= boundsB;
        const auto wrap    = [](int32_t value, int32_t bound)
        {
            return (value % bound + bound) % bound;
        };
        result.wrappedIndex = uint64_t{ 40 } * wrap(signedA, boundsA) + wrap(signedB, boundsB);
        result.wrapped      = Matches(snapshot, result.wrappedIndex);
    }
    return result;
}

class EventHistory
{
public:
    bool ShouldEmitSample(uint32_t site, uint64_t caller, uint64_t a, uint64_t b, uint64_t city, uint64_t player, std::array<uint64_t, 4> values)
    {
        const auto key = std::tuple{ site, caller, a, b, city, player, values };
        if (samples_.contains(key))
        {
            return false;
        }
        if (samples_.size() >= kEventCap)
        {
            return true;
        }
        samples_.insert(key);
        return true;
    }

    bool ShouldEmit(EventKind kind, uint64_t caller, uint64_t index, uint64_t player, uint64_t value, std::string_view decision)
    {
        const auto key   = std::tuple{ caller, index, player };
        auto       found = entries_.find(key);
        if (found == entries_.end())
        {
            // Each new key consumes an event, so the process cap bounds the map.
            if (entries_.size() >= kEventCap)
            {
                // Only the truncation marker can remain for an untracked key.
                return true;
            }
            found = entries_.try_emplace(key).first;
        }
        auto& last = kind == EventKind::Decision ? found->second.decision : found->second.returned;
        if (last && last->value == value && last->decision == decision)
        {
            return false;
        }
        last = Value{ value, std::string(decision) };
        return true;
    }

    void Reset() noexcept
    {
        entries_.clear();
        samples_.clear();
    }

private:
    struct Value
    {
        uint64_t    value;
        std::string decision;
    };

    struct LastEvents
    {
        std::optional<Value> decision;
        std::optional<Value> returned;
    };

    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, LastEvents>                                            entries_;
    std::set<std::tuple<uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, std::array<uint64_t, 4>>> samples_;
};

enum class YieldOwner : uint32_t
{
    Food       = 0x82CF1CE8,
    Production = 0x82CF1AF0,
    Trade      = 0x82CF17C8,
};

class YieldScope
{
public:
    YieldScope(YieldOwner owner, uint64_t caller, uint64_t a, uint64_t b, uint64_t city, uint64_t player) noexcept;
    ~YieldScope() noexcept;
    YieldScope(const YieldScope&)            = delete;
    YieldScope& operator=(const YieldScope&) = delete;
    void        Returned(int32_t value) noexcept;

    YieldScope*     previous = nullptr;
    YieldOwner      owner;
    uint64_t        caller;
    uint64_t        a;
    uint64_t        b;
    uint64_t        city;
    uint64_t        player;
    CoordinateMatch coordinate;
    bool            enabled         = false;
    bool            retained        = false;
    uint64_t        resourceOrdinal = UINT64_MAX;
    size_t          snapshotSize    = 0;
    uintptr_t       snapshotId      = 0;
};

class CityScope
{
public:
    CityScope(uint64_t caller, uint64_t city, uint64_t mode) noexcept;
    ~CityScope() noexcept;
    CityScope(const CityScope&)            = delete;
    CityScope& operator=(const CityScope&) = delete;

    CityScope* previous = nullptr;
    uint64_t   caller;
    uint64_t   city;
    uint64_t   mode;
    bool       enabled = false;
    bool       matched = false;
};

void FoodStep(uint32_t site, const char* inputName, uint64_t total, uint64_t input, uint64_t auxiliary, uint64_t resolvedPlayer) noexcept;
void CityTotals(uint64_t stack) noexcept;
void OverlayFood(uint64_t a, uint64_t b, uint64_t food, uint64_t manager) noexcept;
void OverlayCache(uint64_t a, uint64_t b, uint64_t icon, uint64_t food, uint64_t manager) noexcept;

class ReaderScope
{
public:
    ReaderScope(uint64_t lr, uint64_t a, uint64_t b, uint64_t player) noexcept;
    ~ReaderScope();
    ReaderScope(const ReaderScope&)            = delete;
    ReaderScope& operator=(const ReaderScope&) = delete;
    void         Returned(int32_t ordinal) noexcept;

    ReaderScope* previous         = nullptr;
    bool         enabled          = false;
    bool         matched          = false;
    bool         retained         = false;
    uint64_t     caller           = 0;
    uint64_t     coordinateA      = 0;
    uint64_t     coordinateB      = 0;
    uint64_t     player           = 0;
    uint64_t     tileIndex        = 0;
    size_t       snapshotSize     = 0;
    uintptr_t    snapshotId       = 0;
    uint64_t     decisionSequence = 0;
    const char*  decision         = "native return";
};

void Decision(const resource_identity_overrides::Snapshot& snapshot, uint64_t index, uint64_t before, uint64_t after, uint64_t terrainBase, const char* decision) noexcept;

} // namespace rerevved::resource_identity_trace
