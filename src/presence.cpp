#include "presence.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include <rex/discord_rpc.h>

#include <gameplay_state.h>

#include "presence_model.h"

namespace rerevved
{

namespace
{

constexpr char kDiscordClientId[]    = "1539761702416162938";
constexpr auto kPresencePollInterval = std::chrono::seconds{ 1 };

std::atomic<bool> gPresenceRunning{ false };
std::thread       gPresenceThread;

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

void presenceThread(PresenceModel lastSent)
{
    std::optional<PresenceModel> retainedGameplay;
    auto                         lastPublish = std::chrono::steady_clock::now();

    while (gPresenceRunning.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(kPresencePollInterval);
        if (!gPresenceRunning.load(std::memory_order_acquire))
        {
            break;
        }

        ReRevvedGameplayState state{};
        const bool            stateAvailable =
            ReRevvedGetGameplayState(&state, sizeof(state)) ==
            REREVVED_GAMEPLAY_OK;
        if (stateAvailable)
        {
            PresenceModel gameplay;
            if (TryBuildGameplayPresence(state, gameplay))
            {
                retainedGameplay = std::move(gameplay);
            }
            else if ((state.validFields &
                      REREVVED_GAMEPLAY_VALID_FRONTEND) != 0 &&
                     !state.gameplayActive)
            {
                retainedGameplay.reset();
            }
        }

        const PresenceModel pending =
            SelectPresence(stateAvailable ? &state : nullptr,
                           retainedGameplay);
        const auto now = std::chrono::steady_clock::now();
        if (pending != lastSent &&
            now - lastPublish >= kPresencePublishInterval)
        {
            rex::discord_rpc::SetPresence(toDiscordPresence(pending));
            lastSent    = pending;
            lastPublish = now;
        }
    }
}

} // namespace

void StartPresence()
{
    if (gPresenceRunning.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    const PresenceModel initial = SelectPresence(nullptr, std::nullopt);
    rex::discord_rpc::Start(kDiscordClientId, toDiscordPresence(initial));
    gPresenceThread = std::thread(presenceThread, initial);
}

void StopPresence()
{
    if (!gPresenceRunning.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    if (gPresenceThread.joinable())
    {
        gPresenceThread.join();
    }
    rex::discord_rpc::Stop();
}

} // namespace rerevved
