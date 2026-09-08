#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <gameplay_state.h>

namespace rerevved
{

struct PresenceModel
{
    std::string details;
    std::string state;
    std::string largeImageKey;
    std::string largeImageText;
    std::string smallImageKey;
    std::string smallImageText;

    bool operator==(const PresenceModel&) const = default;
};

inline constexpr auto kPresencePublishInterval = std::chrono::seconds{ 5 };

bool          TryBuildGameplayPresence(const GameplayState& state,
                                       PresenceModel&       presence);
PresenceModel SelectPresence(
    const GameplayState*                state,
    const std::optional<PresenceModel>& retainedGameplay);

} // namespace rerevved
