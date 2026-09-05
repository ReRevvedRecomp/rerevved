#include "terrain_yield_rules_registry.h"

#include <cstdint>

#include <rex/ppc.h>

namespace
{

void ApplyBaseValue(PPCRegister&                  guest_terrain,
                    PPCRegister&                  base_value,
                    ReRevvedTerrainYieldComponent component)
{
    ReRevvedTerrainId terrain = REREVVED_TERRAIN_UNKNOWN;
    if (!rerevved::terrain_yield_rules::TryMapGuestTerrain(
            guest_terrain.s32, terrain))
    {
        return;
    }

    ReRevvedTerrainYieldEvaluation evaluation{};
    if (rerevved::terrain_yield_rules::TryEvaluate(
            terrain, component, base_value.s32, evaluation) &&
        (evaluation.replacement_count != 0 ||
         evaluation.additive_count != 0) &&
        (evaluation.status_flags &
         REREVVED_TERRAIN_YIELD_EVALUATION_OVERFLOW) == 0)
    {
        base_value.s64 = evaluation.final_value;
    }
}

} // namespace

void ReRevvedApplyTerrainTradeBase(PPCRegister& guest_terrain,
                                   PPCRegister& base_value)
{
    ApplyBaseValue(guest_terrain,
                   base_value,
                   REREVVED_TERRAIN_YIELD_TRADE);
}

void ReRevvedApplyTerrainProductionBase(PPCRegister& guest_terrain,
                                        PPCRegister& base_value)
{
    ApplyBaseValue(guest_terrain,
                   base_value,
                   REREVVED_TERRAIN_YIELD_PRODUCTION);
}

void ReRevvedApplyTerrainFoodBase(PPCRegister& guest_terrain,
                                  PPCRegister& base_value)
{
    ApplyBaseValue(guest_terrain,
                   base_value,
                   REREVVED_TERRAIN_YIELD_FOOD);
}
