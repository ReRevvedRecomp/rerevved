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

// Schema 2 is the ordinary payload and schema 5 adds the turn limit to it. Schema 3
// appends a practice block, written only for a Game of the Week practice save.
// Schema 4 is the ordinary payload marked isolation-only, written for a practice or
// isolated game's other saves. Schemas 1 to 4 carry no turn limit and load the native one.
constexpr uint8_t kSchemaVersion = 5;

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
    int32_t turnLimit                      = 0; // gameplay_fixes turn limit index, schema 5 only.
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

// Called last by ResetAiCivilizationsOnLoad, before native visual readers run.
void RestoreAfterLoadReset() noexcept;

} // namespace rerevved::save_sidecar
