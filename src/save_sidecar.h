#pragma once

#include "ai_economy_settings.h"
#include "resource_identity_overrides.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rerevved::save_sidecar
{

constexpr uint8_t kSchemaVersion = 2;

struct State
{
    bool                                  ragingBarbarians = false;
    bool                                  occEnabled       = false;
    int32_t                               occPopulation    = 33;
    int32_t                               resources        = 0;
    resource_identity_overrides::Snapshot overrides;
    ai_economy::Policy                    aiEconomy;
    bool                                  occFromSetup                   = false;
    int32_t                               barbarianDefeats               = 0;
    bool                                  historyKnown                   = false; // Schema 2 whole-game history; schema 1 loads untracked.
    bool                                  unitBuilt                      = false;
    bool                                  battleFought                   = false;
    bool                                  deepOceanEntered               = false;
    int32_t                               firstFoundingTurn              = -1;
    int32_t                               initialAiOpponents             = -1;
    bool                                  operator==(const State&) const = default;
};

struct Identity
{
    std::string slot;
    uint32_t    device = 0;
    std::string digest;
};

enum class ReadFailure
{
    None,
    Missing,
    Unreadable,
    DigestUnreadable,
    DigestMismatch,
    Rejected,
};

std::vector<uint8_t>  Serialize(const State& state, const Identity& identity);
std::optional<State>  Deserialize(std::span<const uint8_t> bytes, const Identity& identity);
std::filesystem::path SidecarPath(const std::filesystem::path& nativeFile);
std::optional<State>  Read(const std::filesystem::path& nativeFile, std::string_view slot, uint32_t device, ReadFailure* failure = nullptr);
bool                  Save(bool nativeSuccess, const std::filesystem::path& nativeFile, std::string_view slot, uint32_t device, const State& state);

// Retain the staged state through every reset/freeze in one load transaction.
class PendingLoad
{
public:
    void Stage(std::optional<State> state)
    {
        state_ = std::move(state);
    }

    const std::optional<State>& AfterReset() const noexcept
    {
        return state_;
    }

private:
    std::optional<State> state_;
};

// Called last by ResetAiCivilizationsOnLoad, before native visual readers run.
void RestoreAfterLoadReset() noexcept;

} // namespace rerevved::save_sidecar
