#include "discord_invites_model.h"

#include <algorithm>
#include <array>
#include <utility>

namespace rerevved::discord_invites
{

namespace
{

constexpr std::array<std::string_view, 3> kPerLaunchOptions = {
    "live_invite",
    "discord_join_launch",
    "profile_copy_from_default",
};

// The SDK command line accepts --name, --name=value and --no-name; a string
// option such as --live_invite also takes its value as the next argument.
bool isPerLaunchArgument(std::string_view argument, bool& consumesNext)
{
    consumesNext = false;
    if (!argument.starts_with("--"))
    {
        return false;
    }
    argument.remove_prefix(2);
    if (argument.starts_with("no-"))
    {
        argument.remove_prefix(3);
    }
    const auto             equals = argument.find('=');
    const std::string_view name   = argument.substr(0, equals);
    if (std::find(kPerLaunchOptions.begin(), kPerLaunchOptions.end(), name) ==
        kPerLaunchOptions.end())
    {
        return false;
    }
    consumesNext = equals == std::string_view::npos && name == "live_invite";
    return true;
}

std::string withReason(std::string_view lead, std::string_view reason, std::string_view fallback)
{
    std::string text(lead);
    text += "\n\n";
    text += reason.empty() ? fallback : reason;
    return text;
}

} // namespace

std::optional<PlayerMessage> JoinMessage(const JoinStatus& status)
{
    switch (status.result)
    {
        case JoinResult::kPending:
        case JoinResult::kAccepted:
            return std::nullopt;
        case JoinResult::kRestartRequired:
            return PlayerMessage{
                MessageKind::kHelp,
                withReason("Restart ReRevved to join this game from Discord.",
                           status.reason,
                           "The current System Link settings cannot join this session.") +
                    "\n\nClose ReRevved, then click Join in Discord again.",
            };
        case JoinResult::kInvalid:
            return PlayerMessage{
                MessageKind::kWarning,
                withReason("Could not join from Discord.",
                           status.reason,
                           "The invite is not a valid ReRevved invite."),
            };
        case JoinResult::kUnavailable:
            return PlayerMessage{
                MessageKind::kWarning,
                withReason("Could not join from Discord.",
                           status.reason,
                           "ReRevved could not check the invite. Try Join in Discord again later."),
            };
    }
    return std::nullopt;
}

std::string_view JoinResultName(JoinResult result)
{
    switch (result)
    {
        case JoinResult::kPending:
            return "pending";
        case JoinResult::kAccepted:
            return "accepted";
        case JoinResult::kRestartRequired:
            return "restart required";
        case JoinResult::kInvalid:
            return "invalid";
        case JoinResult::kUnavailable:
            return "unavailable";
    }
    return "unknown";
}

PartyState::PartyState(std::function<std::string()> makePartyId)
: makePartyId(std::move(makePartyId))
{
}

PartyPublish PartyState::Update(const SessionStatus& session, bool invitesEnabled, const HostInvite& invite)
{
    if (!session.active || session.host != wasHost)
    {
        partyId.clear();
    }
    wasHost = session.active && session.host;
    if (!session.active)
    {
        return {};
    }
    if (partyId.empty())
    {
        partyId = makePartyId();
    }

    PartyPublish publish;
    publish.partyId = partyId;
    if (session.maxMembers > 0)
    {
        publish.max  = static_cast<int>(std::min<uint32_t>(session.maxMembers, 1024));
        publish.size = std::clamp(static_cast<int>(std::min<uint32_t>(session.members, 1024)), 1, publish.max);
    }
    if (invitesEnabled && session.host)
    {
        publish.secret = invite.code;
    }
    return publish;
}

InviteSession::InviteSession(InviteClient& client, std::function<std::string()> makePartyId, bool invitesEnabled)
: client(client)
, invitesEnabled(invitesEnabled)
, party(std::move(makePartyId))
{
}

void InviteSession::DeliverJoin(std::string secret)
{
    std::lock_guard lock(deliveredMutex);
    delivered = std::move(secret);
}

void InviteSession::Step(const SessionStatus& session)
{
    publish(session);
    if (invitesEnabled)
    {
        join();
        return;
    }
    // Never touch the invite or join API when off.
    std::lock_guard lock(deliveredMutex);
    if (delivered)
    {
        delivered.reset();
        client.Log("Discord join ignored: discord_invites is off");
    }
}

void InviteSession::publish(const SessionStatus& session)
{
    // Only a host has an invite, and asking for one requests a relay token.
    const HostInvite   invite = invitesEnabled && session.active && session.host ? client.CurrentHostInvite()
                                                                                 : HostInvite{};
    const PartyPublish next   = party.Update(session, invitesEnabled, invite);
    if (next == lastSent)
    {
        return;
    }
    if (next.secret.empty() && !lastSent.secret.empty())
    {
        client.ClearJoinSecret();
    }
    if (next.partyId != lastSent.partyId || next.size != lastSent.size || next.max != lastSent.max)
    {
        if (next.partyId.empty())
        {
            client.ClearParty();
        }
        else
        {
            client.SetParty(next.partyId, next.size, next.max);
        }
    }
    if (!next.secret.empty() && next.secret != lastSent.secret)
    {
        client.SetJoinSecret(next.secret);
    }
    lastSent = next;
}

void InviteSession::join()
{
    std::optional<std::string> secret;
    {
        std::lock_guard lock(deliveredMutex);
        secret.swap(delivered);
    }

    JoinStatus status;
    if (secret)
    {
        client.Log("Discord join received");
        status      = client.BeginJoin(*secret);
        joinPending = true;
    }
    else if (joinPending)
    {
        status = client.PollJoin();
    }
    else
    {
        return;
    }

    if (status.result == JoinResult::kPending)
    {
        return;
    }
    joinPending = false;
    client.Log("Discord join result: " + std::string(JoinResultName(status.result)));
    if (auto message = JoinMessage(status))
    {
        client.ShowMessage(message->kind, message->text);
    }
}

std::string QuoteWindowsArgument(std::string_view argument)
{
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string_view::npos)
    {
        return std::string(argument);
    }

    // CommandLineToArgvW rules: backslashes are literal unless they precede a
    // quote, so double them before an escaped quote and the closing quote.
    std::string quoted      = "\"";
    size_t      backslashes = 0;
    for (const char c : argument)
    {
        if (c == '\\')
        {
            ++backslashes;
            continue;
        }
        if (c == '"')
        {
            quoted.append(backslashes * 2 + 1, '\\');
        }
        else
        {
            quoted.append(backslashes, '\\');
        }
        backslashes = 0;
        quoted += c;
    }
    quoted.append(backslashes * 2, '\\');
    quoted += '"';
    return quoted;
}

std::string BuildLaunchCommand(std::string_view                executablePath,
                               const std::vector<std::string>& arguments)
{
    std::string command = "\"" + std::string(executablePath) + "\"";
    for (size_t i = 0; i < arguments.size(); ++i)
    {
        bool consumesNext = false;
        if (isPerLaunchArgument(arguments[i], consumesNext))
        {
            if (consumesNext)
            {
                ++i;
            }
            continue;
        }
        command += ' ';
        command += QuoteWindowsArgument(arguments[i]);
    }
    command += ' ';
    command += kJoinLaunchArgument;
    return command;
}

std::string ProtocolKeyName(std::string_view applicationId)
{
    return "Software\\Classes\\discord-" + std::string(applicationId);
}

std::string ProtocolDescription(std::string_view applicationId)
{
    return "URL:Run game " + std::string(applicationId) + " protocol";
}

std::string ProtocolIcon(std::string_view executablePath)
{
    return "\"" + std::string(executablePath) + "\",0";
}

} // namespace rerevved::discord_invites
