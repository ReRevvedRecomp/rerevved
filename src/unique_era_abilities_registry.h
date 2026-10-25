#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include <unique_era_abilities.h>

namespace rerevved::unique_era_abilities
{

bool TryEvaluate(CivilizationId            civilization,
                 UnlockEra                 unlockEra,
                 EraAbilityId              nativeAbility,
                 EraAbilityCellEvaluation& evaluation);

int32_t                             NormalizeRequest(const EraAbilityReplacement* rule, EraAbilityReplacement& normalized) noexcept;
std::unique_lock<std::shared_mutex> AcquirePublicationLock();
// The caller holds the publication lock and passes normalized requests.
// failingOffset identifies the request if preparation throws or rejects it.
int32_t BuildCandidate(std::span<const EraAbilityReplacement> requests,
                       std::vector<EraAbilityReplacement>&    candidate,
                       size_t&                                failingOffset,
                       void (*checkpoint)()                             = nullptr,
                       std::span<const EraAbilityReplacement> removed   = {},
                       bool                                   exclusive = false);
void    CommitCandidate(std::vector<EraAbilityReplacement>& candidate) noexcept;

void ResetForTests();

} // namespace rerevved::unique_era_abilities
