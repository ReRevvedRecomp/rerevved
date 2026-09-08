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

PresenceModel makeFallback(std::string details)
{
    return {
        .details        = std::move(details),
        .largeImageKey  = "rerevved",
        .largeImageText = "ReRevved",
    };
}

std::string formatYear(int32_t year)
{
    if (year < 0)
    {
        return std::to_string(-static_cast<int64_t>(year)) + " BC";
    }
    if (year > 0)
    {
        return std::to_string(year) + " AD";
    }
    return "Year 0";
}

} // namespace

bool TryBuildGameplayPresence(const GameplayState& state,
                              PresenceModel&       presence)
{
    constexpr uint32_t kRequiredFields =
        GAMEPLAY_VALID_CIVILIZATION |
        GAMEPLAY_VALID_ERA | GAMEPLAY_VALID_YEAR |
        GAMEPLAY_VALID_TURN_NUMBER;

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
                          std::string(kEras[state.era]) + " Era - " +
                          formatYear(state.year),
        .largeImageKey  = std::string(civilization.imageAsset),
        .largeImageText = std::string(civilization.leader),
    };
    return true;
}

PresenceModel SelectPresence(
    const GameplayState*                state,
    const std::optional<PresenceModel>& retainedGameplay)
{
    const bool gameplayKnown =
        state &&
        (state->validFields & GAMEPLAY_VALID_FRONTEND) != 0 &&
        state->gameplayActive;
    if (!gameplayKnown)
    {
        return makeFallback("Idle");
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
