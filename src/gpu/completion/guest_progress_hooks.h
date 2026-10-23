#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

struct PPCContext;

namespace rerevved::gpu::guest_d3d
{

class IGuestRenderSink;

}

namespace rerevved::gpu::completion
{

class CompletionLedger;

// Set while a ledger is published; every progress hook is inert without one.
extern std::atomic<bool> guestProgressActive;

// The native guest video services publish their ledger. Hooks hold their
// own reference, so a withdrawn ledger stays valid for a record in flight.
void PublishGuestProgress(std::shared_ptr<CompletionLedger> ledger);
void WithdrawGuestProgress();
void PublishGuestRenderAssembler(std::weak_ptr<guest_d3d::IGuestRenderSink> assembler);

// Wrapped entries: 0x8269BCA8, 0x8269BF40, 0x8269E520, 0x8269BE70, and Clear
// 0x826A2970.
constexpr bool GuestProgressEntry(std::uint32_t address) noexcept
{
    return address == 0x8269BCA8 || address == 0x8269BF40 || address == 0x8269E520 || address == 0x8269BE70 || address == 0x826A2970;
}

void ObserveGuestProgressEntry(std::uint32_t entry, const PPCContext& ctx) noexcept;

// The guest words and bytes the hooks have tried to read on the calling thread, failed reads included. A test
// seam: the difference across a hook shows what it read, for a probe that must read nothing after a refusal.
std::uint64_t GuestProgressReadCount() noexcept;

// Records before Original runs; it never changes guest state.
template <std::uint32_t Address>
inline void ObserveGuestProgress(const PPCContext& ctx) noexcept
{
    if constexpr (GuestProgressEntry(Address))
    {
        if (guestProgressActive.load(std::memory_order_relaxed)) [[unlikely]]
            ObserveGuestProgressEntry(Address, ctx);
    }
}

// Bodies of the mid-assembly hooks in rerevved_hooks.cpp.
namespace detail
{

void RecordPresentFlip(std::uint32_t g, bool request) noexcept;
void RecordUnmodeled(std::uint32_t site) noexcept;
void RecordBarrier(std::uint32_t site) noexcept;
// The scaler-update waits of 0x826A4458 at 0x826A44E0 and 0x826A45C8.
void RecordDisplayWait(std::uint32_t site) noexcept;
void CloseRingEpochWrites() noexcept;
void RecordGuestClear(const std::array<std::uint32_t, 8>& arguments, std::uint32_t stack, double depth) noexcept;
// The exit 0x826A2960 of the Clear body 0x826A22F8 with its r1 `frame`, which
// closes the call's probe that 0x826A1D90's hook fed; a developer build compares
// the native_clear_derive_check and native_clear_effect_check state there.
void RecordGuestClearReturn(std::uint32_t frame) noexcept;
// `frame` is the body's r1 at the copy hook, which pairs the call's probe; 0
// matches none. A developer build compares native_resolve_derive_check there.
void RecordGuestResolveCopy(std::uint32_t g, std::uint32_t frame = 0) noexcept;
// The Resolve return 0x8269F360 with the body's r1, for the same probe; a developer
// build compares the native_resolve_effect_check watched words there.
void RecordGuestResolveReturn(std::uint32_t frame) noexcept;

// native_consume_resolve replays 0x8269E520 and returns true to skip Original.
// Diagnostics needing body/midasm hooks prevent consume; cvar off or no
// services returns false and counts nothing.
bool ConsumeGuestResolve(const PPCContext& ctx, bool diagnostics) noexcept;

// native_consume_clear replays 0x826A2970 and skips Original; hook observations
// use 0x826A1D90.
bool ConsumeGuestClear(const PPCContext& ctx, bool diagnostics) noexcept;
void RecordGpuTimestamp(std::uint32_t address, std::uint32_t g) noexcept;
// Tile-replay worker: tiled segments at 0x8269C4E8, BeginTiling 0x826AC0A8,
// deferred kicks at 0x8269BDE8, direct main submits at 0x8269BE5C, worker
// submits at 0x826AB890 and 0x826AB990 and the worker interpreter entry 0x826ABB90.
void RecordTiledSegment(std::uint32_t physical, std::uint32_t dwords) noexcept;
void BeginTiledList() noexcept;
void RecordDeferredKick(std::uint32_t physical, std::uint32_t dwords) noexcept;
// A consumed Publish or Submit failed after its first guest mutation: the ledger counts it in
// census mode and fails the session in strict mode, with detail "ring_consume_fault: <step>".
void RecordRingConsumeFault(std::string_view step, std::uint32_t site) noexcept;
void RecordDirectSubmit(std::uint32_t physical, std::uint32_t dwords) noexcept;
// The Silent end of a consumed direct Submit: seals the pending direct record without a kick.
void RecordSilentSubmit() noexcept;
void RecordReplaySubmit(std::uint32_t physical, std::uint32_t dwords, std::uint32_t site) noexcept;
void MarkReplayWorker() noexcept;
// Device suspend at 0x8268CFDC and resume at 0x8268D060, and the worker's
// deferred swap from 0x826ABF7C to 0x826ABF94.
// `stack` is the suspend frame's r1, whose saved lr names the caller.
void RecordDeviceSuspend(std::uint32_t stack) noexcept;
void RecordDeviceResume() noexcept;
void BeginDeferredSwap(std::uint32_t g) noexcept;
// Device acquire after its owner store at 0x8268CF3C and release at its
// store 0x8268CF58: the guest hands the device between threads this way.
void RecordDeviceAcquire() noexcept;
void RecordDeviceRelease() noexcept;
void EndDeferredSwap(std::uint32_t g) noexcept;

} // namespace detail

} // namespace rerevved::gpu::completion
