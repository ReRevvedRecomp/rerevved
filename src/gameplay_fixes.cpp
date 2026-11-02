#include "gameplay_fixes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(domination_min_turns, "10", "ReRevved", "Turns before a Domination victory can be declared: 10 (default) or 5. Applies to games started in this launch. Loaded saves keep 10, except Game of the Week practice saves")
    .allowed({ "10", "5" });

REXCVAR_DEFINE_STRING(turn_limit, "2100", "ReRevved", "Year the game can end by turn limit in the next new game: 2100 (default), 2200, 2300, 2500 or unlimited. Game of the Week always uses 2100")
    .allowed({ "2100", "2200", "2300", "2500", "unlimited" });

REXCVAR_DEFINE_STRING(starting_era, "ancient", "ReRevved", "Era every civilization starts in for the next new game: ancient (default), medieval, industrial or modern. Scenarios and Game of the Week keep their own era")
    .allowed({ "ancient", "medieval", "industrial", "modern" });

REXCVAR_DEFINE_STRING(multiplayer_ai_difficulty, "king", "ReRevved", "Difficulty of every AI in the next multiplayer game you host: king (default), emperor or deity. Single player keeps the difficulty chosen at game setup")
    .allowed({ "king", "emperor", "deity" });

REXCVAR_DEFINE_BOOL(skip_spaceship_scenes, false, "ReRevved", "Skip the in-world rocket scene when a spaceship part is completed. Applies to each new part; a network game plays the host's value");

namespace rerevved::gameplay_fixes
{
namespace
{

std::atomic<bool>    shortenedAllowed{ false };
std::atomic<int32_t> restoredMinimum{ 0 };
std::atomic<int32_t> turnLimit{ kTurnLimitNative };
std::atomic<int32_t> startingEra{ kStartingEraNative };
std::atomic<int32_t> multiplayerAiDifficulty{ kMultiplayerAiDifficultyNative };

} // namespace

void NoteNewGame() noexcept
{
    shortenedAllowed.store(true, std::memory_order_relaxed);
    restoredMinimum.store(0, std::memory_order_relaxed);
}

void NoteGameLoad() noexcept
{
    shortenedAllowed.store(false, std::memory_order_relaxed);
    restoredMinimum.store(0, std::memory_order_relaxed);
    turnLimit.store(kTurnLimitNative, std::memory_order_relaxed);
}

void RestoreDominationMinimum(int32_t turns) noexcept
{
    restoredMinimum.store(turns, std::memory_order_relaxed);
}

int32_t RestoredDominationMinimum() noexcept
{
    return restoredMinimum.load(std::memory_order_relaxed);
}

void RestoreTurnLimit(int32_t limit) noexcept
{
    turnLimit.store(limit, std::memory_order_relaxed);
}

int32_t TurnLimitForGame() noexcept
{
    return turnLimit.load(std::memory_order_relaxed);
}

bool ParseTurnLimit(std::string_view text, int32_t& limit) noexcept
{
    constexpr std::array<std::string_view, 5> names = { "2100", "2200", "2300", "2500", "unlimited" };
    const auto                                found = std::ranges::find(names, text);
    if (found == names.end())
    {
        return false;
    }
    limit = static_cast<int32_t>(found - names.begin());
    return true;
}

void RestoreStartingEra(int32_t era) noexcept
{
    startingEra.store(era, std::memory_order_relaxed);
}

int32_t StartingEraForGame() noexcept
{
    return startingEra.load(std::memory_order_relaxed);
}

bool ParseStartingEra(std::string_view text, int32_t& era) noexcept
{
    constexpr std::array<std::string_view, 4> names = { "ancient", "medieval", "industrial", "modern" };
    const auto                                found = std::ranges::find(names, text);
    if (found == names.end())
    {
        return false;
    }
    era = static_cast<int32_t>(found - names.begin());
    return true;
}

void RestoreMultiplayerAiDifficulty(int32_t difficulty) noexcept
{
    multiplayerAiDifficulty.store(difficulty, std::memory_order_relaxed);
}

int32_t MultiplayerAiDifficultyForGame() noexcept
{
    return multiplayerAiDifficulty.load(std::memory_order_relaxed);
}

bool ParseMultiplayerAiDifficulty(std::string_view text, int32_t& difficulty) noexcept
{
    constexpr std::array<std::string_view, 3> names = { "king", "emperor", "deity" };
    const auto                                found = std::ranges::find(names, text);
    if (found == names.end())
    {
        return false;
    }
    difficulty = kMultiplayerAiDifficultyNative + static_cast<int32_t>(found - names.begin());
    return true;
}

bool ShortenedMinimumAllowed() noexcept
{
    return shortenedAllowed.load(std::memory_order_relaxed);
}

bool ParseDominationMinimumTurns(std::string_view text, int32_t& turns) noexcept
{
    if (text == "10")
    {
        turns = 10;
    }
    else if (text == "5")
    {
        turns = 5;
    }
    else
    {
        return false;
    }
    return true;
}

int32_t DominationMinimumTurns(std::string_view setting, bool shortenedAllowed) noexcept
{
    int32_t turns = kNativeDominationMinimumTurns;
    if (!shortenedAllowed || !ParseDominationMinimumTurns(setting, turns))
    {
        return kNativeDominationMinimumTurns;
    }
    return turns;
}

int32_t RebaseDominationElapsed(int32_t elapsed, int32_t minimumTurns) noexcept
{
    const int64_t rebased = int64_t{ elapsed } - minimumTurns + kNativeDominationMinimumTurns;
    return static_cast<int32_t>(std::clamp<int64_t>(rebased, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

} // namespace rerevved::gameplay_fixes
