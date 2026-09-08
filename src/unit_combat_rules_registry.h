#pragma once

#include <unit_combat_rules.h>

namespace rerevved::unit_combat_rules
{

bool TryEvaluate(ReRevvedCivilizationId        civilization,
                 ReRevvedUnitTypeId            baseUnitType,
                 ReRevvedUnitIdentityId        identity,
                 ReRevvedTerrainId             terrain,
                 ReRevvedUnitCombatProperty    property,
                 ReRevvedUnitCombatEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unit_combat_rules
