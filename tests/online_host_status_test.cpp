#include "online_host_status.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

namespace
{

using namespace rerevved::online_host;
using rex::port_mapping::Failure;
using rex::port_mapping::State;

rex::online_host::Status hosting(State state, std::string address = {})
{
    rex::online_host::Status status;
    status.hosting      = true;
    status.router.state = state;
    status.address      = std::move(address);
    return status;
}

rex::online_host::Status failed(Failure failure)
{
    auto status           = hosting(State::kFailed);
    status.router.failure = failure;
    return status;
}

} // namespace

TEST_CASE("online_host_status", "[ctest][online-host-status][local-fast]")
{
    UNSCOPED_INFO("a launch that does not host reads Off");
    CHECK(StatusCaption({}, true) == "Not hosting");

    UNSCOPED_INFO("Discord Join leads once friends have an address");
    CHECK(StatusCaption(hosting(State::kOpen, "203.0.113.7:30100"), true) == "Hosting. Friends join from your Discord profile or invite");
    UNSCOPED_INFO("the code leads when Discord Join cannot work");
    CHECK(StatusCaption(hosting(State::kOpen, "203.0.113.7:30100"), false) == "Hosting. Press Copy Friends' Code and send it");
    UNSCOPED_INFO("a configured address needs no router");
    CHECK(StatusCaption(hosting(State::kClosed, "play.example.net:30100"), true) == "Hosting. Friends join from your Discord profile or invite");

    UNSCOPED_INFO("no address yet");
    CHECK(StatusCaption(hosting(State::kOpening), true) == "Starting: asking your router for a port");
    CHECK(StatusCaption(hosting(State::kOpen), true) == "Hosting on this PC only: friends can't reach it");

    UNSCOPED_INFO("a failure names its cause and never offers Discord or the code");
    CHECK(StatusCaption(failed(Failure::kNoNetwork), true) == "Can't host: no network connection");
    CHECK(StatusCaption(failed(Failure::kNoDiscovery), true) == "Can't host: turn on UPnP in your router");
    CHECK(StatusCaption(failed(Failure::kNoRouter), true) == "Can't host: turn on UPnP in your router");
    CHECK(StatusCaption(failed(Failure::kPortsTaken), true) == "Can't host: the router port is already in use");
    CHECK(StatusCaption(failed(Failure::kSharedAddress), true) == "Can't host: your Internet provider blocks hosting");
    CHECK(StatusCaption(failed(Failure::kRefused), true) == "Can't host: your router refused the port");
    auto error  = hosting(State::kOpen, "203.0.113.7:30100");
    error.error = "the port is in use";
    CHECK(StatusCaption(error, true) == "Can't host: the port is in use");

    UNSCOPED_INFO("fixed captions fit the status row");
    for (const Failure failure : { Failure::kNoNetwork, Failure::kNoDiscovery, Failure::kNoRouter, Failure::kPortsTaken, Failure::kSharedAddress, Failure::kRefused })
        CHECK(StatusCaption(failed(failure), true).size() <= 60);
    CHECK(StatusCaption(hosting(State::kOpen, "203.0.113.7:30100"), true).size() <= 60);
    CHECK(StatusCaption(hosting(State::kOpen), true).size() <= 60);

    const std::string exe = "C:\\Games\\ReRevved\\rerevved.exe";
    UNSCOPED_INFO("hosting adds the host option and Player Match");
    CHECK(RelaunchCommand(exe, {}, true) == "\"C:\\Games\\ReRevved\\rerevved.exe\" --online_host=true --live_sessions=true");
    UNSCOPED_INFO("stopping drops every hosting option and keeps the rest");
    CHECK(RelaunchCommand(exe, { "--profile=friend", "--online_host=true", "--live_sessions=true", "--log_file=C:\\My Logs\\run.log" }, false) ==
          "\"C:\\Games\\ReRevved\\rerevved.exe\" --profile=friend \"--log_file=C:\\My Logs\\run.log\"");
    UNSCOPED_INFO("hosting again does not repeat or contradict the options");
    CHECK(RelaunchCommand(exe, { "--online_host", "--no-live_sessions", "--live_sessions=false", "--no-online_host" }, true) ==
          RelaunchCommand(exe, {}, true));
    UNSCOPED_INFO("a friend's invite and a Discord launch marker do not carry over");
    CHECK(RelaunchCommand(exe, { "--live_invite", "rr1.abc", "--discord_join_launch=true", "--profile=a" }, true) ==
          "\"C:\\Games\\ReRevved\\rerevved.exe\" --profile=a --online_host=true --live_sessions=true");
    UNSCOPED_INFO("only exact option names are filtered");
    CHECK(RelaunchCommand(exe, { "--online_host_port=30200" }, false) == "\"C:\\Games\\ReRevved\\rerevved.exe\" --online_host_port=30200");
}
