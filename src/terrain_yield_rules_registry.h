#pragma once

#include <cstdint>

#include <terrain_yield_rules.h>

namespace rerevved::terrain_yield_rules
{

bool TryEvaluate(ReRevvedTerrainId terrain,
                 ReRevvedTerrainYieldComponent component,
                 int32_t native_value,
                 ReRevvedTerrainYieldEvaluation& evaluation);

bool TryMapGuestTerrain(int32_t guest_terrain, ReRevvedTerrainId& terrain);

bool TryAddChecked(int64_t accumulator, int64_t value, int64_t& result);

void ResetForTests();

} // namespace rerevved::terrain_yield_rules
