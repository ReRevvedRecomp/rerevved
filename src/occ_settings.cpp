#include "occ_settings.h"

#include <algorithm>
#include <mutex>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(one_city_challenge, false, "ReRevved", "One City Challenge for the next new game or save load");

namespace rerevved::occ
{
namespace
{

// Apple libc++ has no std::atomic<std::shared_ptr>.
std::mutex                    policyMutex;
std::shared_ptr<const Policy> frozenPolicy = std::make_shared<const Policy>(false, kMinimumPopulation);

} // namespace

void FreezeForGame(bool standardRules)
{
    auto            next = std::make_shared<const Policy>(!standardRules && REXCVAR_GET(one_city_challenge), kMinimumPopulation);
    std::lock_guard lock(policyMutex);
    frozenPolicy = std::move(next);
}

std::shared_ptr<const Policy> ForGame() noexcept
{
    std::lock_guard lock(policyMutex);
    return frozenPolicy;
}

void RestoreForGame(Policy policy)
{
    auto            next = std::make_shared<const Policy>(policy);
    std::lock_guard lock(policyMutex);
    frozenPolicy = std::move(next);
}

EraAbilityId EffectiveEraAbility(const Policy& policy, CivilizationId civilization, UnlockEra era, EraAbilityId publishedAbility) noexcept
{
    return policy.enabled && civilization == CIVILIZATION_MONGOLIAN && era == UNLOCK_ERA_ANCIENT
               ? ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING
               : publishedAbility;
}

const char* NationSelectEraText(CivilizationId civilization, UnlockEra era)
{
    // Civilization selection precedes the per-game freeze.
    return civilization == CIVILIZATION_MONGOLIAN && era == UNLOCK_ERA_ANCIENT && REXCVAR_GET(one_city_challenge)
               ? "Start with Horseback Riding"
               : nullptr;
}

int32_t CandidateThreshold(const Policy& policy, int32_t type, int32_t nativeThreshold) noexcept
{
    return policy.enabled && type == kSettlers ? std::max(nativeThreshold, policy.minimumPopulation) : nativeThreshold;
}

int32_t CompletionThreshold(const Policy& policy, int32_t type, int32_t nativeThreshold) noexcept
{
    // The native completion branch accepts P > K; the candidate branch accepts P >= T.
    return policy.enabled && type == kSettlers ? std::max(nativeThreshold, policy.minimumPopulation - 1) : nativeThreshold;
}

bool RejectAiChoice(const Policy& policy, int32_t prefix, int32_t total, int32_t ordinal) noexcept
{
    return policy.enabled && (prefix < 0 || total > 100 || prefix >= total || ordinal < prefix || ordinal >= total);
}

bool ApplyGoldReward(const Policy& policy, int32_t owner, bool applying, RewardCities& cities)
{
    if (!policy.enabled)
    {
        return false;
    }
    if (!applying)
    {
        return true;
    }
    for (int32_t index = 0; index < kCitySlots; ++index)
    {
        const City before = cities.Read(index);
        if (before.owner != owner || before.owner == -1 || before.population <= 0)
        {
            continue;
        }
        if (before.population < kPopulationCap)
        {
            cities.WritePopulation(index, before.population + 1);
            cities.Refresh(index);
        }
        cities.Observe(index, before, cities.Read(index));
    }
    return true;
}

} // namespace rerevved::occ
