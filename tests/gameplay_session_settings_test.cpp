#include "gameplay_session_settings.h"

#include "ai_economy_settings.h"
#include "cvar_scope.hpp"
#include "gameplay_fixes.h"
#include "gotw_settings.h"
#include "mod_script_rules.h"
#include "occ_settings.h"
#include "raging_barbarians_settings.h"
#include "registry_scope.hpp"
#include "rules_fixtures.hpp"

#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>
#include <span>
#include <string>
#include <vector>

REXCVAR_DECLARE(bool, one_city_challenge);
REXCVAR_DECLARE(bool, raging_barbarians);
REXCVAR_DECLARE(bool, gotw_custom_week);
REXCVAR_DECLARE(bool, gotw_standard_rules);
REXCVAR_DECLARE(int32_t, ai_research_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_production_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_gold_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_food_bonus_percent);
REXCVAR_DECLARE(int32_t, ai_culture_bonus_percent);

namespace
{
namespace session = rerevved::gameplay_session;
namespace host    = rerevved::host_settings;

host::GameSnapshot preparedSnapshot;
uint32_t           preparationCalls = 0;

host::GameSnapshot capturePrepared(bool)
{
    ++preparationCalls;
    return preparedSnapshot;
}

struct RestorePolicies
{
    const std::shared_ptr<const host::HostSettings> network   = session::ForNetworkGame();
    const rerevved::ai_economy::Policy              economy   = rerevved::ai_economy::ForGame();
    const rerevved::occ::Policy                     occ       = *rerevved::occ::ForGame();
    const bool                                      raging    = rerevved::raging_barbarians::Evaluate(0).enabled;
    const bool                                      standard  = rerevved::gotw::StandardRulesForGame();
    const bool                                      shortened = rerevved::gameplay_fixes::ShortenedMinimumAllowed();

    ~RestorePolicies()
    {
        session::ResetOnLoad();
        if (network)
            session::Publish({ host::SnapshotState::Received, *network, {} }, true);
        rerevved::ai_economy::RestoreForGame(economy);
        rerevved::occ::RestoreForGame(occ);
        rerevved::raging_barbarians::RestoreForGame(raging);
        rerevved::gotw::FreezeStandardRules(2, standard);
        rerevved::gameplay_fixes::NoteNewGame(!shortened);
    }
};

} // namespace

TEST_CASE("host gameplay policy precedes title freezes", "[ctest][host-settings]")
{
    RestorePolicies restore;
    REREVVED_RESTORE_CVAR(one_city_challenge);
    REREVVED_RESTORE_CVAR(raging_barbarians);
    REREVVED_RESTORE_CVAR(gotw_custom_week);
    REREVVED_RESTORE_CVAR(gotw_standard_rules);
    REREVVED_RESTORE_CVAR(ai_research_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_production_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_gold_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_food_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_culture_bonus_percent);
    REXCVAR_SET(one_city_challenge, false);
    REXCVAR_SET(raging_barbarians, false);
    REXCVAR_SET(gotw_custom_week, false);
    REXCVAR_SET(gotw_standard_rules, false);
    REXCVAR_SET(ai_research_bonus_percent, 0);
    REXCVAR_SET(ai_production_bonus_percent, 0);
    REXCVAR_SET(ai_gold_bonus_percent, 0);
    REXCVAR_SET(ai_food_bonus_percent, 0);
    REXCVAR_SET(ai_culture_bonus_percent, 0);

    host::HostSettings settings;
    settings.aiCivilizations          = 2;
    settings.oneCityChallenge         = true;
    settings.ragingBarbarians         = true;
    settings.resources                = 3;
    settings.aiResearchBonusPercent   = 50;
    settings.aiProductionBonusPercent = 40;
    settings.aiGoldBonusPercent       = 25;
    settings.aiFoodBonusPercent       = 75;
    settings.aiCultureBonusPercent    = 200;
    settings.dominationMinimumTurns   = 5;
    settings.gotwStandardRules        = true;
    const auto snapshot               = host::ResolveForGame(true, { rex::session_data::State::kReceived, host::Encode(settings), true }, {});
    REQUIRE(snapshot.settings);
    CHECK_FALSE(session::ApplyNewGame(snapshot, true, 6));
    REQUIRE(session::ForNetworkGame());
    CHECK(session::ForNetworkGame()->aiCivilizations == 2);
    CHECK(session::ForNetworkGame()->resources == 3);
    CHECK(session::ForNetworkGame()->dominationMinimumTurns == 5);
    CHECK((rerevved::ai_economy::ForGame() == rerevved::ai_economy::Policy{ 50, 40, 25, 75, 200 }));
    CHECK(rerevved::occ::ForGame()->enabled);
    CHECK(rerevved::raging_barbarians::Evaluate(0).enabled);
    CHECK_FALSE(REXCVAR_GET(one_city_challenge));
    CHECK_FALSE(REXCVAR_GET(raging_barbarians));
    CHECK(REXCVAR_GET(ai_research_bonus_percent) == 0);
    CHECK(REXCVAR_GET(ai_production_bonus_percent) == 0);
    CHECK(REXCVAR_GET(ai_gold_bonus_percent) == 0);
    CHECK(REXCVAR_GET(ai_food_bonus_percent) == 0);
    CHECK(REXCVAR_GET(ai_culture_bonus_percent) == 0);

    const auto weekly = host::ResolveForGame(false, {}, settings);
    CHECK(session::ApplyNewGame(weekly, false, 2));
    CHECK((rerevved::ai_economy::ForGame() == rerevved::ai_economy::Policy{}));
    CHECK_FALSE(rerevved::occ::ForGame()->enabled);
    CHECK_FALSE(rerevved::raging_barbarians::Evaluate(0).enabled);
    CHECK_FALSE(rerevved::gameplay_fixes::ShortenedMinimumAllowed());

    session::ResetOnLoad();
    CHECK_FALSE(session::ForNetworkGame());
}

TEST_CASE("rejected host settings retain policies and gate native setup", "[ctest][host-settings]")
{
    RestorePolicies restore;
    rerevved::ai_economy::RestoreForGame({ 60, 70 });
    const auto rejected = host::ResolveForGame(true, {}, {});
    REQUIRE_FALSE(rejected.settings);
    CHECK_FALSE(session::ApplyNewGame(rejected, true, 6));
    CHECK((rerevved::ai_economy::ForGame() == rerevved::ai_economy::Policy{ 60, 70 }));
    CHECK_FALSE(session::SetupBlocked());
    CHECK_FALSE(session::SetupActive());
    {
        session::SetupGate denied(false);
        CHECK(session::SetupBlocked());
        CHECK_FALSE(session::SetupActive());
        {
            session::SetupGate nested(true);
            CHECK_FALSE(session::SetupBlocked());
            CHECK(session::SetupActive());
        }
        CHECK(session::SetupBlocked());
        CHECK_FALSE(session::SetupActive());
    }
    CHECK_FALSE(session::SetupBlocked());
    CHECK_FALSE(session::SetupActive());
}

TEST_CASE("weekly preparation is bound to its setup and cleared by new games", "[ctest][host-settings]")
{
    RestorePolicies restore;
    session::ResetOnLoad();
    preparationCalls = 0;
    preparedSnapshot = { host::SnapshotState::Local, host::HostSettings{}, {} };
    REQUIRE(session::detail::PrepareWeekly(0x83000000, 2, capturePrepared));
    CHECK_FALSE(session::ForNetworkGame());
    CHECK_FALSE(session::detail::Prepare(false, 0x83001000, 2, capturePrepared).settings);
    CHECK_FALSE(session::ForNetworkGame());
    CHECK(preparationCalls == 1);
    preparedSnapshot = host::ResolveForGame(true, {}, {});
    REQUIRE_FALSE(session::detail::PrepareWeekly(0x83000000, 2, capturePrepared));
    preparedSnapshot = { host::SnapshotState::Local, host::HostSettings{}, {} };
    SECTION("a normal new game discards the failed weekly attempt")
    {
        REQUIRE(session::detail::Prepare(false, 0x83000000, 1, capturePrepared).settings);
    }
    SECTION("a saved-game load discards the failed weekly attempt")
    {
        session::ResetOnLoad();
        REQUIRE(session::detail::Prepare(false, 0x83000000, 2, capturePrepared).settings);
    }
    CHECK(preparationCalls == 3);
}

TEST_CASE("setup mode classifies current local load and network routes", "[ctest][host-settings]")
{
    RestorePolicies restore;
    CHECK(session::ClassifySetupMode(1) == session::SetupKind::Local);
    CHECK(session::ClassifySetupMode(2) == session::SetupKind::Local);
    CHECK(session::ClassifySetupMode(3) == session::SetupKind::Load);
    CHECK(session::ClassifySetupMode(4) == session::SetupKind::Network);
    CHECK(session::ClassifySetupMode(5) == session::SetupKind::Network);
    CHECK(session::ClassifySetupMode(6) == session::SetupKind::Network);
    CHECK(session::ClassifySetupMode(0) == session::SetupKind::Unknown);
    CHECK(session::ClassifySetupMode(7) == session::SetupKind::Unknown);
    preparationCalls = 0;
    CHECK_FALSE(session::detail::Prepare(false, 0x83000000, 0, capturePrepared).settings);
    CHECK(preparationCalls == 0);
}

TEST_CASE("load setup is inactive and retains policy until load succeeds", "[ctest][host-settings]")
{
    RestorePolicies    restore;
    host::HostSettings settings;
    settings.aiResearchBonusPercent = 60;
    const host::GameSnapshot previous{ host::SnapshotState::Received, settings, {} };
    session::ApplyNewGame(previous, true, 6);
    const auto network = session::ForNetworkGame();
    REQUIRE(network);
    preparationCalls = 0;
    {
        session::SetupGate newGame(true);
        CHECK(session::SetupActive());
        {
            session::SetupGate load(true, false);
            CHECK_FALSE(session::SetupBlocked());
            CHECK_FALSE(session::SetupActive());
            CHECK_FALSE(session::detail::Prepare(false, 0x83000000, 3, capturePrepared).settings);
            CHECK(preparationCalls == 0);
            CHECK(session::ForNetworkGame() == network);
            CHECK(rerevved::ai_economy::ForGame().researchBonusPercent == 60);
        }
        CHECK(session::SetupActive());
    }
    CHECK_FALSE(session::SetupActive());
    CHECK_FALSE(session::SetupBlocked());
    CHECK(session::ForNetworkGame() == network);
    session::ResetOnLoad();
    CHECK_FALSE(session::ForNetworkGame());
}

TEST_CASE("Standard Rules game suspends Lua mod rules until another game or a load", "[ctest][host-settings]")
{
    RestorePolicies                            restore;
    const rerevved::tests::ScopedRegistryReset registryScope;
    namespace fixtures = rerevved::tests::rules_fixtures;
    const auto modHost = rerevved::CreateModScriptRulesHost();
    const auto empty   = fixtures::snapshot();
    const auto script  = rex::system::RunModScript("main.lua", R"lua(return {
    provider_id = "test.hills",
    rules = { { id = "hills-production", kind = "terrain_yield", terrain = "hill", component = "production", operation = "add", value = 1 } },
})lua");
    REQUIRE(script.ok);
    const std::vector<std::string>             domains{ "terrain_yield" };
    const rex::system::ModScriptPublishRequest request{ "package", "main.lua", "rerevved.rules", 1, domains, script.value };
    rex::system::ModScriptPublishResult        result;
    auto                                       prepared = modHost->PrepareReplacement(std::span(&request, 1), result);
    REQUIRE(prepared);
    prepared->Commit();
    prepared.reset();
    const auto modded = fixtures::snapshot();
    REQUIRE(modded != empty);

    host::HostSettings settings;
    settings.gotwStandardRules = true;
    const auto standard        = host::ResolveForGame(false, {}, settings);
    REQUIRE(standard.settings);

    CHECK(session::ApplyNewGame(standard, false, 2));
    CHECK(fixtures::snapshot() == empty);
    SECTION("a following new game that is not Standard Rules restores them")
    {
        CHECK_FALSE(session::ApplyNewGame(standard, false, 1));
        CHECK(fixtures::snapshot() == modded);
    }
    SECTION("a following Game of the Week without Standard Rules restores them")
    {
        settings.gotwStandardRules = false;
        CHECK_FALSE(session::ApplyNewGame(host::ResolveForGame(false, {}, settings), false, 2));
        CHECK(fixtures::snapshot() == modded);
    }
    SECTION("a following Standard Rules game keeps them suspended")
    {
        CHECK(session::ApplyNewGame(standard, false, 2));
        CHECK(fixtures::snapshot() == empty);
    }
    SECTION("a saved-game load restores them")
    {
        session::ResetOnLoad();
        CHECK(fixtures::snapshot() == modded);
    }
    session::ResetOnLoad();
}

TEST_CASE("local settings read the AI bonus preferences", "[ctest][host-settings]")
{
    REREVVED_RESTORE_CVAR(ai_research_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_production_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_gold_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_food_bonus_percent);
    REREVVED_RESTORE_CVAR(ai_culture_bonus_percent);
    REXCVAR_SET(ai_research_bonus_percent, 10);
    REXCVAR_SET(ai_production_bonus_percent, 20);
    REXCVAR_SET(ai_gold_bonus_percent, 30);
    REXCVAR_SET(ai_food_bonus_percent, 40);
    REXCVAR_SET(ai_culture_bonus_percent, 200);
    const auto local = host::LocalSettings();
    CHECK(local.aiResearchBonusPercent == 10);
    CHECK(local.aiProductionBonusPercent == 20);
    CHECK(local.aiGoldBonusPercent == 30);
    CHECK(local.aiFoodBonusPercent == 40);
    CHECK(local.aiCultureBonusPercent == 200);
}
