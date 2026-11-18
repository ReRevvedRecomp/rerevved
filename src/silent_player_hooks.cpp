#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "silent_player.h"

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

} // namespace

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
