#include "rush_cost.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>

namespace
{

} // namespace

TEST_CASE("rush_cost_producer_consumer", "[ctest][rush-cost]")
{
    int32_t cost = 0;
    UNSCOPED_INFO("multiply remaining production before division");
    CHECK((rerevved::TryCalculateRushCost(3, 2, 10, cost) && cost == 15));
    UNSCOPED_INFO("round fractional gold up after partial production");
    CHECK((rerevved::TryCalculateRushCost(3, 2, 2, cost) && cost == 3));
    UNSCOPED_INFO("preserve the divisor-one control path");
    CHECK((rerevved::TryCalculateRushCost(3, 1, 10, cost) && cost == 30));

    const rerevved::RushCostRepair zeroRepair{ true, 0, 6, 10, 15 };
    int32_t                        bought = -1;
    int32_t                        after  = -1;
    UNSCOPED_INFO("apply exactly ten production from zero");
    CHECK((rerevved::TryCoordinateRushProduction(
               zeroRepair, 0, 6, 15, 0, bought, after) &&
           bought == 10 && after == 10));

    const rerevved::RushCostRepair partialRepair{ true, 188, 6, 2, 3 };
    UNSCOPED_INFO("reject producer-consumer cost disagreement");
    CHECK(!rerevved::TryCoordinateRushProduction(
        partialRepair, 188, 6, 2, 8, bought, after));
    UNSCOPED_INFO("apply exactly the coordinated remaining production");
    CHECK((rerevved::TryCoordinateRushProduction(
               partialRepair, 188, 6, 3, 8, bought, after) &&
           bought == 2 && after == 10));
}
