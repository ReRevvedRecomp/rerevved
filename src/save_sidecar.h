#pragma once

#include "ai_economy_settings.h"
#include "gotw_practice.h"
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

// Schema 2 ordinary; 3 adds a practice block; 4 isolates other practice/isolated saves; 5 adds turn limit;
// 6 adds Always At War; 7 adds the ships sunk count; 8 adds the technology cost multiplier byte.
// Schemas 1 to 4 load the native turn limit; schemas 1 to 5 load Always At War off, schemas 1 to 6 load no
// ships sunk, and schemas 1 to 7 load a technology cost multiplier of 1.
constexpr uint8_t kSchemaVersion = 8;

struct State
{
    bool                                  ragingBarbarians = false;
    bool                                  occEnabled       = false;
    int32_t                               occPopulation    = 33;
    int32_t                               resources        = 0;
    resource_identity_overrides::Snapshot overrides;
    ai_economy::Policy                    aiEconomy;
    bool                                  occFromSetup       = false;
    int32_t                               barbarianDefeats   = 0;
    bool                                  historyKnown       = false; // Schema 2 whole-game history. Schema 1 loads untracked.
    bool                                  unitBuilt          = false;
    bool                                  battleFought       = false;
    bool                                  deepOceanEntered   = false;
    int32_t                               firstFoundingTurn  = -1;
    int32_t                               initialAiOpponents = -1;
    std::optional<gotw::PracticeBlock>    practice;
    // Schema 4 isolates non-resumable practice/isolated saves by origin only; exclusive with a practice block.
    bool    practiceMarker                 = false;
    int32_t turnLimit                      = 0; // gameplay_fixes turn limit index, schema 5 and later.
    bool    alwaysAtWar                    = false;
    int32_t navalVictories                 = 0; // schema 7
    bool    operator==(const State&) const = default;
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
// claimsPractice marks a schema 3 or 4 Game of the Week origin even if invalid or mismatched.
std::optional<State> Read(const std::filesystem::path& nativeFile, std::string_view slot, uint32_t device, ReadFailure* failure = nullptr, bool* claimsPractice = nullptr);
bool                 Save(bool nativeSuccess, const std::filesystem::path& nativeFile, std::string_view slot, uint32_t device, const State& state);

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

// False for a network stream restore, which reaches the load reset without a transaction.
bool FileLoadActive() noexcept;

// Called last by ResetAiCivilizationsOnLoad, before native visual readers run.
void RestoreAfterLoadReset() noexcept;

} // namespace rerevved::save_sidecar
