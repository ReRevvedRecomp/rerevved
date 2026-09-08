#include "presence_model.h"

#include <array>
#include <chrono>
#include <cstddef>
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

GameplayState gameplayState(int32_t civilization,
                            int32_t era,
                            int32_t year,
                            int32_t turn = 61)
{
    GameplayState state{};
    state.structSize     = sizeof(state);
    state.validFields    = GAMEPLAY_VALID_FRONTEND |
                           GAMEPLAY_VALID_CIVILIZATION |
                           GAMEPLAY_VALID_ERA |
                           GAMEPLAY_VALID_YEAR |
                           GAMEPLAY_VALID_TURN_NUMBER;
    state.gameplayActive = 1;
    state.available      = 1;
    state.civilization   = civilization;
    state.era            = era;
    state.year           = year;
    state.turn           = turn;
    return state;
}

} // namespace

int main()
{
    static_assert(sizeof(GameplayState) == 80);
    static_assert(offsetof(GameplayState, available) == 40);
    static_assert(offsetof(GameplayState, civilization) == 44);
    static_assert(offsetof(GameplayState, era) == 48);
    static_assert(offsetof(GameplayState, year) == 52);
    static_assert(offsetof(GameplayState, turn) == 56);
    static_assert(offsetof(GameplayState, reserved) == 60);

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
        const auto              state = gameplayState(civilization, civilization % 4, 1025);
        rerevved::PresenceModel presence;
        require(rerevved::TryBuildGameplayPresence(state, presence),
                "playable civilization formats");
        require(presence.largeImageKey == kCivilizationAssets[civilization],
                "image asset key matches civilization");
        require(presence.smallImageKey.empty(),
                "civilization uses one image asset");
    }

    auto                    americans = gameplayState(7, 1, 1025);
    rerevved::PresenceModel presence;
    require(rerevved::TryBuildGameplayPresence(americans, presence),
            "American checkpoint formats");
    require(presence.details == "Playing as the Americans.",
            "American details text");
    require(presence.state == "Turn 61 | Medieval Era - 1025 AD",
            "American calendar text");

    auto bc = gameplayState(0, 0, -4000, 0);
    require(rerevved::TryBuildGameplayPresence(bc, presence) &&
                presence.state == "Turn 0 | Ancient Era - 4000 BC",
            "BC year formatting");
    auto zero = gameplayState(0, 0, 0);
    require(rerevved::TryBuildGameplayPresence(zero, presence) &&
                presence.state == "Turn 61 | Ancient Era - Year 0",
            "year-zero formatting");

    auto unknown = gameplayState(16, 1, 1050);
    require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "unknown civilization rejected atomically");
    unknown.civilization = GAMEPLAY_CIVILIZATION_UNKNOWN;
    require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "negative civilization rejected atomically");
    unknown.civilization = 7;
    unknown.era          = 4;
    require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "out-of-range era rejected atomically");
    unknown.era = GAMEPLAY_ERA_UNKNOWN;
    require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "negative era rejected atomically");
    unknown.era = 1;
    unknown.validFields &= ~GAMEPLAY_VALID_YEAR;
    unknown.year = GAMEPLAY_YEAR_UNKNOWN;
    require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "invalid year rejected atomically");
    unknown.validFields |= GAMEPLAY_VALID_YEAR;
    unknown.validFields &= ~GAMEPLAY_VALID_TURN_NUMBER;
    unknown.turn = GAMEPLAY_TURN_UNKNOWN;
    require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "invalid turn rejected atomically");
    unknown.validFields |= GAMEPLAY_VALID_TURN_NUMBER;
    unknown.civilization = 16;
    const auto generic   = rerevved::SelectPresence(&unknown, std::nullopt);
    require(generic.largeImageKey == "rerevved" &&
                generic.details == "In game" && generic.state.empty() &&
                generic.smallImageKey.empty(),
            "unknown gameplay uses complete logo fallback");

    rerevved::PresenceModel retained;
    require(rerevved::TryBuildGameplayPresence(americans, retained),
            "retained gameplay seed formats");
    auto changed  = retained;
    changed.state = "Turn 62 | Medieval Era - 1050 AD";
    require(changed != retained, "complete activity equality supports deduplication");
    americans.available = 0;
    require(rerevved::SelectPresence(&americans, retained) == retained,
            "temporary gameplay gate retains last complete activity");
    americans.gameplayActive = 0;
    const auto menu          = rerevved::SelectPresence(&americans, retained);
    require(menu.largeImageKey == "rerevved" &&
                menu.details == "Idle" && menu.state.empty() &&
                menu.smallImageKey.empty(),
            "menu transition clears gameplay presentation");

    require(rerevved::kPresencePublishInterval == std::chrono::seconds{ 5 },
            "publish interval is five seconds");
    return failures == 0 ? 0 : 1;
}
