#pragma once

#include <cstdint>

#include <unit_production_cost_rules.h>

namespace rerevved::unit_production_cost_rules
{

bool TryEvaluate(CivilizationId                civilization,
                 UnitTypeId                    baseUnitType,
                 UnitIdentityId                identity,
                 UnitProductionCostEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unit_production_cost_rules
