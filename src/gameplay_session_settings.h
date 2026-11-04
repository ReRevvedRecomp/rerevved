#pragma once

#include "host_settings.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rerevved::gameplay_session
{

// The network mod loadout lock reason must match exactly for translation.
inline constexpr std::string_view kModsLockedReason        = "Mods can't be changed during a multiplayer game.";
inline constexpr std::string_view kHostSettingLockedReason = "Set by the host for this multiplayer game.";
// Lobby bytes are frozen when the host opens the lobby, so the host's later edits only arrive at game start.
inline constexpr std::string_view kHostSettingLobbyReason = "Set by the host. The host can still change it until the game starts.";

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

// A file load runs inside a save sidecar transaction; a network stream transfer does not.
enum class LoadCase
{
    FileLoad,
    NetworkRestore,
    NetworkNoSnapshot,
};

constexpr LoadCase ClassifyLoad(bool sidecarTransaction, bool networkGame, bool hostSnapshot) noexcept
{
    return sidecarTransaction || !networkGame ? LoadCase::FileLoad
           : hostSnapshot                     ? LoadCase::NetworkRestore
                                              : LoadCase::NetworkNoSnapshot;
}

constexpr std::string_view LoadCaseName(LoadCase loadCase) noexcept
{
    switch (loadCase)
    {
        case LoadCase::FileLoad:
            return "file load";
        case LoadCase::NetworkRestore:
            return "network restore";
        case LoadCase::NetworkNoSnapshot:
            return "network restore without host snapshot";
    }
    return "unknown";
}

// Prepare before native new-game initialization and before freezing gameplay settings.
host_settings::GameSnapshot Prepare(bool networkGame, uint32_t setup, int32_t mode);

struct Admission
{
    bool        admitted = false;
    std::string diagnostic; // The refusal text shown to the player.
};

// Changes no game state, so a refusal leaves the previous game intact.
Admission AdmitNetworkGame(uint32_t setup, int32_t mode);
// The seed writer precedes outer setup. Retain even failed attempts until setup
// consumes them, so later preference edits cannot authorize a different seed.
// Return captured settings for the first seed latch. AcceptedWeeklyWeek is its
// effective week after an accepted Game of the Week Prepare consumes the attempt, else 0.
std::optional<host_settings::HostSettings>         PrepareWeekly(uint32_t setup, std::optional<int32_t> mode) noexcept;
uint32_t                                           AcceptedWeeklyWeek() noexcept;
void                                               Publish(const host_settings::GameSnapshot& snapshot, bool networkGame);
bool                                               ApplyNewGame(const host_settings::GameSnapshot& snapshot, bool networkGame, int32_t mode);
std::shared_ptr<const host_settings::HostSettings> ForNetworkGame() noexcept;
void                                               ResetOnLoad(LoadCase loadCase = LoadCase::FileLoad) noexcept;
// Local settings are never read; a null host restores the native defaults.
int32_t                                RestoreNetworkPolicy(const host_settings::HostSettings* host) noexcept;
bool                                   NetworkGameActive() noexcept;
std::unique_lock<std::recursive_mutex> LockRuleBoundary();
std::unique_lock<std::recursive_mutex> TryLockRuleBoundary();
bool                                   RuleReplacementAllowed() noexcept;

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
// A changed or missing token drops the staged snapshot.
using StampProvider = std::optional<std::vector<uint8_t>> (*)();
Admission                                  AdmitNetworkGame(uint32_t setup, int32_t mode, SnapshotProvider capture, StampProvider stamp);
host_settings::GameSnapshot                Prepare(bool networkGame, uint32_t setup, int32_t mode, SnapshotProvider capture, StampProvider stamp = nullptr);
std::optional<host_settings::HostSettings> PrepareWeekly(uint32_t setup, std::optional<int32_t> mode, SnapshotProvider capture) noexcept;

} // namespace detail

} // namespace rerevved::gameplay_session
