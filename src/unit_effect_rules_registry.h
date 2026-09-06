#pragma once

#include <cstdint>

#include <unit_effect_rules.h>

namespace rerevved::unit_effect_rules
{

bool TryEvaluate(ReRevvedCivilizationId        civilization,
                 ReRevvedUnitTypeId            base_unit_type,
                 ReRevvedUnitIdentityId        identity,
                 ReRevvedUnitEffectId          effect,
                 int32_t                       native_level,
                 ReRevvedUnitEffectEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unit_effect_rules
