#include "video_display_mode_stub.h"

#include "ring_stubs.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <thread>

// The retained SDK export the owner calls at 0x826A6260.
extern "C" void __imp__VdSetDisplayMode(PPCContext& ctx, uint8_t* base);

REXCVAR_DEFINE_BOOL(native_video_display_mode_stub,
                    false,
                    "ReRevved",
                    "Skip only the VdSetDisplayMode import at 0x826A6260 in the display owner 0x826A61F0 and return 0 in the full r3 as the SDK export does, when "
                    "ring stub services are published, the native detached video route and the retained SDK export are present and the call has the owner's "
                    "frame and flags; the owner's display-information call and device stores still run")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_video_display_mode_exit_check,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native: force the display mode import to run and compare its result, continuation and reconvergence with the SDK "
                    "contract after every call; observes only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

namespace
{

std::atomic<std::uint64_t>                                             enterCount{ 0 }, consumedCount{ 0 }, originalCount{ 0 };
std::array<std::atomic<std::uint64_t>, kVideoDisplayModeRefusalCount>  refusalCounts{};
std::atomic<std::uint64_t>                                             exitOkCount{ 0 }, exitUnreadableCount{ 0 }, exitUnpairedCount{ 0 }, reentryCount{ 0 }, finalSkippedCount{ 0 };
std::array<std::atomic<std::uint64_t>, kVideoDisplayModeMismatchCount> mismatchCounts{};
// Started calls whose observations have not finished. A gauge: it survives a drain.
std::atomic<std::uint64_t>                     pendingCount{ 0 };
std::atomic<std::uint64_t>                     nextInvocation{ 1 };
std::atomic<std::uint32_t>                     pendingLimit{ kVideoDisplayModeMaxPending };
std::atomic<std::uint32_t>                     okLogged{ 0 };
constexpr std::uint32_t                        kOkLogLimit = 16;
std::timed_mutex                               reportMutex;
std::atomic<const VideoDisplayModeOperations*> testOperations{ nullptr };
std::atomic<void (*)()>                        reserveProbe{ nullptr };

// One call's observation state. Each guest thread has at most one live call in
// this owner; a call nested below a live frame, or one that finds a record it
// cannot pair, is inconclusive and never exit_ok.
struct Record
{
    bool                                                 active = false, poisoned = false, replaced = false, returned = false;
    bool                                                 resultValid = false, providerValid = false, identityChanged = false, infoValid = false;
    bool                                                 returnBad = false, continuationBad = false, lrBad = false, unreadable = false;
    std::uint64_t                                        invocation = 0, spFull = 0, gFull = 0, flagsFull = 0, entryLr = 0;
    std::uint64_t                                        resultFull = 0, continuationR3 = 0, reconvergedLr = 0;
    std::uint32_t                                        sp = 0, g = 0, pc = 0, width = 0, height = 0, actualWidth = 0;
    VideoDisplayModeIdentity                             identity;
    std::array<std::uint8_t, kVideoDisplayModeInfoBytes> info{};
};

thread_local Record        record;
thread_local std::uint64_t touches = 0;

// The checks and the big-endian access of a guest `lwz` over the kernel's memory.
rex::memory::Memory* kernelMemory() noexcept
{
    auto* kernel = rex::system::kernel_state();
    return kernel ? kernel->memory() : nullptr;
}

bool rangeAllows(rex::memory::Memory* memory, std::uint32_t address, std::uint32_t size, rex::memory::PageAccess needed) noexcept
{
    if (!memory || size == 0 || address < 0x10000 || address > 0xFFFFFFFFU - (size - 1))
        return false;
    const auto last    = address + size - 1;
    auto*      heap    = memory->LookupHeap(address);
    const auto granted = heap && memory->LookupHeap(last) == heap ? static_cast<unsigned>(heap->QueryRangeAccess(address, last)) : 0U;
    return (granted & static_cast<unsigned>(needed)) == static_cast<unsigned>(needed);
}

bool readKernel32(std::uint32_t address, std::uint32_t& value) noexcept
{
    auto* memory = kernelMemory();
    if (!rangeAllows(memory, address, 4, rex::memory::PageAccess::kReadOnly))
        return false;
    std::uint32_t raw = 0;
    std::memcpy(&raw, memory->TranslateVirtual<const std::uint8_t*>(address), sizeof(raw));
    value = rex::byte_swap(raw);
    return true;
}

std::uint64_t addressOf(const void* pointer) noexcept
{
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

// The concrete SDK objects the native route depends on. Each pointer is tested
// before it is used.
VideoDisplayModeEnvironment productionEnvironment()
{
    VideoDisplayModeEnvironment environment;
    auto*                       kernel  = rex::system::kernel_state();
    auto*                       runtime = kernel ? kernel->emulator() : nullptr;
    if (!runtime)
        return environment;
    environment.runtime           = true;
    environment.identity.runtime  = addressOf(runtime);
    environment.graphics          = runtime->graphics_system() != nullptr;
    const auto delegate           = runtime->detached_video_delegate();
    environment.delegate          = delegate != nullptr;
    environment.identity.delegate = addressOf(delegate.get());
    if (auto* dispatcher = runtime->function_dispatcher())
    {
        environment.dispatcher      = true;
        auto* target                = dispatcher->GetFunction(kVideoDisplayModeImportAddress);
        environment.targetPresent   = target != nullptr;
        environment.targetMatches   = target == &__imp__VdSetDisplayMode;
        environment.identity.target = addressOf(reinterpret_cast<const void*>(target));
    }
    environment.identity.ring = addressOf(CurrentRingStubServices().get());
    return environment;
}

const VideoDisplayModeOperations& productionOperations()
{
    static const VideoDisplayModeOperations operations{
        { readKernel32 },
        productionEnvironment,
        [](std::string_view message)
        {
            REXLOG_INFO("{}", message);
        },
    };
    return operations;
}

const VideoDisplayModeOperations& activeOperations()
{
    const auto* test = testOperations.load(std::memory_order_acquire);
    return test ? *test : productionOperations();
}

constexpr std::uint32_t low32(std::uint64_t value) noexcept
{
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint16_t be16(const std::array<std::uint8_t, kVideoDisplayModeInfoBytes>& bytes, std::uint32_t offset) noexcept
{
    return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

// The call-time services check: the same predicate decides the services
// refusal and the provider_valid of an observed call.
bool servicesAdmit(const VideoDisplayModeState& state) noexcept
{
    const auto& environment = state.environment;
    return state.lease && state.ready && environment.runtime && !environment.graphics && environment.delegate;
}

bool providerAdmits(const VideoDisplayModeEnvironment& environment) noexcept
{
    return environment.dispatcher && environment.targetPresent && environment.targetMatches;
}

std::string hexOf(const std::array<std::uint8_t, kVideoDisplayModeInfoBytes>& bytes)
{
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const auto byte : bytes)
        hex += std::format("{:02X}", byte);
    return hex;
}

void logRecord(const VideoDisplayModeOperations& operations, const Record& r, std::string_view verdict)
{
    if (!operations.log)
        return;
    const bool ok = verdict == "ok";
    if (ok && okLogged.fetch_add(1, std::memory_order_relaxed) >= kOkLogLimit)
        return;
    operations.log(std::format("Native video display mode check: pc={:08X} thread={} invocation={} sp={:08X} sp_full={:016X} g={:08X} g_full={:016X} flags_full={:016X} "
                               "entry_lr={:016X} replaced={} result_valid={} result_full={:016X} return_lr_valid=0 continuation_r3={:016X} reconverged_lr={:016X} "
                               "runtime_id={:016X} service_id={:016X} ring_service_id={:016X} provider_target={:016X} provider_valid={} display_info_valid={} "
                               "display_info_be_hex={} display_width={} display_height={} actual_display_width={} return={} continuation={} lr={} provider={} "
                               "unreadable={} verdict={}",
                               r.pc,
                               std::hash<std::thread::id>{}(std::this_thread::get_id()),
                               r.invocation,
                               r.sp,
                               r.spFull,
                               r.g,
                               r.gFull,
                               r.flagsFull,
                               r.entryLr,
                               r.replaced ? 1 : 0,
                               r.resultValid ? 1 : 0,
                               r.resultFull,
                               r.continuationR3,
                               r.reconvergedLr,
                               r.identity.runtime,
                               r.identity.delegate,
                               r.identity.ring,
                               r.identity.target,
                               r.providerValid && !r.identityChanged ? 1 : 0,
                               r.infoValid ? 1 : 0,
                               r.infoValid ? hexOf(r.info) : std::string("-"),
                               r.width,
                               r.height,
                               r.actualWidth,
                               r.returnBad ? "bad" : "ok",
                               r.continuationBad ? "bad" : "ok",
                               r.lrBad ? "bad" : "ok",
                               r.providerValid && !r.identityChanged ? "ok" : "bad",
                               r.unreadable ? 1 : 0,
                               verdict));
}

// Ends the thread's record: inconclusive when `conclusive` is false, the
// record was poisoned, the provider was not valid at the entry or an identity
// changed; never exit_ok then.
void finishRecord(const VideoDisplayModeOperations& operations, bool conclusive) noexcept
{
    try
    {
        auto finished = record;
        record        = {};
        pendingCount.fetch_sub(1, std::memory_order_relaxed);
        if (!conclusive || finished.poisoned || finished.identityChanged || !finished.providerValid)
        {
            exitUnpairedCount.fetch_add(1, std::memory_order_relaxed);
            logRecord(operations, finished, "unpaired");
            return;
        }
        const bool bad[kVideoDisplayModeMismatchCount]{ finished.returnBad, finished.continuationBad, finished.lrBad };
        bool       any = false;
        for (std::size_t i = 0; i < kVideoDisplayModeMismatchCount; ++i)
            if (bad[i])
            {
                mismatchCounts[i].fetch_add(1, std::memory_order_relaxed);
                any = true;
            }
        const bool unreadable = finished.unreadable || !finished.infoValid;
        if (unreadable)
            exitUnreadableCount.fetch_add(1, std::memory_order_relaxed);
        else if (!any)
            exitOkCount.fetch_add(1, std::memory_order_relaxed);
        logRecord(operations, finished, unreadable ? "unreadable" : any ? "mismatch"
                                                                        : "ok");
    }
    catch (...)
    {
        record = {};
    }
}

// Closes a stale record of this thread, or refuses a call nested below a live
// one. Runs before the lease so that no log is written under it.
std::optional<VideoDisplayModeRefusal> prepareRecord(const VideoDisplayModeOperations& operations, std::uint32_t sp) noexcept
{
    try
    {
        ++touches;
        if (record.active)
        {
            if (sp < record.sp)
            {
                // Below the live frame: nested, so neither call can be paired.
                record.poisoned = true;
                reentryCount.fetch_add(1, std::memory_order_relaxed);
                return VideoDisplayModeRefusal::Reentrant;
            }
            // The earlier frame is gone; its observations can no longer arrive.
            finishRecord(operations, false);
        }
        return std::nullopt;
    }
    catch (...)
    {
        record = {};
        return VideoDisplayModeRefusal::Unpaired;
    }
}

// The thread's observation record for this call, or why there is none. Neither
// allocates nor logs.
std::optional<VideoDisplayModeRefusal> armRecord(const VideoDisplayModeCall& call, const VideoDisplayModeIdentity& identity, bool providerValid) noexcept
{
    // Reserve the slot first: a separate check and increment would let
    // concurrent entries pass the bound together.
    auto pending = pendingCount.load(std::memory_order_relaxed);
    do
    {
        if (pending >= pendingLimit.load(std::memory_order_relaxed))
            return VideoDisplayModeRefusal::Unpaired;
        if (auto* probe = reserveProbe.load(std::memory_order_relaxed))
            probe();
    } while (!pendingCount.compare_exchange_weak(pending, pending + 1, std::memory_order_relaxed));
    record               = {};
    record.active        = true;
    record.invocation    = nextInvocation.fetch_add(1, std::memory_order_relaxed);
    record.sp            = low32(call.r1);
    record.g             = low32(call.r31);
    record.spFull        = call.r1;
    record.gFull         = call.r31;
    record.flagsFull     = call.r3;
    record.entryLr       = call.lr;
    record.identity      = identity;
    record.providerValid = providerValid;
    return std::nullopt;
}

bool matches(std::uint64_t r1, std::uint64_t r31) noexcept
{
    return record.active && record.spFull == r1 && record.gFull == r31;
}

} // namespace

bool VideoDisplayModeStubRequested() noexcept
{
    return RingStubsPublished() && REXCVAR_GET(native_video_display_mode_stub);
}

bool VideoDisplayModeExitCheckRequested() noexcept
{
    return REXCVAR_GET(native_video_display_mode_exit_check);
}

bool VideoDisplayModeActive() noexcept
{
    return VideoDisplayModeExitCheckRequested() || VideoDisplayModeStubRequested();
}

bool VideoDisplayModeStubReportingRequested() noexcept
{
    return REXCVAR_GET(native_video_display_mode_stub) || REXCVAR_GET(native_video_display_mode_exit_check);
}

std::optional<VideoDisplayModeRefusal> DeriveVideoDisplayModeStub(const VideoDisplayModeMemory& memory, const VideoDisplayModeCall& call, const VideoDisplayModeState& state, VideoDisplayModePlan& plan)
{
    using R = VideoDisplayModeRefusal;
    if (!servicesAdmit(state))
        return R::Services;
    if (!providerAdmits(state.environment))
        return R::Provider;
    if (state.diagnostics)
        return R::Diagnostics;

    // The owner's frame: a 16-byte aligned SP in the user range whose backchain
    // word is the caller's SP, SP + 192, written by the owner's stwu.
    if ((call.r1 >> 32) != 0)
        return R::Stack;
    const std::uint32_t sp = low32(call.r1), device = low32(call.r31);
    if (sp < 0x10000 || (sp & 15U) != 0 || sp > 0xFFFFFFFFU - kVideoDisplayModeFrameBytes)
        return R::Stack;
    std::uint32_t backchain = 0;
    if (!memory.read32(sp, backchain) || backchain != sp + kVideoDisplayModeFrameBytes)
        return R::Stack;

    // The raw 64-bit flags: bit 30 and no bit outside the sixteen optional combinations.
    if ((call.r3 >> 32) != 0 || (low32(call.r3) & ~kVideoDisplayModeOptionalFlags) != kVideoDisplayModeRequiredFlag)
        return R::Shape;

    if (state.record)
        return *state.record;

    plan          = {};
    plan.r1       = call.r1;
    plan.r3       = call.r3;
    plan.r31      = call.r31;
    plan.lr       = call.lr;
    plan.sp       = sp;
    plan.device   = device;
    plan.identity = state.environment.identity;
    return std::nullopt;
}

void ApplyVideoDisplayModeStub(const VideoDisplayModePlan& plan, std::uint64_t& r3) noexcept
{
    static_cast<void>(plan);
    r3 = 0;
}

bool ReplaceVideoDisplayModeCall(const VideoDisplayModeCall& call, std::uint64_t& r3) noexcept
{
    if (!VideoDisplayModeActive()) [[likely]]
        return false;
    return ReplaceVideoDisplayModeCallWith(activeOperations(), call, r3);
}

bool ReplaceVideoDisplayModeCallWith(const VideoDisplayModeOperations& operations, const VideoDisplayModeCall& call, std::uint64_t& r3) noexcept
{
    // Held from the identity reads through the full r3 assignment; a withdrawal
    // waits for it. It ends when this function returns, before the generated
    // code runs Original.
    std::optional<RingStubLease> lease;
    VideoDisplayModePlan         plan;
    bool                         entered = false, consume = false;
    try
    {
        const bool stub = VideoDisplayModeStubRequested(), check = VideoDisplayModeExitCheckRequested();
        if (!stub && !check)
            return false;
        enterCount.fetch_add(1, std::memory_order_relaxed);
        entered                = true;
        const std::uint32_t sp = low32(call.r1), device = low32(call.r31);
        const auto          prior = prepareRecord(operations, sp);
        lease.emplace();
        VideoDisplayModeState state;
        state.lease       = static_cast<bool>(*lease);
        state.ready       = state.lease && RingStubsReady(device);
        state.diagnostics = check;
        if (state.lease)
            state.environment = operations.environment();
        const bool providerValid = servicesAdmit(state) && providerAdmits(state.environment);
        state.record             = prior ? prior : armRecord(call, state.environment.identity, providerValid);
        if (stub)
        {
            const auto refusal = DeriveVideoDisplayModeStub(operations.memory, call, state, plan);
            if (!refusal)
                consume = true;
            else
                refusalCounts[static_cast<std::size_t>(*refusal)].fetch_add(1, std::memory_order_relaxed);
        }
        if (!consume)
            originalCount.fetch_add(1, std::memory_order_relaxed);
    }
    catch (...)
    {
        // Nothing has been written yet; the import runs.
        if (entered && !consume)
            originalCount.fetch_add(1, std::memory_order_relaxed);
        consume = false;
    }
    if (!consume)
        return false;
    // The commit: nothing below can throw, so a consume is never undone.
    if (operations.beforeCommit)
        operations.beforeCommit();
    ApplyVideoDisplayModeStub(plan, r3);
    if (record.active)
        record.replaced = true;
    consumedCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ObserveVideoDisplayModeReturn(std::uint64_t r1, std::uint64_t r3, std::uint32_t r5, std::uint64_t r31) noexcept
{
    if (!VideoDisplayModeStubReportingRequested()) [[likely]]
        return;
    try
    {
        if (!VideoDisplayModeActive() && !record.active)
            return;
        // Zero is the owner's own bypass of the import, not a missing call.
        if (r5 == 0)
            return;
        ++touches;
        if (!matches(r1, r31) || record.returned)
        {
            exitUnpairedCount.fetch_add(1, std::memory_order_relaxed);
            // A second return for the live call cannot be paired either.
            if (matches(r1, r31))
                record.poisoned = true;
            return;
        }
        auto& r       = record;
        r.pc          = kVideoDisplayModeReturnAddress;
        r.resultFull  = r3;
        r.resultValid = true;
        r.returnBad   = r3 != 0;
        r.returned    = true;
    }
    catch (...)
    {
        if (record.active)
            record.poisoned = true;
    }
}

void ObserveVideoDisplayModeReconverged(std::uint64_t r1, std::uint64_t r3, std::uint32_t r5, std::uint64_t r31, std::uint64_t lr) noexcept
{
    if (!VideoDisplayModeStubReportingRequested()) [[likely]]
        return;
    try
    {
        if (!VideoDisplayModeActive() && !record.active)
            return;
        if (r5 == 0)
            return;
        ++touches;
        const auto& operations = activeOperations();
        if (!matches(r1, r31))
        {
            exitUnpairedCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!record.returned)
        {
            // The return was not observed: the pair is incomplete.
            record.pc = kVideoDisplayModeReconvergedAddress;
            finishRecord(operations, false);
            return;
        }
        auto& r           = record;
        r.pc              = kVideoDisplayModeReconvergedAddress;
        r.continuationR3  = r3;
        r.reconvergedLr   = lr;
        r.continuationBad = r3 != r.spFull + kVideoDisplayModeInfoOffset;
        r.lrBad           = lr != kVideoDisplayModeReconvergedAddress;
        // Any bound identity that moved since the entry makes the pair unusable.
        // A call that was already unusable at the entry needs no second probe.
        if (r.providerValid)
        {
            const auto environment = operations.environment ? operations.environment() : VideoDisplayModeEnvironment{};
            r.identityChanged      = !(environment.identity == r.identity);
        }
        // The whole 0x58-byte information structure, through checked reads.
        bool readable = true;
        for (std::uint32_t i = 0; readable && i < kVideoDisplayModeInfoBytes / 4; ++i)
        {
            std::uint32_t word = 0;
            if (!operations.memory.read32(r.sp + kVideoDisplayModeInfoOffset + 4 * i, word))
            {
                readable = false;
                break;
            }
            for (std::uint32_t b = 0; b < 4; ++b)
                r.info[4 * i + b] = static_cast<std::uint8_t>(word >> (24 - 8 * b));
        }
        r.infoValid  = readable;
        r.unreadable = !readable;
        if (readable)
        {
            r.width       = be16(r.info, kVideoDisplayModeWidthOffset);
            r.height      = be16(r.info, kVideoDisplayModeHeightOffset);
            r.actualWidth = be16(r.info, kVideoDisplayModeActualWidthOffset);
        }
        finishRecord(operations, true);
    }
    catch (...)
    {
        if (record.active)
            record.poisoned = true;
    }
}

VideoDisplayModeStubCounts TakeVideoDisplayModeStubCounts() noexcept
{
    VideoDisplayModeStubCounts counts;
    counts.enter    = enterCount.exchange(0, std::memory_order_relaxed);
    counts.consumed = consumedCount.exchange(0, std::memory_order_relaxed);
    counts.original = originalCount.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kVideoDisplayModeRefusalCount; ++i)
        counts.refusal[i] = refusalCounts[i].exchange(0, std::memory_order_relaxed);
    counts.exitOk = exitOkCount.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kVideoDisplayModeMismatchCount; ++i)
        counts.mismatch[i] = mismatchCounts[i].exchange(0, std::memory_order_relaxed);
    counts.exitUnreadable = exitUnreadableCount.exchange(0, std::memory_order_relaxed);
    counts.exitUnpaired   = exitUnpairedCount.exchange(0, std::memory_order_relaxed);
    counts.reentry        = reentryCount.exchange(0, std::memory_order_relaxed);
    counts.finalSkipped   = finalSkippedCount.exchange(0, std::memory_order_relaxed);
    counts.pending        = pendingCount.load(std::memory_order_relaxed);
    return counts;
}

std::string FormatVideoDisplayModeStubCounts(const VideoDisplayModeStubCounts& counts)
{
    auto fields = std::format(" video_display_mode_enter={} video_display_mode_consumed={} video_display_mode_original={}", counts.enter, counts.consumed, counts.original);
    for (std::size_t i = 0; i < kVideoDisplayModeRefusalCount; ++i)
        fields += std::format(" video_display_mode_refusal_{}={}", kVideoDisplayModeRefusalNames[i], counts.refusal[i]);
    fields += std::format(" video_display_mode_exit_ok={}", counts.exitOk);
    for (std::size_t i = 0; i < kVideoDisplayModeMismatchCount; ++i)
        fields += std::format(" video_display_mode_exit_mismatch_{}={}", kVideoDisplayModeMismatchNames[i], counts.mismatch[i]);
    fields += std::format(" video_display_mode_exit_unreadable={} video_display_mode_exit_unpaired={} video_display_mode_reentry={} video_display_mode_pending={}",
                          counts.exitUnreadable,
                          counts.exitUnpaired,
                          counts.reentry,
                          counts.pending);
    if (counts.final)
        fields += std::format(" video_display_mode_final_incomplete={} video_display_mode_final_skipped={}", counts.pending != 0 || counts.finalSkipped != 0 ? 1 : 0, counts.finalSkipped);
    return fields;
}

namespace detail
{

std::timed_mutex& VideoDisplayModeReportMutex() noexcept
{
    return reportMutex;
}

} // namespace detail

std::string ReportVideoDisplayModeStub(bool shutdown, const VideoDisplayModeReportOperations& operations) noexcept
{
    if (!VideoDisplayModeStubReportingRequested())
        return {};
    const auto log = [&](std::string_view fields)
    {
        try
        {
            if (operations.log)
                operations.log(fields);
            else
                REXLOG_INFO("Native video display mode: shutdown{}", fields);
        }
        catch (...)
        {
        }
    };
    try
    {
        std::unique_lock lock(reportMutex, std::defer_lock);
        if (shutdown)
        {
            // A drain on another thread may still hold the lock; the final waits only briefly.
            if (!lock.try_lock_for(kVideoDisplayModeShutdownWait))
            {
                finalSkippedCount.fetch_add(1, std::memory_order_relaxed);
                auto fields = std::format(" video_display_mode_pending={} video_display_mode_final_incomplete=1 video_display_mode_final_skipped=1", pendingCount.load(std::memory_order_relaxed));
                log(fields);
                return fields;
            }
        }
        else
            lock.lock();
        auto counts  = TakeVideoDisplayModeStubCounts();
        counts.final = shutdown;
        auto fields  = operations.format(counts);
        if (shutdown)
            log(fields);
        return fields;
    }
    catch (...)
    {
    }
    return {};
}

void SetVideoDisplayModeOperationsForTesting(const VideoDisplayModeOperations* operations) noexcept
{
    testOperations.store(operations, std::memory_order_release);
}

std::uint64_t VideoDisplayModeThreadTouchesForTesting() noexcept
{
    return touches;
}

void SetVideoDisplayModePendingLimitForTesting(std::uint32_t limit) noexcept
{
    pendingLimit.store(limit, std::memory_order_relaxed);
}

void SetVideoDisplayModeReserveProbeForTesting(void (*probe)()) noexcept
{
    reserveProbe.store(probe, std::memory_order_relaxed);
}

void ResetVideoDisplayModeLogBudgetForTesting() noexcept
{
    okLogged.store(0, std::memory_order_relaxed);
}

VideoDisplayModeEnvironment ProductionVideoDisplayModeEnvironmentForTesting()
{
    return productionEnvironment();
}

bool ProductionVideoDisplayModeRead32ForTesting(std::uint32_t address, std::uint32_t& value) noexcept
{
    return readKernel32(address, value);
}

} // namespace rerevved::gpu::guest_d3d
