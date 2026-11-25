#pragma once

#include <optional>
#include <string_view>

#include <rex/ui/overlay/settings_layout.h>

#include "host_settings.h"

namespace rerevved
{

// F1 Settings layout shared with Options: categories, order and labels.
void ConfigureSettingsLayout(rex::ui::SettingsLayout& layout);

// Hosting lasts a whole launch: the rows show what this launch can do.
void ApplyHostingRows(rex::ui::SettingsLayout& layout, bool hosting);

// The host's value in the text form the cvar's getter returns; a null host plays the native defaults.
std::optional<rex::ui::SettingsRowLock> HostRowLock(const host_settings::HostSettings* host, std::string_view name);

inline constexpr std::string_view kWindowSizeLockedReason = "Turn off Fullscreen to choose a window size.";

// Window Size does nothing in fullscreen, so its row is locked at its own value.
std::optional<rex::ui::SettingsRowLock> FullscreenRowLock(bool fullscreen, std::string_view windowSize, std::string_view name);

std::optional<rex::ui::SettingsRowLock> LobbyRowLock(const rex::session_data::Status& status, std::string_view name);

} // namespace rerevved
