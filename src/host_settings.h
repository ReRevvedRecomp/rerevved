#pragma once

// Host session data supplies each game's gameplay policy and rule identity.
// This model does not access guest memory. LocalSettings samples preferences
// and accepted mod metadata in gameplay_session_settings.cpp.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rex/session_data.h>

namespace rerevved::host_settings
{

constexpr int32_t kMaxAiCivilizations     = 6;
constexpr int32_t kDefaultAiCivilizations = 4;

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
    int32_t             aiCivilizations = kDefaultAiCivilizations;
    std::vector<ModRef> mods;
    // The channel could not hold the full mod list; only a prefix was sent.
    bool                    modsTruncated            = false;
    bool                    ragingBarbarians         = false;
    bool                    oneCityChallenge         = false;
    int32_t                 resources                = 0; // resource_settings::Mode
    int32_t                 aiResearchBonusPercent   = 0;
    int32_t                 aiProductionBonusPercent = 0;
    int32_t                 aiGoldBonusPercent       = 0;
    int32_t                 aiFoodBonusPercent       = 0;
    int32_t                 aiCultureBonusPercent    = 0;
    int32_t                 dominationMinimumTurns   = 10;
    int32_t                 combatSpeed              = 0; // 0 normal, 1 fast. Presentation only.
    int32_t                 turnLimit                = 0;
    int32_t                 startingEra              = 0;
    int32_t                 multiplayerAiDifficulty  = 2; // Native difficulty: King 2, Emperor 3 or Deity 4.
    bool                    skipSpaceshipScenes      = false;
    int32_t                 unitMovement             = 0;     // gameplay_fixes::UnitMovement: 0 normal, 1 fast, 2 instant. Presentation only.
    bool                    alwaysAtWar              = false; // Local-only, never serialized.
    bool                    gotwCustomWeek           = false;
    uint32_t                gotwWeek                 = 0; // Zero when the native week applies.
    uint32_t                gotwSeed                 = 0; // Zero when the native seed applies.
    bool                    gotwCustomCivilization   = false;
    uint32_t                gotwCivilization         = 0; // Zero when the native civilization applies.
    bool                    gotwStandardRules        = true;
    bool                    gotwAllowSaving          = false; // Local-only practice permission, never serialized.
    bool                    rulesDigestPresent       = false;
    std::array<uint8_t, 32> rulesDigest{}; // SHA-256 of accepted normalized rule content.
};

// Format 8 has a fixed gameplay header and optional content digest, followed
// by sorted mods with length-prefixed id, version and display name fields.
// Normalize local selections before publication. Decode rejects noncanonical values.
// The local-only Game of the Week civilization is never serialized.
// Gameplay bit 0x20 must equal Raging bit 0x01: peers must support capital grace.
// Domination turns are 5 or 10; turn limit and starting era are indices 0-4, multiplayer AI difficulty 2-4,
// spaceship scene skip 0-1 and unit movement 0-2.
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

enum class ContentMismatch
{
    None,
    Mods,  // A list is truncated or the loadouts differ.
    Rules, // The content digest is missing on either side or different.
};

// The lobby warning runs the same comparison as game-start admission.
ContentMismatch CompareContent(const HostSettings& host, const HostSettings& local);

struct LobbyWarning
{
    std::string title;
    std::string message;
};

// Lobby bytes are not game-ready data, so a mismatch here only warns; admission still decides at game start.
std::optional<LobbyWarning> CheckLobbyContent(const rex::session_data::Status& status, const HostSettings& local);

// Call after SDK PrepareForGame/WaitForHostData, before freezing gameplay settings.
// Treat the returned owned value as immutable. No local gameplay fallback is
// returned if the network fails or mod content is unverified or different.
GameSnapshot ResolveForGame(bool                             networkGame,
                            const rex::session_data::Status& status,
                            const HostSettings&              local);

// Publish after each accepted loadout transaction. Readers copy the snapshot
// instead of consulting the live catalog, which a rescan can replace.
// Identities and digest are replaced together. LocalSettings reads them once.
void             PublishLocalModSnapshot(LocalModSnapshot snapshot);
void             InvalidateLocalModSnapshot() noexcept;
LocalModSnapshot LocalModSnapshotForSettings();

// Publishing identities alone clears content verification.
void                PublishLocalMods(std::vector<ModRef> mods);
std::vector<ModRef> LocalMods();

// This machine's gameplay preferences and accepted mod metadata.
HostSettings LocalSettings();

// Truncated host mod lists report that the full comparison was unavailable and
// never return an empty mismatch string.
std::string DescribeModMismatch(const std::vector<ModRef>& host,
                                const std::vector<ModRef>& local,
                                bool                       hostTruncated = false);

// Returns -1 with source "unavailable" when a network game has no valid prepared
// snapshot. Use ResolveForGame to validate all gameplay settings.
int32_t ResolveAiLimit(bool                             networkGame,
                       const rex::session_data::Status& status,
                       int32_t                          local,
                       std::string*                     source);

} // namespace rerevved::host_settings
