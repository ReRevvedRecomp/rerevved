#pragma once

#include <cstdint>

#include <unit_effect_rules.h>

namespace rerevved::unit_effect_rules
{

bool TryEvaluate(CivilizationId        civilization,
                 UnitTypeId            baseUnitType,
                 UnitIdentityId        identity,
                 UnitEffectId          effect,
                 int32_t               nativeLevel,
                 UnitEffectEvaluation& evaluation);

// Returns the title-owned storage mask for a named native special upgrade.
// This remains an internal bridge so the public ABI exposes named effects
// instead of guest bit numbers.
bool TryGetNativeSpecialUpgradeMask(UnitEffectId effect,
                                    uint32_t&    mask);

void ResetForTests();

} // namespace rerevved::unit_effect_rules
