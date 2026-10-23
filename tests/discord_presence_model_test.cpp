#include "discord_presence_model.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <string_view>

namespace
{

GameplayState gameplayState(int32_t civilization,
                            int32_t era,
                            int32_t turn = 61)
{
    GameplayState state{};
    state.structSize     = sizeof(state);
    state.validFields    = GAMEPLAY_VALID_FRONTEND |
                           GAMEPLAY_VALID_CIVILIZATION |
                           GAMEPLAY_VALID_ERA |
                           GAMEPLAY_VALID_TURN_NUMBER;
    state.gameplayActive = 1;
    state.available      = 1;
    state.civilization   = civilization;
    state.era            = era;
    state.year           = GAMEPLAY_YEAR_UNKNOWN;
    state.turn           = turn;
    return state;
}

} // namespace

TEST_CASE("discord_presence_model", "[ctest][discord-presence-model][local-fast]")
{
    constexpr std::array<std::string_view, 16> kCivilizationAssets = {
        "civ-roman",
        "civ-egyptian",
        "civ-greek",
        "civ-spanish",
        "civ-german",
        "civ-russian",
        "civ-chinese",
        "civ-american",
        "civ-japanese",
        "civ-french",
        "civ-indian",
        "civ-arabian",
        "civ-aztec",
        "civ-zulu",
        "civ-mongolian",
        "civ-english",
    };

    for (int32_t civilization = 0; civilization < 16; ++civilization)
    {
        const auto              state = gameplayState(civilization, civilization % 4);
        rerevved::PresenceModel presence;
        UNSCOPED_INFO("playable civilization formats");
        CHECK(rerevved::TryBuildGameplayPresence(state, presence));
        UNSCOPED_INFO("image asset key matches civilization");
        CHECK(presence.largeImageKey == kCivilizationAssets[civilization]);
        UNSCOPED_INFO("civilization uses one image asset");
        CHECK(presence.smallImageKey.empty());
    }

    auto                    americans = gameplayState(7, 1);
    rerevved::PresenceModel presence;
    UNSCOPED_INFO("American checkpoint formats");
    CHECK(rerevved::TryBuildGameplayPresence(americans, presence));
    UNSCOPED_INFO("American details text");
    CHECK(presence.details == "Playing as the Americans.");
    UNSCOPED_INFO("American turn and era text omit the year");
    CHECK(presence.state == "Turn 61 | Medieval Era");

    auto unknown = gameplayState(16, 1);
    UNSCOPED_INFO("unknown civilization rejected atomically");
    CHECK(!rerevved::TryBuildGameplayPresence(unknown, presence));
    unknown.civilization = GAMEPLAY_CIVILIZATION_UNKNOWN;
    UNSCOPED_INFO("negative civilization rejected atomically");
    CHECK(!rerevved::TryBuildGameplayPresence(unknown, presence));
    unknown.civilization = 7;
    unknown.era          = 4;
    UNSCOPED_INFO("out-of-range era rejected atomically");
    CHECK(!rerevved::TryBuildGameplayPresence(unknown, presence));
    unknown.era = GAMEPLAY_ERA_UNKNOWN;
    UNSCOPED_INFO("negative era rejected atomically");
    CHECK(!rerevved::TryBuildGameplayPresence(unknown, presence));
    unknown.era = 1;
    unknown.validFields &= ~GAMEPLAY_VALID_TURN_NUMBER;
    unknown.turn = GAMEPLAY_TURN_UNKNOWN;
    UNSCOPED_INFO("invalid turn rejected atomically");
    CHECK(!rerevved::TryBuildGameplayPresence(unknown, presence));
    unknown.validFields |= GAMEPLAY_VALID_TURN_NUMBER;
    unknown.civilization = 16;
    const auto generic   = rerevved::SelectPresence(&unknown, std::nullopt, rerevved::LobbyRole::kHost);
    UNSCOPED_INFO("unknown gameplay uses complete logo fallback");
    CHECK((generic.largeImageKey == "rerevved" &&
           generic.details == "In game" && generic.state.empty() &&
           generic.smallImageKey.empty()));

    rerevved::PresenceModel retained;
    UNSCOPED_INFO("retained gameplay seed formats");
    CHECK(rerevved::TryBuildGameplayPresence(americans, retained));
    auto changed  = retained;
    changed.state = "Turn 62 | Medieval Era";
    UNSCOPED_INFO("complete activity equality supports deduplication");
    CHECK(changed != retained);
    americans.available = 0;
    UNSCOPED_INFO("temporary gameplay gate retains last complete activity");
    CHECK(rerevved::SelectPresence(&americans, retained, rerevved::LobbyRole::kJoined) == retained);
    americans.gameplayActive = 0;
    const auto menu          = rerevved::SelectPresence(&americans, retained, rerevved::LobbyRole::kNone);
    UNSCOPED_INFO("menu transition clears gameplay presentation");
    CHECK((menu.largeImageKey == "rerevved" &&
           menu.details == "In the menus" && menu.state.empty() &&
           menu.smallImageKey.empty()));
    const auto hosting = rerevved::SelectPresence(&americans, retained, rerevved::LobbyRole::kHost);
    UNSCOPED_INFO("hosted session outside gameplay shows the lobby");
    CHECK((hosting.largeImageKey == "rerevved" && hosting.details == "In a lobby" &&
           hosting.state == "Hosting" && hosting.smallImageKey.empty()));
    const auto joined = rerevved::SelectPresence(&americans, retained, rerevved::LobbyRole::kJoined);
    UNSCOPED_INFO("joined session outside gameplay shows the lobby");
    CHECK((joined.largeImageKey == "rerevved" && joined.details == "In a lobby" &&
           joined.state == "Joined"));
    const auto startup = rerevved::SelectPresence(nullptr, std::nullopt, rerevved::LobbyRole::kNone);
    UNSCOPED_INFO("no gameplay state shows the menus");
    CHECK((startup.details == "In the menus" && startup.state.empty()));
    auto playing = gameplayState(7, 1, 1025);
    UNSCOPED_INFO("gameplay text wins over the lobby");
    CHECK(rerevved::SelectPresence(&playing, std::nullopt, rerevved::LobbyRole::kHost).details ==
          "Playing as the Americans.");

    UNSCOPED_INFO("publish interval is five seconds");
    CHECK(rerevved::kPresencePublishInterval == std::chrono::seconds{ 5 });
}
