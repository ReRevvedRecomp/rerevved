#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rex/online_host.h>

namespace rerevved::online_host
{

// One English line of at most 60 characters, like the SDK's connection
// status. `discordJoin` sends friends to Join on the host's Discord profile
// before the code.
std::string StatusCaption(const rex::online_host::Status& status, bool discordJoin);

// Drops the per-launch arguments and any hosting option. `host` adds the
// options that host with Player Match on, which Discord Join needs.
std::string RelaunchCommand(std::string_view                executablePath,
                            const std::vector<std::string>& arguments,
                            bool                            host);

} // namespace rerevved::online_host
