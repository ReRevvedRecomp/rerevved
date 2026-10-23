#pragma once

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, discord_invites);
REXCVAR_DECLARE(bool, discord_join_launch);

namespace rerevved
{

// Discord application shared by Rich Presence, joins and launch registration.
inline constexpr char kDiscordClientId[] = "1539761702416162938";

// discord_invites as read at startup; it needs a restart to change.
bool DiscordInvitesEnabled();

void StartPresence();
void StopPresence();

} // namespace rerevved
