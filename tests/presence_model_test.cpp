#include "presence_model.h"

#include <array>
#include <chrono>
#include <cstddef>
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

ReRevvedGameplayState GameplayState(int32_t civilization,
                                    int32_t era,
                                    int32_t year,
                                    int32_t turn = 61)
{
    ReRevvedGameplayState state{};
    state.struct_size     = sizeof(state);
    state.valid_fields    = REREVVED_GAMEPLAY_VALID_FRONTEND |
                            REREVVED_GAMEPLAY_VALID_CIVILIZATION |
                            REREVVED_GAMEPLAY_VALID_ERA |
                            REREVVED_GAMEPLAY_VALID_YEAR |
                            REREVVED_GAMEPLAY_VALID_TURN_NUMBER;
    state.gameplay_active = 1;
    state.available       = 1;
    state.civilization    = civilization;
    state.era             = era;
    state.year            = year;
    state.turn            = turn;
    return state;
}

} // namespace

int main()
{
    static_assert(sizeof(ReRevvedGameplayState) == 80);
    static_assert(offsetof(ReRevvedGameplayState, available) == 40);
    static_assert(offsetof(ReRevvedGameplayState, civilization) == 44);
    static_assert(offsetof(ReRevvedGameplayState, era) == 48);
    static_assert(offsetof(ReRevvedGameplayState, year) == 52);
    static_assert(offsetof(ReRevvedGameplayState, turn) == 56);
    static_assert(offsetof(ReRevvedGameplayState, reserved) == 60);

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
        const auto              state = GameplayState(civilization, civilization % 4, 1025);
        rerevved::PresenceModel presence;
        Require(rerevved::TryBuildGameplayPresence(state, presence),
                "playable civilization formats");
        Require(presence.large_image_key == kCivilizationAssets[civilization],
                "image asset key matches civilization");
        Require(presence.small_image_key.empty(),
                "civilization uses one image asset");
    }

    auto                    americans = GameplayState(7, 1, 1025);
    rerevved::PresenceModel presence;
    Require(rerevved::TryBuildGameplayPresence(americans, presence),
            "American checkpoint formats");
    Require(presence.details == "Playing as the Americans.",
            "American details text");
    Require(presence.state == "Turn 61 | Medieval Era - 1025 AD",
            "American calendar text");

    auto bc = GameplayState(0, 0, -4000, 0);
    Require(rerevved::TryBuildGameplayPresence(bc, presence) &&
                presence.state == "Turn 0 | Ancient Era - 4000 BC",
            "BC year formatting");
    auto zero = GameplayState(0, 0, 0);
    Require(rerevved::TryBuildGameplayPresence(zero, presence) &&
                presence.state == "Turn 61 | Ancient Era - Year 0",
            "year-zero formatting");

    auto unknown = GameplayState(16, 1, 1050);
    Require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "unknown civilization rejected atomically");
    unknown.civilization = REREVVED_GAMEPLAY_CIVILIZATION_UNKNOWN;
    Require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "negative civilization rejected atomically");
    unknown.civilization = 7;
    unknown.era          = 4;
    Require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "out-of-range era rejected atomically");
    unknown.era = REREVVED_GAMEPLAY_ERA_UNKNOWN;
    Require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "negative era rejected atomically");
    unknown.era = 1;
    unknown.valid_fields &= ~REREVVED_GAMEPLAY_VALID_YEAR;
    unknown.year = REREVVED_GAMEPLAY_YEAR_UNKNOWN;
    Require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "invalid year rejected atomically");
    unknown.valid_fields |= REREVVED_GAMEPLAY_VALID_YEAR;
    unknown.valid_fields &= ~REREVVED_GAMEPLAY_VALID_TURN_NUMBER;
    unknown.turn = REREVVED_GAMEPLAY_TURN_UNKNOWN;
    Require(!rerevved::TryBuildGameplayPresence(unknown, presence),
            "invalid turn rejected atomically");
    unknown.valid_fields |= REREVVED_GAMEPLAY_VALID_TURN_NUMBER;
    unknown.civilization = 16;
    const auto generic   = rerevved::SelectPresence(&unknown, std::nullopt);
    Require(generic.large_image_key == "rerevved" &&
                generic.details == "In game" && generic.state.empty() &&
                generic.small_image_key.empty(),
            "unknown gameplay uses complete logo fallback");

    rerevved::PresenceModel retained;
    Require(rerevved::TryBuildGameplayPresence(americans, retained),
            "retained gameplay seed formats");
    auto changed  = retained;
    changed.state = "Turn 62 | Medieval Era - 1050 AD";
    Require(changed != retained, "complete activity equality supports deduplication");
    americans.available = 0;
    Require(rerevved::SelectPresence(&americans, retained) == retained,
            "temporary gameplay gate retains last complete activity");
    americans.gameplay_active = 0;
    const auto menu           = rerevved::SelectPresence(&americans, retained);
    Require(menu.large_image_key == "rerevved" &&
                menu.details == "Idle" && menu.state.empty() &&
                menu.small_image_key.empty(),
            "menu transition clears gameplay presentation");

    Require(rerevved::kPresencePublishInterval == std::chrono::seconds{ 5 },
            "publish interval is five seconds");
    return failures == 0 ? 0 : 1;
}
