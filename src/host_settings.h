#pragma once

// Host session data supplies peer ai_civilizations and enabled-mod comparison.
// Guest-memory-free model; only LocalSettings reads the running title, defined
// in app.cpp.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rex/session_data.h>

namespace rerevved::host_settings
{

constexpr int32_t kMaxAiCivilizations = 4;

struct ModRef
{
    std::string id;
    std::string version;
    // Shown in warnings only; never compared.
    std::string displayName;
};

struct HostSettings
{
    int32_t             aiCivilizations = kMaxAiCivilizations;
    std::vector<ModRef> mods;
    // The channel could not hold the full mod list; only a prefix was sent.
    bool modsTruncated = false;
};

// Version 1: version, ai_civilizations, flags (bit 0 truncated), mod count,
// then per mod the id, version and display name, each a length byte and bytes.
// Mods are sorted by id, so equal settings encode to equal bytes.
std::vector<uint8_t> Encode(const HostSettings& settings);

// Empty for an unknown version, reserved flag bits, short or trailing bytes, or
// mods not in strictly ascending id order. Clamps ai_civilizations to 0..4.
std::optional<HostSettings> Decode(std::span<const uint8_t> bytes);

// The mods this machine activated, published once before the guest starts so
// the host data provider and the new-game hook never read the live catalog,
// which a rescan can replace. Empty until published.
void                PublishLocalMods(std::vector<ModRef> mods);
std::vector<ModRef> LocalMods();

// This machine's ai_civilizations and published mods.
HostSettings LocalSettings();

// Truncated host mod lists report that the full comparison was unavailable and
// never return an empty mismatch string.
std::string DescribeModMismatch(const std::vector<ModRef>& host,
                                const std::vector<ModRef>& local,
                                bool                       hostTruncated = false);

// Host limit provenance is local, host, received, or fallback depending on
// network source and usability.
int32_t ResolveAiLimit(bool                             networkGame,
                       const rex::session_data::Status& status,
                       int32_t                          local,
                       std::string*                     source);

} // namespace rerevved::host_settings
