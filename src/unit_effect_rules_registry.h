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

// Returns the title-owned storage mask for a named native special upgrade.
// This remains an internal bridge so the public ABI exposes named effects
// instead of guest bit numbers.
bool TryGetNativeSpecialUpgradeMask(ReRevvedUnitEffectId effect,
                                    uint32_t&            mask);

void ResetForTests();

} // namespace rerevved::unit_effect_rules
