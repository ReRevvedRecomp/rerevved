#include "great_general_attachment.h"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{

int failures = 0;

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

rerevved::GreatGeneralUnitState liveUnit(int32_t player,
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
    const auto carrier   = liveUnit(2, 4, 13, 22, 13, -1);
    const auto general   = liveUnit(2, 12, 30, 23, 13, 4);
    int16_t    repairedX = 0;
    int16_t    repairedY = 0;
    require(rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, general, repairedX, repairedY) &&
                repairedX == 22 && repairedY == 13,
            "copy the final carrier tile to its live General");

    auto alreadyAttached = general;
    alreadyAttached.x    = carrier.x;
    require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, alreadyAttached, repairedX, repairedY),
            "leave an attached General unchanged");

    auto deletedGeneral = general;
    deletedGeneral.slot = 0xFF;
    require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, deletedGeneral, repairedX, repairedY),
            "do not resurrect a deleted General");

    auto deadCarrier  = carrier;
    deadCarrier.flags = 0x80000000;
    require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                deadCarrier, general, repairedX, repairedY),
            "do not follow a dead carrier");

    auto ordinaryUnit = general;
    ordinaryUnit.type = 29;
    require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, ordinaryUnit, repairedX, repairedY),
            "require the Great General type discriminator");

    auto wrongLink        = general;
    wrongLink.carrierLink = 5;
    require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, wrongLink, repairedX, repairedY),
            "require the live carrier link");

    auto otherPlayer   = general;
    otherPlayer.player = 3;
    require(!rerevved::TryPlanGreatGeneralCoordinateRepair(
                carrier, otherPlayer, repairedX, repairedY),
            "never cross player ownership");

    return failures == 0 ? 0 : 1;
}
