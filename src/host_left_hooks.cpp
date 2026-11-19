#include <cstdint>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/notifications.h>

namespace
{

// Nonzero while the lobby is open; listener +0x48 then posts lobby event 13 instead.
constexpr uint32_t kLobbyGlobal = 0x8314F2E4;

// Set while this thread runs GT2's socket error handler: this player lost the network, so
// a migration to a remote player is only a step toward this player being left alone.
thread_local int socketErrors = 0;

struct Indices
{
    int32_t host  = -1;
    int32_t local = -1;
    bool    lobby = false;
};

const uint8_t* guestBase()
{
    return REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(0);
}

bool inLobby(const uint8_t* base)
{
    return rex::memory::load_and_swap<uint32_t>(base + kLobbyGlobal) != 0;
}

Indices read(uint32_t state)
{
    const auto* base = guestBase();
    if (state == 0)
    {
        return {};
    }
    return { static_cast<int32_t>(rex::memory::load_and_swap<uint32_t>(base + state + 8)),
             static_cast<int32_t>(rex::memory::load_and_swap<uint32_t>(base + state + 12)),
             inLobby(base) };
}

} // namespace

void NoteHostMigrated(PPCRegister& state)
{
    try
    {
        const auto players = read(state.u32);
        REXLOG_WARN("Host left: player {} is the new host; local player {}{}{}",
                    players.host,
                    players.local,
                    players.lobby ? " in the lobby" : "",
                    socketErrors != 0 ? " after this player's network failed" : "");
        // A local new host may get the native "Disconnected" box instead, so only the others are told.
        if (!players.lobby && players.host != players.local && socketErrors == 0)
        {
            rex::ui::PostNotification("Host left", "Another player is now hosting this game.");
        }
    }
    catch (...)
    {
    }
}

void NoteHostLost(PPCRegister& state)
{
    try
    {
        const auto players = read(state.u32);
        REXLOG_WARN("Host left: no player can take over from host {}; local player {} leaves the game", players.host, players.local);
    }
    catch (...)
    {
    }
}

// The native game shows nothing here and goes on alone, unless this player was still
// waiting on its own hot join or resync transfer.
void EnterSocketError()
{
    if (++socketErrors != 1)
    {
        return;
    }
    try
    {
        const auto* base  = guestBase();
        const bool  lobby = inLobby(base);
        REXLOG_WARN("Host left: this player's network failed, disconnecting them from the other players{}", lobby ? " in the lobby" : "");
        if (!lobby)
        {
            rex::ui::PostNotification("Connection lost", "Your network connection failed. The other players have left this game.");
        }
    }
    catch (...)
    {
    }
}

void LeaveSocketError()
{
    --socketErrors;
}
