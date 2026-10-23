#include "great_general_attachment.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>

namespace
{

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

TEST_CASE("great_general_attachment", "[ctest][great-general-attachment]")
{
    const auto carrier   = liveUnit(2, 4, 13, 22, 13, -1);
    const auto general   = liveUnit(2, 12, 30, 23, 13, 4);
    int16_t    repairedX = 0;
    int16_t    repairedY = 0;
    UNSCOPED_INFO("copy the final carrier tile to its live General");
    CHECK((rerevved::TryPlanGreatGeneralCoordinateRepair(
               carrier, general, repairedX, repairedY) &&
           repairedX == 22 && repairedY == 13));

    auto alreadyAttached = general;
    alreadyAttached.x    = carrier.x;
    UNSCOPED_INFO("leave an attached General unchanged");
    CHECK(!rerevved::TryPlanGreatGeneralCoordinateRepair(
        carrier, alreadyAttached, repairedX, repairedY));

    auto deletedGeneral = general;
    deletedGeneral.slot = 0xFF;
    UNSCOPED_INFO("do not resurrect a deleted General");
    CHECK(!rerevved::TryPlanGreatGeneralCoordinateRepair(
        carrier, deletedGeneral, repairedX, repairedY));

    auto deadCarrier  = carrier;
    deadCarrier.flags = 0x80000000;
    UNSCOPED_INFO("do not follow a dead carrier");
    CHECK(!rerevved::TryPlanGreatGeneralCoordinateRepair(
        deadCarrier, general, repairedX, repairedY));

    auto ordinaryUnit = general;
    ordinaryUnit.type = 29;
    UNSCOPED_INFO("require the Great General type discriminator");
    CHECK(!rerevved::TryPlanGreatGeneralCoordinateRepair(
        carrier, ordinaryUnit, repairedX, repairedY));

    auto wrongLink        = general;
    wrongLink.carrierLink = 5;
    UNSCOPED_INFO("require the live carrier link");
    CHECK(!rerevved::TryPlanGreatGeneralCoordinateRepair(
        carrier, wrongLink, repairedX, repairedY));

    auto otherPlayer   = general;
    otherPlayer.player = 3;
    UNSCOPED_INFO("never cross player ownership");
    CHECK(!rerevved::TryPlanGreatGeneralCoordinateRepair(
        carrier, otherPlayer, repairedX, repairedY));
}
