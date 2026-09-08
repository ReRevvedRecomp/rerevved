#include "terrain_yield_rules_registry.h"

#include <cstdint>

#include <rex/ppc.h>

namespace
{

void applyBaseValue(PPCRegister&                  guestTerrain,
                    PPCRegister&                  baseValue,
                    ReRevvedTerrainYieldComponent component)
{
    ReRevvedTerrainId terrain = REREVVED_TERRAIN_UNKNOWN;
    if (!rerevved::terrain_yield_rules::TryMapGuestTerrain(
            guestTerrain.s32, terrain))
    {
        return;
    }

    ReRevvedTerrainYieldEvaluation evaluation{};
    if (rerevved::terrain_yield_rules::TryEvaluate(
            terrain, component, baseValue.s32, evaluation) &&
        (evaluation.replacementCount != 0 ||
         evaluation.additiveCount != 0) &&
        (evaluation.statusFlags &
         REREVVED_TERRAIN_YIELD_EVALUATION_OVERFLOW) == 0)
    {
        baseValue.s64 = evaluation.finalValue;
    }
}

} // namespace

void ReRevvedApplyTerrainTradeBase(PPCRegister& guestTerrain,
                                   PPCRegister& baseValue)
{
    applyBaseValue(guestTerrain,
                   baseValue,
                   REREVVED_TERRAIN_YIELD_TRADE);
}

void ReRevvedApplyTerrainProductionBase(PPCRegister& guestTerrain,
                                        PPCRegister& baseValue)
{
    applyBaseValue(guestTerrain,
                   baseValue,
                   REREVVED_TERRAIN_YIELD_PRODUCTION);
}

void ReRevvedApplyTerrainFoodBase(PPCRegister& guestTerrain,
                                  PPCRegister& baseValue)
{
    applyBaseValue(guestTerrain,
                   baseValue,
                   REREVVED_TERRAIN_YIELD_FOOD);
}
