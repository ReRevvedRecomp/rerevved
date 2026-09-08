#include "rush_cost.h"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{

int failures = 0;

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main()
{
    int32_t cost = 0;
    Require(rerevved::TryCalculateRushCost(3, 2, 10, cost) && cost == 15,
            "multiply remaining production before division");
    Require(rerevved::TryCalculateRushCost(3, 2, 2, cost) && cost == 3,
            "round fractional gold up after partial production");
    Require(rerevved::TryCalculateRushCost(3, 1, 10, cost) && cost == 30,
            "preserve the divisor-one control path");

    const rerevved::RushCostRepair zero_repair{ true, 0, 6, 10, 15 };
    int32_t                        bought = -1;
    int32_t                        after  = -1;
    Require(rerevved::TryCoordinateRushProduction(
                zero_repair, 0, 6, 15, 0, bought, after) &&
                bought == 10 && after == 10,
            "apply exactly ten production from zero");

    const rerevved::RushCostRepair partial_repair{ true, 188, 6, 2, 3 };
    Require(!rerevved::TryCoordinateRushProduction(
                partial_repair, 188, 6, 2, 8, bought, after),
            "reject producer-consumer cost disagreement");
    Require(rerevved::TryCoordinateRushProduction(
                partial_repair, 188, 6, 3, 8, bought, after) &&
                bought == 2 && after == 10,
            "apply exactly the coordinated remaining production");

    return failures == 0 ? 0 : 1;
}
