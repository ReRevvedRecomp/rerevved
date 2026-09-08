#pragma once

#include <cstdint>

#include <terrain_yield_rules.h>

namespace rerevved::terrain_yield_rules
{

bool TryEvaluate(TerrainId               terrain,
                 TerrainYieldComponent   component,
                 int32_t                 nativeValue,
                 TerrainYieldEvaluation& evaluation);

bool TryMapGuestTerrain(int32_t guestTerrain, TerrainId& terrain);

bool TryAddChecked(int64_t accumulator, int64_t value, int64_t& result);

void ResetForTests();

} // namespace rerevved::terrain_yield_rules
