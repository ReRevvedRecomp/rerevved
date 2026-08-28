#pragma once

#include <cstdint>

#include <unique_unit_rules.h>

namespace rerevved::unique_unit_rules
{

bool TryEvaluate(ReRevvedCivilizationId              civilization,
                 ReRevvedUnitTypeId                  base_unit_type,
                 ReRevvedUnitIdentityId              identity,
                 ReRevvedUniqueUnitScalarProperty    property,
                 int32_t                             native_value,
                 ReRevvedUniqueUnitScalarEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unique_unit_rules
