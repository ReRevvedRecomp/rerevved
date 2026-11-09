#include "lobby_content_watch.h"

#include "gameplay_session_settings.h"

#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

namespace host = rerevved::host_settings;
using rex::session_data::State;
using rex::session_data::Status;

int                      localReads = 0;
std::vector<std::string> posted;

host::HostSettings withMods(std::vector<host::ModRef> mods)
{
    host::HostSettings settings;
    settings.mods = std::move(mods);
    return settings;
}

host::HostSettings hostSettings;
host::HostSettings localSettings;

host::HostSettings readLocal()
{
    ++localReads;
    return localSettings;
}

void post(const host::LobbyWarning& warning)
{
    posted.push_back(warning.title + "|" + warning.message);
}

Status lobby(const host::HostSettings& settings, State state = State::kReceived, bool ready = false)
{
    return { state, host::Encode(settings), ready };
}

} // namespace

TEST_CASE("ready_press_is_refused_only_when_admission_would_refuse", "[ctest][native-settings]")
{
    namespace watch = rerevved::lobby_content_watch;
    hostSettings    = withMods({ { "founding-militia", "1.0", "Founding Militia" } });
    localSettings   = {};

    const auto refusal = watch::ReadyRefusal(lobby(hostSettings), readLocal);
    REQUIRE(refusal);
    CHECK(refusal->title == "Mods differ from the host");

    CHECK_FALSE(watch::ReadyRefusal(lobby(localSettings), readLocal));
    CHECK_FALSE(watch::ReadyRefusal(lobby(hostSettings, State::kHost), readLocal));
    CHECK_FALSE(watch::ReadyRefusal({ State::kPending, {}, false }, readLocal));
    CHECK_FALSE(watch::ReadyRefusal({ State::kNone, {}, false }, readLocal));
}

TEST_CASE("lobby content watch warns once for each set of host bytes", "[ctest][lobby-content-watch]")
{
    localReads = 0;
    posted.clear();
    hostSettings  = withMods({ { "hills_production", "1.2.0", "Hills Production" } });
    localSettings = withMods({});
    rerevved::lobby_content_watch::Watch watch;

    const auto status = lobby(hostSettings);
    watch.Poll(status, readLocal, post);
    watch.Poll(status, readLocal, post);
    watch.Poll(status, readLocal, post);
    REQUIRE(posted.size() == 1);
    CHECK(posted[0] == "Mods differ from the host|Hills Production: host on, you off. Match the host's mods to play this game.");
    CHECK(localReads == 1);

    auto changed = hostSettings;
    changed.mods.push_back({ "keshik_movement", "0.4.1", "Keshik Movement" });
    watch.Poll(lobby(changed), readLocal, post);
    CHECK(posted.size() == 2);
    watch.Poll({}, readLocal, post);
    watch.Poll(lobby(changed), readLocal, post);
    CHECK(posted.size() == 3);
}

TEST_CASE("lobby content watch stays quiet for matching content and game-ready bytes", "[ctest][lobby-content-watch]")
{
    localReads = 0;
    posted.clear();
    hostSettings                    = withMods({ { "hills_production", "1.2.0", "Hills Production" } });
    hostSettings.rulesDigestPresent = true;
    hostSettings.rulesDigest.fill(7);
    localSettings = hostSettings;
    rerevved::lobby_content_watch::Watch watch;

    watch.Poll(lobby(hostSettings), readLocal, post);
    CHECK(posted.empty());
    CHECK(localReads == 1);

    localSettings = withMods({});
    watch.Poll(lobby(withMods({ { "other", "1", "Other" } })), readLocal, post);
    REQUIRE(posted.size() == 1);

    rerevved::lobby_content_watch::Watch other;
    other.Poll(lobby(hostSettings, State::kReceived, true), readLocal, post);
    other.Poll(lobby(hostSettings, State::kHost), readLocal, post);
    other.Poll(lobby(hostSettings, State::kPending), readLocal, post);
    CHECK(posted.size() == 1);
}

TEST_CASE("lobby content watch skips a poll while another thread holds the rule boundary", "[ctest][lobby-content-watch]")
{
    localReads = 0;
    posted.clear();
    hostSettings  = withMods({ { "hills_production", "1.2.0", "Hills Production" } });
    localSettings = withMods({});
    rerevved::lobby_content_watch::Watch watch;
    const auto                           status = lobby(hostSettings);

    std::mutex              gate;
    std::condition_variable changed;
    bool                    held     = false;
    bool                    released = false;
    std::thread             holder(
        [&]
        {
            const auto       boundary = rerevved::gameplay_session::LockRuleBoundary();
            std::unique_lock lock(gate);
            held = true;
            changed.notify_all();
            changed.wait(lock, [&]
                         {
                             return released;
                         });
        });
    {
        std::unique_lock lock(gate);
        changed.wait(lock, [&]
                     {
                         return held;
                     });
    }
    watch.Poll(status, readLocal, post);
    CHECK(posted.empty());
    CHECK(localReads == 0);
    {
        std::lock_guard lock(gate);
        released = true;
    }
    changed.notify_all();
    holder.join();

    watch.Poll(status, readLocal, post);
    CHECK(posted.size() == 1);
    CHECK(localReads == 1);
}
