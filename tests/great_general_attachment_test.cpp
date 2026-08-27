#include "great_general_attachment.h"

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

rerevved::GreatGeneralUnitState LiveUnit(int32_t player,
                                         int32_t unit,
                                         uint8_t type,
                                         int16_t x,
                                         int16_t y,
                                         int16_t link)
{
    return { player, unit, 0, type, 0, x, y, link };
}

} // namespace

int main()
{
    const auto carrier    = LiveUnit(2, 4, 13, 22, 13, -1);
    const auto general    = LiveUnit(2, 12, 30, 23, 13, 4);
    int16_t    repaired_x = 0;
    int16_t    repaired_y = 0;
    Require(rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, general, repaired_x, repaired_y) &&
                repaired_x == 22 && repaired_y == 13,
            "copy the final carrier tile to its live General");

    auto already_attached = general;
    already_attached.x    = carrier.x;
    Require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, already_attached, repaired_x, repaired_y),
            "leave an attached General unchanged");

    auto deleted_general = general;
    deleted_general.slot = 0xFF;
    Require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, deleted_general, repaired_x, repaired_y),
            "do not resurrect a deleted General");

    auto dead_carrier  = carrier;
    dead_carrier.flags = 0x80000000;
    Require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                dead_carrier, general, repaired_x, repaired_y),
            "do not follow a dead carrier");

    auto ordinary_unit = general;
    ordinary_unit.type = 29;
    Require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, ordinary_unit, repaired_x, repaired_y),
            "require the Great General type discriminator");

    auto wrong_link         = general;
    wrong_link.carrier_link = 5;
    Require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, wrong_link, repaired_x, repaired_y),
            "require the live carrier link");

    auto other_player   = general;
    other_player.player = 3;
    Require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, other_player, repaired_x, repaired_y),
            "never cross player ownership");

    return failures == 0 ? 0 : 1;
}
