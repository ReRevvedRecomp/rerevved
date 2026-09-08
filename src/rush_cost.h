#pragma once

#include <cstdint>

namespace rerevved
{

struct RushCostRepair
{
    bool     valid       = false;
    uint32_t city_offset = 0;
    int32_t  item        = -1;
    int32_t  remaining   = 0;
    int32_t  cost        = 0;
};

bool TryCalculateRushCost(int32_t  multiplier,
                          int32_t  divisor,
                          int32_t  remaining,
                          int32_t& cost);

bool TryCoordinateRushProduction(const RushCostRepair& repair,
                                 uint32_t              city_offset,
                                 int32_t               item,
                                 int32_t               submitted_cost,
                                 int32_t               production_before,
                                 int32_t&              production_bought,
                                 int32_t&              production_after);

} // namespace rerevved
