#include "terrain_yield_rules_registry.h"

#include <cstdint>

#include <rex/ppc.h>

namespace
{

void applyBaseValue(PPCRegister&          guestTerrain,
                    PPCRegister&          baseValue,
                    TerrainYieldComponent component)
{
    TerrainId terrain = TERRAIN_UNKNOWN;
    if (!rerevved::terrain_yield_rules::TryMapGuestTerrain(
            guestTerrain.s32, terrain))
    {
        return;
    }

    TerrainYieldEvaluation evaluation{};
    if (rerevved::terrain_yield_rules::TryEvaluate(
            terrain, component, baseValue.s32, evaluation) &&
        (evaluation.replacementCount != 0 ||
         evaluation.additiveCount != 0) &&
        (evaluation.statusFlags &
         TERRAIN_YIELD_EVALUATION_OVERFLOW) == 0)
    {
        baseValue.s64 = evaluation.finalValue;
    }
}

} // namespace

void ApplyTerrainTradeBase(PPCRegister& guestTerrain,
                           PPCRegister& baseValue)
{
    applyBaseValue(guestTerrain,
                   baseValue,
                   TERRAIN_YIELD_TRADE);
}

void ApplyTerrainProductionBase(PPCRegister& guestTerrain,
                                PPCRegister& baseValue)
{
    applyBaseValue(guestTerrain,
                   baseValue,
                   TERRAIN_YIELD_PRODUCTION);
}

void ApplyTerrainFoodBase(PPCRegister& guestTerrain,
                          PPCRegister& baseValue)
{
    applyBaseValue(guestTerrain,
                   baseValue,
                   TERRAIN_YIELD_FOOD);
}
