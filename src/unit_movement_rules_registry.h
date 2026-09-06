#pragma once

#include <cstdint>

#include <unit_movement_rules.h>

namespace rerevved::unit_movement_rules
{

bool TryEvaluate(ReRevvedCivilizationId          civilization,
                 ReRevvedUnitTypeId              base_unit_type,
                 ReRevvedUnitIdentityId          identity,
                 int32_t                         native_value,
                 ReRevvedUnitMovementEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unit_movement_rules
