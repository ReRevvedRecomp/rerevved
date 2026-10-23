#include "ring_stubs.h"

#if REREVVED_DEVELOPER
#include "original_residue_probe.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(native_ring_stubs,
                    true,
                    "ReRevved",
                    "On by default for renderer=native: run the guest ring helpers, fences, callbacks, timestamps and the VdSwap call without "
                    "ring or PM4 writes; the completion ledger still completes them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if REREVVED_DEVELOPER
REXCVAR_DEFINE_BOOL(native_ring_consume_publish,
                    false,
                    "ReRevved",
                    "With native_ring_stubs: run Publish 0x8269C400 and Submit 0x8269BD98 on the host, keeping their CPU stores and dropping the PM4 bytes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

namespace rerevved::gpu::guest_d3d
{

namespace
{

using namespace ring_layout;

std::array<std::atomic<std::uint64_t>, kRingStubCount> stubbedCalls{};
std::array<std::atomic<std::uint64_t>, kRingStubCount> originalCalls{};

std::array<std::atomic<std::uint64_t>, kPresentOutcomeCount> presentOutcomes{};

std::mutex                         servicesMutex;
std::shared_ptr<IRingStubServices> publishedServices;
bool                               stubsDisabled = false;
std::atomic<RingStubRunner>        stubRunner{ nullptr };

} // namespace

std::atomic<bool>          ringStubsActive{ false };
std::atomic<std::uint32_t> ringStubsValidatedDevice{ 0 };

namespace
{

// Leases held across all threads, and by this thread.
std::atomic<std::uint32_t>      ringStubLeases{ 0 };
std::atomic<RingStubLeaseProbe> ringStubLeaseProbe{ nullptr };
thread_local std::uint32_t      ownRingStubLeases = 0;
constexpr std::chrono::seconds  kLeaseDrainTimeout{ 5 };

// Waits until no other thread holds a lease. Called after ringStubsActive is
// cleared and with no lock held, so a consume that needs one cannot be starved.
void DrainRingStubLeases(std::string_view who) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + kLeaseDrainTimeout;
    while (ringStubLeases.load(std::memory_order_seq_cst) > ownRingStubLeases)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            REXLOG_ERROR("native_ring_stubs: {} stopped waiting for {} consume lease(s) after {} s", who, ringStubLeases.load() - ownRingStubLeases, kLeaseDrainTimeout.count());
            return;
        }
        std::this_thread::yield();
    }
}

} // namespace

void SetRingStubLeaseProbe(RingStubLeaseProbe probe) noexcept
{
    ringStubLeaseProbe.store(probe, std::memory_order_relaxed);
}

bool AcquireRingStubLease() noexcept
{
    ringStubLeases.fetch_add(1, std::memory_order_seq_cst);
    if (const auto probe = ringStubLeaseProbe.load(std::memory_order_relaxed)) [[unlikely]]
        probe();
    if (!ringStubsActive.load(std::memory_order_seq_cst))
    {
        ringStubLeases.fetch_sub(1, std::memory_order_seq_cst);
        return false;
    }
    ++ownRingStubLeases;
    return true;
}

void ReleaseRingStubLease() noexcept
{
    --ownRingStubLeases;
    ringStubLeases.fetch_sub(1, std::memory_order_seq_cst);
}

void RegisterRingStubRunner(RingStubRunner runner) noexcept
{
    stubRunner.store(runner, std::memory_order_release);
}

bool RunRingStub(std::uint32_t address, PPCContext& ctx, std::uint8_t* base) noexcept
{
    const auto runner = stubRunner.load(std::memory_order_acquire);
    return runner && runner(address, ctx, base);
}

void CountRingStub(RingStub stub, bool stubbed) noexcept
{
    auto& counts = stubbed ? stubbedCalls : originalCalls;
    counts[static_cast<std::size_t>(stub)].fetch_add(1, std::memory_order_relaxed);
}

void CountPresentOutcome(PresentOutcome outcome) noexcept
{
    presentOutcomes[static_cast<std::size_t>(outcome)].fetch_add(1, std::memory_order_relaxed);
}

RingStubCounts TakeRingStubCounts() noexcept
{
    RingStubCounts counts;
    for (std::size_t i = 0; i < kRingStubCount; ++i)
    {
        counts.stubbed[i]  = stubbedCalls[i].exchange(0, std::memory_order_relaxed);
        counts.original[i] = originalCalls[i].exchange(0, std::memory_order_relaxed);
    }
    // Outcomes before the entry count. A call in flight at a take can split its
    // entry and outcome across windows; totals satisfy the invariant.
    for (std::size_t i = kPresentOutcomeCount; i-- > 0;)
        counts.present[i] = presentOutcomes[i].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatRingStubCounts(const RingStubCounts& counts)
{
    std::string fields;
    for (std::size_t i = 0; i < kRingStubCount; ++i)
    {
        const std::string_view prefix = i < kFirstTilingCounter ? " stub_" : " ";
        if (counts.stubbed[i])
        {
            fields += prefix;
            fields += kRingStubNames[i];
            fields += '=';
            fields += std::to_string(counts.stubbed[i]);
        }
        if (counts.original[i])
        {
            fields += prefix;
            fields += kRingStubNames[i];
            fields += "_original=";
            fields += std::to_string(counts.original[i]);
        }
    }
    for (std::size_t i = 0; i < kPresentOutcomeCount; ++i)
    {
        if (counts.present[i])
        {
            fields += ' ';
            fields += kPresentOutcomeNames[i];
            fields += '=';
            fields += std::to_string(counts.present[i]);
        }
    }
    return fields;
}

bool RingStubsRequested() noexcept
{
    return REXCVAR_GET(native_ring_stubs);
}

void PublishRingStubServices(std::shared_ptr<IRingStubServices> services)
{
    std::lock_guard lock(servicesMutex);
    publishedServices = std::move(services);
    stubsDisabled     = false;
    ringStubsActive.store(publishedServices != nullptr, std::memory_order_seq_cst);
#if REREVVED_DEVELOPER
    if (RingConsumeCvar())
        RetireRingConsumeMonitor();
#endif
}

void WithdrawRingStubServices()
{
    {
        std::lock_guard lock(servicesMutex);
        ringStubsActive.store(false, std::memory_order_seq_cst);
    }
    // In-flight consumes finish before the services go.
    DrainRingStubLeases("withdrawal");
    std::lock_guard lock(servicesMutex);
    publishedServices.reset();
#if REREVVED_DEVELOPER
    if (RingConsumeCvar())
        RetireRingConsumeMonitor();
#endif
}

std::shared_ptr<IRingStubServices> CurrentRingStubServices()
{
    std::lock_guard lock(servicesMutex);
    return publishedServices;
}

void DisableRingStubs(std::string_view reason)
{
    {
        std::lock_guard lock(servicesMutex);
        if (stubsDisabled)
            return;
        stubsDisabled = true;
        ringStubsActive.store(false, std::memory_order_seq_cst);
    }
    DrainRingStubLeases("disable");
#if REREVVED_DEVELOPER
    if (RingConsumeCvar())
        RetireRingConsumeMonitor();
#endif
    REXLOG_ERROR("native_ring_stubs: disabled for this session, every ring helper runs Original; the completion ledger keeps its records: {}", reason);
}

RingLayoutCheck CheckRingLayout(IRingStubMemory& memory, std::uint32_t g)
{
    const auto fail = [](RingLayout verdict, std::string detail)
    {
        return RingLayoutCheck{ verdict, std::move(detail) };
    };
    if (g < 0x10000 || g > 0xFFFFFFFFU - kDeviceEnd || !memory.Read32(g) || !memory.Read32(g + kDeviceEnd - 4))
        return fail(RingLayout::Mismatch, std::format("device {:#010x} unreadable", g));
    const auto global = memory.Read32(kDeviceGlobal);
    if (!global)
        return fail(RingLayout::Mismatch, "device global 0x820009f0 unreadable");
    const auto device = *global ? memory.Read32(*global) : std::optional<std::uint32_t>{ 0 };
    if (!device)
        return fail(RingLayout::Mismatch, std::format("device global {:#010x} unreadable", *global));
    if (*device == 0)
        return fail(RingLayout::NotReady, "device global not published");
    if (*device != g)
        return fail(RingLayout::Mismatch, std::format("device global names {:#010x}, not {:#010x}", *device, g));
    const auto block   = memory.Read32(g + kProgressBlock);
    const auto scratch = memory.Read32(g + kScratchBlock);
    if (!block || !scratch)
        return fail(RingLayout::Mismatch, "block pointers unreadable");
    if (*block == 0 || *scratch == 0)
        return fail(RingLayout::NotReady, "progress or scratch block not allocated");
    if (!memory.Physical(*block) || !memory.Read32(*block + kTimestampEnd - 4))
        return fail(RingLayout::Mismatch, std::format("progress block {:#010x} unmapped", *block));
    if (!memory.Physical(*scratch) || !memory.Read32(*scratch + 0x1C))
        return fail(RingLayout::Mismatch, std::format("scratch block {:#010x} unmapped", *scratch));
    const auto mask = memory.Read32(g + kRingMask);
    if (!mask || *mask == 0 || (*mask & (*mask + 1)) != 0)
        return fail(RingLayout::Mismatch, std::format("ring mask {:#x} is not 2^n - 1", mask.value_or(0)));
    const auto fence = memory.Read32(g + kNextFence);
    if (!fence || (*fence & 1) == 0)
        return fail(RingLayout::Mismatch, std::format("next fence {:#x} is not odd", fence.value_or(0)));
    return {};
}

bool ApplyFenceFields(IRingStubMemory& memory, std::uint32_t g)
{
    const auto block  = memory.Read32(g + kProgressBlock);
    const auto fence  = memory.Read32(g + kNextFence);
    const auto cursor = memory.Read32(g + kCursor);
    const auto low    = memory.Read32(g + kFenceLow);
    const auto sink   = memory.Read32(g + kCpuFenceSink);
    const auto high   = memory.Read8(g + kModeHigh);
    if (!block || !fence || !cursor || !low || !sink || !high || !memory.Read32(*block))
        return false;
    if (!memory.Write32(g + kFenceCursorCopy, *cursor) || !memory.Write32(g + kFenceLowCopy, *low))
        return false;
    // CPU fence mode completes the fence on the CPU at once, as the original does.
    if (*sink == 0 && (*high & kModeCpuFence) &&
        (!memory.Write32(*block + kFenceOffset, *fence) || !memory.Write32(*block + kFenceCursorOffset, *cursor | (*low & 3))))
        return false;
    return memory.Write32(g + kNextFence, *fence + 2);
}

PublishBranch ClassifyPublish(IRingStubMemory& memory, std::uint32_t g, PublishDiagnostic* diagnostic)
{
    const auto high   = memory.Read8(g + kModeHigh);
    const auto low    = memory.Read8(g + kModeLow);
    const auto tiling = memory.Read32(g + kTiling);
    const auto result = [&](PublishBranch branch)
    {
        if (diagnostic)
            *diagnostic = { static_cast<std::uint8_t>((high.has_value() ? 1 : 0) | (low.has_value() ? 2 : 0) | (tiling.has_value() ? 4 : 0)), high.value_or(0), low.value_or(0), tiling.value_or(0), branch };
        return branch;
    };
    if (!high || !low || !tiling)
        return result(PublishBranch::Unreadable);
    if (*high & kModeRingStarved)
        return result(PublishBranch::Starved);
    if (*low & kModeLowRecording)
        return result(PublishBranch::Recording);
    return result(*tiling ? PublishBranch::Tiling : PublishBranch::Submit);
}

std::optional<Coherency> TakeCoherency(IRingStubMemory& memory, std::uint32_t g)
{
    const auto tiling = memory.Read32(g + kTiling);
    const auto low    = memory.Read8(g + kModeLow);
    if (!tiling || !low || *tiling || (*low & kModeLowRecording))
        return std::nullopt;
    return SwapCoherency(memory, g);
}

std::optional<Coherency> SwapCoherency(IRingStubMemory& memory, std::uint32_t g)
{
    const auto extra = memory.Read8(g + kModeExtra);
    if (!extra)
        return std::nullopt;
    Coherency coherency;
    coherency.extra = (*extra & kModeExtraCoherent) != 0;
    // 0x8269B3F8 swaps the range empty only when one is pending.
    const auto range = memory.Read32(g + kCoherencyRange);
    const auto end   = memory.Read32(g + kCoherencyRange + 4);
    if (!range || !end)
        return std::nullopt;
    if (((std::uint64_t(*range) << 32) | *end) != kEmptyCoherencyRange)
    {
        const auto taken = memory.Exchange64(g + kCoherencyRange, kEmptyCoherencyRange);
        if (!taken)
            return std::nullopt;
        coherency.range = *taken != kEmptyCoherencyRange;
    }
    return coherency;
}

std::optional<std::uint32_t> CpuFenceSwitch(IRingStubMemory& memory, std::uint32_t g)
{
    const auto low    = memory.Read8(g + kModeLow);
    const auto high   = memory.Read8(g + kModeHigh);
    const auto global = memory.Read32(kCpuFenceGlobal);
    const auto fence  = memory.Read32(g + kNextFence);
    if (!low || !high || !global || !fence || (*low & kModeLowRecording) || *global == 0 || (*high & kModeCpuFence))
        return std::nullopt;
    return *fence - 2;
}

bool SetCpuFenceMode(IRingStubMemory& memory, std::uint32_t g)
{
    const auto high = memory.Read8(g + kModeHigh);
    return high && memory.Write8(g + kModeHigh, *high | kModeCpuFence);
}

std::optional<bool> CursorPastLimit(IRingStubMemory& memory, std::uint32_t g)
{
    const auto cursor = memory.Read32(g + kCursor);
    const auto limit  = memory.Read32(g + kCursorLimit);
    if (!cursor || !limit)
        return std::nullopt;
    return *cursor > *limit;
}

std::optional<std::uint32_t> RingSpaceWait(IRingStubMemory& memory, std::uint32_t g, std::uint32_t start, std::uint32_t count)
{
    const auto mask = memory.Read32(g + kRingMask);
    if (!mask)
        return std::nullopt;
    return (start + count) & *mask;
}

PublisherPath ClassifyPublisher(IRingStubMemory& memory, std::uint32_t g, PublisherDiagnostic* diagnostic)
{
    const auto sink     = memory.Read32(g + kCpuFenceSink);
    const auto callback = memory.Read32(g + kRingCallback);
    const auto high     = memory.Read8(g + kModeHigh);
    const auto result   = [&](PublisherPath path)
    {
        if (diagnostic)
            *diagnostic = { static_cast<std::uint8_t>((sink.has_value() ? 1 : 0) | (callback.has_value() ? 2 : 0) | (high.has_value() ? 4 : 0)), sink.value_or(0), callback.value_or(0), high.value_or(0), path };
        return path;
    };
    if (!sink || !callback || !high || *sink != 0)
        return result(PublisherPath::Original);
    if (*high & kModeCpuFence)
        return result(PublisherPath::Silent);
    return result(*callback == 0 ? PublisherPath::Kick : PublisherPath::Original);
}

bool RaiseForDirectSubmit(IRingStubMemory& memory, std::uint32_t g, std::uint32_t raise, RaiseDiagnostic* diagnostic)
{
    const auto pending = memory.Read32(g + kWorkerPending);
    if (diagnostic)
        *diagnostic = { pending.has_value(), pending.value_or(0), false, false };
    if (!pending || *pending != 0)
        return false;
    if (raise == 0)
        return true;
    const bool written = memory.Write32(g + kWorkerPending, raise);
    if (diagnostic)
    {
        diagnostic->attemptedWrite = true;
        diagnostic->writeSucceeded = written;
    }
    return written;
}

bool DirectWriterIsStubbed(IRingStubMemory& memory, std::uint32_t g, std::uint32_t returnAddress)
{
    const auto sink     = memory.Read32(g + kCpuFenceSink);
    const auto callback = memory.Read32(g + kRingCallback);
    return returnAddress != kRingInitWriterReturn && sink && callback && *sink == 0 && *callback == 0;
}

std::optional<std::uint32_t> CallbackCursor(IRingStubMemory& memory, std::uint32_t g, std::uint32_t cursor, std::uint32_t flags)
{
    const auto global    = memory.Read32(kDeviceGlobal);
    const auto device    = global ? memory.Read32(*global) : std::nullopt;
    const auto scratch   = device ? memory.Read32(*device + kScratchBlock) : std::nullopt;
    const auto predicate = memory.Read32(g + kPredicate);
    if (!scratch || !*scratch || !memory.Physical(*scratch) || !predicate || (*predicate && !memory.Read32(g + kPredicateMask)))
        return std::nullopt;
    return cursor + 4 * CallbackEmitterDwords(NormalizeCallbackFlags(flags));
}

std::optional<PreSwapPlan> PlanPreSwap(IRingStubMemory& memory, std::uint32_t g, std::uint32_t flipAddress, std::string* refusal)
{
    const auto refuse = [&](std::string detail) -> std::optional<PreSwapPlan>
    {
        if (refusal)
            *refusal = std::move(detail);
        return std::nullopt;
    };
    const auto timing   = memory.Read32(g + kTimingMode);
    const auto interval = memory.Read32(g + kSwapInterval);
    const auto flags    = memory.Read32(g + kSwapFlags);
    if (!timing || !interval || !flags)
        return refuse("state unreadable");
    if (*timing == 2)
        return refuse("timestamp mode 2");
    std::uint32_t code = 0;
    switch (*interval)
    {
        case 0:
        case 1:
            code = 1;
            break;
        case 2:
            code = 2;
            break;
        case 4:
            code = 3;
            break;
        case 0x80000000:
            code = 0;
            break;
        default:
            return refuse(std::format("swap interval {:#x}", *interval));
    }
    return PreSwapPlan{ .parameter = (code << 8) | ((*flags >> 23) & 0x7F) | flipAddress, .interval = code };
}

bool FinishPreSwap(IRingStubMemory& memory, std::uint32_t g)
{
    const auto high = memory.Read8(g + kModeHigh);
    return high && memory.Write8(g + kModeHigh, *high | kModePreSwapDone);
}

std::optional<std::uint32_t> EndTilingCallbackParameter(IRingStubMemory& memory, std::uint32_t g)
{
    const auto head = memory.Read32(g + kTileList);
    if (!head)
        return std::nullopt;
    return *head ? CommandBufferPhysical(*head) + 0xC0000000U : 0;
}

std::optional<bool> EndTilingSubmitsCallback(IRingStubMemory& memory, std::uint32_t g)
{
    const auto sink = memory.Read32(g + kCpuFenceSink);
    const auto high = memory.Read8(g + kModeHigh);
    if (!sink || !high)
        return std::nullopt;
    return *sink != 0 || (*high & kModeCpuFence) == 0;
}

bool EndTilingState(IRingStubMemory& memory, std::uint32_t g)
{
    return memory.Write32(g + kTiling, 0) && memory.Write32(g + kTilingNext, 0);
}

std::uint32_t RingStubLeaseCountForTesting() noexcept
{
    return ringStubLeases.load(std::memory_order_seq_cst);
}

#if REREVVED_DEVELOPER

bool RingConsumeCvar() noexcept
{
    return REXCVAR_GET(native_ring_consume_publish);
}

OriginalResidueReason SubmitRefusalResidueReason(RingConsumeRefusal refusal) noexcept
{
    switch (refusal)
    {
        case RingConsumeRefusal::PendingUnwritable:
            return OriginalResidueReason::RaiseWriteFailed;
        case RingConsumeRefusal::LockStorage:
            return OriginalResidueReason::LockStorage;
        case RingConsumeRefusal::TokenStorage:
            return OriginalResidueReason::TokenStorage;
        case RingConsumeRefusal::TokenWrap:
            return OriginalResidueReason::TokenWrap;
        case RingConsumeRefusal::ListLink:
            return OriginalResidueReason::ListStorage;
        default:
            return OriginalResidueReason::PendingNonzero;
    }
}

RingConsumeRefusal PlanPublishConsume(IRingStubMemory& memory, std::uint32_t g, PublishBranch branch, PublishConsumePlan& plan)
{
    plan        = {};
    plan.branch = branch;
    if (branch == PublishBranch::Recording)
        return RingConsumeRefusal::Recording;
    if (branch == PublishBranch::Unreadable)
        return RingConsumeRefusal::Unclassified;
    const auto cursor = memory.Read32(g + kCursor);
    if (!cursor)
        return RingConsumeRefusal::Cursor;
    const auto start = memory.Read32(g + kTailStart);
    if (!start)
        return RingConsumeRefusal::Start;
    if (!memory.Read32(g + kCursorLimit))
        return RingConsumeRefusal::Limit;
    if (!memory.Writable32(g + kCursor) || !memory.Writable32(g + kTailStart))
        return RingConsumeRefusal::TailStorage;
    // The Submit branch's fence would complete on the CPU ahead of the host draws.
    if (branch == PublishBranch::Submit && FenceRunsInCpuMode(memory, g))
        return RingConsumeRefusal::CpuFence;
    plan.cursor   = *cursor;
    plan.start    = *start;
    plan.next     = std::uint64_t{ *cursor } + 4;
    plan.physical = CommandBufferPhysical(*start);
    if (branch != PublishBranch::Tiling)
        return RingConsumeRefusal::None;
    plan.count = static_cast<std::int32_t>(static_cast<std::uint32_t>(plan.next - *start)) >> 2;
    if (plan.count == 0)
        return RingConsumeRefusal::None;
    const auto head  = memory.Read32(g + kListEnd);
    const auto limit = memory.Read32(g + kListLimit);
    if (!head)
        return RingConsumeRefusal::ListHead;
    if (!limit)
        return RingConsumeRefusal::ListLimit;
    plan.listEnd   = *head;
    plan.listLimit = *limit;
    plan.growth    = static_cast<std::uint32_t>(*head + 8) > *limit;
    // Growth returns its own destination, which the helper checks.
    if (!memory.Writable32(g + kListEnd) || (!plan.growth && (!memory.Writable32(*head) || !memory.Writable32(*head + 4))))
        return RingConsumeRefusal::ListStorage;
    return RingConsumeRefusal::None;
}

bool StoreTilingSegment(IRingStubMemory& memory, std::uint32_t g, std::uint32_t listEnd, std::uint32_t physical, std::int32_t count)
{
    return memory.Write32(listEnd + 4, physical) && memory.Write32(listEnd, static_cast<std::uint32_t>(count) | 0x81000000U) && memory.Write32(g + kListEnd, listEnd + 8);
}

PublishTail StorePublishTail(IRingStubMemory& memory, std::uint32_t g, std::uint64_t branchReturn)
{
    const auto limit = memory.Read32(g + kCursorLimit);
    if (!limit)
        return PublishTail::Failed;
    const auto aligned = static_cast<std::uint32_t>(branchReturn + 0x1F) & 0xFFFFFFE0U;
    if (aligned <= *limit)
        return memory.Write32(g + kTailStart, aligned) && memory.Write32(g + kCursor, aligned - 4) ? PublishTail::Stored : PublishTail::Failed;
    return memory.Write32(g + kCursor, static_cast<std::uint32_t>(branchReturn - 4)) ? PublishTail::Rollover : PublishTail::Failed;
}

RingConsumeRefusal PlanSubmitConsume(IRingStubMemory& memory, std::uint32_t g, std::uint32_t p, std::uint32_t list, std::uint32_t entryPending)
{
    if (!memory.Writable32(g + kWorkerPending))
        return RingConsumeRefusal::PendingUnwritable;
    if (entryPending == 0)
        return RingConsumeRefusal::None;
    if (!memory.Writable32(g + kSubmitLock))
        return RingConsumeRefusal::LockStorage;
    if (p > 0xFFFFFFFFU - 0x10)
        return RingConsumeRefusal::TokenWrap;
    for (const std::uint32_t offset : { 0x4U, 0x8U, 0xCU })
        if (!memory.Writable32(p + offset))
            return RingConsumeRefusal::TokenStorage;
    const auto tail = memory.Read32(list + 8);
    if (!tail || !memory.Writable32(list + 4) || !memory.Writable32(list + 8) || !memory.Writable32(p) || !memory.Writable32(*tail))
        return RingConsumeRefusal::ListLink;
    return RingConsumeRefusal::None;
}

std::optional<std::uint32_t> StorePendingAdd(IRingStubMemory& memory, std::uint32_t g, std::uint32_t add)
{
    const auto pending = memory.Read32(g + kWorkerPending);
    if (!pending)
        return std::nullopt;
    const auto stored = *pending + add;
    if (!memory.Write32(g + kWorkerPending, stored))
        return std::nullopt;
    return stored;
}

bool StoreDeferredToken(IRingStubMemory& memory, std::uint32_t p, std::uint32_t physical, std::uint32_t dwords)
{
    return memory.Write32(p + 4, dwords | 0x81000000U) && memory.Write32(p + 8, physical) && memory.Write32(p + 0xC, 0xC0000000U);
}

bool FenceRunsInCpuMode(IRingStubMemory& memory, std::uint32_t g)
{
    const auto sink = memory.Read32(g + kCpuFenceSink);
    const auto high = memory.Read8(g + kModeHigh);
    return sink && high && *sink == 0 && (*high & kModeCpuFence) != 0;
}

namespace
{

constexpr std::size_t   kWaitSlots         = 64;
constexpr std::uint64_t kClaiming          = ~std::uint64_t{ 0 };
constexpr unsigned      kInvariantExamples = 4;

struct WaitSlot
{
    std::atomic<std::uint64_t> start{ 0 }; // zero when free
    std::atomic<std::uint32_t> kind{ 0 }, device{ 0 }, caller{ 0 }, thread{ 0 }, requested{ 0 }, generation{ 0 };
    // (incarnation << 1) | reported: a claim to report succeeds only for the incarnation it read.
    std::atomic<std::uint64_t> state{ 0 };
};

struct WaitCounters
{
    std::atomic<std::uint64_t> calls{ 0 }, totalUs{ 0 }, maxUs{ 0 };
    std::atomic<std::int64_t>  active{ 0 };
};

std::atomic<std::uint64_t>                              consumeFaults{ 0 }, consumeCpuFences{ 0 }, consumeRecordings{ 0 };
std::atomic<std::uint64_t>                              fenceWatchdogs{ 0 }, drainWatchdogs{ 0 };
std::atomic<bool>                                       retiredIncomplete{ false };
std::atomic<void (*)(std::size_t)>                      pollClaimHook{ nullptr };
std::array<std::atomic<std::uint64_t>, kRingInvariants> invariantCounts{};
std::array<std::atomic<unsigned>, kRingInvariants>      invariantExamples{};
std::array<WaitCounters, kRingWaitKinds>                waitCounters;
std::array<WaitSlot, kWaitSlots>                        waitSlots;
std::atomic<std::int64_t>                               activeConsumeCalls{ 0 };
std::atomic<std::uint32_t>                              monitorGeneration{ 1 };
std::atomic<RingConsumeClock>                           consumeClock{ nullptr };
std::mutex                                              consumeReportMutex;
RingConsumeCounts                                       pendingConsumeReport;
std::mutex                                              faultStepMutex;
std::vector<std::string>                                faultSteps;

std::uint64_t consumeNowUs() noexcept
{
    if (const auto clock = consumeClock.load(std::memory_order_relaxed))
        return std::max<std::uint64_t>(clock(), 1);
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) + 1;
}

std::uint32_t threadToken() noexcept
{
    return static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

} // namespace

void SetRingConsumeClock(RingConsumeClock clock) noexcept
{
    consumeClock.store(clock, std::memory_order_relaxed);
}

void NoteRingInvariant(RingInvariant field, std::uint32_t g, std::uint64_t first, std::uint64_t second) noexcept
{
    const auto index = static_cast<std::size_t>(field);
    if (index >= kRingInvariants)
        return;
    invariantCounts[index].fetch_add(1, std::memory_order_relaxed);
    if (invariantExamples[index].fetch_add(1, std::memory_order_relaxed) >= kInvariantExamples)
        return;
    try
    {
        REXLOG_WARN("native_ring_consume: entry invariant {} violated on device {:08X}: {:#x} {:#x}", kRingInvariantNames[index], g, first, second);
    }
    catch (...)
    {
    }
}

namespace
{

unsigned checkFenceAndPending(IRingStubMemory& memory, std::uint32_t g) noexcept
{
    unsigned   violations = 0;
    const auto fence      = memory.Read32(g + kNextFence);
    if (!fence || (*fence & 1) == 0)
    {
        NoteRingInvariant(RingInvariant::FenceParity, g, fence.value_or(0));
        ++violations;
    }
    if (!memory.Read32(g + kWorkerPending))
    {
        NoteRingInvariant(RingInvariant::PendingUnreadable, g, g + kWorkerPending);
        ++violations;
    }
    return violations;
}

} // namespace

unsigned CheckPublishEntry(IRingStubMemory& memory, std::uint32_t g, const PublishConsumePlan& plan) noexcept
{
    unsigned violations = 0;
    if (plan.start > plan.next)
    {
        NoteRingInvariant(RingInvariant::CursorOrder, g, plan.cursor, plan.start);
        ++violations;
    }
    // The command buffer lies below the hard limit G+0x34 (zero before the device sets it), in
    // the allocation that holds the limit's last dword; C and S belong to that allocation.
    const auto hard = memory.Read32(g + kHardLimit);
    const auto past = [&](std::uint64_t end)
    {
        return hard && *hard != 0 && end > *hard;
    };
    std::optional<RingAllocation> buffer;
    if (hard && *hard >= 4)
        buffer = memory.Allocation(*hard - 4);
    const auto outside = [&](std::uint32_t address)
    {
        return buffer && (address < buffer->base || address - buffer->base >= buffer->size);
    };
    if (!memory.Physical(plan.cursor) || past(plan.next) || outside(plan.cursor))
    {
        NoteRingInvariant(RingInvariant::CursorExtent, g, plan.cursor, hard.value_or(0));
        ++violations;
    }
    if (!memory.Physical(plan.start) || past(plan.start) || outside(plan.start))
    {
        NoteRingInvariant(RingInvariant::StartExtent, g, plan.start, hard.value_or(0));
        ++violations;
    }
    violations += checkFenceAndPending(memory, g);
    // The list needs E + 8 <= its capacity unless the growth helper runs.
    if (plan.branch == PublishBranch::Tiling && plan.count != 0 && !plan.growth && std::uint64_t{ plan.listEnd } + 8 > plan.listLimit)
    {
        NoteRingInvariant(RingInvariant::ListCapacity, g, plan.listEnd, plan.listLimit);
        ++violations;
    }
    return violations;
}

unsigned CheckSubmitEntry(IRingStubMemory& memory, std::uint32_t g) noexcept
{
    return checkFenceAndPending(memory, g);
}

RingWaitScope::RingWaitScope(RingWaitKind kind, std::uint32_t device, std::uint64_t caller, std::uint32_t requested) noexcept
: kind_(kind)
{
    if (!RingConsumePublishRequested())
        return;
    counted_    = true;
    generation_ = monitorGeneration.load(std::memory_order_acquire);
    start_      = consumeNowUs();
    waitCounters[static_cast<std::size_t>(kind)].active.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kWaitSlots; ++i)
    {
        auto&         slot     = waitSlots[i];
        std::uint64_t expected = 0;
        if (!slot.start.compare_exchange_strong(expected, kClaiming, std::memory_order_acquire, std::memory_order_relaxed))
            continue;
        // The new incarnation first; the field stores release, so a reader that sees
        // any new field also sees the new state.
        slot.state.store(((slot.state.load(std::memory_order_relaxed) >> 1) + 1) << 1, std::memory_order_release);
        slot.kind.store(static_cast<std::uint32_t>(kind), std::memory_order_release);
        slot.device.store(device, std::memory_order_release);
        slot.caller.store(static_cast<std::uint32_t>(caller), std::memory_order_release);
        slot.thread.store(threadToken(), std::memory_order_release);
        slot.requested.store(requested, std::memory_order_release);
        slot.generation.store(generation_, std::memory_order_release);
        slot.start.store(start_, std::memory_order_release);
        slot_ = static_cast<int>(i);
        break;
    }
}

RingWaitScope::~RingWaitScope()
{
    if (!counted_)
        return;
    const auto end      = consumeNowUs();
    const auto duration = end > start_ ? end - start_ : 0;
    auto&      counters = waitCounters[static_cast<std::size_t>(kind_)];
    counters.calls.fetch_add(1, std::memory_order_relaxed);
    counters.totalUs.fetch_add(duration, std::memory_order_relaxed);
    auto seen = counters.maxUs.load(std::memory_order_relaxed);
    while (seen < duration && !counters.maxUs.compare_exchange_weak(seen, duration, std::memory_order_relaxed))
    {
    }
    // A lifecycle change already cleared the table and the active counts.
    if (generation_ != monitorGeneration.load(std::memory_order_acquire))
        return;
    counters.active.fetch_sub(1, std::memory_order_relaxed);
    if (slot_ >= 0)
        waitSlots[static_cast<std::size_t>(slot_)].start.store(0, std::memory_order_release);
}

std::vector<RingWaitReport> PollRingWaits(std::uint64_t thresholdUs) noexcept
{
    std::vector<RingWaitReport> reports;
    try
    {
        const auto now = consumeNowUs();
        for (auto& slot : waitSlots)
        {
            const auto started = slot.start.load(std::memory_order_acquire);
            if (started == 0 || started == kClaiming)
                continue;
            const auto     seen = slot.state.load(std::memory_order_acquire);
            RingWaitReport report;
            const auto     kind = slot.kind.load(std::memory_order_acquire);
            report.generation   = slot.generation.load(std::memory_order_acquire);
            report.device       = slot.device.load(std::memory_order_acquire);
            report.caller       = slot.caller.load(std::memory_order_acquire);
            report.thread       = slot.thread.load(std::memory_order_acquire);
            report.requested    = slot.requested.load(std::memory_order_acquire);
            if (slot.start.load(std::memory_order_acquire) != started || slot.state.load(std::memory_order_acquire) != seen || (seen & 1) != 0 || report.generation != monitorGeneration.load(std::memory_order_acquire))
                continue;
            if (kind != static_cast<std::uint32_t>(RingWaitKind::Fence) && kind != static_cast<std::uint32_t>(RingWaitKind::Drain))
                continue;
            report.kind      = static_cast<RingWaitKind>(kind);
            report.elapsedUs = now > started ? now - started : 0;
            if (report.elapsedUs < thresholdUs)
                continue;
            if (const auto hook = pollClaimHook.load(std::memory_order_relaxed))
                hook(static_cast<std::size_t>(&slot - waitSlots.data()));
            // Only the incarnation read above can be marked reported.
            auto expected = seen;
            if (!slot.state.compare_exchange_strong(expected, seen | 1, std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            (report.kind == RingWaitKind::Fence ? fenceWatchdogs : drainWatchdogs).fetch_add(1, std::memory_order_relaxed);
            reports.push_back(report);
        }
    }
    catch (...)
    {
    }
    return reports;
}

void RetireRingConsumeMonitor() noexcept
{
    // The reset below forgets the open waits; the final report still names them.
    bool open = activeConsumeCalls.load(std::memory_order_relaxed) > 0;
    for (const auto& counters : waitCounters)
        open = open || counters.active.load(std::memory_order_relaxed) > 0;
    if (open)
        retiredIncomplete.store(true, std::memory_order_relaxed);
    ResetRingConsumeMonitor();
}

void SetRingPollClaimHookForTesting(void (*hook)(std::size_t slot)) noexcept
{
    pollClaimHook.store(hook, std::memory_order_relaxed);
}

void ResetRingConsumeMonitor() noexcept
{
    monitorGeneration.fetch_add(1, std::memory_order_acq_rel);
    for (auto& slot : waitSlots)
        slot.start.store(0, std::memory_order_release);
    for (auto& counters : waitCounters)
        counters.active.store(0, std::memory_order_relaxed);
}

RingConsumeCall::RingConsumeCall() noexcept
{
    activeConsumeCalls.fetch_add(1, std::memory_order_relaxed);
}

RingConsumeCall::~RingConsumeCall()
{
    activeConsumeCalls.fetch_sub(1, std::memory_order_relaxed);
}

void CountRingConsumeFault(std::string_view step) noexcept
{
    consumeFaults.fetch_add(1, std::memory_order_relaxed);
    try
    {
        std::lock_guard lock(faultStepMutex);
        if (faultSteps.size() >= 32 || std::find(faultSteps.begin(), faultSteps.end(), step) != faultSteps.end())
            return;
        faultSteps.emplace_back(step);
        REXLOG_ERROR("native_ring_consume: a consumed call failed at {}", step);
    }
    catch (...)
    {
    }
}

void CountRingConsumeCpuFence() noexcept
{
    consumeCpuFences.fetch_add(1, std::memory_order_relaxed);
}

void CountRingConsumeRecording() noexcept
{
    consumeRecordings.fetch_add(1, std::memory_order_relaxed);
}

RingConsumeCounts TakeRingConsumeCounts(bool final) noexcept
{
    RingConsumeCounts out;
    out.fault                   = consumeFaults.exchange(0, std::memory_order_relaxed);
    out.publishOriginalCpuFence = consumeCpuFences.exchange(0, std::memory_order_relaxed);
    out.recordingOriginal       = consumeRecordings.exchange(0, std::memory_order_relaxed);
    out.watchdogFence           = fenceWatchdogs.exchange(0, std::memory_order_relaxed);
    out.watchdogDrain           = drainWatchdogs.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kRingInvariants; ++i)
        out.invariant[i] = invariantCounts[i].exchange(0, std::memory_order_relaxed);
    bool waiting = false;
    for (std::size_t i = 0; i < kRingWaitKinds; ++i)
    {
        out.wait[i].calls   = waitCounters[i].calls.exchange(0, std::memory_order_relaxed);
        out.wait[i].totalUs = waitCounters[i].totalUs.exchange(0, std::memory_order_relaxed);
        out.wait[i].maxUs   = waitCounters[i].maxUs.exchange(0, std::memory_order_relaxed);
        const auto active   = waitCounters[i].active.load(std::memory_order_relaxed);
        out.wait[i].active  = active > 0 ? static_cast<std::uint64_t>(active) : 0;
        waiting             = waiting || active > 0;
    }
    if (final)
    {
        const bool retired = retiredIncomplete.exchange(false, std::memory_order_relaxed);
        if (waiting || retired || activeConsumeCalls.load(std::memory_order_relaxed) > 0)
            out.finalIncomplete = 1;
    }
    return out;
}

void MergeRingConsumeCounts(RingConsumeCounts& into, const RingConsumeCounts& from) noexcept
{
    into.fault += from.fault;
    into.publishOriginalCpuFence += from.publishOriginalCpuFence;
    into.recordingOriginal += from.recordingOriginal;
    into.watchdogFence += from.watchdogFence;
    into.watchdogDrain += from.watchdogDrain;
    into.finalIncomplete = into.finalIncomplete | from.finalIncomplete;
    for (std::size_t i = 0; i < kRingInvariants; ++i)
        into.invariant[i] += from.invariant[i];
    for (std::size_t i = 0; i < kRingWaitKinds; ++i)
    {
        into.wait[i].calls += from.wait[i].calls;
        into.wait[i].totalUs += from.wait[i].totalUs;
        into.wait[i].maxUs  = std::max(into.wait[i].maxUs, from.wait[i].maxUs);
        into.wait[i].active = from.wait[i].active;
    }
}

std::string FormatRingConsumeCounts(const RingConsumeCounts& counts)
{
    std::string fields;
    const auto  add = [&](std::string_view name, std::uint64_t value)
    {
        fields += std::format(" rc_{}={}", name, value);
    };
    if (counts.fault)
        add("fault", counts.fault);
    if (counts.publishOriginalCpuFence)
        add("publish_original_cpu_fence", counts.publishOriginalCpuFence);
    if (counts.recordingOriginal)
        add("recording_original", counts.recordingOriginal);
    for (std::size_t i = 0; i < kRingInvariants; ++i)
        if (counts.invariant[i])
            add(std::format("invariant_{}", kRingInvariantNames[i]), counts.invariant[i]);
    for (std::size_t i = 0; i < kRingWaitKinds; ++i)
    {
        const auto& wait = counts.wait[i];
        if (!wait.calls && !wait.active)
            continue;
        add(std::format("wait_{}_calls", kRingWaitNames[i]), wait.calls);
        add(std::format("wait_{}_total_us", kRingWaitNames[i]), wait.totalUs);
        add(std::format("wait_{}_max_us", kRingWaitNames[i]), wait.maxUs);
        add(std::format("wait_{}_active", kRingWaitNames[i]), wait.active);
    }
    if (counts.watchdogFence)
        add("watchdog_fence", counts.watchdogFence);
    if (counts.watchdogDrain)
        add("watchdog_drain", counts.watchdogDrain);
    if (counts.finalIncomplete)
        add("final_incomplete", counts.finalIncomplete);
    return fields;
}

std::string ReportRingConsume(const RingConsumeReportOperations& operations) noexcept
{
    try
    {
        std::lock_guard lock(consumeReportMutex);
        MergeRingConsumeCounts(pendingConsumeReport, TakeRingConsumeCounts(true));
        const auto fields = FormatRingConsumeCounts(pendingConsumeReport);
        if (operations.log)
            operations.log(fields);
        else
            REXLOG_INFO("Native ring consume: shutdown{}", fields);
        pendingConsumeReport = {};
        return fields;
    }
    catch (...)
    {
    }
    return {};
}

#endif

} // namespace rerevved::gpu::guest_d3d
