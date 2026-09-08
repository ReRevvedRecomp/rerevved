#pragma once

#include <cstdint>

#include <unit_production_cost_rules.h>

namespace rerevved::unit_production_cost_rules
{

bool TryEvaluate(ReRevvedCivilizationId                civilization,
                 ReRevvedUnitTypeId                    baseUnitType,
                 ReRevvedUnitIdentityId                identity,
                 ReRevvedUnitProductionCostEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unit_production_cost_rules
