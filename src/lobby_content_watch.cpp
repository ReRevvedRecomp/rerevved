#include "lobby_content_watch.h"

#include "gameplay_session_settings.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <rex/logging.h>
#include <rex/ui/notifications.h>

namespace rerevved::lobby_content_watch
{

namespace
{

constexpr std::chrono::milliseconds kPollInterval{ 500 };

std::atomic<bool>       running{ false };
std::mutex              wakeMutex;
std::condition_variable wake;
std::thread             thread;

void postWarning(const host_settings::LobbyWarning& warning)
{
    REXLOG_WARN("Lobby content: {}", warning.title);
    rex::ui::PostNotification(warning.title, warning.message);
}

host_settings::HostSettings localSettings()
{
    return host_settings::LocalSettings();
}

void run()
{
    Watch watch;
    while (running.load(std::memory_order_acquire))
    {
        try
        {
            watch.Poll(rex::session_data::Current(), localSettings, postWarning);
        }
        catch (...)
        {
            REXLOG_ERROR("Lobby content: the comparison failed");
        }
        std::unique_lock lock(wakeMutex);
        wake.wait_for(lock, kPollInterval, []
                      {
                          return !running.load(std::memory_order_acquire);
                      });
    }
}

} // namespace

void Watch::Poll(const rex::session_data::Status& status, LocalProvider local, Poster post)
{
    if (status.state != rex::session_data::State::kReceived)
    {
        seen_.reset();
        return;
    }
    if (status.game_ready || (seen_ && *seen_ == status.bytes))
    {
        return;
    }
    // A stuck setup can hold the rule boundary; skipping leaves seen_ unset so a later poll retries.
    std::optional<host_settings::LobbyWarning> warning;
    {
        const auto boundary = gameplay_session::TryLockRuleBoundary();
        if (!boundary.owns_lock())
        {
            return;
        }
        seen_   = status.bytes;
        warning = host_settings::CheckLobbyContent(status, local());
    }
    if (warning)
    {
        post(*warning);
    }
}

std::optional<host_settings::LobbyWarning> ReadyRefusal(const rex::session_data::Status& status, LocalProvider local)
{
    const auto boundary = gameplay_session::TryLockRuleBoundary();
    if (!boundary.owns_lock())
    {
        return std::nullopt;
    }
    return host_settings::CheckLobbyContent(status, local());
}

void Start()
{
    if (running.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    thread = std::thread(run);
}

void Stop()
{
    if (!running.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    {
        // Taking the lock orders the flag change before the waiter's next check.
        std::lock_guard lock(wakeMutex);
    }
    wake.notify_all();
    if (thread.joinable())
    {
        thread.join();
    }
}

} // namespace rerevved::lobby_content_watch

bool RefuseLobbyReady()
try
{
    const auto refusal = rerevved::lobby_content_watch::ReadyRefusal(rex::session_data::Current(), rerevved::host_settings::LocalSettings);
    if (!refusal)
    {
        return false;
    }
    REXLOG_WARN("Lobby content: ready press refused");
    rex::ui::PostNotification("You can't ready up", refusal->message);
    return true;
}
catch (...)
{
    return false;
}
