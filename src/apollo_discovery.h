#pragma once

// Guest reads stay in apollo_discovery_hooks.cpp so host tests need no guest.

#include <array>
#include <cstdint>
#include <vector>

namespace rerevved::apollo_discovery
{

// Apollo is normalized Wonder 17; once complete, technologies 1..46 query as owned.
constexpr int32_t  kApolloWonder     = 17;
constexpr int32_t  kFirstTechnology  = 1;
constexpr int32_t  kLastTechnology   = 46;
constexpr int32_t  kTechnologyWords  = 48;
constexpr int32_t  kPlayers          = 8;
constexpr int32_t  kCities           = 128;
constexpr uint32_t kPendingCityItems = 0x1100; // city+0x24: Wonder (0x1000)
// or spaceship part (0x100) awaits its native consumer.
constexpr uint32_t kIdleMarkerValue = 0xFFFFFFFFu;

using OwnershipWords = std::array<uint32_t, kTechnologyWords>;

// W = U32[0x830EDC48 + 4 * t]: zero means no first-discovery claimant
constexpr bool IsUnclaimed(uint32_t word) noexcept
{
    return word == 0;
}

inline std::vector<int32_t> EligibleTechnologies(const OwnershipWords& words)
{
    std::vector<int32_t> eligible;
    for (int32_t technology = kFirstTechnology; technology <= kLastTechnology; ++technology)
    {
        if (IsUnclaimed(words[technology]))
        {
            eligible.push_back(technology);
        }
    }
    return eligible;
}

// Unresolved presentation: completion 0x830E3348[p], context 0x830E3328[p], recursive marker 0x830E3430.
struct StageWords
{
    uint32_t completionMarker = 0;
    uint32_t context          = 0;
    uint32_t recursiveMarker  = 0;
    uint32_t cityFlags        = 0;
};

constexpr bool IsIdleMarker(uint32_t value) noexcept
{
    return value == 0 || value == kIdleMarkerValue;
}

constexpr bool DrainReady(const StageWords& stage) noexcept
{
    return IsIdleMarker(stage.completionMarker) && IsIdleMarker(stage.context) && IsIdleMarker(stage.recursiveMarker) &&
           (stage.cityFlags & kPendingCityItems) == 0;
}

} // namespace rerevved::apollo_discovery
