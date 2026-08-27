#pragma once

#include <cstdint>

namespace rerevved
{

struct GreatGeneralUnitState
{
    int32_t  player       = -1;
    int32_t  unit         = -1;
    uint8_t  slot         = 0xFF;
    uint8_t  type         = 0;
    uint32_t flags        = 0;
    int16_t  x            = 0;
    int16_t  y            = 0;
    int16_t  carrier_link = -1;
};

bool TryPlanGreatGeneralCoordinateRepair(
    const GreatGeneralUnitState& carrier,
    const GreatGeneralUnitState& general,
    int16_t&                     repaired_x,
    int16_t&                     repaired_y);

} // namespace rerevved
