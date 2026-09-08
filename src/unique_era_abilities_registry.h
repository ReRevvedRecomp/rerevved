#pragma once

#include <unique_era_abilities.h>

namespace rerevved::unique_era_abilities
{

bool TryEvaluate(CivilizationId            civilization,
                 UnlockEra                 unlockEra,
                 EraAbilityId              nativeAbility,
                 EraAbilityCellEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unique_era_abilities
