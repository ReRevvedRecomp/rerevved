#include "overlay_settings.h"

#include "gameplay_session_settings.h"
#include "gotw_settings.h"
#include "host_settings.h"

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rex/ui/overlay/settings_layout.h>

namespace
{
namespace host = rerevved::host_settings;

using TextByName = std::vector<std::pair<std::string, std::string>>;

host::HostSettings hostWithEveryFieldChanged()
{
    host::HostSettings settings;
    settings.aiCivilizations          = 6;
    settings.ragingBarbarians         = true;
    settings.oneCityChallenge         = true;
    settings.resources                = 3;
    settings.aiResearchBonusPercent   = 10;
    settings.aiProductionBonusPercent = 20;
    settings.aiGoldBonusPercent       = 30;
    settings.aiFoodBonusPercent       = 40;
    settings.aiCultureBonusPercent    = 50;
    settings.dominationMinimumTurns   = 5;
    settings.combatSpeed              = 1;
    settings.turnLimit                = 4;
    settings.startingEra              = 3;
    settings.multiplayerAiDifficulty  = 4;
    settings.skipSpaceshipScenes      = true;
    settings.unitMovement             = 2;
    settings.gotwCustomWeek           = true;
    settings.gotwWeek                 = 12;
    settings.gotwSeed                 = rerevved::gotw::SeedForWeek(12);
    settings.gotwStandardRules        = false;
    return settings;
}

void checkTexts(const host::HostSettings* settings, const TextByName& expected)
{
    for (const auto& [name, text] : expected)
    {
        INFO(name);
        const auto lock = rerevved::HostRowLock(settings, name);
        REQUIRE(lock);
        CHECK(lock->value == text);
        CHECK(lock->reason == rerevved::gameplay_session::kHostSettingLockedReason);
    }
}

std::set<std::string> layoutRows()
{
    rex::ui::SettingsLayout layout;
    rerevved::ConfigureSettingsLayout(layout);
    std::set<std::string> rows;
    for (const auto& tab : layout.tabs)
    {
        for (const auto& section : tab.sections)
        {
            rows.insert(section.sources.begin(), section.sources.end());
        }
    }
    return rows;
}

} // namespace

TEST_CASE("host_row_lock_shows_the_host_value_as_cvar_text", "[ctest][native-settings]")
{
    const auto settings = hostWithEveryFieldChanged();
    checkTexts(&settings, {
                              { "ai_civilizations", "6" },
                              { "raging_barbarians", "true" },
                              { "one_city_challenge", "true" },
                              { "resources", "3" },
                              { "ai_research_bonus_percent", "10" },
                              { "ai_production_bonus_percent", "20" },
                              { "ai_gold_bonus_percent", "30" },
                              { "ai_food_bonus_percent", "40" },
                              { "ai_culture_bonus_percent", "50" },
                              { "domination_min_turns", "5" },
                              { "combat_speed", "normal" },
                              { "turn_limit", "unlimited" },
                              { "starting_era", "modern" },
                              { "multiplayer_ai_difficulty", "deity" },
                              { "spaceship_scenes", "true" },
                              { "unit_movement", "normal" },
                              { "gotw_custom_week", "true" },
                              { "gotw_week", "12" },
                              { "gotw_standard_rules", "false" },
                              { "always_at_war", "false" },
                              { "gotw_allow_saving", "false" },
                              { "gotw_custom_civilization", "false" },
                              { "gotw_civilization", "0" },
                          });
}

TEST_CASE("host_row_lock_without_a_snapshot_shows_the_native_defaults", "[ctest][native-settings]")
{
    checkTexts(nullptr, {
                            { "raging_barbarians", "false" },
                            { "one_city_challenge", "false" },
                            { "resources", "0" },
                            { "ai_research_bonus_percent", "0" },
                            { "ai_production_bonus_percent", "0" },
                            { "ai_gold_bonus_percent", "0" },
                            { "ai_food_bonus_percent", "0" },
                            { "ai_culture_bonus_percent", "0" },
                            { "domination_min_turns", "10" },
                            { "combat_speed", "normal" },
                            { "turn_limit", "2100" },
                            { "starting_era", "ancient" },
                            { "multiplayer_ai_difficulty", "king" },
                            { "spaceship_scenes", "true" },
                            { "unit_movement", "normal" },
                            { "gotw_custom_week", "false" },
                            { "gotw_week", "" },
                        });
}

TEST_CASE("lobby_row_lock_shows_the_hosts_lobby_bytes_to_a_guest", "[ctest][native-settings]")
{
    host::HostSettings settings = hostWithEveryFieldChanged();
    settings.gotwCustomWeek     = false;
    const auto bytes            = host::Encode(settings);

    const auto lock = rerevved::LobbyRowLock({ rex::session_data::State::kReceived, bytes }, "one_city_challenge");
    REQUIRE(lock);
    CHECK(lock->value == "true");
    CHECK(lock->reason == rerevved::gameplay_session::kHostSettingLobbyReason);
    CHECK(rerevved::LobbyRowLock({ rex::session_data::State::kReceived, bytes }, "ai_gold_bonus_percent")->value == "30");

    CHECK_FALSE(rerevved::LobbyRowLock({ rex::session_data::State::kReceived, bytes }, "fullscreen"));
    CHECK_FALSE(rerevved::LobbyRowLock({ rex::session_data::State::kHost, bytes }, "one_city_challenge"));
    CHECK_FALSE(rerevved::LobbyRowLock({ rex::session_data::State::kNone, {} }, "one_city_challenge"));
    CHECK_FALSE(rerevved::LobbyRowLock({ rex::session_data::State::kReceived, { 0xFF } }, "one_city_challenge"));
}

TEST_CASE("host_row_lock_leaves_player_rows_alone", "[ctest][native-settings]")
{
    const host::HostSettings settings;
    for (const char* name : { "fullscreen", "show_fps_counter", "game_language", "show_popup_rankings", "system_link_profile_name" })
    {
        INFO(name);
        CHECK_FALSE(rerevved::HostRowLock(&settings, name));
        CHECK_FALSE(rerevved::HostRowLock(nullptr, name));
    }
}

TEST_CASE("every_wire_field_moves_a_settings_row", "[ctest][native-settings]")
{
    const std::set<std::string> rows = layoutRows();

    host::HostSettings before;
    before.gotwCustomWeek = true;
    before.gotwWeek       = 5;
    before.gotwSeed       = rerevved::gotw::SeedForWeek(5);

    const std::vector<std::pair<const char*, std::function<void(host::HostSettings&)>>> fields = {
        { "aiCivilizations", [](host::HostSettings& s)
          {
              s.aiCivilizations = 2;
          } },
        { "ragingBarbarians", [](host::HostSettings& s)
          {
              s.ragingBarbarians = true;
          } },
        { "oneCityChallenge", [](host::HostSettings& s)
          {
              s.oneCityChallenge = true;
          } },
        { "resources", [](host::HostSettings& s)
          {
              s.resources = 1;
          } },
        { "aiResearchBonusPercent", [](host::HostSettings& s)
          {
              s.aiResearchBonusPercent = 5;
          } },
        { "aiProductionBonusPercent", [](host::HostSettings& s)
          {
              s.aiProductionBonusPercent = 5;
          } },
        { "aiGoldBonusPercent", [](host::HostSettings& s)
          {
              s.aiGoldBonusPercent = 5;
          } },
        { "aiFoodBonusPercent", [](host::HostSettings& s)
          {
              s.aiFoodBonusPercent = 5;
          } },
        { "aiCultureBonusPercent", [](host::HostSettings& s)
          {
              s.aiCultureBonusPercent = 5;
          } },
        { "dominationMinimumTurns", [](host::HostSettings& s)
          {
              s.dominationMinimumTurns = 5;
          } },
        { "turnLimit", [](host::HostSettings& s)
          {
              s.turnLimit = 2;
          } },
        { "startingEra", [](host::HostSettings& s)
          {
              s.startingEra = 2;
          } },
        { "multiplayerAiDifficulty", [](host::HostSettings& s)
          {
              s.multiplayerAiDifficulty = 3;
          } },
        { "gotwCustomWeek", [](host::HostSettings& s)
          {
              s.gotwCustomWeek = false;
              s.gotwWeek       = 0;
              s.gotwSeed       = 0;
          } },
        { "gotwWeek", [](host::HostSettings& s)
          {
              s.gotwWeek = 6;
              s.gotwSeed = rerevved::gotw::SeedForWeek(6);
          } },
        { "gotwStandardRules", [](host::HostSettings& s)
          {
              s.gotwStandardRules = false;
          } },
    };

    for (const auto& [field, change] : fields)
    {
        INFO(field);
        auto after = before;
        change(after);
        REQUIRE(host::Encode(after) != host::Encode(before));

        bool moved = false;
        for (const auto& name : rows)
        {
            const auto was = rerevved::HostRowLock(&before, name);
            const auto now = rerevved::HostRowLock(&after, name);
            moved          = moved || (was && now && was->value != now->value);
        }
        CHECK(moved);
    }
}
