#include "presence_model.h"

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
    std::string_view image_asset;
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

PresenceModel MakeFallback(std::string details)
{
    return {
        .details          = std::move(details),
        .large_image_key  = "rerevved",
        .large_image_text = "ReRevved",
    };
}

std::string FormatYear(int32_t year)
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

bool TryBuildGameplayPresence(const ReRevvedGameplayState& state,
                              PresenceModel&               presence)
{
    constexpr uint32_t kRequiredFields =
        REREVVED_GAMEPLAY_VALID_CIVILIZATION |
        REREVVED_GAMEPLAY_VALID_ERA | REREVVED_GAMEPLAY_VALID_YEAR |
        REREVVED_GAMEPLAY_VALID_TURN_NUMBER;

    if (!state.available ||
        (state.valid_fields & kRequiredFields) != kRequiredFields ||
        state.civilization < 0 ||
        static_cast<size_t>(state.civilization) >= kCivilizations.size() ||
        state.era < 0 || static_cast<size_t>(state.era) >= kEras.size())
    {
        return false;
    }

    const auto& civilization = kCivilizations[state.civilization];
    presence                 = {
        .details          = "Playing as the " + std::string(civilization.people) + ".",
        .state            = "Turn " + std::to_string(state.turn) + " | " +
                            std::string(kEras[state.era]) + " Era - " +
                            FormatYear(state.year),
        .large_image_key  = std::string(civilization.image_asset),
        .large_image_text = std::string(civilization.leader),
    };
    return true;
}

PresenceModel SelectPresence(
    const ReRevvedGameplayState*        state,
    const std::optional<PresenceModel>& retained_gameplay)
{
    const bool gameplay_known =
        state &&
        (state->valid_fields & REREVVED_GAMEPLAY_VALID_FRONTEND) != 0 &&
        state->gameplay_active;
    if (!gameplay_known)
    {
        return MakeFallback("Idle");
    }

    PresenceModel gameplay;
    if (TryBuildGameplayPresence(*state, gameplay))
    {
        return gameplay;
    }
    if (retained_gameplay)
    {
        return *retained_gameplay;
    }
    return MakeFallback("In game");
}

} // namespace rerevved
