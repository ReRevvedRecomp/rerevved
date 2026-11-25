#include "online_host_status.h"

#include "discord_invites.h"

namespace rerevved::online_host
{

namespace
{

using rex::port_mapping::Failure;
using rex::port_mapping::State;

// The SDK command line accepts --name, --name=value and --no-name.
bool isPlayerMatchArgument(std::string_view argument)
{
    if (!argument.starts_with("--"))
    {
        return false;
    }
    argument.remove_prefix(2);
    if (argument.starts_with("no-"))
    {
        argument.remove_prefix(3);
    }
    const std::string_view name = argument.substr(0, argument.find('='));
    return name == "live_sessions";
}

std::string_view failureCaption(Failure failure)
{
    switch (failure)
    {
        case Failure::kNoNetwork:
            return "Can't host: no network connection";
        case Failure::kNoDiscovery:
            return "Can't host: turn on UPnP in your router";
        case Failure::kNoRouter:
            return "Can't host: turn on UPnP in your router";
        case Failure::kPortsTaken:
            return "Can't host: the router port is already in use";
        case Failure::kSharedAddress:
            return "Can't host: your Internet provider blocks hosting";
        case Failure::kRefused:
        case Failure::kNone:
            break;
    }
    return "Can't host: your router refused the port";
}

} // namespace

std::string StatusCaption(const rex::online_host::Status& status, bool discordJoin)
{
    if (!status.hosting)
    {
        return "Not hosting";
    }
    if (!status.error.empty())
    {
        return "Can't host: " + status.error;
    }
    if (status.router.state == State::kFailed)
    {
        return std::string(failureCaption(status.router.failure));
    }
    if (status.address.empty())
    {
        return status.router.state == State::kOpening ? "Starting: asking your router for a port" : "Hosting on this PC only: friends can't reach it";
    }
    return discordJoin ? "Hosting. Friends join from your Discord profile or invite" : "Hosting. Press Copy Friends' Code and send it";
}

std::string RelaunchCommand(std::string_view                executablePath,
                            const std::vector<std::string>& arguments,
                            bool                            host)
{
    std::string command = "\"" + std::string(executablePath) + "\"";
    for (const std::string& argument : discord_invites::PersistentArguments(arguments))
    {
        // The host option is per-launch and already gone; Player Match was added with it.
        if (isPlayerMatchArgument(argument))
        {
            continue;
        }
        command += ' ';
        command += discord_invites::QuoteWindowsArgument(argument);
    }
    if (host)
    {
        command += " --online_host=true --live_sessions=true";
    }
    return command;
}

} // namespace rerevved::online_host
