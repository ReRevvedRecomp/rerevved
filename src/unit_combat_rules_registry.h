#pragma once

#include <unit_combat_rules.h>

namespace rerevved::unit_combat_rules
{

bool TryEvaluate(CivilizationId        civilization,
                 UnitTypeId            baseUnitType,
                 UnitIdentityId        identity,
                 TerrainId             terrain,
                 UnitCombatProperty    property,
                 UnitCombatEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unit_combat_rules
