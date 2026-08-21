#include "game_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <game_state.h>

namespace rerevved::gameplay
{

struct Snapshot
{
    uint64_t frame_sequence    = 0;
    uint32_t frontend_root     = 0;
    uint32_t frontend_state    = 0;
    uint32_t frontend_key      = UINT32_MAX;
    uint32_t active_player     = UINT32_MAX;
    uint32_t human_player_mask = 0;
    uint32_t interface_gate    = 0;
    bool     frontend_known    = false;
    bool     gameplay_active   = false;
    bool     turn_owner_known  = false;
    bool     human_turn        = false;
    bool     interface_known   = false;
    bool     interface_update  = false;
    bool     available         = false;
};

namespace
{

constexpr uint32_t kInterfaceGateGlobal   = 0x8314F28C;
constexpr uint32_t kFrontendRootGlobal    = 0x82FFD624;
constexpr uint32_t kActivePlayerGlobal    = 0x8312B8E8;
constexpr uint32_t kHumanPlayerMaskGlobal = 0x8312E608;

struct PublishedSlot
{
    std::atomic<int32_t> users{ 0 };
    Snapshot             snapshot;
};

// UI readers may make the inactive slot temporarily unavailable, but the guest
// frame writer never waits for them. Skipping one publication is harmless.
std::array<PublishedSlot, 2> g_published_slots;
std::atomic<uint32_t>        g_active_slot{ 0 };
std::atomic<bool>            g_snapshot_published{ false };
uint32_t                     g_writer_slot    = 0;
uint64_t                     g_frame_sequence = 0;

bool IsGuestPointer(uint32_t address)
{
    return address >= 0x10000 && address < 0xFFFFF000;
}

bool IsGuestReadableRange(rex::memory::Memory* memory,
                          uint32_t             address,
                          uint32_t             extent)
{
    if (!memory || extent == 0 || address > UINT32_MAX - extent)
    {
        return false;
    }

    const uint32_t end = address + extent;
    if (!IsGuestPointer(address) || !IsGuestPointer(end - 1))
    {
        return false;
    }

    auto* heap = memory->LookupHeap(address);
    return heap && memory->LookupHeap(end - 1) == heap &&
           heap->QueryRangeAccess(address, end - 1) !=
               rex::memory::PageAccess::kNoAccess;
}

bool TryReadU8(rex::memory::Memory* memory, uint32_t address, uint8_t& value)
{
    if (!IsGuestReadableRange(memory, address, sizeof(value)))
    {
        return false;
    }
    value = *memory->TranslateVirtual<const uint8_t*>(address);
    return true;
}

bool TryReadU32(rex::memory::Memory* memory, uint32_t address, uint32_t& value)
{
    if (!IsGuestReadableRange(memory, address, sizeof(value)))
    {
        return false;
    }

    const auto* source = memory->TranslateVirtual<const uint8_t*>(address);
    value              = (uint32_t{ source[0] } << 24) |
                         (uint32_t{ source[1] } << 16) |
                         (uint32_t{ source[2] } << 8) | uint32_t{ source[3] };
    return true;
}

} // namespace

static Snapshot ReadGuestSnapshot()
{
    Snapshot state{};
    auto*    runtime = rex::Runtime::instance();
    auto*    memory  = runtime ? runtime->memory() : nullptr;
    if (!memory)
    {
        return state;
    }

    if (TryReadU32(memory, kFrontendRootGlobal, state.frontend_root) &&
        state.frontend_root != 0 &&
        TryReadU32(memory,
                   state.frontend_root + 0x70,
                   state.frontend_state) &&
        state.frontend_state != 0 &&
        TryReadU32(memory,
                   state.frontend_state + 0x4,
                   state.frontend_key))
    {
        state.frontend_known  = true;
        state.gameplay_active = state.frontend_key == 2;
    }

    if (TryReadU32(memory, kActivePlayerGlobal, state.active_player) &&
        TryReadU32(memory,
                   kHumanPlayerMaskGlobal,
                   state.human_player_mask) &&
        state.active_player < 32 && state.human_player_mask != 0)
    {
        state.turn_owner_known = true;
        state.human_turn =
            (state.human_player_mask &
             (uint32_t{ 1 } << state.active_player)) != 0;
    }

    uint8_t interface_byte = 0;
    if (TryReadU32(memory,
                   kInterfaceGateGlobal,
                   state.interface_gate) &&
        state.interface_gate != 0 &&
        TryReadU8(memory,
                  state.interface_gate + 0x5,
                  interface_byte))
    {
        state.interface_known  = true;
        state.interface_update = interface_byte != 0;
    }

    state.available = state.frontend_known && state.gameplay_active &&
                      state.interface_known && state.interface_update &&
                      state.turn_owner_known && state.human_turn;
    return state;
}

void PublishFrameSnapshot()
{
    const uint32_t next_slot = g_writer_slot ^ 1u;
    auto&          slot      = g_published_slots[next_slot];
    int32_t        expected  = 0;
    if (!slot.users.compare_exchange_strong(expected,
                                            -1,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed))
    {
        return;
    }

    Snapshot state       = ReadGuestSnapshot();
    state.frame_sequence = ++g_frame_sequence;
    slot.snapshot        = state;
    slot.users.store(0, std::memory_order_release);
    g_writer_slot = next_slot;
    g_active_slot.store(next_slot, std::memory_order_release);
    g_snapshot_published.store(true, std::memory_order_release);
}

static bool GetPublishedSnapshot(Snapshot& out)
{
    if (!g_snapshot_published.load(std::memory_order_acquire))
    {
        return false;
    }

    for (;;)
    {
        const uint32_t slot_index =
            g_active_slot.load(std::memory_order_acquire);
        auto&   slot     = g_published_slots[slot_index];
        int32_t expected = slot.users.load(std::memory_order_relaxed);
        if (expected < 0 ||
            !slot.users.compare_exchange_weak(expected,
                                              expected + 1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed))
        {
            continue;
        }

        if (slot_index != g_active_slot.load(std::memory_order_acquire))
        {
            slot.users.fetch_sub(1, std::memory_order_release);
            continue;
        }

        out = slot.snapshot;
        slot.users.fetch_sub(1, std::memory_order_release);
        return true;
    }
}

} // namespace rerevved::gameplay

static_assert(sizeof(ReRevvedGameplayState) == 80);

extern "C" uint32_t ReRevvedGameplayAbiVersion(void)
{
    return REREVVED_GAMEPLAY_ABI_VERSION;
}

extern "C" int ReRevvedGetGameplayState(
    ReRevvedGameplayState* out,
    uint32_t               out_size)
{
    if (!out)
    {
        return REREVVED_GAMEPLAY_ERR_INVALID_ARGUMENT;
    }

    std::memset(out, 0, std::min<size_t>(out_size, sizeof(*out)));
    if (out_size < sizeof(*out))
    {
        return REREVVED_GAMEPLAY_ERR_BUFFER_TOO_SMALL;
    }

    out->struct_size   = sizeof(*out);
    out->active_player = REREVVED_GAMEPLAY_PLAYER_UNKNOWN;

    rerevved::gameplay::Snapshot snapshot{};
    if (!rerevved::gameplay::GetPublishedSnapshot(snapshot))
    {
        return REREVVED_GAMEPLAY_ERR_UNAVAILABLE;
    }

    if (snapshot.frontend_known)
    {
        out->valid_fields |= REREVVED_GAMEPLAY_VALID_FRONTEND;
    }
    if (snapshot.turn_owner_known)
    {
        out->valid_fields |= REREVVED_GAMEPLAY_VALID_TURN;
        out->active_player     = static_cast<int32_t>(snapshot.active_player);
        out->human_player_mask = snapshot.human_player_mask;
    }
    if (snapshot.interface_known)
    {
        out->valid_fields |= REREVVED_GAMEPLAY_VALID_INTERFACE;
    }

    out->frame_sequence   = snapshot.frame_sequence;
    out->gameplay_active  = snapshot.gameplay_active ? 1 : 0;
    out->interface_update = snapshot.interface_update ? 1 : 0;
    out->turn_owner_known = snapshot.turn_owner_known ? 1 : 0;
    out->human_turn       = snapshot.human_turn ? 1 : 0;
    out->available        = snapshot.available ? 1 : 0;
    return REREVVED_GAMEPLAY_OK;
}
