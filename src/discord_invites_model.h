#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rerevved::discord_invites
{

// Mirrors rex::live_invite::HostInvite so this model builds without the SDK.
struct HostInvite
{
    std::string code;
    uint32_t    members    = 0;
    uint32_t    maxMembers = 0;
};

// Mirrors rex::live_invite::SessionStatus.
struct SessionStatus
{
    bool     active     = false;
    bool     host       = false;
    uint32_t members    = 0;
    uint32_t maxMembers = 0;
};

// Mirrors rex::live_invite::JoinResult.
enum class JoinResult
{
    kPending,
    kAccepted,
    kRestartRequired,
    kInvalid,
    kUnavailable,
};

struct JoinStatus
{
    JoinResult  result = JoinResult::kUnavailable;
    std::string reason;
};

enum class MessageKind
{
    kHelp,
    kWarning,
};

struct PlayerMessage
{
    MessageKind kind = MessageKind::kHelp;
    std::string text;

    bool operator==(const PlayerMessage&) const = default;
};

// The player-facing message for a finished join, or nothing for an accepted
// or still pending one.
std::optional<PlayerMessage> JoinMessage(const JoinStatus& status);

// Log text for a join result; never includes the secret.
std::string_view JoinResultName(JoinResult result);

// What Discord should carry: an empty partyId clears the party and an empty
// secret clears the join secret.
struct PartyPublish
{
    std::string partyId;
    int         size = 0;
    int         max  = 0;
    std::string secret;

    bool operator==(const PartyPublish&) const = default;
};

// Party size includes active hosted and joined sessions, even with invites off.
// The random ID lasts until the session ends or switches between hosting and
// joining. It is never derived from the invite code. Only a host with
// invitesEnabled publishes its code as the join secret.
class PartyState
{
public:
    explicit PartyState(std::function<std::string()> makePartyId);

    PartyPublish Update(const SessionStatus& session, bool invitesEnabled, const HostInvite& invite);

private:
    std::function<std::string()> makePartyId;
    std::string                  partyId;
    bool                         wasHost = false;
};

// Stands in for rex::discord_rpc and rex::live_invite so the presence thread
// step can be driven by a fake client in tests.
class InviteClient
{
public:
    virtual ~InviteClient() = default;

    virtual HostInvite CurrentHostInvite()                                    = 0;
    virtual JoinStatus BeginJoin(std::string_view code)                       = 0;
    virtual JoinStatus PollJoin()                                             = 0;
    virtual void       SetParty(const std::string& id, int size, int max)     = 0;
    virtual void       ClearParty()                                           = 0;
    virtual void       SetJoinSecret(const std::string& secret)               = 0;
    virtual void       ClearJoinSecret()                                      = 0;
    virtual void       ShowMessage(MessageKind kind, const std::string& text) = 0;
    virtual void       Log(const std::string& line)                           = 0;
};

// Party publishing, invite publishing and join handling for one run.
// `invitesEnabled` is the discord_invites value read once at startup; the
// setting needs a restart. When it is off, no invite or join call is made.
class InviteSession
{
public:
    InviteSession(InviteClient& client, std::function<std::string()> makePartyId, bool invitesEnabled);

    // Called on the Discord worker thread; only hands the secret over.
    void DeliverJoin(std::string secret);

    // Called on the presence thread once per poll with the current session.
    void Step(const SessionStatus& session);

private:
    void publish(const SessionStatus& session);
    void join();

    InviteClient&              client;
    const bool                 invitesEnabled;
    PartyState                 party;
    PartyPublish               lastSent;
    std::mutex                 deliveredMutex;
    std::optional<std::string> delivered;
    bool                       joinPending = false;
};

// Arguments removed from the relaunch command: Discord launches carry their
// own marker, and the invite and one-time profile copy apply to one launch.
inline constexpr std::string_view kJoinLaunchArgument = "--discord_join_launch=true";

// Quotes one argument so CommandLineToArgvW returns it unchanged.
std::string QuoteWindowsArgument(std::string_view argument);

// The shell\open\command value: the quoted executable path, the current
// launch arguments without the per-launch ones, then kJoinLaunchArgument.
std::string BuildLaunchCommand(std::string_view                executablePath,
                               const std::vector<std::string>& arguments);

std::string ProtocolKeyName(std::string_view applicationId);
std::string ProtocolDescription(std::string_view applicationId);
std::string ProtocolIcon(std::string_view executablePath);

} // namespace rerevved::discord_invites
