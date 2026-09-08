#pragma once

#include <cstdint>

#include <unit_movement_rules.h>

namespace rerevved::unit_movement_rules
{

bool TryEvaluate(ReRevvedCivilizationId          civilization,
                 ReRevvedUnitTypeId              baseUnitType,
                 ReRevvedUnitIdentityId          identity,
                 int32_t                         nativeValue,
                 ReRevvedUnitMovementEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unit_movement_rules
