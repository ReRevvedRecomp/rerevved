#pragma once

#include <cstdint>

#include <unique_unit_rules.h>

namespace rerevved::unique_unit_rules
{

bool TryEvaluate(CivilizationId              civilization,
                 UnitTypeId                  baseUnitType,
                 UnitIdentityId              identity,
                 UniqueUnitScalarProperty    property,
                 int32_t                     nativeValue,
                 UniqueUnitScalarEvaluation& evaluation);

void ResetForTests();

} // namespace rerevved::unique_unit_rules
