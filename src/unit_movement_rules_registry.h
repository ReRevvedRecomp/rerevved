#pragma once

#include <cstdint>

#include <unit_movement_rules.h>

namespace rerevved::unit_movement_rules
{

bool TryEvaluate(CivilizationId          civilization,
                 UnitTypeId              baseUnitType,
                 UnitIdentityId          identity,
                 int32_t                 nativeValue,
                 UnitMovementEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unit_movement_rules
