#include "online_host.h"

#include <chrono>
#include <mutex>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#include <shellapi.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/online_host.h>

#include "discord_presence.h"
#include "online_host_status.h"

namespace rerevved::online_host
{

namespace
{

std::mutex  gHostMutex;
HostActions gHost;

// Friends can use Discord Join when the Join Button and Player Match are on.
bool discordJoin()
{
    return DiscordInvitesEnabled() && rex::cvar::Query<bool>("live_sessions");
}

// The settings pages read the row far more often than the status changes,
// and each read of the SDK status encodes the friends' code.
std::string statusCaption()
{
    static std::mutex                            mutex;
    static std::string                           caption;
    static std::chrono::steady_clock::time_point read;
    std::lock_guard                              lock(mutex);
    const auto                                   now = std::chrono::steady_clock::now();
    if (caption.empty() || now - read >= std::chrono::milliseconds(500))
    {
        caption = StatusCaption(rex::online_host::Current(), discordJoin());
        read    = now;
    }
    return caption;
}

#if defined(_WIN32)

std::mutex   gRelaunchMutex;
std::wstring gRelaunchCommand;

std::string toUtf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int   size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring toWide(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int    size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring relaunchCommand(bool host)
{
    std::wstring exe(32768, L'\0');
    const DWORD  length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (length == 0 || length >= exe.size())
    {
        return {};
    }
    exe.resize(length);

    std::vector<std::string> arguments;
    int                      count = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count))
    {
        for (int i = 1; i < count; ++i)
        {
            arguments.push_back(toUtf8(argv[i]));
        }
        LocalFree(argv);
    }
    return toWide(RelaunchCommand(toUtf8(exe), arguments, host));
}

#endif

void relaunch(bool host)
{
    const auto status  = rex::online_host::Current();
    const bool healthy = status.hosting && status.error.empty() && status.router.state != rex::port_mapping::State::kFailed;
    // Host again after a failure retries it; otherwise there is nothing to change.
    if (host ? healthy : !status.hosting)
    {
        return;
    }
#if defined(_WIN32)
    std::lock_guard lock(gHostMutex);
    if (!gHost.quit)
    {
        return;
    }
    std::wstring command = relaunchCommand(host);
    if (command.empty())
    {
        REXLOG_WARN("Online hosting: the restart command could not be built");
        return;
    }
    {
        std::lock_guard relaunchLock(gRelaunchMutex);
        gRelaunchCommand = std::move(command);
    }
    REXLOG_INFO("Online hosting: restarting the game {}", host ? "to host" : "without hosting");
    gHost.quit();
#else
    REXLOG_WARN("Online hosting: restart the game {} --online_host=true", host ? "with" : "without");
#endif
}

void copyFriendsCode()
{
    std::string code = rex::online_host::Current().friends_code;
    if (code.empty())
    {
        return;
    }
    std::lock_guard lock(gHostMutex);
    if (gHost.copy)
    {
        gHost.copy(std::move(code));
    }
}

rex::cvar::FlagRegistrar gStatusFlag({ .name        = "online_host_status",
                                       .type        = rex::cvar::FlagType::String,
                                       .category    = "Online",
                                       .description = "Whether this launch hosts an online game and how friends join it",
                                       .setter      = [](std::string_view)
                                       {
                                           return false;
                                       },
                                       .getter        = statusCaption,
                                       .lifecycle     = rex::cvar::Lifecycle::kInitOnly,
                                       .default_value = "Not hosting",
                                       .audience      = rex::cvar::Audience::kPlayer,
                                       .persistence   = rex::cvar::Persistence::kNeverPersist });

} // namespace

void SetHostActions(HostActions actions)
{
    std::lock_guard lock(gHostMutex);
    gHost = std::move(actions);
}

void RelaunchIfRequested()
{
#if defined(_WIN32)
    std::wstring command;
    {
        std::lock_guard lock(gRelaunchMutex);
        command = std::exchange(gRelaunchCommand, {});
    }
    if (command.empty())
    {
        return;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
    {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    else
    {
        REXLOG_WARN("Online hosting: the game could not be started again ({})", GetLastError());
    }
#endif
}

} // namespace rerevved::online_host

REXCVAR_DEFINE_COMMAND(online_host_start, []
                       {
                           rerevved::online_host::relaunch(true);
                       },
                       "Online",
                       "Restart the game and host an online game for friends on this PC");
REXCVAR_DEFINE_COMMAND(online_host_stop, []
                       {
                           rerevved::online_host::relaunch(false);
                       },
                       "Online",
                       "Restart the game without hosting");
REXCVAR_DEFINE_COMMAND(online_host_copy_code, rerevved::online_host::copyFriendsCode, "Online", "Copy the friends' code of the hosted online game; the code is a credential");
