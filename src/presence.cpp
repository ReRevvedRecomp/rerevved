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

std::atomic<bool> g_presence_running{ false };
std::thread       g_presence_thread;

rex::discord_rpc::Presence ToDiscordPresence(const PresenceModel& model)
{
    rex::discord_rpc::Presence presence;
    presence.details_          = model.details;
    presence.state_            = model.state;
    presence.large_image_key_  = model.large_image_key;
    presence.large_image_text_ = model.large_image_text;
    presence.small_image_key_  = model.small_image_key;
    presence.small_image_text_ = model.small_image_text;
    return presence;
}

void PresenceThread(PresenceModel last_sent)
{
    std::optional<PresenceModel> retained_gameplay;
    auto                         last_publish = std::chrono::steady_clock::now();

    while (g_presence_running.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(kPresencePollInterval);
        if (!g_presence_running.load(std::memory_order_acquire))
        {
            break;
        }

        ReRevvedGameplayState state{};
        const bool            state_available =
            ReRevvedGetGameplayState(&state, sizeof(state)) ==
            REREVVED_GAMEPLAY_OK;
        if (state_available)
        {
            PresenceModel gameplay;
            if (TryBuildGameplayPresence(state, gameplay))
            {
                retained_gameplay = std::move(gameplay);
            }
            else if ((state.valid_fields &
                      REREVVED_GAMEPLAY_VALID_FRONTEND) != 0 &&
                     !state.gameplay_active)
            {
                retained_gameplay.reset();
            }
        }

        const PresenceModel pending =
            SelectPresence(state_available ? &state : nullptr,
                           retained_gameplay);
        const auto now = std::chrono::steady_clock::now();
        if (pending != last_sent &&
            now - last_publish >= kPresencePublishInterval)
        {
            rex::discord_rpc::SetPresence(ToDiscordPresence(pending));
            last_sent    = pending;
            last_publish = now;
        }
    }
}

} // namespace

void StartPresence()
{
    if (g_presence_running.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    const PresenceModel initial = SelectPresence(nullptr, std::nullopt);
    rex::discord_rpc::Start(kDiscordClientId, ToDiscordPresence(initial));
    g_presence_thread = std::thread(PresenceThread, initial);
}

void StopPresence()
{
    if (!g_presence_running.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    if (g_presence_thread.joinable())
    {
        g_presence_thread.join();
    }
    rex::discord_rpc::Stop();
}

} // namespace rerevved
