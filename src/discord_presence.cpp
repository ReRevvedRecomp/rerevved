#include "discord_presence.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#include <shellapi.h>
#endif

#include <fmt/format.h>
#include <rex/discord_rpc.h>
#include <rex/live_invite.h>
#include <rex/logging.h>
#include <rex/system.h>

#include <gameplay_state.h>

#include "discord_invites_model.h"
#include "discord_presence_model.h"

REXCVAR_DEFINE_BOOL(discord_invites, true, "Online", "Let Discord friends join your Player Match games (needs the Discord app and live_sessions)")
    .audience(rex::cvar::Audience::kPlayer)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Set by the launch command registered for Discord's Join button.
REXCVAR_DEFINE_BOOL(discord_join_launch, false, "Online", "This launch was started by Discord to join a game")
    .audience(rex::cvar::Audience::kHidden)
    .persistence(rex::cvar::Persistence::kSessionOnly);

namespace rerevved
{

namespace
{

namespace invites = discord_invites;

constexpr auto kPresencePollInterval = std::chrono::seconds{ 1 };

std::atomic<bool> gPresenceRunning{ false };
std::thread       gPresenceThread;
// Wakes the poll wait at once on stop, so shutdown never waits out an interval.
std::mutex              gPresenceWakeMutex;
std::condition_variable gPresenceWake;

rex::discord_rpc::Presence toDiscordPresence(const PresenceModel& model)
{
    rex::discord_rpc::Presence presence;
    presence.details_          = model.details;
    presence.state_            = model.state;
    presence.large_image_key_  = model.largeImageKey;
    presence.large_image_text_ = model.largeImageText;
    presence.small_image_key_  = model.smallImageKey;
    presence.small_image_text_ = model.smallImageText;
    return presence;
}

invites::JoinStatus toModelStatus(const rex::live_invite::JoinStatus& status)
{
    using rex::live_invite::JoinResult;
    invites::JoinStatus result{ .reason = status.reason };
    switch (status.result)
    {
        case JoinResult::kPending:
            result.result = invites::JoinResult::kPending;
            break;
        case JoinResult::kAccepted:
            result.result = invites::JoinResult::kAccepted;
            break;
        case JoinResult::kRestartRequired:
            result.result = invites::JoinResult::kRestartRequired;
            break;
        case JoinResult::kInvalid:
            result.result = invites::JoinResult::kInvalid;
            break;
        case JoinResult::kUnavailable:
            result.result = invites::JoinResult::kUnavailable;
            break;
    }
    return result;
}

invites::SessionStatus currentSession()
{
    const auto session = rex::live_invite::CurrentSession();
    return { session.active, session.host, session.members, session.max_members };
}

LobbyRole lobbyRole(const invites::SessionStatus& session)
{
    if (!session.active)
    {
        return LobbyRole::kNone;
    }
    return session.host ? LobbyRole::kHost : LobbyRole::kJoined;
}

class DiscordInviteClient final : public invites::InviteClient
{
public:
    invites::HostInvite CurrentHostInvite() override
    {
        auto invite = rex::live_invite::CurrentHostInvite();
        return { std::move(invite.code), invite.members, invite.max_members };
    }

    invites::JoinStatus BeginJoin(std::string_view code) override
    {
        return toModelStatus(rex::live_invite::BeginJoin(code));
    }

    invites::JoinStatus PollJoin() override
    {
        return toModelStatus(rex::live_invite::PollJoin());
    }

    void SetParty(const std::string& id, int size, int max) override
    {
        rex::discord_rpc::SetParty(id, size, max);
    }

    void ClearParty() override
    {
        rex::discord_rpc::ClearParty();
    }

    void SetJoinSecret(const std::string& secret) override
    {
        rex::discord_rpc::SetJoinSecret(secret);
    }

    void ClearJoinSecret() override
    {
        rex::discord_rpc::ClearJoinSecret();
    }

    void ShowMessage(invites::MessageKind kind, const std::string& text) override
    {
        // The box is modal; keep it off the presence thread so polling and
        // shutdown continue while it is open.
        const auto type = kind == invites::MessageKind::kHelp ? rex::SimpleMessageBoxType::Help
                                                              : rex::SimpleMessageBoxType::Warning;
        std::thread([type, text]
                    {
                        rex::ShowSimpleMessageBox(type, text);
                    })
            .detach();
    }

    void Log(const std::string& line) override
    {
        REXLOG_INFO("{}", line);
    }
};

std::string makePartyId()
{
    std::random_device device;
    std::string        id;
    for (int i = 0; i < 4; ++i)
    {
        id += fmt::format("{:08x}", static_cast<uint32_t>(device()));
    }
    return id;
}

DiscordInviteClient                     gInviteClient;
std::unique_ptr<invites::InviteSession> gInviteSession;

#if defined(_WIN32)

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

std::wstring executablePath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
        {
            return {};
        }
        if (length < path.size())
        {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::vector<std::string> launchArguments()
{
    std::vector<std::string> arguments;
    int                      count = 0;
    LPWSTR*                  argv  = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv)
    {
        return arguments;
    }
    for (int i = 1; i < count; ++i)
    {
        arguments.push_back(toUtf8(argv[i]));
    }
    LocalFree(argv);
    return arguments;
}

bool setRegistryString(HKEY key, const wchar_t* subkey, const wchar_t* name, const std::wstring& value)
{
    const LSTATUS status = RegSetKeyValueW(key, subkey, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS)
    {
        REXLOG_WARN("Discord launch registration: writing a registry value failed ({})", status);
        return false;
    }
    return true;
}

std::optional<std::wstring> readRegistryString(HKEY root, const std::wstring& subkey)
{
    DWORD bytes = 0;
    if (RegGetValueW(root, subkey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subkey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) !=
        ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

// Registers discord-<application id> the way Discord's RPC library does, so
// Discord can start ReRevved for a Join click.
void registerLaunchProtocol()
{
    const std::wstring exe = executablePath();
    if (exe.empty())
    {
        REXLOG_WARN("Discord launch registration: executable path unavailable");
        return;
    }
    const std::string  exeUtf8    = toUtf8(exe);
    const std::wstring keyName    = toWide(invites::ProtocolKeyName(kDiscordClientId));
    const std::wstring command    = toWide(invites::BuildLaunchCommand(exeUtf8, launchArguments()));
    const std::wstring commandKey = keyName + L"\\shell\\open\\command";
    if (readRegistryString(HKEY_CURRENT_USER, commandKey) == command)
    {
        return;
    }

    HKEY          key    = nullptr;
    const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, keyName.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS)
    {
        REXLOG_WARN("Discord launch registration: creating the protocol key failed ({})", status);
        return;
    }
    const bool written =
        setRegistryString(key, nullptr, nullptr, toWide(invites::ProtocolDescription(kDiscordClientId))) &&
        setRegistryString(key, nullptr, L"URL Protocol", L"") &&
        setRegistryString(key, L"DefaultIcon", nullptr, toWide(invites::ProtocolIcon(exeUtf8))) &&
        setRegistryString(key, L"shell\\open\\command", nullptr, command);
    RegCloseKey(key);
    if (written)
    {
        REXLOG_INFO("Discord launch registration updated");
    }
}

#else

void registerLaunchProtocol()
{
}

#endif

void presenceThread(PresenceModel lastSent)
{
    std::optional<PresenceModel> retainedGameplay;
    auto                         lastPublish = std::chrono::steady_clock::now();

    while (gPresenceRunning.load(std::memory_order_acquire))
    {
        {
            std::unique_lock lock(gPresenceWakeMutex);
            if (gPresenceWake.wait_for(lock, kPresencePollInterval, []
                                       {
                                           return !gPresenceRunning.load(std::memory_order_acquire);
                                       }))
            {
                break;
            }
        }

        GameplayState state{};
        const bool    stateAvailable =
            GetGameplayState(&state, sizeof(state)) ==
            GAMEPLAY_OK;
        if (stateAvailable)
        {
            PresenceModel gameplay;
            if (TryBuildGameplayPresence(state, gameplay))
            {
                retainedGameplay = std::move(gameplay);
            }
            else if ((state.validFields &
                      GAMEPLAY_VALID_FRONTEND) != 0 &&
                     !state.gameplayActive)
            {
                retainedGameplay.reset();
            }
        }

        const invites::SessionStatus session = currentSession();
        const PresenceModel          pending =
            SelectPresence(stateAvailable ? &state : nullptr,
                           retainedGameplay,
                           lobbyRole(session));
        const auto now = std::chrono::steady_clock::now();
        if (pending != lastSent &&
            now - lastPublish >= kPresencePublishInterval)
        {
            rex::discord_rpc::SetPresence(toDiscordPresence(pending));
            lastSent    = pending;
            lastPublish = now;
        }

        // Party and join secret changes go out on every poll rather than on
        // the presence interval, so a new join button appears promptly.
        gInviteSession->Step(session);
    }
}

} // namespace

bool DiscordInvitesEnabled()
{
    // Read once, during startup and before any settings UI exists, so later
    // console or settings writes never race the presence thread.
    static const bool enabled = REXCVAR_GET(discord_invites);
    return enabled;
}

void StartPresence()
{
    if (gPresenceRunning.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    const bool enabled = DiscordInvitesEnabled();
    gInviteSession     = std::make_unique<invites::InviteSession>(gInviteClient, makePartyId, enabled);
    if (enabled)
    {
        registerLaunchProtocol();
        rex::discord_rpc::SetJoinHandler([](const std::string& secret)
                                         {
                                             gInviteSession->DeliverJoin(secret);
                                         });
    }

    const PresenceModel initial = SelectPresence(nullptr, std::nullopt, LobbyRole::kNone);
    rex::discord_rpc::Start(kDiscordClientId, toDiscordPresence(initial));
    gPresenceThread = std::thread(presenceThread, initial);
}

void StopPresence()
{
    if (!gPresenceRunning.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    {
        // Taking the lock orders the flag change before the waiter's next check.
        std::lock_guard lock(gPresenceWakeMutex);
    }
    gPresenceWake.notify_all();
    if (gPresenceThread.joinable())
    {
        gPresenceThread.join();
    }
    if (DiscordInvitesEnabled())
    {
        rex::discord_rpc::SetJoinHandler({});
    }
    rex::discord_rpc::Stop();
    gInviteSession.reset();
}

} // namespace rerevved
