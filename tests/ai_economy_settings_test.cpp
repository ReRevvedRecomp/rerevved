#include "ai_economy_settings.h"
#include "cvar_scope.hpp"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <stdexcept>

REXCVAR_DECLARE(int32_t, ai_research_cost_percent);
REXCVAR_DECLARE(int32_t, ai_production_cost_percent);

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
    REREVVED_RESTORE_CVAR(ai_research_cost_percent);
    REREVVED_RESTORE_CVAR(ai_production_cost_percent);
    REXCVAR_SET(ai_research_cost_percent, 33);
    REXCVAR_SET(ai_production_cost_percent, 67);
    economy::FreezeForGame();
    CHECK((economy::ForGame() == economy::Policy{ 33, 67 }));
    REXCVAR_SET(ai_research_cost_percent, 5);
    REXCVAR_SET(ai_production_cost_percent, 10);
    CHECK((economy::ForGame() == economy::Policy{ 33, 67 }));

    economy::BeginLoad();
    CHECK(economy::ForGame() == economy::Policy{});
    economy::ResetForLoad();
    economy::RestoreForGame({ 45, 65 });
    CHECK(economy::ForGame() == economy::Policy{});
    economy::FinishLoad(false);
    CHECK((economy::ForGame() == economy::Policy{ 45, 65 }));
    economy::ResetForLoad();
    CHECK(economy::ForGame() == economy::Policy{});
    economy::BeginLoad();
    economy::FinishLoad(false);
    CHECK(economy::ForGame() == economy::Policy{});
}

TEST_CASE("ai_economy_standard_and_network_policy", "[ctest][ai-economy]")
{
    const PolicyReset reset;
    REREVVED_RESTORE_CVAR(ai_research_cost_percent);
    REREVVED_RESTORE_CVAR(ai_production_cost_percent);
    REXCVAR_SET(ai_research_cost_percent, 40);
    REXCVAR_SET(ai_production_cost_percent, 60);
    economy::FreezeForGame(true);
    CHECK(economy::ForGame() == economy::Policy{});
    CHECK(economy::ForSave(false) == economy::Policy{});

    // An ordinary save owns its policy even after a Standard Rules game.
    economy::BeginLoad();
    economy::ResetForLoad();
    economy::RestoreForGame({ 45, 65 });
    economy::FinishLoad(false);
    CHECK((economy::ForGame() == economy::Policy{ 45, 65 }));
    CHECK(economy::ForSave(true) == economy::Policy{});
    CHECK((economy::ForGame() == economy::Policy{ 45, 65 }));
    economy::BeginLoad();
    economy::ResetForLoad();
    economy::RestoreForGame({ 45, 65 });
    economy::FinishLoad(true);
    CHECK(economy::ForGame() == economy::Policy{});

    economy::FreezeForGame();
    CHECK((economy::ForGame() == economy::Policy{ 40, 60 }));
    economy::RestoreForGame({ 0, 60 });
    CHECK(economy::ForGame() == economy::Policy{});
    economy::RestoreForGame({ 40, 101 });
    CHECK(economy::ForGame() == economy::Policy{});
}

TEST_CASE("ai_economy_failed_load_preserves_policy", "[ctest][ai-economy]")
{
    const PolicyReset reset;
    economy::RestoreForGame({ 33, 67 });
    {
        economy::LoadScope load;
        CHECK(economy::ForGame() == economy::Policy{});
        load.Finish(false);
    }
    CHECK((economy::ForGame() == economy::Policy{ 33, 67 }));

    // Reaching the reset seam without metadata is a new native-cost game.
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
        economy::RestoreForGame({ 45, 65 });
        load.Finish(false);
    }
    CHECK((economy::ForGame() == economy::Policy{ 45, 65 }));
}

TEST_CASE("ai_economy_load_exception_cleanup", "[ctest][ai-economy]")
{
    const PolicyReset reset;
    economy::RestoreForGame({ 33, 67 });
    const auto failLoad = [](bool afterReset)
    {
        economy::LoadScope load;
        if (afterReset)
        {
            economy::ResetForLoad();
            economy::RestoreForGame({ 45, 65 });
        }
        throw std::runtime_error("native load failed");
    };
    CHECK_THROWS_AS(failLoad(false), std::runtime_error);
    CHECK((economy::ForGame() == economy::Policy{ 33, 67 }));
    CHECK_THROWS_AS(failLoad(true), std::runtime_error);
    CHECK(economy::ForGame() == economy::Policy{});
    economy::RestoreForGame({ 40, 60 });
    CHECK((economy::ForGame() == economy::Policy{ 40, 60 }));
}

TEST_CASE("ai_economy_cost_rounding", "[ctest][ai-economy]")
{
    CHECK(economy::ScaleCost(101, 33) == 33);
    CHECK(economy::ScaleCost(1, 1) == 1);
    CHECK(economy::ScaleCost(0, 50) == 0);
    CHECK(economy::ScaleCost(-1, 50) == -1);
    CHECK(economy::ScaleCost(101, 100) == 101);
    CHECK(economy::ScaleCost(101, 0) == 101);
    CHECK(economy::ScaleCost(std::numeric_limits<int32_t>::max(), 99) == 2126008810);

    // A scalar already reduced by an identity rule is the AI rule's input.
    CHECK(economy::ScaleUnitScalar(5, 2, 50) == 2);
    CHECK(economy::ScaleUnitScalar(3, 1, 1) == 2);
    CHECK(economy::ScaleUnitScalar(1, 1, 1) == 1);
    CHECK(economy::ScaleUnitScalar(5, -1, 50) == 5);
    CHECK(economy::ScaleUnitScalar(5, 0, 50) == 5);
    CHECK(economy::ScaleUnitScalar(5, 2, 100) == 5);
    CHECK(economy::ScaleUnitScalar(std::numeric_limits<int32_t>::max(), 2, 50) == std::numeric_limits<int32_t>::max());
}
