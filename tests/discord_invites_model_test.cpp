#include "discord_invites_model.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace rerevved::discord_invites;

struct IdSource
{
    int next = 0;

    std::string operator()()
    {
        return "party" + std::to_string(++next);
    }
};

// Records every Discord and invite call the presence thread step makes.
class FakeClient final : public InviteClient
{
public:
    HostInvite               invite;
    std::vector<JoinStatus>  joinReplies;
    std::vector<std::string> calls;
    std::vector<std::string> begun;
    std::vector<std::string> messages;
    std::vector<std::string> logs;
    int                      inviteQueries = 0;

    HostInvite CurrentHostInvite() override
    {
        ++inviteQueries;
        return invite;
    }

    JoinStatus BeginJoin(std::string_view code) override
    {
        begun.emplace_back(code);
        return nextReply();
    }

    JoinStatus PollJoin() override
    {
        return nextReply();
    }

    void SetParty(const std::string& id, int size, int max) override
    {
        calls.push_back("party " + id + " " + std::to_string(size) + "/" + std::to_string(max));
    }

    void ClearParty() override
    {
        calls.push_back("clear party");
    }

    void SetJoinSecret(const std::string& secret) override
    {
        calls.push_back("secret " + secret);
    }

    void ClearJoinSecret() override
    {
        calls.push_back("clear secret");
    }

    void ShowMessage(MessageKind kind, const std::string& text) override
    {
        messages.push_back((kind == MessageKind::kHelp ? "help: " : "warning: ") + text);
    }

    void Log(const std::string& line) override
    {
        logs.push_back(line);
    }

    std::vector<std::string> takeCalls()
    {
        return std::exchange(calls, {});
    }

private:
    JoinStatus nextReply()
    {
        if (joinReplies.empty())
        {
            return { JoinResult::kUnavailable, "no reply" };
        }
        JoinStatus reply = joinReplies.front();
        joinReplies.erase(joinReplies.begin());
        return reply;
    }
};

using Calls = std::vector<std::string>;

void testLaunchCommand()
{
    const std::string exe = "C:\\Games\\ReRevved\\rerevved.exe";
    UNSCOPED_INFO("bare launch adds the join marker");
    CHECK(BuildLaunchCommand(exe, {}) ==
          "\"C:\\Games\\ReRevved\\rerevved.exe\" --discord_join_launch=true");

    const std::string command = BuildLaunchCommand(
        exe,
        { "--profile=friend", "--live_invite=rr1.abc", "--live_invite", "rr1.def", "--discord_join_launch=true", "--discord_join_launch", "--no-discord_join_launch", "--profile_copy_from_default", "--profile_copy_from_default=false", "--log_file=C:\\My Logs\\run.log", "--window_title=say \"hi\"", "--trailing=C:\\dir with space\\", "", "positional" });
    UNSCOPED_INFO("per-launch arguments are removed and the rest quoted");
    CHECK(command == "\"C:\\Games\\ReRevved\\rerevved.exe\" --profile=friend "
                     "\"--log_file=C:\\My Logs\\run.log\" \"--window_title=say \\\"hi\\\"\" "
                     "\"--trailing=C:\\dir with space\\\\\" \"\" positional --discord_join_launch=true");

    UNSCOPED_INFO("only exact option names are filtered");
    CHECK(BuildLaunchCommand(exe, { "--live_invite_extra=1" }).find("--live_invite_extra=1") != std::string::npos);
    UNSCOPED_INFO("relaunching from Discord keeps the same command");
    CHECK(BuildLaunchCommand(exe, { "--profile=a", "--discord_join_launch=true" }) ==
          BuildLaunchCommand(exe, { "--profile=a" }));

    UNSCOPED_INFO("plain argument is unquoted");
    CHECK(QuoteWindowsArgument("plain") == "plain");
    UNSCOPED_INFO("backslashes without quotes stay literal");
    CHECK(QuoteWindowsArgument("a\\\\b") == "a\\\\b");
    UNSCOPED_INFO("backslashes before a quote are doubled");
    CHECK(QuoteWindowsArgument("a\\\"b") == "\"a\\\\\\\"b\"");

    UNSCOPED_INFO("protocol key under HKCU classes");
    CHECK(ProtocolKeyName("1539761702416162938") == "Software\\Classes\\discord-1539761702416162938");
    UNSCOPED_INFO("protocol description matches discord-rpc");
    CHECK(ProtocolDescription("1539761702416162938") == "URL:Run game 1539761702416162938 protocol");
    UNSCOPED_INFO("icon names the executable");
    CHECK(ProtocolIcon(exe) == "\"C:\\Games\\ReRevved\\rerevved.exe\",0");
}

void testJoinMessages()
{
    UNSCOPED_INFO("pending shows nothing");
    CHECK(!JoinMessage({ JoinResult::kPending, {} }));
    UNSCOPED_INFO("accepted shows nothing");
    CHECK(!JoinMessage({ JoinResult::kAccepted, {} }));

    const auto restart = JoinMessage({ JoinResult::kRestartRequired, "Relay settings differ." });
    UNSCOPED_INFO("restart required gives the reason and the relaunch step");
    CHECK((restart && restart->kind == MessageKind::kHelp &&
           restart->text == "Restart ReRevved to join this game from Discord.\n\n"
                            "Relay settings differ.\n\n"
                            "Close ReRevved, then click Join in Discord again."));

    const auto invalid = JoinMessage({ JoinResult::kInvalid, "Bad code." });
    UNSCOPED_INFO("invalid warns with the reason");
    CHECK((invalid && invalid->kind == MessageKind::kWarning && invalid->text.ends_with("Bad code.")));
    const auto unavailable = JoinMessage({ JoinResult::kUnavailable, {} });
    UNSCOPED_INFO("unavailable without a reason uses fallback text");
    CHECK((unavailable && unavailable->kind == MessageKind::kWarning &&
           unavailable->text == "Could not join from Discord.\n\n"
                                "ReRevved could not check the invite. "
                                "Try Join in Discord again later."));
}

constexpr SessionStatus kNoSession{};
constexpr SessionStatus kHosting{ true, true, 1, 4 };
constexpr SessionStatus kJoined{ true, false, 2, 4 };

SessionStatus withMembers(SessionStatus session, uint32_t members)
{
    session.members = members;
    return session;
}

void testPartyState()
{
    PartyState party{ IdSource{} };
    UNSCOPED_INFO("no session publishes nothing");
    CHECK(party.Update(kNoSession, true, { "rr1.code", 1, 4 }) == PartyPublish{});

    UNSCOPED_INFO("a hosted party is shown without the invite setting");
    CHECK(party.Update(kHosting, false, { "rr1.code", 1, 4 }) == PartyPublish{ "party1", 1, 4, "" });
    UNSCOPED_INFO("hosting without a code sets the party only");
    CHECK(party.Update(kHosting, true, {}) == PartyPublish{ "party1", 1, 4, "" });
    UNSCOPED_INFO("code arrival keeps the party id and adds the secret");
    CHECK(party.Update(withMembers(kHosting, 2), true, { "rr1.code", 2, 4 }) ==
          PartyPublish{ "party1", 2, 4, "rr1.code" });
    UNSCOPED_INFO("token refresh keeps the party id");
    CHECK(party.Update(kHosting, true, { "rr1.next", 1, 4 }).partyId == "party1");
    UNSCOPED_INFO("a lapsed code keeps the session's id");
    CHECK(party.Update(kHosting, true, {}).partyId == "party1");
    UNSCOPED_INFO("the host always counts as a member");
    CHECK(party.Update(withMembers(kHosting, 0), true, {}).size == 1);
    UNSCOPED_INFO("size never exceeds the maximum");
    CHECK(party.Update(withMembers(kHosting, 9), true, {}).size == 4);

    UNSCOPED_INFO("switching to joined gets a new id and never a secret");
    CHECK(party.Update(kJoined, true, { "rr1.code", 1, 4 }) == PartyPublish{ "party2", 2, 4, "" });
    UNSCOPED_INFO("a joined session keeps its id");
    CHECK(party.Update(kJoined, true, {}).partyId == "party2");
    UNSCOPED_INFO("session end clears everything");
    CHECK(party.Update(kNoSession, true, {}) == PartyPublish{});
    UNSCOPED_INFO("a new session gets a new id");
    CHECK(party.Update(kJoined, false, {}).partyId == "party3");
    UNSCOPED_INFO("switching to host gets a new id");
    CHECK(party.Update(kHosting, true, {}).partyId == "party4");
}

void testSessionDisabled()
{
    FakeClient    client;
    InviteSession session(client, IdSource{}, false);

    client.invite      = { "rr1.code", 1, 4 };
    client.joinReplies = { { JoinResult::kAccepted, {} } };
    session.DeliverJoin("rr1.secret");
    session.Step(kNoSession);
    UNSCOPED_INFO("no session publishes nothing");
    CHECK(client.takeCalls().empty());
    session.Step(kHosting);
    UNSCOPED_INFO("the party size is published without invites");
    CHECK(client.takeCalls() == Calls{ "party party1 1/4" });
    session.Step(withMembers(kHosting, 2));
    UNSCOPED_INFO("member changes are published without invites");
    CHECK(client.takeCalls() == Calls{ "party party1 2/4" });
    session.Step(kNoSession);
    UNSCOPED_INFO("session end clears the party");
    CHECK(client.takeCalls() == Calls{ "clear party" });
    UNSCOPED_INFO("disabled never asks for an invite");
    CHECK(client.inviteQueries == 0);
    UNSCOPED_INFO("disabled never begins or polls a join");
    CHECK((client.begun.empty() && client.joinReplies.size() == 1));
    UNSCOPED_INFO("a join delivered while disabled is dropped once");
    CHECK(client.logs == std::vector<std::string>{ "Discord join ignored: discord_invites is off" });
}

void testSessionPublishing()
{
    FakeClient    client;
    InviteSession session(client, IdSource{}, true);

    session.Step(kNoSession);
    UNSCOPED_INFO("no session neither asks nor publishes");
    CHECK((client.inviteQueries == 0 && client.takeCalls().empty()));

    client.invite = { "", 1, 4 };
    session.Step(kHosting);
    UNSCOPED_INFO("party is published before the code arrives");
    CHECK(client.takeCalls() == Calls{ "party party1 1/4" });
    session.Step(kHosting);
    UNSCOPED_INFO("unchanged values are not re-sent");
    CHECK(client.takeCalls().empty());

    client.invite = { "rr1.code", 1, 4 };
    session.Step(kHosting);
    UNSCOPED_INFO("first secret goes out on the next poll");
    CHECK(client.takeCalls() == Calls{ "secret rr1.code" });
    session.Step(withMembers(kHosting, 2));
    UNSCOPED_INFO("member change updates only the party");
    CHECK(client.takeCalls() == Calls{ "party party1 2/4" });

    const int queries = client.inviteQueries;
    session.Step(kJoined);
    UNSCOPED_INFO("joining another session withdraws the secret and changes the party");
    CHECK(client.takeCalls() == Calls{ "clear secret", "party party2 2/4" });
    UNSCOPED_INFO("a joined session never asks for an invite");
    CHECK(client.inviteQueries == queries);

    session.Step(kNoSession);
    UNSCOPED_INFO("session end withdraws the party");
    CHECK(client.takeCalls() == Calls{ "clear party" });
}

void testSessionJoin()
{
    FakeClient    client;
    InviteSession session(client, IdSource{}, true);

    client.joinReplies = { { JoinResult::kPending, {} }, { JoinResult::kPending, {} }, { JoinResult::kAccepted, {} } };
    session.DeliverJoin("rr1.secret");
    session.Step(kNoSession);
    UNSCOPED_INFO("delivered secret starts a join");
    CHECK(client.begun == std::vector<std::string>{ "rr1.secret" });
    session.Step(kNoSession);
    session.Step(kNoSession);
    UNSCOPED_INFO("accepted join shows no message");
    CHECK(client.messages.empty());
    session.Step(kJoined);
    UNSCOPED_INFO("polling stops once the join resolves");
    CHECK(client.joinReplies.empty());

    client.joinReplies = { { JoinResult::kPending, {} },
                           { JoinResult::kRestartRequired, "Different relay." },
                           { JoinResult::kUnavailable, "should not be polled" } };
    session.DeliverJoin("rr1.other");
    session.Step(kJoined);
    session.Step(kJoined);
    session.Step(kJoined);
    UNSCOPED_INFO("restart required is shown once");
    CHECK((client.messages.size() == 1 && client.messages[0].starts_with("help: ") &&
           client.messages[0].find("Different relay.") != std::string::npos));
    UNSCOPED_INFO("no polling after the restart result");
    CHECK(client.joinReplies.size() == 1);

    for (const auto& line : client.logs)
    {
        UNSCOPED_INFO("logs never contain the secret");
        CHECK(line.find("rr1.") == std::string::npos);
    }
    UNSCOPED_INFO("each join logs its receipt and result");
    CHECK((client.logs.size() == 4 && client.logs[1] == "Discord join result: accepted" &&
           client.logs[3] == "Discord join result: restart required"));
}

} // namespace

TEST_CASE("discord_invites_model", "[ctest][discord-invites-model][local-fast]")
{
    testLaunchCommand();
    testJoinMessages();
    testPartyState();
    testSessionDisabled();
    testSessionPublishing();
    testSessionJoin();
}
