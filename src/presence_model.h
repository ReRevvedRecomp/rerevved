#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <game_state.h>

namespace rerevved
{

struct PresenceModel
{
    std::string details;
    std::string state;
    std::string large_image_key;
    std::string large_image_text;
    std::string small_image_key;
    std::string small_image_text;

    bool operator==(const PresenceModel&) const = default;
};

inline constexpr auto kPresencePublishInterval = std::chrono::seconds{ 5 };

bool          TryBuildGameplayPresence(const ReRevvedGameplayState& state,
                                       PresenceModel&               presence);
PresenceModel SelectPresence(
    const ReRevvedGameplayState*        state,
    const std::optional<PresenceModel>& retained_gameplay);

} // namespace rerevved
