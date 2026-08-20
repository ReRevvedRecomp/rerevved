#include "presence.h"

#include <rex/discord_rpc.h>

namespace rerevved
{

namespace
{

constexpr char kDiscordClientId[] = "1539761702416162938";

} // namespace

void StartPresence()
{
    rex::discord_rpc::Presence initial;
    initial.details_          = "Playing Civilization Revolution";
    initial.large_image_key_  = "rerevved";
    initial.large_image_text_ = "ReRevved";
    rex::discord_rpc::Start(kDiscordClientId, initial);
}

void StopPresence()
{
    rex::discord_rpc::Stop();
}

} // namespace rerevved
