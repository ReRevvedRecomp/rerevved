#include "discord_presence_model.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace rerevved
{

namespace
{

struct CivilizationPresence
{
    std::string_view people;
    std::string_view leader;
    std::string_view imageAsset;
};

constexpr std::array<CivilizationPresence, 16> kCivilizations = {
    CivilizationPresence{ "Romans", "Julius Caesar", "civ-roman" },
    CivilizationPresence{ "Egyptians", "Cleopatra", "civ-egyptian" },
    CivilizationPresence{ "Greeks", "Alexander the Great", "civ-greek" },
    CivilizationPresence{ "Spanish", "Isabella", "civ-spanish" },
    CivilizationPresence{ "Germans", "Otto von Bismarck", "civ-german" },
    CivilizationPresence{ "Russians", "Catherine the Great", "civ-russian" },
    CivilizationPresence{ "Chinese", "Mao Zedong", "civ-chinese" },
    CivilizationPresence{ "Americans", "Abraham Lincoln", "civ-american" },
    CivilizationPresence{ "Japanese", "Tokugawa Ieyasu", "civ-japanese" },
    CivilizationPresence{ "French", "Napoleon", "civ-french" },
    CivilizationPresence{ "Indians", "Mohandas Gandhi", "civ-indian" },
    CivilizationPresence{ "Arabs", "Saladin", "civ-arabian" },
    CivilizationPresence{ "Aztecs", "Montezuma II", "civ-aztec" },
    CivilizationPresence{ "Zulu", "Shaka", "civ-zulu" },
    CivilizationPresence{ "Mongols", "Genghis Khan", "civ-mongolian" },
    CivilizationPresence{ "English", "Elizabeth I", "civ-english" },
};

constexpr std::array<std::string_view, 4> kEras = {
    "Ancient",
    "Medieval",
    "Industrial",
    "Modern",
};

PresenceModel makeFallback(std::string details, std::string state = {})
{
    return {
        .details        = std::move(details),
        .state          = std::move(state),
        .largeImageKey  = "rerevved",
        .largeImageText = "ReRevved",
    };
}

} // namespace

bool TryBuildGameplayPresence(const GameplayState& state,
                              PresenceModel&       presence)
{
    constexpr uint32_t kRequiredFields =
        GAMEPLAY_VALID_CIVILIZATION |
        GAMEPLAY_VALID_ERA | GAMEPLAY_VALID_TURN_NUMBER;

    if (!state.available ||
        (state.validFields & kRequiredFields) != kRequiredFields ||
        state.civilization < 0 ||
        static_cast<size_t>(state.civilization) >= kCivilizations.size() ||
        state.era < 0 || static_cast<size_t>(state.era) >= kEras.size())
    {
        return false;
    }

    const auto& civilization = kCivilizations[state.civilization];
    presence                 = {
        .details        = "Playing as the " + std::string(civilization.people) + ".",
        .state          = "Turn " + std::to_string(state.turn) + " | " +
                          std::string(kEras[state.era]) + " Era",
        .largeImageKey  = std::string(civilization.imageAsset),
        .largeImageText = std::string(civilization.leader),
    };
    return true;
}

PresenceModel SelectPresence(
    const GameplayState*                state,
    const std::optional<PresenceModel>& retainedGameplay,
    LobbyRole                           lobby)
{
    const bool gameplayKnown =
        state &&
        (state->validFields & GAMEPLAY_VALID_FRONTEND) != 0 &&
        state->gameplayActive;
    if (!gameplayKnown)
    {
        switch (lobby)
        {
            case LobbyRole::kHost:
                return makeFallback("In a lobby", "Hosting");
            case LobbyRole::kJoined:
                return makeFallback("In a lobby", "Joined");
            case LobbyRole::kNone:
                break;
        }
        return makeFallback("In the menus");
    }

    PresenceModel gameplay;
    if (TryBuildGameplayPresence(*state, gameplay))
    {
        return gameplay;
    }
    if (retainedGameplay)
    {
        return *retainedGameplay;
    }
    return makeFallback("In game");
}

} // namespace rerevved
