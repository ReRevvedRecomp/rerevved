#pragma once

// Host session data supplies the per-game gameplay policy and rule identity.
// Guest-memory-free model; LocalSettings samples preferences and accepted mod
// metadata in gameplay_session_settings.cpp.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rex/session_data.h>

namespace rerevved::host_settings
{

constexpr int32_t kMaxAiCivilizations      = 4;
constexpr int32_t kMaxLocalAiCivilizations = 6;

struct ModRef
{
    std::string id;
    std::string version;
    // Shown in warnings only; never compared.
    std::string displayName;
};

struct LocalModSnapshot
{
    bool                    complete = true;
    std::vector<ModRef>     mods;
    bool                    rulesDigestPresent = false;
    std::array<uint8_t, 32> rulesDigest{};
};

struct HostSettings
{
    int32_t             aiCivilizations = kMaxAiCivilizations;
    std::vector<ModRef> mods;
    // The channel could not hold the full mod list; only a prefix was sent.
    bool                    modsTruncated           = false;
    bool                    ragingBarbarians        = false;
    bool                    oneCityChallenge        = false;
    int32_t                 resources               = 0; // resource_settings::Mode
    int32_t                 aiResearchCostPercent   = 100;
    int32_t                 aiProductionCostPercent = 100;
    int32_t                 dominationMinimumTurns  = 10;
    int32_t                 combatSpeed             = 0; // 0 normal, 1 fast; presentation only.
    bool                    gotwCustomWeek          = false;
    uint32_t                gotwWeek                = 0; // Zero when the native week applies.
    uint32_t                gotwSeed                = 0; // Zero when the native seed applies.
    bool                    gotwStandardRules       = true;
    bool                    rulesDigestPresent      = false;
    std::array<uint8_t, 32> rulesDigest{}; // SHA-256 of accepted normalized rule content.
};

// Format 2 has a fixed gameplay header and optional content digest, followed
// by sorted mods with length-prefixed id, version and display name fields.
// Normalize local selections before publication; decode rejects noncanonical values.
std::vector<uint8_t> Encode(const HostSettings& settings);

HostSettings Normalize(HostSettings settings, bool networkGame = true);
// Empty for old/unknown versions, malformed fields, short/trailing bytes or
// mods not in strictly ascending id order.
std::optional<HostSettings> Decode(std::span<const uint8_t> bytes);

enum class SnapshotState
{
    Local,
    Host,
    Received,
    Pending,
    Missing,
    Incompatible,
};

struct GameSnapshot
{
    SnapshotState               state = SnapshotState::Missing;
    std::optional<HostSettings> settings;
    std::string                 diagnostic;
};

// Call after SDK PrepareForGame/WaitForHostData, before every gameplay freezer.
// Returns an owned immutable-by-convention value. No local gameplay fallback is
// returned on a network failure or unverified/different mod content.
GameSnapshot ResolveForGame(bool                             networkGame,
                            const rex::session_data::Status& status,
                            const HostSettings&              local);

// Publish after each accepted loadout transaction. Readers copy the snapshot
// instead of consulting the live catalog, which a rescan can replace.
// Identities and digest are replaced together; LocalSettings reads them once.
void             PublishLocalModSnapshot(LocalModSnapshot snapshot);
void             InvalidateLocalModSnapshot() noexcept;
LocalModSnapshot LocalModSnapshotForSettings();

// Legacy identity-only publication clears content verification.
void                PublishLocalMods(std::vector<ModRef> mods);
std::vector<ModRef> LocalMods();

// This machine's gameplay preferences and accepted mod metadata.
HostSettings LocalSettings();

// Truncated host mod lists report that the full comparison was unavailable and
// never return an empty mismatch string.
std::string DescribeModMismatch(const std::vector<ModRef>& host,
                                const std::vector<ModRef>& local,
                                bool                       hostTruncated = false);

// Legacy AI-only reader: -1 with source "unavailable" when a network game has
// no valid prepared snapshot. New callers use ResolveForGame for the full gate.
int32_t ResolveAiLimit(bool                             networkGame,
                       const rex::session_data::Status& status,
                       int32_t                          local,
                       std::string*                     source);

} // namespace rerevved::host_settings
