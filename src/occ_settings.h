#pragma once

#include <cstdint>
#include <memory>

#include <unique_era_abilities.h>

namespace rerevved::occ
{

constexpr int32_t kSettlers          = 0;
constexpr int32_t kMinimumPopulation = 33;
constexpr int32_t kCitySlots         = 128;
constexpr int32_t kPopulationCap     = 31;

static_assert(kMinimumPopulation > kPopulationCap && kMinimumPopulation <= 127);

struct Policy
{
    bool    enabled;
    int32_t minimumPopulation;
};

// Only new-game and load ingress publish a new immutable policy.
void                          FreezeForGame(bool standardRules = false);
void                          RestoreForGame(Policy policy);
std::shared_ptr<const Policy> ForGame() noexcept;
EraAbilityId                  EffectiveEraAbility(const Policy& policy, CivilizationId civilization, UnlockEra era, EraAbilityId publishedAbility) noexcept;
const char*                   NationSelectEraText(CivilizationId civilization, UnlockEra era);
int32_t                       CandidateThreshold(const Policy& policy, int32_t type, int32_t nativeThreshold) noexcept;
int32_t                       CompletionThreshold(const Policy& policy, int32_t type, int32_t nativeThreshold) noexcept;
bool                          RejectAiChoice(const Policy& policy, int32_t prefix, int32_t total, int32_t ordinal) noexcept;

struct City
{
    int32_t owner;
    int32_t population;
};

class RewardCities
{
public:
    virtual ~RewardCities()                                         = default;
    virtual City Read(int32_t index)                                = 0;
    virtual void WritePopulation(int32_t index, int32_t population) = 0;
    virtual void Refresh(int32_t index)                             = 0;
    virtual void Observe(int32_t index, City before, City after)    = 0;
};

// True skips the native Settler branch, including its nonapplying preview.
bool ApplyGoldReward(const Policy& policy, int32_t owner, bool applying, RewardCities& cities);

} // namespace rerevved::occ
