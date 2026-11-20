// GT2 never times out a connected player, so one silent past silent_player_timeout is closed.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/notifications.h>

#include "silent_player.h"

REXCVAR_DEFINE_INT32(silent_player_timeout,
                     rerevved::silent_player::kDefaultTimeoutSeconds,
                     "ReRevved",
                     "Seconds without any network message from a player before they are removed from the game; 0 never removes them")
    .range(0, 600);

REX_EXTERN(__imp__sub_82A148E8);
REX_EXTERN(__imp__sub_82A14C78);
REX_EXTERN(__imp__sub_82A14218);
REX_EXTERN(__imp__sub_82A15268);

namespace
{

// GT2 connection: address +0, port +4, state +0xC (5 is connected).
constexpr uint32_t kConnected = 5;

std::mutex                       mutex;
rerevved::silent_player::Tracker tracker;

uint64_t nowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Connection
{
    uint32_t ip    = 0;
    uint16_t port  = 0;
    uint32_t state = 0;
};

Connection read(uint32_t connection)
{
    if (connection == 0)
    {
        return {};
    }
    const auto* base = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(connection);
    return { rex::memory::load_and_swap<uint32_t>(base), rex::memory::load_and_swap<uint16_t>(base + 4), rex::memory::load_and_swap<uint32_t>(base + 0xC) };
}

// The hard close sub_82A14BF8 without its free. Returns the closed callback's result, as the
// connect timeout does: 1 lets sub_82A11588 free the connection, 0 means the socket was freed.
uint32_t close(PPCContext& caller, uint8_t* base, uint32_t connection)
{
    PPCContext call = caller;
    call.r3.u64     = connection;
    __imp__sub_82A14C78(call, base);
    call.r3.u64 = connection;
    __imp__sub_82A14218(call, base);
    call.r3.u64 = connection;
    call.r4.u64 = 0;
    __imp__sub_82A15268(call, base);
    caller.fpscr = call.fpscr;
    return call.r3.u32;
}

} // namespace

REX_HOOK_RAW(sub_82A148E8)
{
    const uint32_t connection = ctx.r3.u32;
    const auto     player     = read(connection);
    const auto     seconds    = REXCVAR_GET(silent_player_timeout);
    if (player.state == kConnected && seconds > 0)
    {
        std::optional<uint64_t> expired;
        {
            std::lock_guard lock(mutex);
            expired = tracker.Expired(connection, nowMs(), static_cast<uint64_t>(seconds) * 1000);
        }
        if (expired)
        {
            REXLOG_WARN("Silent player: removed {}.{}.{}.{}:{} after {}s without a message",
                        player.ip >> 24,
                        player.ip >> 16 & 0xFF,
                        player.ip >> 8 & 0xFF,
                        player.ip & 0xFF,
                        player.port,
                        *expired / 1000);
            ctx.r3.u64 = close(ctx, base, connection) != 0 ? 1 : 0;
            try
            {
                rex::ui::PostNotification("Player left", "A player's connection timed out.");
            }
            catch (...)
            {
            }
            return;
        }
    }
    __imp__sub_82A148E8(ctx, base);
}

void NotePlayerHeard(PPCRegister& connection)
{
    std::optional<uint64_t> resumed;
    {
        std::lock_guard lock(mutex);
        resumed = tracker.Received(connection.u32, nowMs());
    }
    if (resumed)
    {
        const auto player = read(connection.u32);
        REXLOG_INFO("Silent player: {}.{}.{}.{}:{} heard again after {}s",
                    player.ip >> 24,
                    player.ip >> 16 & 0xFF,
                    player.ip >> 8 & 0xFF,
                    player.ip & 0xFF,
                    player.port,
                    *resumed / 1000);
    }
}

void CheckSilentPlayer(PPCRegister& connection)
{
    const auto              player = read(connection.u32);
    std::optional<uint64_t> silent;
    {
        std::lock_guard lock(mutex);
        silent = tracker.Think(connection.u32, player.state == kConnected, nowMs());
    }
    if (silent)
    {
        REXLOG_WARN("Silent player: {}.{}.{}.{}:{} not heard from for {}s",
                    player.ip >> 24,
                    player.ip >> 16 & 0xFF,
                    player.ip >> 8 & 0xFF,
                    player.ip & 0xFF,
                    player.port,
                    *silent / 1000);
    }
}
