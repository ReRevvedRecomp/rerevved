#include "gameplay_fixes.h"

#include <algorithm>
#include <atomic>
#include <limits>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(domination_min_turns, "10", "ReRevved", "Turns before a Domination victory can be declared: 10 (default), 5 or 1. Applies to games started in this launch. Loaded saves and Game of the Week with Standard Rules keep 10")
    .allowed({ "10", "5", "1" });

namespace rerevved::gameplay_fixes
{
namespace
{

std::atomic<bool> shortenedAllowed{ false };

} // namespace

void NoteNewGame(bool standardRules) noexcept
{
    shortenedAllowed.store(!standardRules, std::memory_order_relaxed);
}

void NoteGameLoad() noexcept
{
    shortenedAllowed.store(false, std::memory_order_relaxed);
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
    else if (text == "1")
    {
        turns = 1;
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
