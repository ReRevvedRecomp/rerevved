#include "gpu/guest_gpu_service.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <rex/chrono/clock.h>
#include <rex/crypto/sha256.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "gpu/completion/guest_progress_hooks.h"
#include "gpu/guest_d3d/ring_stubs.h"

namespace rerevved::gpu
{

using rex::X_STATUS;

namespace
{

constexpr uint32_t kGpuMmioBaseAddress = 0x7FC80000;
constexpr uint32_t kGpuMmioAddressMask = 0xFFFF0000;
constexpr uint32_t kGpuMmioSize        = 0x0000FFFF;

constexpr uint32_t kRbEdramTiming                     = 0x0F00;
constexpr uint32_t kRbBcControl                       = 0x0F01;
constexpr uint32_t kR500D1ModeVCounter                = 0x194C;
constexpr uint32_t kInterruptStatus                   = 0x1951;
constexpr uint32_t kAvivoD1ModeViewportSize           = 0x1961;
constexpr uint32_t kCpRbWptr                          = 0x01C5;
constexpr uint32_t kAvivoD1GraphPrimarySurfaceAddress = 0x1844;

constexpr uint32_t kDefaultCpu          = 2;
constexpr uint64_t kLoggedSwaps         = 16;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kPhysicalMemorySize  = kPhysicalAddressMask + 1;

bool isPhysicalRangeSafe(uint32_t ptr, uint32_t length)
{
    if (ptr == 0 || (ptr & 0x3) != 0)
    {
        return false;
    }
    const uint32_t physicalPtr = ptr & kPhysicalAddressMask;
    return length <= kPhysicalMemorySize - physicalPtr;
}

// Census mode repeats its summary at this swap interval and at shutdown.
constexpr uint64_t kCensusSummarySwaps = 600;

void logCensus(const completion::CompletionLedger& ledger, uint64_t swaps, std::string_view when)
{
    const auto forms = ledger.Census();
    REXGPU_INFO("census-summary: {} at swap {}: {} distinct forms", when, swaps, forms.size());
    for (const auto& form : forms)
    {
        REXGPU_INFO("census-summary: count={} {} ({} at {:08X})",
                    form.count,
                    form.failure.detail,
                    completion::ProgressKindName(form.failure.kind),
                    form.failure.address);
    }
}

using State = GuestVideoServices::State;

struct MmioBridge
{
    std::mutex              mutex;
    std::condition_variable idle;
    State*                  service           = nullptr;
    uint32_t                callbacksInFlight = 0;
};

MmioBridge& getMmioBridge()
{
    // The SDK permits only one process-wide MMIO handler. A process-lifetime
    // bridge remains valid after that handler has copied its callback context.
    static MmioBridge bridge;
    return bridge;
}

bool bindMmioBridge(State* service)
{
    auto&           bridge = getMmioBridge();
    std::lock_guard lock(bridge.mutex);
    if (bridge.service || bridge.callbacksInFlight != 0)
    {
        return false;
    }
    bridge.service = service;
    return true;
}

void unbindMmioBridge(State* service)
{
    auto&            bridge = getMmioBridge();
    std::unique_lock lock(bridge.mutex);
    if (bridge.service != service)
    {
        return;
    }
    bridge.service = nullptr;
    bridge.idle.wait(lock, [&bridge]
                     {
                         return bridge.callbacksInFlight == 0;
                     });
}

class MmioCallbackLease
{
public:
    explicit MmioCallbackLease(void* callbackContext)
    : bridge(static_cast<MmioBridge*>(callbackContext))
    {
        if (!bridge)
        {
            return;
        }
        std::lock_guard lock(bridge->mutex);
        boundService = bridge->service;
        if (boundService)
        {
            ++bridge->callbacksInFlight;
        }
    }

    ~MmioCallbackLease()
    {
        if (!boundService)
        {
            return;
        }
        std::lock_guard lock(bridge->mutex);
        --bridge->callbacksInFlight;
        if (bridge->callbacksInFlight == 0)
        {
            bridge->idle.notify_all();
        }
    }

    State* service() const
    {
        return boundService;
    }

private:
    MmioBridge* bridge       = nullptr;
    State*      boundService = nullptr;
};

} // namespace

VblankStep AdvanceVblank(uint64_t now, uint64_t lastFrameTime, uint64_t intervalTicks)
{
    VblankStep step{ 0, lastFrameTime };
    if (intervalTicks == 0)
    {
        return step;
    }
    while (now - step.lastFrameTime >= intervalTicks && step.due < kMaxVblankCatchUp)
    {
        step.lastFrameTime += intervalTicks;
        ++step.due;
    }
    if (step.due == kMaxVblankCatchUp && now - step.lastFrameTime >= intervalTicks)
    {
        step.lastFrameTime = now;
    }
    return step;
}

// Ledger effects as aligned big-endian words in committed physical pages. The
// memory pointer is atomic so the ledger never takes the service mutex.
class ProgressSink final : public completion::IGuestProgressSink
{
public:
    explicit ProgressSink(GuestVideoServices::State& owner)
    : owner(owner)
    {
    }

    bool                        WritePhysical(uint32_t address, uint32_t value) override;
    bool                        WriteGuestImage(uint32_t address, std::span<const std::uint8_t> bytes) override;
    std::optional<uint32_t>     ReadPhysical(uint32_t address) override;
    completion::InterruptResult RaiseCallbackInterrupt(uint32_t cpu) override;

    std::atomic<rex::memory::Memory*> memory{ nullptr };

private:
    uint8_t* word(uint32_t address) const;

    GuestVideoServices::State& owner;
};

struct GuestVideoServices::State
{
    mutable std::mutex mutex;

    rex::runtime::FunctionDispatcher* functionDispatcher = nullptr;
    rex::system::KernelState*         kernelState        = nullptr;
    rex::memory::Memory*              memory             = nullptr;

    // Set by Start before any thread or hook can reach it.
    std::shared_ptr<completion::CompletionLedger>         ledger;
    ProgressSink                                          progressSink{ *this };
    std::mutex                                            failureMutex;
    std::function<void(const completion::LedgerFailure&)> reportFailure;
    bool                                                  progressPublished = false;
    // Guarded by mutex; cleared when producers stop.
    std::function<std::optional<std::string>(const rex::system::DetachedVideoSwap&)> assembleSwap;
    std::function<std::optional<DrawFailure>()>                                      pollDrawFailure;

    rex::system::object_ref<rex::system::XHostThread> completionThread;
    std::atomic<bool>                                 completionRunning{ false };
    bool                                              completionExited = true;
    std::condition_variable                           completionExit;

    // One source-1 request at a time, served on the vblank thread.
    bool                        interruptPending   = false;
    uint32_t                    interruptCpu       = 0;
    uint64_t                    interruptRequest   = 0;
    uint64_t                    interruptServed    = 0;
    completion::InterruptResult interruptResult    = completion::InterruptResult::Stopped;
    uint64_t                    callbackInterrupts = 0;
    std::condition_variable     interruptDone;

    uint32_t ringPointer         = 0;
    uint32_t ringSizeLog2        = 0;
    uint32_t writebackPointer    = 0;
    uint32_t writebackBlockLog2  = 0;
    uint32_t ringGeneration      = 0;
    uint32_t writebackGeneration = 0;
    bool     configurationFrozen = false;

    uint32_t interruptCallback = 0;
    uint32_t interruptUserData = 0;

    std::array<uint32_t, 0x10000 / 4> registerValues{};
    uint32_t                          lastWritePointer         = 0;
    uint64_t                          writePointerPublications = 0;
    // Publications before the current writeback generation started.
    uint64_t writebackPublicationBase = 0;

    std::atomic<uint64_t> vblankCount{ 0 };

    std::deque<SwapEpoch> swapEpochs;
    uint64_t              swapCount          = 0;
    uint64_t              swapSizeMismatches = 0;

    CompletionDebtMonitor                  debtMonitor{ std::chrono::seconds(2) };
    CompletionDebtMonitor::Clock::duration debtThreshold = std::chrono::seconds(2);
    guest_d3d::NativeDrawMode              drawMode      = guest_d3d::NativeDrawMode::Strict;
    std::atomic<uint64_t>                  censusSummarySwap{ 0 };
    // A ledger span waiting for a writeback that was never registered has no
    // WPTR/RPTR pair for debtMonitor; it is timed here, once per registration.
    std::optional<CompletionDebtMonitor::Clock::time_point> registrationWaitSince;
    bool                                                    registrationDebtReported = false;
    std::function<void(const CompletionDebt&)>              reportDebt;

    rex::system::object_ref<rex::system::XHostThread> vblankWorkerThread;
    std::atomic<bool>                                 vblankWorkerRunning{ false };
    bool                                              vblankWorkerExited = true;
    std::condition_variable                           vblankWorkerExit;

    bool started         = false;
    bool active          = false;
    bool mmioRegistered  = false;
    bool shutdownStarted = false;

    bool unknownMmioLogged   = false;
    bool firstWriteLogged    = false;
    bool missingThreadLogged = false;
    bool partialSwapLogged   = false;

    uint32_t readRegister(uint32_t addr);
    void     writeRegister(uint32_t addr, uint32_t value);
    int      runVblankWorker();
    int      runCompletionWorker();
    void     markVblank();
    void     serviceCallbackInterrupt();
    bool     executeInterrupt(uint64_t source, uint32_t cpu, uint32_t callback, uint32_t userData, rex::runtime::FunctionDispatcher* dispatch);
    void     checkCompletionDebt();
    void     checkRegistrationDebt();
#if REREVVED_DEVELOPER
    void checkRingConsumeWaits();
#endif
};

uint8_t* ProgressSink::word(uint32_t address) const
{
    auto* target = memory.load(std::memory_order_acquire);
    if (!target || (address & 0x3) != 0 || address > kPhysicalAddressMask - 3)
    {
        return nullptr;
    }
    // Released or never-committed pages are not guest progress targets.
    if (target->GetPhysicalHeap()->QueryRangeAccess(address, address + 3) == rex::memory::PageAccess::kNoAccess)
    {
        return nullptr;
    }
    return target->TranslatePhysical<uint8_t*>(address);
}

bool ProgressSink::WriteGuestImage(uint32_t address, std::span<const std::uint8_t> bytes)
{
    auto* target = memory.load(std::memory_order_acquire);
    if (!target || bytes.empty() || address > kPhysicalAddressMask || bytes.size() - 1 > kPhysicalAddressMask - address)
        return false;
    const auto last   = address + static_cast<uint32_t>(bytes.size() - 1);
    const auto access = target->GetPhysicalHeap()->QueryRangeAccess(address, last);
    if ((static_cast<unsigned>(access) & static_cast<unsigned>(rex::memory::PageAccess::kReadWrite)) !=
        static_cast<unsigned>(rex::memory::PageAccess::kReadWrite))
        return false;
    std::memcpy(target->TranslatePhysical<std::uint8_t*>(address), bytes.data(), bytes.size());
    std::atomic_thread_fence(std::memory_order_release);
    static std::atomic<unsigned> logged{ 0 };
    if (logged.fetch_add(1) < 4)
        REXGPU_INFO("Native resolve written: physical={:08X} bytes={} sha256={}", address, bytes.size(), rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
    return true;
}

bool ProgressSink::WritePhysical(uint32_t address, uint32_t value)
{
    auto* destination = word(address);
    if (!destination)
    {
        return false;
    }
    rex::memory::store_and_swap<uint32_t>(destination, value);
    // Each record's stores are visible before the next record's.
    std::atomic_thread_fence(std::memory_order_release);
    return true;
}

std::optional<uint32_t> ProgressSink::ReadPhysical(uint32_t address)
{
    const auto* source = word(address);
    if (!source)
    {
        return std::nullopt;
    }
    const uint32_t value = rex::memory::load_and_swap<uint32_t>(source);
    std::atomic_thread_fence(std::memory_order_acquire);
    return value;
}

completion::InterruptResult ProgressSink::RaiseCallbackInterrupt(uint32_t cpu)
{
    std::unique_lock lock(owner.mutex);
    if (!owner.completionRunning.load(std::memory_order_acquire) || !owner.active)
    {
        return completion::InterruptResult::Stopped;
    }
    if (!owner.interruptCallback)
    {
        return completion::InterruptResult::NoHandler;
    }
    const uint64_t request = ++owner.interruptRequest;
    owner.interruptPending = true;
    owner.interruptCpu     = cpu;
    owner.interruptDone.wait(lock, [&]
                             {
                                 return owner.interruptServed >= request ||
                                        !owner.completionRunning.load(std::memory_order_acquire);
                             });
    return owner.interruptServed >= request ? owner.interruptResult : completion::InterruptResult::Stopped;
}

namespace
{

// What the ring stubs reach: the CP_RB_WPTR store 0x8269B900 would make,
// applied as the MMIO write applies it.
class RingStubServices final : public guest_d3d::IRingStubServices
{
public:
    explicit RingStubServices(std::weak_ptr<GuestVideoServices::State> state)
    : state(std::move(state))
    {
    }

    void Kick(uint32_t writePointer) override
    {
        if (const auto owner = state.lock())
            owner->writeRegister(kGpuMmioBaseAddress + kCpRbWptr * 4, writePointer);
    }

private:
    std::weak_ptr<GuestVideoServices::State> state;
};

uint32_t readRegisterThunk(void*, void* callbackContext, uint32_t addr)
{
    MmioCallbackLease lease(callbackContext);
    if (!lease.service())
    {
        return 0;
    }
    return lease.service()->readRegister(addr);
}

void writeRegisterThunk(void*, void* callbackContext, uint32_t addr, uint32_t value)
{
    MmioCallbackLease lease(callbackContext);
    if (lease.service())
    {
        lease.service()->writeRegister(addr, value);
    }
}

} // namespace

GuestVideoServices::GuestVideoServices()
: state(std::make_shared<State>())
{
}

GuestVideoServices::~GuestVideoServices()
{
    Shutdown();
}

namespace
{

using ThreadBody = int (State::*)();

struct HostThreadSlot
{
    rex::system::object_ref<rex::system::XHostThread> State::* thread;
    bool State::*           exited;
    std::condition_variable State::* exit;
};

// The worker owns a state reference, so a thread that misses its join
// deadline never outlives the fields it reads. Caller holds the mutex.
X_STATUS startHostThread(const std::shared_ptr<State>& state, rex::system::KernelState* kernelState, HostThreadSlot slot, ThreadBody body, const char* name)
{
    State& owner       = *state;
    owner.*slot.exited = false;
    try
    {
        owner.*slot.thread = rex::system::object_ref<rex::system::XHostThread>(
            new rex::system::XHostThread(kernelState, 128 * 1024, 0, [shared = state, slot, body]()
                                         {
                                             State&    self   = *shared;
                                             const int result = (self.*body)();
                                             {
                                                 // The owner keeps the handle: XThread::Exit still runs after
                                                 // this body returns, and the join waits for it.
                                                 std::lock_guard exitLock(self.mutex);
                                                 self.*slot.exited = true;
                                             }
                                             (self.*slot.exit).notify_all();
                                             return result;
                                         }));
        (owner.*slot.thread)->set_name(name);
    }
    catch (...)
    {
        owner.*slot.exited = true;
        (owner.*slot.thread).reset();
        return X_STATUS_UNSUCCESSFUL;
    }
    const X_STATUS status = (owner.*slot.thread)->Create();
    if (XFAILED(status))
    {
        owner.*slot.exited = true;
        (owner.*slot.thread).reset();
    }
    return status;
}

constexpr HostThreadSlot kCompletionThread{ &State::completionThread, &State::completionExited, &State::completionExit };
constexpr HostThreadSlot kVblankThread{ &State::vblankWorkerThread, &State::vblankWorkerExited, &State::vblankWorkerExit };

} // namespace

X_STATUS GuestVideoServices::Start(rex::Runtime& runtime, Options options)
{
    auto* functionDispatcher = runtime.function_dispatcher();
    auto* kernelState        = runtime.kernel_state();
    auto* memory             = runtime.memory();
    if (!functionDispatcher || !kernelState || !memory)
    {
        return X_STATUS_INVALID_PARAMETER;
    }

    std::unique_lock lock(state->mutex);
    if (state->started || state->shutdownStarted)
    {
        return X_STATUS_INVALID_PARAMETER;
    }
    state->started            = true;
    state->functionDispatcher = functionDispatcher;
    state->kernelState        = kernelState;
    state->memory             = memory;
    state->debtMonitor        = CompletionDebtMonitor(options.debtThreshold);
    state->debtThreshold      = options.debtThreshold;
    state->drawMode           = options.drawMode;
    state->reportDebt         = std::move(options.reportDebt);
    state->assembleSwap       = std::move(options.assembleSwap);
    state->pollDrawFailure    = std::move(options.pollDrawFailure);
    {
        std::lock_guard failureLock(state->failureMutex);
        state->reportFailure = std::move(options.reportFailure);
    }
    state->progressSink.memory.store(memory, std::memory_order_release);

    completion::LedgerConfig ledger;
    ledger.mode   = options.drawMode;
    ledger.sink   = &state->progressSink;
    ledger.draws  = options.draws;
    ledger.failed = [weak = std::weak_ptr<State>(state)](const completion::LedgerFailure& failure)
    {
        REXGPU_ERROR("GuestVideoServices: GPU completion failed: {} ({} at {:08X}); no new records are taken, closed spans before the failing one still release when fully owned, the failing span and every later one never release",
                     failure.detail,
                     completion::ProgressKindName(failure.kind),
                     failure.address);
        const auto shared = weak.lock();
        if (!shared)
        {
            return;
        }
        std::function<void(const completion::LedgerFailure&)> report;
        {
            std::lock_guard failureLock(shared->failureMutex);
            report = shared->reportFailure;
        }
        if (report)
        {
            report(failure);
        }
    };
    ledger.census = [](const completion::LedgerFailure& failure)
    {
        REXGPU_ERROR("census: GuestVideoServices: GPU completion failed: {} ({} at {:08X}); census mode skips it and continues",
                     failure.detail,
                     completion::ProgressKindName(failure.kind),
                     failure.address);
    };
    state->ledger = std::make_shared<completion::CompletionLedger>(std::move(ledger));
    // Registrations that arrived before Start open the first epoch.
    if (state->ringGeneration)
    {
        state->ledger->BeginRingEpoch();
    }
    if (state->writebackGeneration)
    {
        state->ledger->RegisterWriteback(state->writebackPointer & kPhysicalAddressMask);
    }

    const auto clearDependencies = [this]
    {
        state->functionDispatcher = nullptr;
        state->kernelState        = nullptr;
        state->memory             = nullptr;
        state->reportDebt         = {};
        state->assembleSwap       = {};
        state->progressSink.memory.store(nullptr, std::memory_order_release);
        std::lock_guard failureLock(state->failureMutex);
        state->reportFailure = {};
    };
    const auto stopStarted = [&]
    {
        lock.unlock();
        StopProducers(std::chrono::seconds(1));
        lock.lock();
        clearDependencies();
    };

    state->completionRunning.store(true, std::memory_order_release);
    X_STATUS status = startHostThread(state, kernelState, kCompletionThread, &State::runCompletionWorker, "GPU Completion");
    if (XFAILED(status))
    {
        state->completionRunning.store(false, std::memory_order_release);
        clearDependencies();
        return status;
    }
    state->vblankWorkerRunning.store(true, std::memory_order_release);
    status = startHostThread(state, kernelState, kVblankThread, &State::runVblankWorker, "GPU VSync");
    if (XFAILED(status))
    {
        state->vblankWorkerRunning.store(false, std::memory_order_release);
        stopStarted();
        return status;
    }

    if (!bindMmioBridge(state.get()))
    {
        stopStarted();
        return X_STATUS_UNSUCCESSFUL;
    }

    if (!memory->AddVirtualMappedRange(
            kGpuMmioBaseAddress,
            kGpuMmioAddressMask,
            kGpuMmioSize,
            &getMmioBridge(),
            reinterpret_cast<rex::runtime::MMIOReadCallback>(readRegisterThunk),
            reinterpret_cast<rex::runtime::MMIOWriteCallback>(writeRegisterThunk)))
    {
        lock.unlock();
        unbindMmioBridge(state.get());
        lock.lock();
        stopStarted();
        return X_STATUS_UNSUCCESSFUL;
    }

    state->mmioRegistered    = true;
    state->active            = true;
    state->progressPublished = true;
    completion::PublishGuestProgress(state->ledger);
    // native_ring_stubs: the guest writes no ring word or PM4 packet; the
    // ledger, published as always, completes what the stubs record.
    const bool ringStubs = guest_d3d::RingStubsRequested();
    if (ringStubs)
        guest_d3d::PublishRingStubServices(std::make_shared<RingStubServices>(state));
    lock.unlock();
    if (ringStubs)
        REXGPU_INFO("GuestVideoServices: native_ring_stubs: ring helpers, fences, callbacks, timestamps and the VdSwap call write no ring "
                    "or PM4 words; the ledger completes them");

    REXGPU_INFO("GuestVideoServices: MMIO, vblank and GPU completion active; draws {}; no PM4 execution",
                options.drawMode == guest_d3d::NativeDrawMode::Census ? "census (bring-up, skips and logs each failure form)"
                : options.drawMode == guest_d3d::NativeDrawMode::Skip ? "skip (bring-up, partial)"
                                                                      : "strict");
    return X_STATUS_SUCCESS;
}

bool GuestVideoServices::StopProducers(std::chrono::milliseconds timeout)
{
    const auto                                    deadline = std::chrono::steady_clock::now() + timeout;
    std::shared_ptr<completion::CompletionLedger> ledger;
    bool                                          withdraw  = false;
    bool                                          summarize = false;
    {
        std::lock_guard lock(state->mutex);
        ledger    = state->ledger;
        withdraw  = std::exchange(state->progressPublished, false);
        summarize = withdraw;
        // Refuse and drop source-1 requests before the ledger closes, so no
        // callback interrupt is raised for a closed ledger.
        state->completionRunning.store(false, std::memory_order_release);
        state->interruptPending = false;
        state->assembleSwap     = {};
    }
    state->interruptDone.notify_all();
    // The consumes stop first and the ones in flight finish against an open
    // ledger, before it closes and before the services go.
    if (withdraw)
        guest_d3d::WithdrawRingStubServices();
    if (ledger)
    {
        ledger->Close();
    }
    if (withdraw)
        completion::WithdrawGuestProgress();
    if (ledger && summarize)
    {
        const auto snapshot = ledger->Snapshot();
        REXGPU_INFO("GuestVideoServices: GPU completion closed: ring_epoch={} spans_released={} partial_spans={} dropped={} "
                    "callbacks={} deferred={} replays={} lists_started={} max_lists={} swaps={} partial_swaps={} suspends={} deferred_swaps={} "
                    "last_fence={} failure={}",
                    snapshot.ringEpoch,
                    snapshot.spansReleased,
                    snapshot.partialSpans,
                    snapshot.spansDropped,
                    snapshot.callbacksReleased,
                    snapshot.deferredSpans,
                    snapshot.replaySubmits,
                    snapshot.tiledListsStarted,
                    snapshot.maxTiledLists,
                    snapshot.swaps,
                    snapshot.partialSwaps,
                    snapshot.deviceSuspends,
                    snapshot.deferredSwaps,
                    snapshot.fenceReleased ? fmt::format("{:08X}", snapshot.lastReleasedFence) : std::string("none"),
                    snapshot.failure.value_or("none"));
        if (state->drawMode == guest_d3d::NativeDrawMode::Census)
        {
            logCensus(*ledger, snapshot.swaps, "shutdown");
        }
    }

    std::unique_lock lock(state->mutex);
    state->completionRunning.store(false, std::memory_order_release);
    state->interruptDone.notify_all();
    const bool completionExited = state->completionExit.wait_until(lock, deadline, [this]
                                                                   {
                                                                       return state->completionExited;
                                                                   });
    state->vblankWorkerRunning.store(false, std::memory_order_release);
    const bool vblankExited = state->vblankWorkerExit.wait_until(lock, deadline, [this]
                                                                 {
                                                                     return state->vblankWorkerExited;
                                                                 });
    // The body's exit flag precedes XThread::Exit, which still writes guest
    // and kernel state; the join ends only when the native thread has ended.
    const auto joinNative = [&](rex::system::object_ref<rex::system::XHostThread>& slot, bool flagged)
    {
        if (!flagged)
        {
            return false;
        }
        if (!slot)
        {
            return true;
        }
        auto       thread    = slot;
        const auto remaining = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                                        std::chrono::milliseconds::zero());
        lock.unlock();
        const bool ended = !thread->thread() || rex::thread::Wait(thread->thread(), false, remaining) == rex::thread::WaitResult::kSuccess;
        lock.lock();
        if (ended)
        {
            slot.reset();
        }
        return ended;
    };
    const bool completionStopped = joinNative(state->completionThread, completionExited);
    const bool vblankStopped     = joinNative(state->vblankWorkerThread, vblankExited);
    if (!completionStopped || !vblankStopped)
    {
        if (timeout.count() == 0)
        {
            REXGPU_ERROR("GuestVideoServices: producer join skipped: deadline expired (completion {}, vblank {})",
                         completionStopped ? "stopped" : "running",
                         vblankStopped ? "stopped" : "running");
        }
        else
        {
            REXGPU_ERROR("GuestVideoServices: producers did not stop within {} ms (completion {}, vblank {})",
                         timeout.count(),
                         completionStopped ? "stopped" : "running",
                         vblankStopped ? "stopped" : "running");
        }
    }
    return completionStopped && vblankStopped;
}

bool GuestVideoServices::Shutdown(std::chrono::milliseconds timeout)
{
    {
        std::lock_guard lock(state->mutex);
        if (state->shutdownStarted)
        {
            return state->vblankWorkerExited && state->completionExited && !state->vblankWorkerThread && !state->completionThread;
        }
        state->shutdownStarted   = true;
        state->interruptCallback = 0;
        state->interruptUserData = 0;
    }

    const bool stopped = StopProducers(timeout);
    unbindMmioBridge(state.get());

    std::lock_guard lock(state->mutex);
    state->active             = false;
    state->mmioRegistered     = false;
    state->functionDispatcher = nullptr;
    state->kernelState        = nullptr;
    state->memory             = nullptr;
    state->reportDebt         = {};
    if (stopped)
    {
        state->progressSink.memory.store(nullptr, std::memory_order_release);
    }
    std::lock_guard failureLock(state->failureMutex);
    state->reportFailure = {};
    return stopped;
}

void GuestVideoServices::SetInterruptCallback(uint32_t callback, uint32_t userData)
{
    {
        std::lock_guard lock(state->mutex);
        if (state->shutdownStarted)
        {
            return;
        }
        state->interruptCallback = callback;
        state->interruptUserData = userData;
    }
    REXGPU_INFO("GuestVideoServices: interrupt callback={:08X} user_data={:08X}", callback, userData);
}

void GuestVideoServices::InitializeRingBuffer(uint32_t ptr, uint32_t sizeLog2)
{
    bool valid = sizeLog2 <= 26;
    if (valid)
    {
        const uint32_t ringBytes = uint32_t(1) << (sizeLog2 + 3);
        valid                    = isPhysicalRangeSafe(ptr, ringBytes);
    }

    std::shared_ptr<completion::CompletionLedger> ledger;
    bool                                          accepted = false;
    {
        std::lock_guard lock(state->mutex);
        if (state->shutdownStarted)
        {
            return;
        }
        ledger = state->ledger;
        if (state->configurationFrozen || !valid)
        {
            state->configurationFrozen = true;
            REXGPU_ERROR("GuestVideoServices: ignoring ring ptr={:08X} size_log2={} ({}); retained generation {} ptr={:08X} size_log2={}",
                         ptr,
                         sizeLog2,
                         valid ? "metadata frozen" : "unsafe range, metadata frozen",
                         state->ringGeneration,
                         state->ringPointer,
                         state->ringSizeLog2);
        }
        else
        {
            // The guest re-initializes its ring during boot; each registration starts a ledger epoch.
            const uint32_t previousPointer  = state->ringPointer;
            const uint32_t previousSizeLog2 = state->ringSizeLog2;
            state->ringPointer              = ptr;
            state->ringSizeLog2             = sizeLog2;
            const uint32_t generation       = ++state->ringGeneration;
            accepted                        = true;
            state->registrationWaitSince.reset();
            state->registrationDebtReported = false;
            REXGPU_INFO("GuestVideoServices: ring generation {} ptr={:08X} size_log2={} (previous ptr={:08X} size_log2={})",
                        generation,
                        ptr,
                        sizeLog2,
                        previousPointer,
                        previousSizeLog2);
        }
    }
    if (!ledger)
    {
        return;
    }
    if (accepted)
    {
        ledger->BeginRingEpoch();
    }
    else
    {
        ledger->Fail(fmt::format("ring_registration_rejected {:08X} size_log2 {}", ptr, sizeLog2), completion::ProgressKind::Unmodeled, 0);
    }
}

void GuestVideoServices::EnableReadPointerWriteBack(uint32_t ptr, uint32_t blockSizeLog2)
{
    const bool valid = blockSizeLog2 <= 29 && isPhysicalRangeSafe(ptr, sizeof(uint32_t));

    std::shared_ptr<completion::CompletionLedger> ledger;
    bool                                          accepted = false;
    {
        std::lock_guard lock(state->mutex);
        if (state->shutdownStarted)
        {
            return;
        }
        ledger = state->ledger;
        if (state->configurationFrozen || !valid)
        {
            state->configurationFrozen = true;
            REXGPU_ERROR("GuestVideoServices: ignoring writeback ptr={:08X} block_size_log2={} ({}); retained generation {} ptr={:08X} block_size_log2={}",
                         ptr,
                         blockSizeLog2,
                         valid ? "metadata frozen" : "unsafe range, metadata frozen",
                         state->writebackGeneration,
                         state->writebackPointer,
                         state->writebackBlockLog2);
        }
        else
        {
            const uint32_t previousPointer   = state->writebackPointer;
            const uint32_t previousBlockLog2 = state->writebackBlockLog2;
            state->writebackPointer          = ptr;
            state->writebackBlockLog2        = blockSizeLog2;
            const uint32_t generation        = ++state->writebackGeneration;
            accepted                         = true;
            state->registrationWaitSince.reset();
            state->registrationDebtReported = false;
            // Each generation gets its own debt observation and report.
            state->writebackPublicationBase = state->writePointerPublications;
            state->debtMonitor.Reset();
            REXGPU_INFO("GuestVideoServices: writeback generation {} ptr={:08X} block_size_log2={} (previous ptr={:08X} block_size_log2={}); "
                        "written only by released ledger spans",
                        generation,
                        ptr,
                        blockSizeLog2,
                        previousPointer,
                        previousBlockLog2);
        }
    }
    if (!ledger)
    {
        return;
    }
    if (accepted)
    {
        ledger->RegisterWriteback(ptr & kPhysicalAddressMask);
    }
    else
    {
        ledger->Fail(fmt::format("writeback_registration_rejected {:08X}", ptr), completion::ProgressKind::Unmodeled, 0);
    }
}

void GuestVideoServices::OnSwap(const rex::system::DetachedVideoSwap& swap)
{
    SwapEpoch epoch;
    epoch.reservation         = swap.reservation;
    epoch.frontbufferPhysical = swap.frontbuffer_physical;
    std::copy(std::begin(swap.fetch), std::end(swap.fetch), epoch.fetch.begin());
    epoch.textureFormat = swap.texture_format;
    epoch.width         = swap.width;
    epoch.height        = swap.height;
    // Fetch dword 2 stores the 2D size minus one in 13-bit fields.
    const uint32_t fetchWidth  = (swap.fetch[2] & 0x1FFF) + 1;
    const uint32_t fetchHeight = ((swap.fetch[2] >> 13) & 0x1FFF) + 1;
    epoch.sizeMatchesFetch     = fetchWidth == swap.width && fetchHeight == swap.height;

    std::shared_ptr<completion::CompletionLedger>                                    ledger;
    std::function<std::optional<std::string>(const rex::system::DetachedVideoSwap&)> assemble;
    {
        std::lock_guard lock(state->mutex);
        if (state->shutdownStarted)
        {
            return;
        }
        ledger   = state->ledger;
        assemble = state->assembleSwap;
    }
    // The frame closes before the ledger's swap record, so a failed frame
    // stops the session at this swap in both draw modes.
    if (assemble)
    {
        std::optional<std::string> failure;
        try
        {
            failure = assemble(swap);
        }
        catch (...)
        {
            failure = "swap_assembly_threw";
        }
        if (failure && ledger)
        {
            ledger->Fail("native frame at VdSwap: " + *failure, completion::ProgressKind::Swap, swap.frontbuffer_physical);
        }
    }
    // The swap record keeps its place among this thread's progress records.
    if (ledger)
    {
        epoch.partial = ledger->Swap(swap.frontbuffer_physical);
    }
    if (ledger && state->drawMode == guest_d3d::NativeDrawMode::Census)
    {
        // A swap the ledger refused leaves the count unchanged; log each multiple once.
        if (const auto swaps = ledger->Snapshot().swaps; swaps % kCensusSummarySwaps == 0 && state->censusSummarySwap.exchange(swaps) != swaps)
        {
            logCensus(*ledger, swaps, "periodic");
        }
    }

    bool first        = false;
    bool firstPartial = false;
    {
        std::lock_guard lock(state->mutex);
        if (state->shutdownStarted)
        {
            return;
        }
        epoch.sequence     = ++state->swapCount;
        epoch.vblankCount  = state->vblankCount.load(std::memory_order_acquire);
        epoch.writePointer = state->lastWritePointer;
        if (!epoch.sizeMatchesFetch)
        {
            ++state->swapSizeMismatches;
        }
        if (epoch.partial && !state->partialSwapLogged)
        {
            state->partialSwapLogged = true;
            firstPartial             = true;
        }
        if (state->swapEpochs.size() == kMaxSwapEpochs)
        {
            state->swapEpochs.pop_front();
        }
        state->swapEpochs.push_back(epoch);
        first = epoch.sequence == 1;
    }
    if (first)
    {
        REXGPU_INFO("GuestVideoServices: first VdSwap reservation={:08X} frontbuffer={:08X} {}x{} format={}",
                    epoch.reservation,
                    epoch.frontbufferPhysical,
                    epoch.width,
                    epoch.height,
                    epoch.textureFormat);
    }
    // Bounded diagnostic: pairs with the first frontbuffer resolves the ledger logs.
    if (epoch.sequence <= kLoggedSwaps)
    {
        REXGPU_INFO("GuestVideoServices: VdSwap {} frontbuffer physical {:08X} fetch base {:08X} partial={}",
                    epoch.sequence,
                    epoch.frontbufferPhysical,
                    epoch.fetch[1] & 0xFFFFF000u,
                    epoch.partial);
    }
    if (firstPartial)
    {
        REXGPU_WARN("GuestVideoServices: VdSwap {} is partial: skip mode released skipped draws or unsupported clears",
                    epoch.sequence);
    }
}

uint64_t GuestVideoServices::VblankCount() const
{
    return state->vblankCount.load(std::memory_order_acquire);
}

uint32_t GuestVideoServices::InterruptCallback() const
{
    std::lock_guard lock(state->mutex);
    return state->interruptCallback;
}

uint32_t GuestVideoServices::LastWritePointer() const
{
    std::lock_guard lock(state->mutex);
    return state->lastWritePointer;
}

uint64_t GuestVideoServices::WritePointerPublications() const
{
    std::lock_guard lock(state->mutex);
    return state->writePointerPublications;
}

bool GuestVideoServices::ConfigurationFrozen() const
{
    std::lock_guard lock(state->mutex);
    return state->configurationFrozen;
}

uint32_t GuestVideoServices::RingGeneration() const
{
    std::lock_guard lock(state->mutex);
    return state->ringGeneration;
}

uint32_t GuestVideoServices::WritebackGeneration() const
{
    std::lock_guard lock(state->mutex);
    return state->writebackGeneration;
}

uint32_t GuestVideoServices::WritebackPointer() const
{
    std::lock_guard lock(state->mutex);
    return state->writebackPointer;
}

uint64_t GuestVideoServices::SwapCount() const
{
    std::lock_guard lock(state->mutex);
    return state->swapCount;
}

uint64_t GuestVideoServices::SwapSizeMismatches() const
{
    std::lock_guard lock(state->mutex);
    return state->swapSizeMismatches;
}

std::vector<SwapEpoch> GuestVideoServices::SwapEpochs() const
{
    std::lock_guard lock(state->mutex);
    return std::vector<SwapEpoch>(state->swapEpochs.begin(), state->swapEpochs.end());
}

bool GuestVideoServices::ProducersRunning() const
{
    std::lock_guard lock(state->mutex);
    // A thread counts until its native thread has ended and its handle is released.
    return !state->vblankWorkerExited || !state->completionExited || state->vblankWorkerThread || state->completionThread;
}

bool GuestVideoServices::CompletionDebtReported() const
{
    std::lock_guard lock(state->mutex);
    return state->debtMonitor.Reported();
}

uint64_t GuestVideoServices::CallbackInterrupts() const
{
    std::lock_guard lock(state->mutex);
    return state->callbackInterrupts;
}

bool GuestVideoServices::CallbackInterruptPending() const
{
    std::lock_guard lock(state->mutex);
    return state->interruptPending;
}

std::shared_ptr<completion::CompletionLedger> GuestVideoServices::Ledger() const
{
    std::lock_guard lock(state->mutex);
    return state->ledger;
}

uint32_t GuestVideoServices::State::readRegister(uint32_t addr)
{
    const uint32_t reg = (addr & 0xFFFF) / 4;

    switch (reg)
    {
        case kRbEdramTiming:
            return 0x08100748;
        case kRbBcControl:
            return 0x0000200E;
        case kR500D1ModeVCounter:
        {
            rex::system::X_VIDEO_MODE videoMode;
            rex::kernel::xboxkrnl::VdQueryVideoMode(&videoMode);
            return std::min(uint32_t(videoMode.display_height), uint32_t(0x0FFF));
        }
        case kInterruptStatus:
            return 1;
        case kAvivoD1ModeViewportSize:
        {
            rex::system::X_VIDEO_MODE videoMode;
            rex::kernel::xboxkrnl::VdQueryVideoMode(&videoMode);
            const uint32_t width  = std::min(uint32_t(videoMode.display_width), uint32_t(0x0FFF));
            const uint32_t height = std::min(uint32_t(videoMode.display_height), uint32_t(0x0FFF));
            return (width << 16) | height;
        }
        default:
            break;
    }

    bool     reportUnknown = false;
    uint32_t value         = 0;
    {
        std::lock_guard lock(mutex);
        value = registerValues[reg];
        if (!unknownMmioLogged)
        {
            unknownMmioLogged = true;
            reportUnknown     = true;
        }
    }
    if (reportUnknown)
    {
        REXGPU_WARN("GuestVideoServices: unknown GPU register read {:04X}", reg);
    }
    return value;
}

void GuestVideoServices::State::writeRegister(uint32_t addr, uint32_t value)
{
    const uint32_t                                reg           = (addr & 0xFFFF) / 4;
    bool                                          reportUnknown = false;
    bool                                          reportFirst   = false;
    std::shared_ptr<completion::CompletionLedger> kicked;
    {
        std::lock_guard lock(mutex);
        registerValues[reg] = value;
        if (reg == kCpRbWptr)
        {
            // No command execution: the kick only closes the ledger's open span.
            lastWritePointer = value;
            ++writePointerPublications;
            kicked = ledger;
            if (!firstWriteLogged)
            {
                firstWriteLogged = true;
                reportFirst      = true;
            }
        }
        else if (reg != kAvivoD1GraphPrimarySurfaceAddress)
        {
            if (!unknownMmioLogged)
            {
                unknownMmioLogged = true;
                reportUnknown     = true;
            }
        }
    }

    // The kick runs on the publishing guest thread, after its span's records.
    if (kicked)
    {
        kicked->Kick(value);
    }
    if (reportFirst)
    {
        REXGPU_INFO("GuestVideoServices: first CP_RB_WPTR write {:08X} closes ledger span; PM4 is not executed", value);
    }
    if (reportUnknown)
    {
        REXGPU_WARN("GuestVideoServices: unknown GPU register write {:04X}={:08X}", reg, value);
    }
}

int GuestVideoServices::State::runVblankWorker()
{
    rex::system::X_VIDEO_MODE videoMode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&videoMode);
    const double   refreshRateHz      = std::max(1.0, double(float(videoMode.refresh_rate)));
    const uint64_t guestTickFrequency = rex::chrono::Clock::guest_tick_frequency();
    const uint64_t vsyncIntervalTicks = std::max(
        uint64_t(1), uint64_t(double(guestTickFrequency) / refreshRateHz));
    uint64_t lastFrameTime = rex::chrono::Clock::QueryGuestTickCount();

    while (vblankWorkerRunning.load(std::memory_order_acquire))
    {
        const auto step = AdvanceVblank(rex::chrono::Clock::QueryGuestTickCount(), lastFrameTime, vsyncIntervalTicks);
        for (uint32_t i = 0; i < step.due && vblankWorkerRunning.load(std::memory_order_acquire); ++i)
        {
            markVblank();
        }
        lastFrameTime = step.lastFrameTime;
        serviceCallbackInterrupt();
        checkRegistrationDebt();
        checkCompletionDebt();
#if REREVVED_DEVELOPER
        checkRingConsumeWaits();
#endif
        rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
}

int GuestVideoServices::State::runCompletionWorker()
{
    completion::LedgerSnapshot logged;
    bool                       drawFailureReported = false;
    while (completionRunning.load(std::memory_order_acquire))
    {
        const bool progressed = ledger && ledger->Pump();
        if (ledger && pollDrawFailure && !drawFailureReported)
        {
            if (auto failure = pollDrawFailure())
            {
                // Census mode keeps polling: each skipped host failure is
                // counted, until a fatal one fails the ledger.
                drawFailureReported = drawMode != guest_d3d::NativeDrawMode::Census || failure->fatal;
                ledger->Fail(std::move(failure->detail), completion::ProgressKind::Draw, 0, failure->fatal);
            }
        }
        if (!progressed)
        {
            rex::thread::Sleep(std::chrono::milliseconds(1));
            continue;
        }
        const auto snapshot = ledger->Snapshot();
        if (!logged.spansReleased && snapshot.spansReleased)
        {
            REXGPU_INFO("GuestVideoServices: first ledger span released: ring_epoch={} fence={} writeback={:08X}",
                        snapshot.ringEpoch,
                        snapshot.fenceReleased ? fmt::format("{:08X}", snapshot.lastReleasedFence) : std::string("none"),
                        snapshot.writebackPointer);
        }
        if (!logged.partialSpans && snapshot.partialSpans)
        {
            REXGPU_WARN("GuestVideoServices: first partial span released; skipped draws per reason: {}",
                        fmt::join(snapshot.skipped, ","));
        }
        if (!logged.callbacksReleased && snapshot.callbacksReleased)
        {
            REXGPU_INFO("GuestVideoServices: first source-1 callback released: callback={:08X}", snapshot.lastCallback);
        }
        logged = snapshot;
    }
    return 0;
}

bool GuestVideoServices::State::executeInterrupt(uint64_t                          source,
                                                 uint32_t                          cpu,
                                                 uint32_t                          callback,
                                                 uint32_t                          userData,
                                                 rex::runtime::FunctionDispatcher* dispatch)
{
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread)
    {
        bool report = false;
        {
            std::lock_guard lock(mutex);
            if (!missingThreadLogged)
            {
                missingThreadLogged = true;
                report              = true;
            }
        }
        if (report)
        {
            REXGPU_WARN("GuestVideoServices: interrupt thread has no current XThread");
        }
        return false;
    }
    thread->SetActiveCpu(static_cast<uint8_t>(cpu));
    uint64_t args[] = { source, userData };
    dispatch->ExecuteInterrupt(thread->thread_state(), callback, args, std::size(args));
    return true;
}

void GuestVideoServices::State::markVblank()
{
    uint32_t                          callback = 0;
    uint32_t                          userData = 0;
    rex::runtime::FunctionDispatcher* dispatch = nullptr;
    {
        std::lock_guard lock(mutex);
        if (!active)
        {
            return;
        }
        vblankCount.fetch_add(1, std::memory_order_acq_rel);
        if (interruptCallback && functionDispatcher)
        {
            callback = interruptCallback;
            userData = interruptUserData;
            dispatch = functionDispatcher;
        }
    }

    if (callback && dispatch)
    {
        executeInterrupt(0, kDefaultCpu, callback, userData, dispatch);
    }
}

// Source 1 runs on this thread too, so the guest never sees two interrupts at once.
void GuestVideoServices::State::serviceCallbackInterrupt()
{
    uint64_t                          request  = 0;
    uint32_t                          cpu      = 0;
    uint32_t                          callback = 0;
    uint32_t                          userData = 0;
    rex::runtime::FunctionDispatcher* dispatch = nullptr;
    {
        std::lock_guard lock(mutex);
        if (!interruptPending || !completionRunning.load(std::memory_order_acquire))
        {
            interruptPending = false;
            return;
        }
        interruptPending = false;
        request          = interruptRequest;
        cpu              = interruptCpu;
        if (active)
        {
            callback = interruptCallback;
            userData = interruptUserData;
            dispatch = functionDispatcher;
        }
    }

    auto result = completion::InterruptResult::NoHandler;
    if (callback && dispatch && executeInterrupt(1, cpu, callback, userData, dispatch))
    {
        result = completion::InterruptResult::Raised;
    }
    {
        std::lock_guard lock(mutex);
        interruptServed = request;
        interruptResult = result;
        if (result == completion::InterruptResult::Raised)
        {
            ++callbackInterrupts;
        }
    }
    interruptDone.notify_all();
}

void GuestVideoServices::State::checkRegistrationDebt()
{
    if (!ledger)
    {
        return;
    }
    // The ledger lock is never taken under the service lock.
    const bool                                 waiting = ledger->AwaitingWritebackRegistration();
    const auto                                 now     = CompletionDebtMonitor::Clock::now();
    CompletionDebt                             debt;
    std::function<void(const CompletionDebt&)> report;
    std::chrono::milliseconds                  held{};
    {
        std::lock_guard lock(mutex);
        if (!active || registrationDebtReported)
        {
            return;
        }
        if (!waiting)
        {
            registrationWaitSince.reset();
            return;
        }
        if (!registrationWaitSince)
        {
            registrationWaitSince = now;
            return;
        }
        if (now - *registrationWaitSince < debtThreshold)
        {
            return;
        }
        registrationDebtReported      = true;
        held                          = std::chrono::duration_cast<std::chrono::milliseconds>(now - *registrationWaitSince);
        debt.ringPointer              = ringPointer;
        debt.ringSizeLog2             = ringSizeLog2;
        debt.writebackPointer         = writebackPointer;
        debt.writePointer             = lastWritePointer;
        debt.writePointerPublications = writePointerPublications - writebackPublicationBase;
        debt.swapCount                = swapCount;
        debt.vblankCount              = vblankCount.load(std::memory_order_acquire);
        debt.ringGeneration           = ringGeneration;
        debt.writebackGeneration      = writebackGeneration;
        debt.configurationFrozen      = configurationFrozen;
        report                        = reportDebt;
    }
    debt.ledger = ledger->Snapshot();
    REXGPU_ERROR("GuestVideoServices: GPU completion debt: span {} (wptr {:08X}) held {} ms awaiting writeback registration; "
                 "ring={:08X} size_log2={} ring_generation={} writeback_generation={} last registered writeback={:08X} wptr={:08X}",
                 debt.ledger.oldestSpan,
                 debt.ledger.writePointer,
                 held.count(),
                 debt.ringPointer,
                 debt.ringSizeLog2,
                 debt.ringGeneration,
                 debt.writebackGeneration,
                 debt.writebackPointer,
                 debt.writePointer);
    if (report)
    {
        report(debt);
    }
}

void GuestVideoServices::State::checkCompletionDebt()
{
    CompletionDebt                             debt;
    std::function<void(const CompletionDebt&)> report;
    {
        std::lock_guard lock(mutex);
        if (!active || !memory || writebackGeneration == 0 || debtMonitor.Reported())
        {
            return;
        }
        // The guest frees and reallocates its writeback block during boot; a
        // released page is unknown and never read.
        std::optional<uint32_t> writebackWord;
        const uint32_t          physicalAddress = writebackPointer & kPhysicalAddressMask;
        if (memory->GetPhysicalHeap()->QueryRangeAccess(physicalAddress, physicalAddress + 3) !=
            rex::memory::PageAccess::kNoAccess)
        {
            writebackWord = rex::memory::load_and_swap<uint32_t>(
                memory->TranslatePhysical<const uint8_t*>(writebackPointer));
        }
        const uint64_t publications = writePointerPublications - writebackPublicationBase;
        if (!debtMonitor.Observe(CompletionDebtMonitor::Clock::now(),
                                 publications,
                                 lastWritePointer,
                                 writebackWord))
        {
            return;
        }
        debt.ringPointer              = ringPointer;
        debt.ringSizeLog2             = ringSizeLog2;
        debt.writebackPointer         = writebackPointer;
        debt.writePointer             = lastWritePointer;
        debt.readPointer              = writebackWord.value_or(0);
        debt.readPointerReadable      = writebackWord.has_value();
        debt.writePointerPublications = publications;
        debt.swapCount                = swapCount;
        debt.vblankCount              = vblankCount.load(std::memory_order_acquire);
        debt.ringGeneration           = ringGeneration;
        debt.writebackGeneration      = writebackGeneration;
        debt.configurationFrozen      = configurationFrozen;
        report                        = reportDebt;
    }
    // Taken outside the service lock: the ledger never waits on it.
    if (ledger)
    {
        debt.ledger = ledger->Snapshot();
        if (debt.ledger.fenceBlock)
        {
            debt.completedFence = progressSink.ReadPhysical(debt.ledger.fenceBlock + completion::kFenceOffset);
        }
    }

    const auto& ledgerState = debt.ledger;
    REXGPU_ERROR("GuestVideoServices: GPU completion debt: ring={:08X} size_log2={} writeback={:08X} "
                 "wptr={:08X} rptr={} publications={} swaps={} vblanks={} ring_generation={} "
                 "writeback_generation={} frozen={}; span={} closed={} span_wptr={:08X} held_by='{}' unowned='{}' "
                 "requested_fence={} completed_fence={} released_fence={} host_fence_completed={} host_fence_target={} skipped={} "
                 "unmodeled={:08X} failure='{}'",
                 debt.ringPointer,
                 debt.ringSizeLog2,
                 debt.writebackPointer,
                 debt.writePointer,
                 debt.readPointerReadable ? fmt::format("{:08X}", debt.readPointer) : std::string("unreadable"),
                 debt.writePointerPublications,
                 debt.swapCount,
                 debt.vblankCount,
                 debt.ringGeneration,
                 debt.writebackGeneration,
                 debt.configurationFrozen,
                 ledgerState.pending ? fmt::format("{}", ledgerState.oldestSpan) : std::string("none"),
                 ledgerState.oldestClosed,
                 ledgerState.writePointer,
                 ledgerState.oldestRecord,
                 ledgerState.unowned,
                 ledgerState.fenceRequested ? fmt::format("{:08X}", ledgerState.requestedFence) : std::string("none"),
                 // No B is known before the first fence record or fence wait.
                 !ledgerState.fenceBlock ? std::string("unknown")
                 : debt.completedFence   ? fmt::format("{:08X}", *debt.completedFence)
                                         : std::string("unreadable"),
                 ledgerState.fenceReleased ? fmt::format("{:08X}", ledgerState.lastReleasedFence) : std::string("none"),
                 ledgerState.hostCompleted,
                 ledgerState.hostTarget,
                 fmt::join(ledgerState.skipped, ","),
                 ledgerState.unmodeledEmitter,
                 ledgerState.failure.value_or(""));
    if (report)
    {
        report(debt);
    }
}

#if REREVVED_DEVELOPER
// native_ring_consume_publish: a fence wait or drain scope open past the debt bound is
// reported once, from this loop so a wait that never returns is still seen. It reads
// the monitor's table, then the device and the ledger snapshot for the line.
void GuestVideoServices::State::checkRingConsumeWaits()
{
    if (!guest_d3d::RingConsumePublishRequested())
    {
        return;
    }
    const auto reports = guest_d3d::PollRingWaits(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(debtThreshold).count()));
    if (reports.empty())
    {
        return;
    }
    rex::memory::Memory* guestMemory = nullptr;
    uint32_t             wptr = 0, ringGeneration_ = 0, writebackGeneration_ = 0;
    {
        std::lock_guard lock(mutex);
        guestMemory          = memory;
        wptr                 = lastWritePointer;
        ringGeneration_      = ringGeneration;
        writebackGeneration_ = writebackGeneration;
    }
    const auto readWord = [&](uint32_t address) -> std::optional<uint32_t>
    {
        if (!guestMemory || address < 0x10000 || address > 0xFFFFFFFCU)
        {
            return std::nullopt;
        }
        auto* heap = guestMemory->LookupHeap(address);
        if (!heap || guestMemory->LookupHeap(address + 3) != heap ||
            (static_cast<unsigned>(heap->QueryRangeAccess(address, address + 3)) & static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)) !=
                static_cast<unsigned>(rex::memory::PageAccess::kReadOnly))
        {
            return std::nullopt;
        }
        return rex::memory::load_and_swap<uint32_t>(guestMemory->TranslateVirtual<const uint8_t*>(address));
    };
    const auto text = [](const std::optional<uint32_t>& value)
    {
        return value ? fmt::format("{:08X}", *value) : std::string("unreadable");
    };
    const auto snapshot = ledger ? ledger->Snapshot() : completion::LedgerSnapshot{};
    for (const auto& report : reports)
    {
        const auto              pending = readWord(report.device + guest_d3d::ring_layout::kWorkerPending);
        const auto              cursor  = readWord(report.device + guest_d3d::ring_layout::kCursor);
        std::optional<uint32_t> completed;
        if (snapshot.fenceBlock)
        {
            completed = progressSink.ReadPhysical(snapshot.fenceBlock + completion::kFenceOffset);
        }
        const bool fence = report.kind == guest_d3d::RingWaitKind::Fence;
        REXGPU_ERROR("GuestVideoServices: ring consume wait: kind={} device={:08X} ring_generation={} writeback_generation={} thread={:08X} caller={:08X} "
                     "elapsed_ms={} {}={} actual={} n={} cursor={} wptr={:08X} span={} record='{}' host_fence_target={} host_fence_completed={}",
                     guest_d3d::kRingWaitNames[static_cast<size_t>(report.kind)],
                     report.device,
                     ringGeneration_,
                     writebackGeneration_,
                     report.thread,
                     report.caller,
                     report.elapsedUs / 1000,
                     fence ? "requested_fence" : "pending_n_obligation",
                     fence ? fmt::format("{:08X}", report.requested) : std::string("zero"),
                     fence ? text(completed) : text(pending),
                     text(pending),
                     text(cursor),
                     wptr,
                     snapshot.pending ? fmt::format("{}", snapshot.oldestSpan) : std::string("none"),
                     snapshot.oldestRecord,
                     snapshot.hostTarget,
                     snapshot.hostCompleted);
    }
}
#endif

} // namespace rerevved::gpu
