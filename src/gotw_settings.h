#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace rerevved::host_settings
{

struct HostSettings;

}

namespace rerevved::gotw
{

struct Selection
{
    bool     customWeek = false;
    uint32_t nativeWeek = 0;
    uint32_t week       = 0;
    uint32_t seed       = 0;
    uint32_t setup      = 0;
    uint64_t launch     = 0;

    bool IsCustom() const noexcept
    {
        return customWeek;
    }
};

bool     IsValidWeek(uint32_t week) noexcept;
uint32_t SeedForWeek(uint32_t week) noexcept;
// Reject invalid decimal input without changing the caller's previous value.
bool ParseWeek(std::string_view text, uint32_t& week) noexcept;

// Snapshot once at new-game ingress; later consumers use the same decision.
bool FreezeStandardRules(int32_t mode) noexcept;
bool FreezeStandardRules(int32_t mode, bool selected) noexcept;
bool StandardRulesForGame() noexcept;

// Only the accepted GOTW setup call snapshots the pending settings.
Selection LatchForGame(uint32_t nativeWeek, uint32_t setup) noexcept;
Selection LatchForGame(uint32_t nativeWeek, uint32_t setup, const host_settings::HostSettings& settings) noexcept;
Selection ForGame() noexcept;
using UiDispatcher = std::function<void(std::function<void()>)>;
// Install before guest execution; clear before the app context is destroyed.
void                      SetSeedDispatcher(UiDispatcher dispatcher, std::weak_ptr<std::atomic<bool>> alive = {});
void                      RecordAutomaticSeed(uint32_t seed) noexcept;
void                      RecordAutomaticWeek(uint32_t week) noexcept;
bool                      TraceEnabled() noexcept;
inline constexpr uint64_t kTraceEventCap = 512;
bool                      ShouldTraceEvent(bool enabled, uint64_t sequence) noexcept;
bool                      IsolateWeeklyResult(int32_t mode) noexcept;

} // namespace rerevved::gotw
