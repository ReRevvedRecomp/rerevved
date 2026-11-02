// Retail audio volume and rumble hooks; docs/rexglue-runtime.md describes the
// design.

#include "retail_audio.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REX_EXTERN(__imp__sub_82C7DF58);
REX_EXTERN(__imp__sub_82E2DE98);
REX_EXTERN(__imp__sub_82C7AA98);
REX_EXTERN(sub_82D504B8);
REX_EXTERN(sub_82E42380);
REX_EXTERN(sub_82C79D38);

namespace
{

namespace ra = rerevved::retail_audio;

constexpr uint32_t kAudioManagerGlobal = 0x8314F084;
constexpr uint32_t kAudioOwnerGlobal   = 0x8314F284;
constexpr uint32_t kFlagsGlobal        = 0x8314F288;
constexpr uint32_t kInputOwnerGlobal   = 0x8314F2A4;

constexpr int32_t  kAudioCategory     = 2;
constexpr uint32_t kRumbleMask        = 0x20;
constexpr uint32_t kBackendVtableSlot = 0x18; // 0x82C79D38 calls virtual +0x18 of owner+4
constexpr uint32_t kEntryGainOffset   = 0x0;
constexpr uint32_t kEntryDirtyOffset  = 0xC;
constexpr uint32_t kEntryBytes        = 28;

constexpr uint32_t kRetryFrames = 30;

// Generation still to apply; 0 means none.
std::atomic<uint64_t> gReapplyGeneration{ 0 };
std::atomic<uint64_t> gRumbleGeneration{ 0 };
std::atomic<uint32_t> gFrame{ 0 };
std::atomic<uint32_t> gRetryAt{ 0 };
std::atomic<bool>     gRumbleLogged{ false };

struct Guest
{
    PPCContext& ctx;
    uint8_t*    base;
};

bool accessible(uint32_t address, uint32_t size, bool write)
{
    if (size == 0 || address < 0x10000 || uint64_t{ address } + size > 0xFFFFF000ull)
        return false;
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory)
        return false;
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + size - 1) != heap)
        return false;
    const auto access = heap->QueryRangeAccess(address, address + size - 1);
    if (write)
        return access == rex::memory::PageAccess::kReadWrite || access == rex::memory::PageAccess::kExecuteReadWrite;
    return access != rex::memory::PageAccess::kNoAccess;
}

std::optional<uint32_t> loadU32(const Guest& guest, uint32_t address)
{
    if (!accessible(address, 4, false))
        return std::nullopt;
    const uint8_t* source = guest.base + address;
    return uint32_t{ source[0] } << 24 | uint32_t{ source[1] } << 16 | uint32_t{ source[2] } << 8 | uint32_t{ source[3] };
}

bool storeU32(const Guest& guest, uint32_t address, uint32_t value)
{
    if (!accessible(address, 4, true))
        return false;
    uint8_t* target = guest.base + address;
    target[0]       = static_cast<uint8_t>(value >> 24);
    target[1]       = static_cast<uint8_t>(value >> 16);
    target[2]       = static_cast<uint8_t>(value >> 8);
    target[3]       = static_cast<uint8_t>(value);
    return true;
}

// Runs on a copy so the hooked call's registers survive.
template <typename Setup>
uint32_t callGuest(const Guest& guest, PPCFunc* function, Setup&& setup)
{
    PPCContext call = guest.ctx;
    setup(call);
    function(call, guest.base);
    guest.ctx.fpscr = call.fpscr;
    return call.r3.u32;
}

// Same two-step lookup as the retail Options setup 0x82DE66D0.
std::optional<uint32_t> resolveEntry(const Guest& guest, size_t channel)
{
    const auto manager = loadU32(guest, kAudioManagerGlobal);
    const auto owner   = loadU32(guest, kAudioOwnerGlobal);
    if (!manager || !owner || *manager == 0 || *owner == 0)
        return std::nullopt;
    const int32_t index = static_cast<int32_t>(callGuest(guest, sub_82D504B8, [&](PPCContext& call)
                                                         {
                                                             call.r3.u64 = *manager;
                                                             call.r4.s64 = kAudioCategory;
                                                             call.r5.u64 = ra::kChannels[channel].catalogKey;
                                                             call.r6.s64 = 0;
                                                         }));
    if (index < 0)
        return std::nullopt;
    const uint32_t entry = callGuest(guest, sub_82E42380, [&](PPCContext& call)
                                     {
                                         call.r3.u64 = *owner;
                                         call.r4.s64 = index;
                                     });
    if (entry == 0 || !accessible(entry, kEntryBytes, true))
        return std::nullopt;
    return entry;
}

bool applyPosition(const Guest& guest, size_t channel, int position)
{
    const auto entry = resolveEntry(guest, channel);
    if (!entry)
        return false;
    const float gain = ra::PositionToGain(position);
    uint32_t    bits = 0;
    std::memcpy(&bits, &gain, sizeof bits);
    return storeU32(guest, *entry + kEntryGainOffset, bits) && storeU32(guest, *entry + kEntryDirtyOffset, 0);
}

ra::LiveValue readLive(const Guest& guest, size_t channel)
{
    ra::LiveValue value;
    const auto    entry = resolveEntry(guest, channel);
    if (!entry)
        return value;
    const auto bits = loadU32(guest, *entry + kEntryGainOffset);
    if (!bits)
        return value;
    std::memcpy(&value.gain, &(*bits), sizeof value.gain);
    value.available = true;
    return value;
}

void publishLive(const Guest& guest)
{
    ra::LiveSnapshot snapshot;
    snapshot.generation = ra::Queue().Generation();
    for (size_t channel = 0; channel < ra::kChannelCount; ++channel)
        snapshot.volume[channel] = readLive(guest, channel);
    ra::Live().Publish(snapshot);
}

// Keeps the retail mask on so the host Vibration row is the only rumble switch.
bool applyRumbleMask(const Guest& guest)
{
    const auto flagsPointer = loadU32(guest, kFlagsGlobal);
    const auto input        = loadU32(guest, kInputOwnerGlobal);
    if (!flagsPointer || !input || *flagsPointer == 0 || *input == 0)
        return false;
    const auto backend = loadU32(guest, *input + 4);
    if (!backend || *backend == 0)
        return false;
    const auto table = loadU32(guest, *backend);
    if (!table || *table == 0 || !accessible(*table + kBackendVtableSlot, 4, false))
        return false;
    const auto flags = loadU32(guest, *flagsPointer);
    if (!flags || !accessible(*flagsPointer, 4, true))
        return false;
    if ((*flags & kRumbleMask) != 0)
        return true;
    if (!storeU32(guest, *flagsPointer, *flags | kRumbleMask))
        return false;
    callGuest(guest, sub_82C79D38, [&](PPCContext& call)
              {
                  call.r3.u64 = *input;
                  call.f1.f64 = 0.0;
                  call.f2.f64 = 0.0;
              });
    return true;
}

struct Outcome
{
    bool changed = false;
    bool wait    = false;
};

Outcome applyOverrides(const Guest& guest)
{
    Outcome outcome;
    for (size_t channel = 0; channel < ra::kChannelCount; ++channel)
    {
        const int position = ra::OverrideOf(channel);
        if (position == ra::kNoOverride)
            continue;
        if (applyPosition(guest, channel, position))
            outcome.changed = true;
        else
            outcome.wait = true;
    }
    return outcome;
}

bool service(const Guest& guest, bool refresh)
{
    const uint64_t generation = ra::Queue().Generation();
    bool           changed    = refresh;
    bool           wait       = false;

    if (gReapplyGeneration.load(std::memory_order_relaxed) == generation)
    {
        const auto outcome = applyOverrides(guest);
        changed            = changed || outcome.changed;
        if (outcome.wait)
            wait = true;
        else
            gReapplyGeneration.store(0, std::memory_order_relaxed);
    }

    for (const auto& request : ra::Queue().Take())
    {
        if (applyPosition(guest, request.channel, request.position))
        {
            changed = true;
        }
        else
        {
            ra::Queue().Requeue(request);
            wait = true;
        }
    }

    if (gRumbleGeneration.load(std::memory_order_relaxed) == generation)
    {
        if (applyRumbleMask(guest))
        {
            gRumbleGeneration.store(0, std::memory_order_relaxed);
        }
        else
        {
            wait = true;
            if (!gRumbleLogged.exchange(true))
                REXLOG_INFO("Retail audio: rumble owners are not ready; the retail rumble mask waits");
        }
    }

    const uint32_t frame = gFrame.load(std::memory_order_relaxed);
    gRetryAt.store(wait ? frame + kRetryFrames : 0, std::memory_order_relaxed);
    return changed;
}

void drain(PPCContext& ctx, uint8_t* base) noexcept
try
{
    const uint32_t frame   = gFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool     refresh = ra::Live().RefreshRequested();
    const bool     waiting = ra::Queue().HasPending() || gReapplyGeneration.load(std::memory_order_relaxed) != 0 ||
                             gRumbleGeneration.load(std::memory_order_relaxed) != 0;
    if (!refresh && !waiting)
        return;
    // A retry after a failed apply backs off; a page request bypasses it.
    if (!refresh && frame < gRetryAt.load(std::memory_order_relaxed))
        return;
    const Guest guest{ ctx, base };
    const bool  consumed = ra::Live().ConsumeRefresh();
    if (service(guest, consumed))
        publishLive(guest);
}
catch (...)
{
    // A failed drain must not unwind through guest code.
}

} // namespace

namespace rerevved::retail_audio
{

void DrainOnGuest(PPCContext& ctx, uint8_t* base) noexcept
{
    drain(ctx, base);
}

} // namespace rerevved::retail_audio

REX_HOOK_RAW(sub_82C7DF58)
{
    __imp__sub_82C7DF58(ctx, base);
    drain(ctx, base);
}

REX_HOOK_RAW(sub_82E2DE98)
{
    const uint64_t generation = ra::Queue().Advance();
    gReapplyGeneration.store(generation, std::memory_order_relaxed);
    gRumbleGeneration.store(generation, std::memory_order_relaxed);
    __imp__sub_82E2DE98(ctx, base);
    try
    {
        // Retail has stored its own values by now.
        const Guest guest{ ctx, base };
        if (service(guest, true))
            publishLive(guest);
    }
    catch (...)
    {
        // A failed apply must not unwind through guest code.
    }
}

// Retail republishes its volumes after load (0x82E51BE0 and others); reapply.
REX_HOOK_RAW(sub_82C7AA98)
{
    __imp__sub_82C7AA98(ctx, base);
    try
    {
        const Guest guest{ ctx, base };
        const auto  outcome = applyOverrides(guest);
        if (outcome.wait)
        {
            gReapplyGeneration.store(ra::Queue().Generation(), std::memory_order_relaxed);
        }
        publishLive(guest);
    }
    catch (...)
    {
        // A failed apply must not unwind through guest code.
    }
}