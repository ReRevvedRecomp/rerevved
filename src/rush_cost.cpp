#include "rush_cost.h"

#include <cstdint>
#include <limits>

namespace rerevved
{

bool TryCalculateRushCost(int32_t  multiplier,
                          int32_t  divisor,
                          int32_t  remaining,
                          int32_t& cost)
{
    if (multiplier <= 0 || divisor <= 0 || remaining < 0)
    {
        return false;
    }

    const int64_t product = static_cast<int64_t>(multiplier) * remaining;
    const int64_t value   = (product + divisor - 1) / divisor;
    if (value > std::numeric_limits<int32_t>::max())
    {
        return false;
    }

    cost = static_cast<int32_t>(value);
    return true;
}

bool TryCoordinateRushProduction(const RushCostRepair& repair,
                                 uint32_t              cityOffset,
                                 int32_t               item,
                                 int32_t               submittedCost,
                                 int32_t               productionBefore,
                                 int32_t&              productionBought,
                                 int32_t&              productionAfter)
{
    if (!repair.valid || repair.cityOffset != cityOffset ||
        repair.item != item || repair.cost != submittedCost ||
        repair.remaining < 0 || productionBefore < 0)
    {
        return false;
    }

    const int64_t after =
        static_cast<int64_t>(productionBefore) + repair.remaining;
    if (after > std::numeric_limits<int32_t>::max())
    {
        return false;
    }

    productionBought = repair.remaining;
    productionAfter  = static_cast<int32_t>(after);
    return true;
}

} // namespace rerevved
