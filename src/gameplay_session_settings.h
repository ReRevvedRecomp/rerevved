#pragma once

#include "host_settings.h"

#include <memory>
#include <mutex>
#include <optional>

namespace rerevved::gameplay_session
{

enum class SetupKind
{
    Unknown,
    Local,
    Load,
    Network,
};

constexpr SetupKind ClassifySetupMode(int32_t mode) noexcept
{
    switch (mode)
    {
        case 1:
        case 2:
            return SetupKind::Local;
        case 3:
            return SetupKind::Load;
        case 4:
        case 5:
        case 6:
            return SetupKind::Network;
        default:
            return SetupKind::Unknown;
    }
}

// Prepare before native new-game initialization and before freezing gameplay settings.
host_settings::GameSnapshot Prepare(bool networkGame, uint32_t setup, int32_t mode);
// The seed writer precedes outer setup. Retain even failed attempts until setup
// consumes them, so later preference edits cannot authorize a different seed.
// Return captured settings for the first seed latch. AcceptedWeeklyWeek is its
// effective week after an accepted Game of the Week Prepare consumes the attempt, else 0.
std::optional<host_settings::HostSettings>         PrepareWeekly(uint32_t setup, std::optional<int32_t> mode) noexcept;
uint32_t                                           AcceptedWeeklyWeek() noexcept;
void                                               Publish(const host_settings::GameSnapshot& snapshot, bool networkGame);
bool                                               ApplyNewGame(const host_settings::GameSnapshot& snapshot, bool networkGame, int32_t mode);
std::shared_ptr<const host_settings::HostSettings> ForNetworkGame() noexcept;
void                                               ResetOnLoad() noexcept;
std::unique_lock<std::recursive_mutex>             LockRuleBoundary();
bool                                               RuleReplacementAllowed() noexcept;

// A rejected snapshot uses the native inner setup failure return. The outer
// setup owner retains its own failure cleanup and menu transition.
class SetupGate
{
public:
    explicit SetupGate(bool admitted, bool newGame = true) noexcept;
    ~SetupGate();
    SetupGate(const SetupGate&)            = delete;
    SetupGate& operator=(const SetupGate&) = delete;

private:
    bool previousBlocked;
    bool previousActive;
};

bool SetupBlocked() noexcept;
bool SetupActive() noexcept;

namespace detail
{

using SnapshotProvider = host_settings::GameSnapshot (*)(bool networkGame);
host_settings::GameSnapshot                Prepare(bool networkGame, uint32_t setup, int32_t mode, SnapshotProvider capture);
std::optional<host_settings::HostSettings> PrepareWeekly(uint32_t setup, std::optional<int32_t> mode, SnapshotProvider capture) noexcept;

} // namespace detail

} // namespace rerevved::gameplay_session
