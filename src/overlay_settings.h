#pragma once

#include <optional>
#include <string_view>

#include <rex/ui/overlay/settings_layout.h>

#include "host_settings.h"

namespace rerevved
{

// F1 Settings layout shared with Options: categories, order and labels.
void ConfigureSettingsLayout(rex::ui::SettingsLayout& layout);

// The host's value in the text form the cvar's getter returns; a null host plays the native defaults.
std::optional<rex::ui::SettingsRowLock> HostRowLock(const host_settings::HostSettings* host, std::string_view name);

std::optional<rex::ui::SettingsRowLock> LobbyRowLock(const rex::session_data::Status& status, std::string_view name);

} // namespace rerevved
