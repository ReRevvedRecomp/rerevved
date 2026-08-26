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
                                 uint32_t              city_offset,
                                 int32_t               item,
                                 int32_t               submitted_cost,
                                 int32_t               production_before,
                                 int32_t&              production_bought,
                                 int32_t&              production_after)
{
    if (!repair.valid || repair.city_offset != city_offset ||
        repair.item != item || repair.cost != submitted_cost ||
        repair.remaining < 0 || production_before < 0)
    {
        return false;
    }

    const int64_t after =
        static_cast<int64_t>(production_before) + repair.remaining;
    if (after > std::numeric_limits<int32_t>::max())
    {
        return false;
    }

    production_bought = repair.remaining;
    production_after  = static_cast<int32_t>(after);
    return true;
}

} // namespace rerevved
