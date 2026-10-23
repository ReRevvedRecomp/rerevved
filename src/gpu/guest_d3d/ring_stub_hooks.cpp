#include "ring_stubs.h"

#include "developer_hooks.h"
#include "gpu/completion/guest_progress_hooks.h"
#include "indexed_buffer_consume.h"
#include "set_predication_consume.h"

#if REREVVED_DEVELOPER
#include "original_residue_probe.h"
#include "tiling_restore_consume.h"
#endif

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <thread>

// Generated originals the stubs call or fall back to. The route trace owns the
// raw wrappers of the RingStubEntry addresses and of 0x8269BE70; the census
// owns 0x8269CD20; this file owns the others.
REX_EXTERN(__imp__sub_8269C400);
REX_EXTERN(__imp__sub_8269BD98);
REX_EXTERN(__imp__sub_826A9BA0);
REX_EXTERN(__imp__sub_826AC0A8);
REX_EXTERN(__imp__sub_826AC218);
REX_EXTERN(__imp__sub_826AB740);
REX_EXTERN(__imp__sub_8269B118);
REX_EXTERN(sub_8269C400);
REX_EXTERN(sub_8269C588);
REX_EXTERN(sub_8269BD98);
REX_EXTERN(sub_8269BCA8);
REX_EXTERN(sub_8269BE70);
REX_EXTERN(sub_8269BF40);
REX_EXTERN(sub_8269CD20);
// Helpers of 0x826AC218 that write no ring word: the side buffer allocator,
// the list terminator and the allocation-failure reset.
REX_EXTERN(sub_8269B818);
REX_EXTERN(sub_8269B508);
REX_EXTERN(sub_8269B118);
#if REREVVED_DEVELOPER
// native_ring_consume_publish: the helpers Publish and Submit keep calling, and the
// kernel spin lock Submit's deferred check takes.
REX_EXTERN(sub_8269B550);
REX_EXTERN(sub_8269C170);
REX_EXTERN(sub_8269C270);
REX_EXTERN(__imp__KfAcquireSpinLock);
REX_EXTERN(__imp__KfReleaseSpinLock);
#endif

// The swap interval observer at 0x826A4258, inside the pre-swap body the stub replaces.
void ObserveDecodedSwapInterval(PPCRegister& r11, PPCRegister& r31);

namespace rerevved::gpu::guest_d3d
{

namespace
{

using namespace ring_layout;

class GuestMemory final : public IRingStubMemory
{
public:
    std::optional<std::uint32_t> Read32(std::uint32_t address) override
    {
        auto* memory = access(address, 4, rex::memory::PageAccess::kReadOnly);
        if (!memory)
            return std::nullopt;
        std::uint32_t raw = 0;
        std::memcpy(&raw, memory->TranslateVirtual<const std::uint8_t*>(address), sizeof(raw));
        return rex::byte_swap(raw);
    }

    std::optional<std::uint8_t> Read8(std::uint32_t address) override
    {
        auto* memory = access(address, 1, rex::memory::PageAccess::kReadOnly);
        if (!memory)
            return std::nullopt;
        return *memory->TranslateVirtual<const std::uint8_t*>(address);
    }

    bool Write32(std::uint32_t address, std::uint32_t value) override
    {
        auto* memory = access(address, 4, rex::memory::PageAccess::kReadWrite);
        if (!memory)
            return false;
        const std::uint32_t raw = rex::byte_swap(value);
        std::memcpy(memory->TranslateVirtual<std::uint8_t*>(address), &raw, sizeof(raw));
        // Each guest word lands before the next, as the original's stores do.
        std::atomic_thread_fence(std::memory_order_release);
        return true;
    }

    bool Write8(std::uint32_t address, std::uint8_t value) override
    {
        auto* memory = access(address, 1, rex::memory::PageAccess::kReadWrite);
        if (!memory)
            return false;
        *memory->TranslateVirtual<std::uint8_t*>(address) = value;
        return true;
    }

    std::optional<std::uint64_t> Exchange64(std::uint32_t address, std::uint64_t value) override
    {
        auto* memory = (address & 7) == 0 ? access(address, 8, rex::memory::PageAccess::kReadWrite) : nullptr;
        if (!memory)
            return std::nullopt;
        // The ldarx/stdcx. loop of 0x8269B3F8, against other guest writers of the range.
        std::atomic_ref<std::uint64_t> word(*memory->TranslateVirtual<std::uint64_t*>(address));
        return rex::byte_swap(word.exchange(rex::byte_swap(value), std::memory_order_acq_rel));
    }

    std::optional<std::uint32_t> Physical(std::uint32_t address) override
    {
        auto* memory = access(address, 4, rex::memory::PageAccess::kReadOnly);
        if (!memory)
            return std::nullopt;
        const auto physical = memory->GetPhysicalAddress(address);
        if (physical == UINT32_MAX)
            return std::nullopt;
        return physical;
    }

    bool Writable32(std::uint32_t address) override
    {
        return access(address, 4, rex::memory::PageAccess::kReadWrite) != nullptr;
    }

    std::optional<RingAllocation> Allocation(std::uint32_t address) override
    {
        auto*                           memory = access(address, 4, rex::memory::PageAccess::kReadOnly);
        auto*                           heap   = memory ? memory->LookupHeap(address) : nullptr;
        rex::memory::HeapAllocationInfo info{};
        if (!heap || !heap->QueryRegionInfo(address, &info) || !info.state || !info.allocation_size)
            return std::nullopt;
        return RingAllocation{ info.allocation_base, info.allocation_size };
    }

private:
    static rex::memory::Memory* access(std::uint32_t address, std::uint32_t size, rex::memory::PageAccess needed)
    {
        auto* kernel = rex::system::kernel_state();
        auto* memory = kernel ? kernel->memory() : nullptr;
        if (!memory || address < 0x10000 || address > 0xFFFFFFFFU - (size - 1))
            return nullptr;
        const auto last = address + size - 1;
        auto*      heap = memory->LookupHeap(address);
        if (!heap || memory->LookupHeap(last) != heap)
            return nullptr;
        const auto granted = static_cast<unsigned>(heap->QueryRangeAccess(address, last));
        return (granted & static_cast<unsigned>(needed)) == static_cast<unsigned>(needed) ? memory : nullptr;
    }
};

GuestMemory guestMemory;

std::array<std::atomic<bool>, kRingStubCount> loggedStubbed{};
std::array<std::atomic<bool>, kRingStubCount> loggedOriginal{};
std::atomic<std::uint32_t>&                   validatedDevice = ringStubsValidatedDevice;

// The timing-line field: stub_<name> for a ring stub, <name> for a tiling counter.
std::string FieldName(RingStub stub)
{
    const auto index = static_cast<std::size_t>(stub);
    return std::string(index < kFirstTilingCounter ? "stub_" : "") + std::string(kRingStubNames[index]);
}

// Checks the device once; a mismatch disables the stubs for the session. A
// call that is not served counts as that stub's Original call.
bool ready(std::uint32_t g, RingStub stub, std::uint32_t site, [[maybe_unused]] OriginalResidueReadiness* diagnostic = nullptr, IRingStubMemory& memory = guestMemory)
{
    if (validatedDevice.load(std::memory_order_acquire) == g)
    {
#if REREVVED_DEVELOPER
        if (diagnostic)
            *diagnostic = OriginalResidueReadiness::Cached;
#endif
        return true;
    }
    const auto check = CheckRingLayout(memory, g);
    switch (check.verdict)
    {
        case RingLayout::Ok:
        {
#if REREVVED_DEVELOPER
            if (diagnostic)
                *diagnostic = OriginalResidueReadiness::Ok;
#endif
            validatedDevice.store(g, std::memory_order_release);
            REXLOG_INFO("native_ring_stubs: device {:08X} layout checked: B={:08X} S2={:08X} ring_mask={:X} next_fence={:X}",
                        g,
                        memory.Read32(g + kProgressBlock).value_or(0),
                        memory.Read32(g + kScratchBlock).value_or(0),
                        memory.Read32(g + kRingMask).value_or(0),
                        memory.Read32(g + kNextFence).value_or(0));
            return true;
        }
        case RingLayout::NotReady:
#if REREVVED_DEVELOPER
            if (diagnostic)
                *diagnostic = OriginalResidueReadiness::NotReady;
#endif
            NoteRingStub(stub, false, site, std::format("device {:08X} not ready: {}", g, check.detail));
            return false;
        case RingLayout::Mismatch:
#if REREVVED_DEVELOPER
            if (diagnostic)
                *diagnostic = OriginalResidueReadiness::Mismatch;
#endif
            DisableRingStubs(std::format("device {:08X}: {}", g, check.detail));
            NoteRingStub(stub, false, site, "layout mismatch");
            return false;
    }
    return false;
}

// Runs a guest function from a stub as the original's call at `returnAddress`
// would, then restores the stub's own link register.
template <auto Function>
void callGuest(PPCContext&   ctx,
               std::uint8_t* base,
               std::uint32_t returnAddress,
               std::uint32_t r3,
               std::uint32_t r4 = 0,
               std::uint32_t r5 = 0,
               std::uint32_t r6 = 0,
               std::uint32_t r7 = 0,
               std::uint32_t r8 = 0)
{
    const auto lr = ctx.lr;
    ctx.r3.u64    = r3;
    ctx.r4.u64    = r4;
    ctx.r5.u64    = r5;
    ctx.r6.u64    = r6;
    ctx.r7.u64    = r7;
    ctx.r8.u64    = r8;
    ctx.lr        = returnAddress;
    Function(ctx, base);
    ctx.lr = lr;
}

void kick(std::uint32_t g)
{
    if (const auto services = CurrentRingStubServices())
        services->Kick(guestMemory.Read32(g + kRingWriteIndex).value_or(0));
}

void flushIfPastLimit(PPCContext& ctx, std::uint8_t* base, std::uint32_t g, std::uint32_t returnAddress)
{
    if (CursorPastLimit(guestMemory, g).value_or(false))
        callGuest<sub_8269C588>(ctx, base, returnAddress, g);
}

// The barriers 0x8269BAE8's coherency packets carry, recorded where its
// mid-assembly hooks would record them.
void recordCoherency(const std::optional<Coherency>& coherency)
{
    if (!coherency)
        return;
    if (coherency->range)
        completion::detail::RecordBarrier(0x8269BB94);
    if (coherency->extra)
        completion::detail::RecordBarrier(0x8269BC54);
}

// 0x8269C588: the coherency submit's barriers without its packets, the
// publication, then the CPU fence switch, whose fence wait the ledger
// completes. Returns the cursor in r3.
bool flush(PPCContext& ctx, std::uint8_t* base, std::uint32_t g, std::uint32_t site)
{
    recordCoherency(TakeCoherency(guestMemory, g));
    callGuest<sub_8269C400>(ctx, base, 0x8269C61C, g);
    if (const auto fence = CpuFenceSwitch(guestMemory, g))
    {
        callGuest<sub_8269BE70>(ctx, base, 0x8269C660, g, *fence);
        SetCpuFenceMode(guestMemory, g);
    }
    ctx.r3.u64 = guestMemory.Read32(g + kCursor).value_or(0);
    NoteRingStub(RingStub::Flush, true, site);
    return true;
}

// 0x8269C810 without its WAIT_UNTIL dwords: the fence wait on F flushes and
// the ledger releases it, then the worker's release.
bool drain(PPCContext& ctx, std::uint8_t* base, std::uint32_t g, std::uint32_t site)
{
    flushIfPastLimit(ctx, base, g, 0x8269C838);
    const auto fence = guestMemory.Read32(g + kNextFence);
    if (!fence)
    {
        NoteRingStub(RingStub::Drain, false, site, "next fence unreadable");
        return false;
    }
    callGuest<sub_8269BE70>(ctx, base, 0x8269C868, g, *fence, 4);
    {
#if REREVVED_DEVELOPER
        // The pending-N loop, measured with native_ring_consume_publish.
        const RingWaitScope drainWait(RingWaitKind::Drain, g, site);
#endif
        while (guestMemory.Read32(g + kWorkerPending).value_or(0) != 0)
            std::this_thread::yield();
    }
    NoteRingStub(RingStub::Drain, true, site);
    return true;
}

bool publisher(std::uint32_t g, std::uint32_t site)
{
    switch (ClassifyPublisher(guestMemory, g))
    {
        case PublisherPath::Kick:
            kick(g);
            break;
        case PublisherPath::Silent:
            break;
        case PublisherPath::Original:
            NoteRingStub(RingStub::Publisher, false, site, "G+0x541C or G+0x5B0C set");
            return false;
    }
    NoteRingStub(RingStub::Publisher, true, site);
    return true;
}

bool fence(PPCContext& ctx, std::uint32_t g, std::uint32_t site)
{
    if (!ApplyFenceFields(guestMemory, g))
    {
        NoteRingStub(RingStub::Fence, false, site, "device fields unreadable");
        return false;
    }
    // Past the 10 dwords INVALIDATE_STATE and two EVENT_WRITE_SHD would take.
    ctx.r3.u64 = ctx.r4.u32 + 40;
    NoteRingStub(RingStub::Fence, true, site);
    return true;
}

bool callback(PPCContext& ctx, std::uint32_t g, std::uint32_t site)
{
    const auto cursor = CallbackCursor(guestMemory, g, ctx.r4.u32, ctx.r5.u32);
    if (!cursor)
    {
        NoteRingStub(RingStub::Callback, false, site, std::format("{:08X}: emitter state unreadable", ctx.r6.u32));
        return false;
    }
    ctx.r3.u64 = *cursor;
    NoteRingStub(RingStub::Callback, true, site);
    return true;
}

// 0x826A4150 without its packets. 0x826A9C30 is inert outside timestamp mode
// 2, which PlanPreSwap refuses; 0x8269CD20 retains its two dwords.
bool preSwap(PPCContext& ctx, std::uint8_t* base, std::uint32_t g, std::uint32_t site)
{
    const std::uint32_t flip = ctx.r4.u32;
    std::string         refusal;
    const auto          plan = PlanPreSwap(guestMemory, g, flip, &refusal);
    if (!plan)
    {
        NoteRingStub(RingStub::PreSwap, false, site, refusal);
        return false;
    }
    {
        SetPredicationPreSwapScope setPredication(ctx, g);
        callGuest<sub_8269CD20>(ctx, base, 0x826A4178, g, 0);
    }
    if (flip == 0)
    {
        flushIfPastLimit(ctx, base, g, 0x826A419C);
        completion::detail::RecordPresentFlip(g, true);
    }
    {
        // ObserveRendererSwapSource matches this interval to the device in r31.
        PPCRegister interval{}, device{};
        interval.u64 = plan->interval;
        device.u64   = g;
        ObserveDecodedSwapInterval(interval, device);
    }
    flushIfPastLimit(ctx, base, g, 0x826A4280);
    callGuest<sub_8269BF40>(ctx, base, 0x826A429C, g, guestMemory.Read32(g + kCursor).value_or(0), 0, kFlipCallback, plan->parameter);
    guestMemory.Write32(g + kCursor, ctx.r3.u32);
    if (flip == 0)
    {
        flushIfPastLimit(ctx, base, g, 0x826A42BC);
        completion::detail::RecordPresentFlip(g, false);
    }
    FinishPreSwap(guestMemory, g);
    NoteRingStub(RingStub::PreSwap, true, site);
    return true;
}

// 0x826AC218 guest call/store map; calls keep their original order and return
//   addresses.
// 0x8269BAE8             | Coherency packets and their submit omitted: these
//   were its only own packets.
// Remaining calls       | Three side-buffer allocations; G+0x33B0 = G+0x33B4 =
//   0; list terminators; worker-kick callback; fence; both submits.
// Callback/fence wrappers| Progress observers record these buffers; stubs write
//   no packet.
// 0x8269BD98 hook        | Stubs direct submit; worker-owned ring instead runs
//   Original's deferred path, writing no ring word.
// Deferred token        | Chains buffer onto worker list; worker submits at
//   0x826AB890.
// 0x826ABB2C list end    | Only then lowers G+0x2B00; ledger releases fence on
//   worker submit.
// Skipping submit call  | Would strand the fence or pass it before the worker
//   walks the list.
// Return r3             | r24, the allocation-failure bit.
bool endTiling(PPCContext& ctx, std::uint8_t* base, std::uint32_t g, std::uint32_t site)
{
    const std::uint32_t flags = ctx.r5.u32;
    // Every field the stub reads is checked before its first side effect;
    // each is read again where the original reads it.
    const auto high = guestMemory.Read8(g + kModeHigh);
    if (!high || !guestMemory.Read32(g + kTiling) || !EndTilingCallbackParameter(guestMemory, g) || !EndTilingSubmitsCallback(guestMemory, g))
    {
        NoteRingStub(RingStub::TilingEnd, false, site, "device fields unreadable");
        return false;
    }
    if ((*high & kModeAllocFailed) == 0)
        recordCoherency(SwapCoherency(guestMemory, g));
    callGuest<sub_8269B818>(ctx, base, 0x826AC254, g, kEndTilingCallbackDwords, 32);
    const std::uint32_t callbackBuffer = ctx.r3.u32;
    callGuest<sub_8269B818>(ctx, base, 0x826AC268, g, kEndTilingFenceDwords, 32);
    const std::uint32_t fenceBuffer = ctx.r3.u32;
    callGuest<sub_8269B818>(ctx, base, 0x826AC27C, g, kEndTilingTokenDwords, 4);
    std::uint32_t tokens = ctx.r3.u32;
    const auto    mode   = guestMemory.Read8(g + kModeHigh).value_or(0);
    EndTilingState(guestMemory, g);
    if (mode & kModeAllocFailed)
    {
        // An allocation failed: the original drops the bracket.
        callGuest<sub_8269B118>(ctx, base, 0x826AC2A0, g);
        guestMemory.Write8(g + kModeHigh, guestMemory.Read8(g + kModeHigh).value_or(0) & ~kModeAllocFailed);
        ctx.r3.u64 = 1;
        NoteRingStub(RingStub::TilingEnd, true, site);
        return true;
    }
    for (std::size_t i = 0; i < kEndTilingLists.size(); ++i)
        callGuest<sub_8269B508>(ctx, base, 0x826AC2BC + 8 * static_cast<std::uint32_t>(i), g + kEndTilingLists[i]);
    const auto parameter = EndTilingCallbackParameter(guestMemory, g).value_or(0);
    callGuest<sub_8269BF40>(ctx, base, 0x826AC330, g, callbackBuffer - 4, flags | 1, kWorkerKick, parameter);
    const std::uint32_t callbackEnd = ctx.r3.u32 + 4;
    callGuest<sub_8269BCA8>(ctx, base, 0x826AC340, g, fenceBuffer - 4);
    const std::uint32_t fenceEnd = ctx.r3.u32 + 4;
    if (EndTilingSubmitsCallback(guestMemory, g).value_or(true))
    {
        callGuest<sub_8269BD98>(ctx,
                                base,
                                0x826AC3B4,
                                g,
                                tokens,
                                CommandBufferPhysical(callbackBuffer),
                                (callbackEnd - callbackBuffer) / 4,
                                1,
                                g + kPreviousList);
        tokens = ctx.r3.u32;
    }
    callGuest<sub_8269BD98>(ctx, base, 0x826AC3E8, g, tokens, CommandBufferPhysical(fenceBuffer), (fenceEnd - fenceBuffer) / 4, 0, g + kTileList);
    ctx.r3.u64 = 0;
    NoteRingStub(RingStub::TilingEnd, true, site);
    return true;
}

#if REREVVED_DEVELOPER

// The original_residue_probe decision for one Publish or Submit call: whether
// the probe was requested at entry, the caller to attribute, and the reason the
// call runs as it does.
struct ResidueTrace
{
    bool                  active;
    std::uint64_t         lr;
    OriginalResidueReason reason = OriginalResidueReason::Inactive;

    explicit ResidueTrace(std::uint64_t callerLr) noexcept
    : active(OriginalResidueProbeRequested())
    , lr(active ? callerLr : 0)
    {
    }
};

#else

// The shipping build has no residue probe.
struct ResidueTrace
{
    static constexpr bool active = false;

    explicit ResidueTrace(std::uint64_t) noexcept
    {
    }
};

#endif

#if REREVVED_DEVELOPER

// native_ring_consume_publish. A guest call with full-width arguments, as native code
// makes it: no narrowing of r3..r8, the link register restored on return.
template <auto Function>
void callGuest64(PPCContext&   ctx,
                 std::uint8_t* base,
                 std::uint32_t returnAddress,
                 std::uint64_t r3,
                 std::uint64_t r4 = 0,
                 std::uint64_t r5 = 0,
                 std::uint64_t r6 = 0,
                 std::uint64_t r7 = 0,
                 std::uint64_t r8 = 0)
{
    const auto lr = ctx.lr;
    ctx.r3.u64    = r3;
    ctx.r4.u64    = r4;
    ctx.r5.u64    = r5;
    ctx.r6.u64    = r6;
    ctx.r7.u64    = r7;
    ctx.r8.u64    = r8;
    ctx.lr        = returnAddress;
    Function(ctx, base);
    ctx.lr = lr;
}

std::atomic<RingConsumeLockFn> lockAcquireCall{ nullptr };
std::atomic<RingConsumeLockFn> lockReleaseCall{ nullptr };

// The kernel spin lock at G+0x2B04, through the guest import (or the test seam).
// Acquire returns the previous IRQL in r3; release takes it back in r4.
void callSpinLock(bool acquire, PPCContext& ctx, std::uint8_t* base, std::uint64_t lock, std::uint64_t level, std::uint32_t returnAddress)
{
    const auto lr = ctx.lr;
    ctx.r3.u64    = lock;
    ctx.r4.u64    = level;
    ctx.lr        = returnAddress;
    if (const auto call = (acquire ? lockAcquireCall : lockReleaseCall).load(std::memory_order_acquire))
        call(ctx, base);
    else if (acquire)
        __imp__KfAcquireSpinLock(ctx, base);
    else
        __imp__KfReleaseSpinLock(ctx, base);
    ctx.lr = lr;
}

// True while this thread runs the Submit that Publish calls, so the Submit's entry
// check does not repeat the Publish's.
thread_local bool insidePublishConsume = false;

struct PublishNestScope
{
    PublishNestScope() noexcept
    {
        insidePublishConsume = true;
    }

    ~PublishNestScope()
    {
        insidePublishConsume = false;
    }

    PublishNestScope(const PublishNestScope&)            = delete;
    PublishNestScope& operator=(const PublishNestScope&) = delete;
};

std::array<std::atomic<bool>, static_cast<std::size_t>(RingConsumeRefusal::Count)> loggedRefusal{};

// A consumed call keeps no Original; the ledger carries the failure.
void consumeFault(std::string_view step, std::uint32_t site) noexcept
{
    CountRingConsumeFault(step);
    completion::detail::RecordRingConsumeFault(step, site);
}

void noteRefusal(RingConsumeRefusal refusal, std::uint32_t site, std::string_view function) noexcept
{
    const auto index = static_cast<std::size_t>(refusal);
    if (index >= loggedRefusal.size() || loggedRefusal[index].exchange(true, std::memory_order_relaxed))
        return;
    try
    {
        REXLOG_WARN("native_ring_consume: first {} Original: {} (caller {:08X})", function, kRingConsumeRefusalNames[index], site);
    }
    catch (...)
    {
    }
}

enum class ConsumeOutcome : std::uint8_t
{
    Consumed,
    Original,
};

OriginalResidueDispatch publishDispatch(PublishBranch branch) noexcept
{
    switch (branch)
    {
        case PublishBranch::Starved:
            return OriginalResidueDispatch::ConsumedStarved;
        case PublishBranch::Tiling:
            return OriginalResidueDispatch::ConsumedTiling;
        default:
            return OriginalResidueDispatch::ConsumedSubmit;
    }
}

// 0x8269C400 on the host. The branch was classified before any effect; from the
// first mutation on there is no Original, and a failure counts as a fault. The Publish
// lease spans the effects and ends before the rollover helper, which can wait.
ConsumeOutcome consumePublish(PPCContext& ctx, std::uint8_t* base, PublishBranch branch, const ResidueTrace& residue)
{
    const auto                   site = static_cast<std::uint32_t>(ctx.lr);
    const std::uint64_t          g64  = ctx.r3.u64;
    const auto                   g    = static_cast<std::uint32_t>(g64);
    std::optional<RingStubLease> lease;
    lease.emplace();
    if (!*lease)
        return ConsumeOutcome::Original;
    PublishConsumePlan plan;
    const auto         refusal = PlanPublishConsume(guestMemory, g, branch, plan);
    if (refusal != RingConsumeRefusal::None)
    {
        if (refusal == RingConsumeRefusal::Recording || refusal == RingConsumeRefusal::Unclassified)
            CountRingConsumeRecording();
        else if (refusal == RingConsumeRefusal::CpuFence)
            CountRingConsumeCpuFence();
        noteRefusal(refusal, site, "Publish");
        return ConsumeOutcome::Original;
    }
    const RingConsumeCall active;
    CheckPublishEntry(guestMemory, g, plan);
    if (residue.active)
        NoteOriginalResidueDecision(OriginalResidueFunction::Publish, residue.lr, residue.reason, publishDispatch(plan.branch));

    // r28 of the original: where the tail aligns from. r3 on return is separate.
    std::uint64_t branchReturn = plan.next;
    std::uint64_t result       = g64;
    switch (plan.branch)
    {
        case PublishBranch::Submit:
        {
            callGuest64<sub_8269BCA8>(ctx, base, 0x8269C50C, g64, plan.cursor);
            const std::uint64_t submitCursor = ctx.r3.u64 + 4;
            const auto          words        = static_cast<std::int32_t>(static_cast<std::uint32_t>(submitCursor - plan.start)) >> 2;
            {
                const PublishNestScope nested;
                callGuest64<sub_8269BD98>(ctx, base, 0x8269C53C, g64, submitCursor, plan.physical, static_cast<std::uint64_t>(static_cast<std::int64_t>(words)), 0, g64 + kTileList);
            }
            branchReturn = ctx.r3.u64;
            result       = branchReturn;
            break;
        }
        case PublishBranch::Tiling:
        {
            if (plan.count == 0)
                break;
            result            = g64 + kTileList;
            std::uint32_t end = plan.listEnd;
            if (plan.growth)
            {
                callGuest64<sub_8269C170>(ctx, base, 0x8269C4E4, result);
                result = ctx.r3.u64;
                end    = static_cast<std::uint32_t>(result);
            }
            completion::detail::RecordTiledSegment(plan.physical, static_cast<std::uint32_t>(plan.count));
            if (!StoreTilingSegment(guestMemory, g, end, plan.physical, plan.count))
                consumeFault("publish_segment_store", site);
            break;
        }
        default:
            break;
    }

    switch (StorePublishTail(guestMemory, g, branchReturn))
    {
        case PublishTail::Stored:
            ctx.r3.u64 = result;
            break;
        case PublishTail::Failed:
            consumeFault("publish_tail_store", site);
            ctx.r3.u64 = result;
            break;
        case PublishTail::Rollover:
            lease.reset();
            callGuest64<sub_8269C270>(ctx, base, 0x8269C568, g64, 0);
            break;
    }
    return ConsumeOutcome::Consumed;
}

// 0x8269BD98 on the host, over the three arms of the original: entry N zero, the
// locked reread zero, and the locked reread nonzero (the deferred worker-list chain).
// A refusal comes before the first effect and leaves `reason` for the Original call.
ConsumeOutcome consumeSubmit(PPCContext& ctx, std::uint8_t* base, const CompletionScope& completionProbe, ResidueTrace& residue)
{
    const auto          site     = static_cast<std::uint32_t>(ctx.lr);
    const std::uint64_t g64      = ctx.r3.u64;
    const std::uint64_t token    = ctx.r4.u64;
    const std::uint64_t physical = ctx.r5.u64;
    const std::uint64_t dwords   = ctx.r6.u64;
    const std::uint64_t add      = ctx.r7.u64;
    const std::uint64_t list     = ctx.r8.u64;
    const auto          g        = static_cast<std::uint32_t>(g64);
    const RingStubLease lease;
    if (!lease)
    {
        residue.reason = OriginalResidueReason::Inactive;
        NoteRingStub(RingStub::Submit, false, site, "services withdrawn");
        return ConsumeOutcome::Original;
    }
    PublisherDiagnostic publisher;
    RaiseDiagnostic     raise;
    const auto          path = ClassifyPublisher(guestMemory, g, &publisher);
    if (path == PublisherPath::Original)
    {
        residue.reason = OriginalResidueSubmitReason(publisher, raise, false);
        NoteRingStub(RingStub::Submit, false, site, "G+0x541C or G+0x5B0C set");
        return ConsumeOutcome::Original;
    }
    const auto entry = guestMemory.Read32(g + kWorkerPending);
    if (!entry)
    {
        residue.reason = OriginalResidueReason::PendingUnreadable;
        NoteRingStub(RingStub::Submit, false, site, "pending N unreadable");
        return ConsumeOutcome::Original;
    }
    const auto refusal = PlanSubmitConsume(guestMemory, g, static_cast<std::uint32_t>(token), static_cast<std::uint32_t>(list), *entry);
    if (refusal != RingConsumeRefusal::None)
    {
        residue.reason = SubmitRefusalResidueReason(refusal);
        noteRefusal(refusal, site, "Submit");
        NoteRingStub(RingStub::Submit, false, site, kRingConsumeRefusalNames[static_cast<std::size_t>(refusal)]);
        return ConsumeOutcome::Original;
    }

    const RingConsumeCall active;
    if (!insidePublishConsume)
        CheckSubmitEntry(guestMemory, g);
    const auto    physicalWord = static_cast<std::uint32_t>(physical);
    const auto    dwordsWord   = static_cast<std::uint32_t>(dwords);
    const auto    addWord      = static_cast<std::uint32_t>(add);
    std::uint64_t locked       = 0;
    if (*entry != 0)
    {
        callSpinLock(true, ctx, base, g64 + kSubmitLock, 0, 0x8269BDD8);
        locked           = ctx.r3.u64;
        const auto again = guestMemory.Read32(g + kWorkerPending);
        if (!again || *again != 0)
        {
            // The deferred arm: record, store the token, link it, add, unlock.
            if (residue.active)
                NoteOriginalResidueDecision(OriginalResidueFunction::Submit, residue.lr, OriginalResidueReason::PendingNonzero, OriginalResidueDispatch::ConsumedDeferred);
            completionProbe.Stubbed();
            if (!again)
                consumeFault("submit_locked_pending_read", site);
            else
            {
                NoteTilingSubmitArm(TilingArm::Deferred);
                completion::detail::RecordDeferredKick(physicalWord, dwordsWord);
                // A node without its terminator must never become reachable: a failed
                // token store ends here, before the link and the pending add.
                if (!StoreDeferredToken(guestMemory, static_cast<std::uint32_t>(token), physicalWord, dwordsWord))
                    consumeFault("submit_token_store", site);
                else
                {
                    callGuest64<sub_8269B550>(ctx, base, 0x8269BE10, list, token, token + 0x10);
                    ObserveCompletionStoreBegin(0x8269BE1C);
                    if (const auto stored = StorePendingAdd(guestMemory, g, addWord))
                        ObserveCompletionStoreEnd(0x8269BE1C, g, CompletionThread(), CompletionAdd::Deferred, addWord, *stored);
                    else
                        consumeFault("submit_pending_store", site);
                }
            }
            callSpinLock(false, ctx, base, g64 + kSubmitLock, locked, 0x8269BE2C);
            ctx.r3.u64 = token + 0x10;
            NoteRingStub(RingStub::Submit, true, site);
            return ConsumeOutcome::Consumed;
        }
        callSpinLock(false, ctx, base, g64 + kSubmitLock, locked, 0x8269BE2C);
    }

    // The direct arm: N += r7 even for zero, the ledger's direct record, the publisher.
    if (residue.active)
        NoteOriginalResidueDecision(OriginalResidueFunction::Submit,
                                    residue.lr,
                                    path == PublisherPath::Kick ? OriginalResidueReason::ServedKick : OriginalResidueReason::ServedSilent,
                                    path == PublisherPath::Kick ? OriginalResidueDispatch::ServedKick : OriginalResidueDispatch::ServedSilent);
    completionProbe.Stubbed();
    ObserveCompletionStubBegin();
    const auto stored = StorePendingAdd(guestMemory, g, addWord);
    ObserveCompletionStubEnd(g, CompletionThread(), addWord, stored.has_value());
    if (!stored)
        consumeFault("submit_pending_store", site);
    NoteTilingSubmitArm(TilingArm::Direct);
    completion::detail::RecordDirectSubmit(physicalWord, dwordsWord);
    if (path == PublisherPath::Kick)
        kick(g);
    else
        completion::detail::RecordSilentSubmit(); // host-only: no kick, index or RPTR effect
    ctx.r3.u64 = token;
    NoteRingStub(RingStub::Submit, true, site);
    return ConsumeOutcome::Consumed;
}

#endif

RingStub stubFor(std::uint32_t address)
{
    switch (address)
    {
        case 0x8269C588:
            return RingStub::Flush;
        case 0x8269C810:
            return RingStub::Drain;
        case 0x8269B900:
            return RingStub::Publisher;
        case 0x8269B2A0:
            return RingStub::DirectWriter;
        case 0x8269B1F0:
            return RingStub::RingWait;
        case 0x8269BCA8:
            return RingStub::Fence;
        case 0x8269BF40:
            return RingStub::Callback;
        case 0x826A4150:
            return RingStub::PreSwap;
        default:
            return RingStub::TilingCallbacks;
    }
}

bool runRingStub(std::uint32_t address, PPCContext& ctx, std::uint8_t* base) noexcept
{
    try
    {
        const auto site = static_cast<std::uint32_t>(ctx.lr);
        const auto stub = stubFor(address);
        if (address == kWorkerKick)
        {
            // Delivered by the interrupt callback; it always runs as guest code.
            NoteRingStub(RingStub::TilingCallbacks, validatedDevice.load(std::memory_order_acquire) != 0, site, "before a device was checked");
            return false;
        }
        const std::uint32_t g = ctx.r3.u32;
        if (!ready(g, stub, site))
            return false;
        switch (address)
        {
            case 0x8269C588:
                return flush(ctx, base, g, site);
            case 0x8269C810:
                return drain(ctx, base, g, site);
            case 0x8269B900:
                return publisher(g, site);
            case 0x8269B2A0:
                if (!DirectWriterIsStubbed(guestMemory, g, site))
                {
                    NoteRingStub(RingStub::DirectWriter, false, site, site == kRingInitWriterReturn ? "ring initialization" : "G+0x541C or G+0x5B0C set");
                    return false;
                }
                NoteRingStub(RingStub::DirectWriter, true, site);
                return true;
            case 0x8269B1F0:
            {
                const auto position = RingSpaceWait(guestMemory, g, ctx.r4.u32, ctx.r5.u32);
                if (!position)
                {
                    NoteRingStub(RingStub::RingWait, false, site, "ring mask unreadable");
                    return false;
                }
                ctx.r3.u64 = *position;
                NoteRingStub(RingStub::RingWait, true, site);
                return true;
            }
            case 0x8269BCA8:
                return fence(ctx, g, site);
            case 0x8269BF40:
                return callback(ctx, g, site);
            case 0x826A4150:
                return preSwap(ctx, base, g, site);
            default:
                return false;
        }
    }
    catch (...)
    {
        // Bookkeeping never unwinds into guest code.
        return false;
    }
}

[[maybe_unused]] const bool runnerRegistered = (RegisterRingStubRunner(&runRingStub), true);

} // namespace

IRingStubMemory* GuestRingMemory() noexcept
{
    return rex::system::kernel_state() ? &guestMemory : nullptr;
}

#if REREVVED_DEVELOPER
void SetRingConsumeLockCallsForTesting(RingConsumeLockFn acquire, RingConsumeLockFn release) noexcept
{
    lockAcquireCall.store(acquire, std::memory_order_release);
    lockReleaseCall.store(release, std::memory_order_release);
}
#endif

bool RingStubOriginalLoggedForTesting(RingStub stub) noexcept
{
    return loggedOriginal[static_cast<std::size_t>(stub)].load(std::memory_order_relaxed);
}

void ResetRingStubLogsForTesting() noexcept
{
    for (std::size_t i = 0; i < kRingStubCount; ++i)
    {
        loggedStubbed[i].store(false, std::memory_order_relaxed);
        loggedOriginal[i].store(false, std::memory_order_relaxed);
    }
}

bool CheckRingStubReadinessForTesting(IRingStubMemory& memory, std::uint32_t g, RingStub stub, std::uint32_t site, OriginalResidueReadiness* diagnostic)
{
    return ready(g, stub, site, diagnostic, memory);
}

void NoteRingStub(RingStub stub, bool stubbed, std::uint32_t site, std::string_view detail) noexcept
{
    CountRingStub(stub, stubbed);
    auto& logged = stubbed ? loggedStubbed : loggedOriginal;
    if (logged[static_cast<std::size_t>(stub)].exchange(true, std::memory_order_relaxed))
        return;
    try
    {
        if (stubbed)
            REXLOG_INFO("native_ring_stubs: first {} call (caller {:08X})", FieldName(stub), site);
        else
            REXLOG_WARN("native_ring_stubs: first {}_original call (caller {:08X}){}{}", FieldName(stub), site, detail.empty() ? "" : ": ", detail);
    }
    catch (...)
    {
    }
}

} // namespace rerevved::gpu::guest_d3d

namespace guest = rerevved::gpu::guest_d3d;

// Hooks for ring helpers no other wrapper owns. Each runs Original unless the
// stubs are active.

// 0x8269C400 runs Original, which only developer builds with native_ring_consume_publish
// replace; tiled segments store {0x81000000 | dwords, physical} for replay.
REX_HOOK_RAW(sub_8269C400)
{
    guest::ResidueTrace residue(ctx.lr);
#if REREVVED_DEVELOPER
    bool ptReady  = false;
    auto ptBranch = guest::PublishBranch::Unreadable;
    // native_ring_consume_publish with its ledger; only then can the call be consumed.
    const bool consumeEligible = guest::RingConsumePublishRequested() && rerevved::gpu::completion::guestProgressActive.load(std::memory_order_relaxed);
    if (residue.active && guest::RingConsumePublishRequested())
        guest::MarkOriginalResidueRingConsume();
#endif
    if (guest::ringStubsActive.load(std::memory_order_relaxed)) [[unlikely]]
    {
        const auto site = static_cast<std::uint32_t>(ctx.lr);
#if REREVVED_DEVELOPER
        guest::OriginalResidueReadiness readiness{};
        const auto                      readinessOut = residue.active ? &readiness : nullptr;
#else
        guest::OriginalResidueReadiness* const readinessOut = nullptr;
#endif
        if (guest::ready(ctx.r3.u32, guest::RingStub::Publish, site, readinessOut))
        {
            guest::PublishDiagnostic diagnostic;
            const auto               branch = guest::ClassifyPublish(guest::guestMemory, ctx.r3.u32, residue.active ? &diagnostic : nullptr);
#if REREVVED_DEVELOPER
            ptReady  = true;
            ptBranch = branch;
            if (residue.active)
                residue.reason = guest::OriginalResiduePublishReason(diagnostic.branch);
#endif
            switch (branch)
            {
                case guest::PublishBranch::Submit:
                case guest::PublishBranch::Starved:
                    guest::NoteRingStub(guest::RingStub::Publish, true, site);
                    break;
                case guest::PublishBranch::Recording:
                    guest::NoteRingStub(guest::RingStub::Publish, false, site, "recording branch");
                    break;
                case guest::PublishBranch::Tiling:
#if REREVVED_DEVELOPER
                    // The classification count stays. A consumed call is no Original, so
                    // it logs nothing here; a refusal logs its own reason.
                    if (consumeEligible)
                    {
                        guest::CountRingStub(guest::RingStub::Publish, false);
                        break;
                    }
#endif
                    guest::NoteRingStub(guest::RingStub::Publish, false, site, "tiled segment branch");
                    break;
                case guest::PublishBranch::Unreadable:
                    guest::NoteRingStub(guest::RingStub::Publish, false, site, "device fields unreadable");
                    break;
            }
        }
#if REREVVED_DEVELOPER
        else if (residue.active)
            residue.reason = guest::OriginalResidueReadyReason(readiness);
#endif
    }
#if REREVVED_DEVELOPER
    // native_ring_consume_publish: the host runs the call and no Original follows.
    const bool consumeMode = ptReady && consumeEligible;
    if (consumeMode && guest::consumePublish(ctx, base, ptBranch, residue) == guest::ConsumeOutcome::Consumed)
        return;
    if (residue.active)
        guest::NoteOriginalResidueDecision(guest::OriginalResidueFunction::Publish,
                                           residue.lr,
                                           residue.reason,
                                           consumeMode ? guest::OriginalResidueDispatch::Original : guest::OriginalResidueDispatch::Classified);
#endif
    __imp__sub_8269C400(ctx, base);
}

// Direct 0x8269BD98 adds r7 to G+0x2B00 and submits at 0x8269BE5C; deferred
// kick locks G+0x2B04 and writes no ring word.
REX_HOOK_RAW(sub_8269BD98)
{
    guest::ResidueTrace          residue(ctx.lr);
    const guest::CompletionScope completionProbe(0x8269BD98, ctx);
    // Developer tiling probe: the call's site, r7 and G+0x2B00 before and after.
    const guest::TilingSubmitScope tilingProbe(ctx.r3.u32, ctx.lr, ctx.r7.u32);
#if REREVVED_DEVELOPER
    // Requested without its ledger is not the legacy body: that call runs Original.
    const bool requested   = guest::RingConsumePublishRequested();
    const bool consumeMode = requested && rerevved::gpu::completion::guestProgressActive.load(std::memory_order_relaxed);
    if (residue.active && requested)
        guest::MarkOriginalResidueRingConsume();
#endif
    if (guest::ringStubsActive.load(std::memory_order_relaxed)) [[unlikely]]
    {
        const auto          site = static_cast<std::uint32_t>(ctx.lr);
        const std::uint32_t g    = ctx.r3.u32;
#if REREVVED_DEVELOPER
        guest::OriginalResidueReadiness readiness{};
        const auto                      readinessOut = residue.active ? &readiness : nullptr;
#else
        guest::OriginalResidueReadiness* const readinessOut = nullptr;
#endif
        if (guest::ready(g, guest::RingStub::Submit, site, readinessOut))
        {
#if REREVVED_DEVELOPER
            if (consumeMode)
            {
                // native_ring_consume_publish: the host runs the three arms.
                if (guest::consumeSubmit(ctx, base, completionProbe, residue) == guest::ConsumeOutcome::Consumed)
                    return;
            }
            else if (requested)
                guest::NoteRingStub(guest::RingStub::Submit, false, site, "native_ring_consume_publish needs the completion ledger");
            else
#endif
            {
                guest::PublisherDiagnostic publisherDiagnostic;
                guest::RaiseDiagnostic     raiseDiagnostic;
                const auto                 path   = guest::ClassifyPublisher(guest::guestMemory, g, residue.active ? &publisherDiagnostic : nullptr);
                bool                       raised = false;
                if (path != guest::PublisherPath::Original)
                {
                    guest::ObserveCompletionStubBegin();
                    raised = guest::RaiseForDirectSubmit(guest::guestMemory, g, ctx.r7.u32, residue.active ? &raiseDiagnostic : nullptr);
                    guest::ObserveCompletionStubEnd(g, guest::CompletionThread(), ctx.r7.u32, raised);
                }
#if REREVVED_DEVELOPER
                if (residue.active)
                    residue.reason = guest::OriginalResidueSubmitReason(publisherDiagnostic, raiseDiagnostic, raised);
#endif
                if (raised)
                {
                    completionProbe.Stubbed();
                    guest::NoteTilingSubmitArm(guest::TilingArm::Direct);
                    rerevved::gpu::completion::detail::RecordDirectSubmit(ctx.r5.u32, ctx.r6.u32);
                    if (path == guest::PublisherPath::Kick)
                        guest::kick(g);
                    ctx.r3.u64 = ctx.r4.u32;
                    guest::NoteRingStub(guest::RingStub::Submit, true, site);
#if REREVVED_DEVELOPER
                    if (residue.active)
                        guest::NoteOriginalResidueDecision(guest::OriginalResidueFunction::Submit, residue.lr, residue.reason);
#endif
                    return;
                }
                guest::NoteRingStub(guest::RingStub::Submit, false, site, path == guest::PublisherPath::Original ? "G+0x541C or G+0x5B0C set" : "worker owns the ring: deferred chain");
            }
        }
#if REREVVED_DEVELOPER
        else if (residue.active)
            residue.reason = guest::OriginalResidueReadyReason(readiness);
#endif
    }
#if REREVVED_DEVELOPER
    if (residue.active)
        guest::NoteOriginalResidueDecision(guest::OriginalResidueFunction::Submit, residue.lr, residue.reason, requested ? guest::OriginalResidueDispatch::Original : guest::OriginalResidueDispatch::Classified);
#endif
    __imp__sub_8269BD98(ctx, base);
}

// 0x826A9BA0 without its EVENT_WRITE_SHD: the ledger's timestamp record.
REX_HOOK_RAW(sub_826A9BA0)
{
    if (guest::ringStubsActive.load(std::memory_order_relaxed)) [[unlikely]]
    {
        const auto          site = static_cast<std::uint32_t>(ctx.lr);
        const std::uint32_t g    = ctx.r3.u32;
        const std::uint32_t va   = ctx.r4.u32;
        if (guest::ready(g, guest::RingStub::Timestamp, site))
        {
            guest::flushIfPastLimit(ctx, base, g, 0x826A9BD4);
            rerevved::gpu::completion::detail::RecordGpuTimestamp(va, g);
            guest::NoteRingStub(guest::RingStub::Timestamp, true, site);
            return;
        }
    }
    __imp__sub_826A9BA0(ctx, base);
}

// BeginTiling and the replay worker's token handler write no ring word with
// the stubs active and run as guest code; the counts answer whether native
// frames reach the tiling bracket.
REX_HOOK_RAW(sub_826AC0A8)
{
    if (guest::ringStubsActive.load(std::memory_order_relaxed)) [[unlikely]]
    {
        const auto site = static_cast<std::uint32_t>(ctx.lr);
        if (guest::ready(ctx.r3.u32, guest::RingStub::TilingBegin, site))
            guest::NoteRingStub(guest::RingStub::TilingBegin, true, site);
    }
    __imp__sub_826AC0A8(ctx, base);
}

REX_HOOK_RAW(sub_826AC218)
{
    const guest::CompletionScope completionProbe(0x826AC218, ctx);
    // Developer tiling probe: snapshots G[0x2AC0,0x2D00) when the bracket returns.
    const guest::TilingBracketScope tilingProbe(ctx.r3.u32, ctx.r3.u64);
    // The tile record list is read here, before the stub or Original terminates it.
    if (guest::TileListObservationRequested()) [[unlikely]]
        guest::ObserveTileList(guest::guestMemory, ctx.r3.u32);
    if (guest::ringStubsActive.load(std::memory_order_relaxed)) [[unlikely]]
    {
        const auto          site = static_cast<std::uint32_t>(ctx.lr);
        const std::uint32_t g    = ctx.r3.u32;
        if (guest::ready(g, guest::RingStub::TilingEnd, site) && guest::endTiling(ctx, base, g, site))
        {
            completionProbe.Stubbed();
            return;
        }
    }
    __imp__sub_826AC218(ctx, base);
}

REX_HOOK_RAW(sub_826AB740)
{
    if (guest::ringStubsActive.load(std::memory_order_relaxed)) [[unlikely]]
        guest::NoteRingStub(guest::RingStub::TilingTokens,
                            guest::validatedDevice.load(std::memory_order_acquire) != 0,
                            static_cast<std::uint32_t>(ctx.lr),
                            "before a device was checked");
    __imp__sub_826AB740(ctx, base);
}

// B118 runs Original once. Developer builds can replace or check it with the
// default-off tiling restore options.
REX_HOOK_RAW(sub_8269B118)
{
#if REREVVED_DEVELOPER
    if (guest::TilingRestoreReportingRequested()) [[unlikely]]
    {
        guest::NoteTilingRestoreEnter();
        const bool diagnostics = guest::TilingRestoreConsumeRequested() && !guest::TilingRestoreExitCheckRequested() &&
                                 guest::DeveloperRecorderActive();
        if (guest::ConsumeTilingRestore(ctx, diagnostics))
            return;
        const guest::TilingRestoreExitScope exit(ctx);
        guest::NoteTilingRestoreOriginal();
        __imp__sub_8269B118(ctx, base);
        return;
    }
#endif
    __imp__sub_8269B118(ctx, base);
}
