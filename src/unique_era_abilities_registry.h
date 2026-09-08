#pragma once

#include <unique_era_abilities.h>

namespace rerevved::unique_era_abilities
{

bool TryEvaluate(ReRevvedCivilizationId                  civilization,
                 ReRevvedUniqueEraUnlockEra              unlock_era,
                 ReRevvedUniqueEraAbilityId              native_ability,
                 ReRevvedUniqueEraAbilityCellEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unique_era_abilities
