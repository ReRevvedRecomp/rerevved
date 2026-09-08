#pragma once

#include <cstdint>

#include <unique_unit_rules.h>

namespace rerevved::unique_unit_rules
{

bool TryEvaluate(ReRevvedCivilizationId              civilization,
                 ReRevvedUnitTypeId                  baseUnitType,
                 ReRevvedUnitIdentityId              identity,
                 ReRevvedUniqueUnitScalarProperty    property,
                 int32_t                             nativeValue,
                 ReRevvedUniqueUnitScalarEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unique_unit_rules
