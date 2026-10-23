#include "gotw_settings.h"

#include <array>
#include <charconv>
#include <mutex>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(gotw_custom_week, false, "ReRevved", "Choose a custom week for the next Game of the Week");
REXCVAR_DEFINE_BOOL(gotw_standard_rules, true, "ReRevved", "Use standard gameplay rules for Game of the Week");
REXCVAR_DEFINE_BOOL(gotw_trace, false, "ReRevved", "Trace Game of the Week seed writers and map construction; capped at 512 events per process");
REXCVAR_DEFINE_STRING(gotw_week, "1", "ReRevved", "Week number for Game of the Week: 1..2147483647")
    .validator([](std::string_view text)
               {
                   uint32_t week = 0;
                   return rerevved::gotw::ParseWeek(text, week);
               });

namespace rerevved::gotw
{
namespace
{

// Pregame.FPK ccglobaldefines.xml:228, member offset 0x2D34, MapList offset 0x60AC.
// FPK SHA256: 14E50B0382542B345F29B129175CB1B1B9A4538547E8B5567CAA5A864531A2DE
// XML SHA256: C68C7BEE458CD4C5B6A3773AFD2EA07D8CC60E7A606DB5D1B82F3626D44445E3
constexpr std::array<uint32_t, 277> kMapList = {
    1, 2, 3, 4, 6, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 20, 21, 22, 23, 24, 25, 26, 27, 29, 30, 31, 32, 33, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 83, 84, 85, 86, 87, 89, 90, 91, 92, 93, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 116, 117, 118, 119, 120, 121, 122, 123, 125, 126, 127, 128, 129, 131, 132, 133, 134, 135, 136, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 256, 257, 258, 259, 260, 261, 262, 263, 264, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291, 292, 293, 294, 295, 297, 298, 299, 300
};

std::mutex                       selectionMutex;
Selection                        selection;
std::atomic<bool>                standardRules{ false };
std::mutex                       dispatcherMutex;
UiDispatcher                     seedDispatcher;
std::weak_ptr<std::atomic<bool>> uiAlive;

bool parseDecimal(std::string_view text, uint32_t& value) noexcept
{
    if (text.empty() || text.front() < '0' || text.front() > '9')
    {
        return false;
    }
    uint32_t   parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        return false;
    }
    value = parsed;
    return true;
}

void logRejected(const char* control, std::string_view value) noexcept
try
{
    REXLOG_WARN("Game of the Week: invalid custom {} '{}'; using native {} for this launch", control, value, control);
}
catch (...)
{
    // Logging must not prevent the native fallback.
}

void dispatchAutomatic(uint32_t value, uint64_t launch) noexcept
try
{
    std::lock_guard lock(dispatcherMutex);
    const auto      guard = uiAlive.lock();
    if (!seedDispatcher || !guard || !guard->load())
    {
        return;
    }
    seedDispatcher([alive = uiAlive, value, launch]() noexcept
                   {
                       try
                       {
                           const auto guard  = alive.lock();
                           const auto active = ForGame();
                           if (!guard || !guard->load() || active.launch != launch)
                           {
                               return;
                           }
                           if (!active.customWeek && !REXCVAR_GET(gotw_custom_week))
                           {
                               rex::cvar::SetFlagByName("gotw_week", std::to_string(value));
                           }
                       }
                       catch (...)
                       {
                           // A failed UI update must not escape the deferred callback.
                       }
                   });
}
catch (...)
{
    // Allocation or dispatch failure must not unwind through guest code.
}

} // namespace

bool IsValidWeek(uint32_t week) noexcept
{
    return week >= 1 && week <= 0x7FFFFFFFu;
}

uint32_t SeedForWeek(uint32_t week) noexcept
{
    // 0x82E2B91C..0x82E2B930 uses one-based ordinal (week % 277) + 1.
    return kMapList[week % kMapList.size()];
}

bool ParseWeek(std::string_view text, uint32_t& week) noexcept
{
    uint32_t parsed = 0;
    if (!parseDecimal(text, parsed) || !IsValidWeek(parsed))
    {
        return false;
    }
    week = parsed;
    return true;
}

Selection LatchForGame(uint32_t nativeWeek, uint32_t setup) noexcept
try
{
    Selection next;
    next.nativeWeek = nativeWeek;
    next.week       = nativeWeek;
    next.setup      = setup;
    if (REXCVAR_GET(gotw_custom_week))
    {
        next.customWeek = ParseWeek(REXCVAR_GET(gotw_week), next.week);
        if (!next.customWeek)
        {
            logRejected("week", REXCVAR_GET(gotw_week));
        }
    }
    if (next.customWeek)
    {
        next.seed = SeedForWeek(next.week);
    }
    std::lock_guard lock(selectionMutex);
    next.launch = selection.launch + 1;
    selection   = next;
    return next;
}
catch (...)
{
    return {};
}

Selection ForGame() noexcept
try
{
    std::lock_guard lock(selectionMutex);
    return selection;
}
catch (...)
{
    return {};
}

void SetSeedDispatcher(UiDispatcher dispatcher, std::weak_ptr<std::atomic<bool>> alive)
{
    std::lock_guard lock(dispatcherMutex);
    seedDispatcher = std::move(dispatcher);
    uiAlive        = std::move(alive);
}

void RecordAutomaticSeed(uint32_t seed) noexcept
try
{
    std::lock_guard lock(selectionMutex);
    if (!selection.customWeek)
    {
        selection.seed = seed;
    }
}
catch (...)
{
    // Observations must not unwind through guest code.
}

void RecordAutomaticWeek(uint32_t week) noexcept
{
    const auto active = ForGame();
    if (!active.customWeek && active.nativeWeek == week && IsValidWeek(week))
    {
        dispatchAutomatic(week, active.launch);
    }
}

bool FreezeStandardRules(int32_t mode) noexcept
{
    const bool enabled = mode == 2 && REXCVAR_GET(gotw_standard_rules);
    standardRules.store(enabled, std::memory_order_relaxed);
    return enabled;
}

bool StandardRulesForGame() noexcept
{
    return standardRules.load(std::memory_order_relaxed);
}

bool TraceEnabled() noexcept
{
    return REXCVAR_GET(gotw_trace);
}

bool ShouldTraceEvent(bool enabled, uint64_t sequence) noexcept
{
    return enabled && sequence > 0 && sequence <= kTraceEventCap;
}

bool IsolateWeeklyResult(int32_t mode) noexcept
{
    return mode == 2 && ForGame().IsCustom();
}

} // namespace rerevved::gotw
