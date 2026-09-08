#pragma once

#include <unique_era_abilities.h>

namespace rerevved::unique_era_abilities
{

bool TryEvaluate(ReRevvedCivilizationId                  civilization,
                 ReRevvedUniqueEraUnlockEra              unlockEra,
                 ReRevvedUniqueEraAbilityId              nativeAbility,
                 ReRevvedUniqueEraAbilityCellEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unique_era_abilities
