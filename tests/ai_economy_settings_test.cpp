#include "ai_economy_settings.h"
#include "cvar_scope.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <stdexcept>

REXCVAR_DECLARE(int32_t, ai_research_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_production_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_gold_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_food_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_culture_bonus_percent);

namespace
{

namespace economy = rerevved::ai_economy;

struct PolicyReset
{
    ~PolicyReset()
    {
        economy::ResetForLoad();
        economy::FinishLoad(false);
    }
};

} // namespace

TEST_CASE("ai_economy_freeze_and_load", "[ctest][ai-economy]")
{
    const PolicyReset reset;
    REREVVED_RESTORE_CVAR(ai_research_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_production_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_gold_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_food_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_culture_bonus_percent);
    REXCVAR_SET(ai_research_bonus_percent, 33);
    REXCVAR_SET(ai_production_bonus_percent, 67);
    REXCVAR_SET(ai_gold_bonus_percent, 100);
    REXCVAR_SET(ai_food_bonus_percent, 150);
    REXCVAR_SET(ai_culture_bonus_percent, 200);
    economy::FreezeForGame();
    CHECK((economy::ForGame() == economy::Policy{ 33, 67, 100, 150, 200 }));
    REXCVAR_SET(ai_research_bonus_percent, 5);
    REXCVAR_SET(ai_production_bonus_percent, 10);
    REXCVAR_SET(ai_gold_bonus_percent, 11);
    REXCVAR_SET(ai_food_bonus_percent, 12);
    REXCVAR_SET(ai_culture_bonus_percent, 13);
    CHECK((economy::ForGame() == economy::Policy{ 33, 67, 100, 150, 200 }));

    economy::BeginLoad();
    CHECK(economy::ForGame() == economy::Policy{});
    economy::ResetForLoad();
    economy::RestoreForGame({ 45, 65, 10, 20, 30 });
    CHECK(economy::ForGame() == economy::Policy{});
    economy::FinishLoad(false);
    CHECK((economy::ForGame() == economy::Policy{ 45, 65, 10, 20, 30 }));
    economy::ResetForLoad();
    CHECK(economy::ForGame() == economy::Policy{});
    economy::BeginLoad();
    economy::FinishLoad(false);
    CHECK(economy::ForGame() == economy::Policy{});
}

TEST_CASE("ai_economy_standard_and_network_policy", "[ctest][ai-economy]")
{
    const PolicyReset reset;
    REREVVED_RESTORE_CVAR(ai_research_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_production_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_gold_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_food_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_culture_bonus_percent);
    REXCVAR_SET(ai_research_bonus_percent, 40);
    REXCVAR_SET(ai_production_bonus_percent, 60);
    REXCVAR_SET(ai_gold_bonus_percent, 70);
    REXCVAR_SET(ai_food_bonus_percent, 80);
    REXCVAR_SET(ai_culture_bonus_percent, 90);
    economy::FreezeForGame(true);
    CHECK(economy::ForGame() == economy::Policy{});
    CHECK(economy::ForSave(false) == economy::Policy{});

    // An ordinary save owns its policy even after a Standard Rules game.
    economy::BeginLoad();
    economy::ResetForLoad();
    economy::RestoreForGame({ 45, 65, 10, 20, 30 });
    economy::FinishLoad(false);
    CHECK((economy::ForGame() == economy::Policy{ 45, 65, 10, 20, 30 }));
    CHECK(economy::ForSave(true) == economy::Policy{});
    CHECK((economy::ForGame() == economy::Policy{ 45, 65, 10, 20, 30 }));
    economy::BeginLoad();
    economy::ResetForLoad();
    economy::RestoreForGame({ 45, 65, 10, 20, 30 });
    economy::FinishLoad(true);
    CHECK(economy::ForGame() == economy::Policy{});

    economy::FreezeForGame();
    CHECK((economy::ForGame() == economy::Policy{ 40, 60, 70, 80, 90 }));
    economy::RestoreForGame({ 0, 200, 0, 0, 0 });
    CHECK((economy::ForGame() == economy::Policy{ 0, 200, 0, 0, 0 }));

    // Any invalid field restores native behavior for every option.
    for (const auto& invalid : { economy::Policy{ -1, 60, 70, 80, 90 }, economy::Policy{ 40, 201, 70, 80, 90 }, economy::Policy{ 40, 60, 201, 80, 90 }, economy::Policy{ 40, 60, 70, -1, 90 }, economy::Policy{ 40, 60, 70, 80, 201 } })
    {
        economy::RestoreForGame({ 40, 60, 70, 80, 90 });
        economy::RestoreForGame(invalid);
        CHECK(economy::ForGame() == economy::Policy{});
    }
}

TEST_CASE("ai_economy_failed_load_preserves_policy", "[ctest][ai-economy]")
{
    const PolicyReset reset;
    economy::RestoreForGame({ 33, 67, 1, 2, 3 });
    {
        economy::LoadScope load;
        CHECK(economy::ForGame() == economy::Policy{});
        load.Finish(false);
    }
    CHECK((economy::ForGame() == economy::Policy{ 33, 67, 1, 2, 3 }));

    // Reaching the reset seam without metadata starts a game with native costs.
    {
        economy::LoadScope load;
        economy::ResetForLoad();
        load.Finish(false);
    }
    CHECK(economy::ForGame() == economy::Policy{});

    // Successful metadata restore survives the guard's destructor.
    {
        economy::LoadScope load;
        economy::ResetForLoad();
        economy::RestoreForGame({ 45, 65, 10, 20, 30 });
        load.Finish(false);
    }
    CHECK((economy::ForGame() == economy::Policy{ 45, 65, 10, 20, 30 }));
}

TEST_CASE("ai_economy_load_exception_cleanup", "[ctest][ai-economy]")
{
    const PolicyReset reset;
    economy::RestoreForGame({ 33, 67, 1, 2, 3 });
    const auto failLoad = [](bool afterReset)
    {
        economy::LoadScope load;
        if (afterReset)
        {
            economy::ResetForLoad();
            economy::RestoreForGame({ 45, 65, 10, 20, 30 });
        }
        throw std::runtime_error("native load failed");
    };
    CHECK_THROWS_AS(failLoad(false), std::runtime_error);
    CHECK((economy::ForGame() == economy::Policy{ 33, 67, 1, 2, 3 }));
    CHECK_THROWS_AS(failLoad(true), std::runtime_error);
    CHECK(economy::ForGame() == economy::Policy{});
    economy::RestoreForGame({ 40, 60, 70, 80, 90 });
    CHECK((economy::ForGame() == economy::Policy{ 40, 60, 70, 80, 90 }));
}

TEST_CASE("ai_economy_cost_rounding", "[ctest][ai-economy]")
{
    CHECK(economy::ScaleCost(101, 200) == 33);
    CHECK(economy::ScaleCost(1, 200) == 1);
    CHECK(economy::ScaleCost(0, 100) == 0);
    CHECK(economy::ScaleCost(-1, 100) == -1);
    CHECK(economy::ScaleCost(101, 0) == 101);
    CHECK(economy::ScaleCost(101, 201) == 101);
    CHECK(economy::ScaleCost(101, -1) == 101);
    CHECK(economy::ScaleCost(std::numeric_limits<int32_t>::max(), 1) == 2126221432);

    // A bonus of 100 percent halves the cost.
    for (const int32_t cost : { 1, 2, 3, 101, 999, std::numeric_limits<int32_t>::max() })
    {
        CHECK(economy::ScaleCost(cost, 100) == std::max<int32_t>(1, static_cast<int32_t>(int64_t{ cost } * 50 / 100)));
    }

    // A scalar already reduced by an identity rule is the AI rule's input.
    CHECK(economy::ScaleUnitScalar(5, 2, 100) == 2);
    CHECK(economy::ScaleUnitScalar(3, 1, 200) == 2);
    CHECK(economy::ScaleUnitScalar(1, 1, 200) == 1);
    CHECK(economy::ScaleUnitScalar(5, -1, 100) == 5);
    CHECK(economy::ScaleUnitScalar(5, 0, 100) == 5);
    CHECK(economy::ScaleUnitScalar(5, 2, 0) == 5);
    CHECK(economy::ScaleUnitScalar(5, 2, 201) == 5);
    CHECK(economy::ScaleUnitScalar(std::numeric_limits<int32_t>::max(), 2, 100) == std::numeric_limits<int32_t>::max());
}

TEST_CASE("ai_economy_yield_scaling", "[ctest][ai-economy]")
{
    CHECK(economy::ScaleYield(10, 50) == 15);
    CHECK(economy::ScaleYield(3, 33) == 3);
    CHECK(economy::ScaleYield(1, 200) == 3);
    CHECK(economy::ScaleYield(7, 100) == 14);
    CHECK(economy::ScaleYield(10, 0) == 10);
    CHECK(economy::ScaleYield(10, 201) == 10);
    CHECK(economy::ScaleYield(10, -1) == 10);

    // Zero and negative contributions, such as starvation, stay native.
    CHECK(economy::ScaleYield(0, 100) == 0);
    CHECK(economy::ScaleYield(-5, 100) == -5);
    CHECK(economy::ScaleYield(std::numeric_limits<int32_t>::min(), 200) == std::numeric_limits<int32_t>::min());

    // The native halfword caches cap the result. A larger native value is kept.
    CHECK(economy::ScaleYield(20000, 200) == 32767);
    CHECK(economy::ScaleYield(32767, 100) == 32767);
    CHECK(economy::ScaleYield(32768, 100) == 32768);
    CHECK(economy::ScaleYield(std::numeric_limits<int32_t>::max(), 200) == std::numeric_limits<int32_t>::max());
}
