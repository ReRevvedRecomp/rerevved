#include "native_draw_dispatch.h"

#if REREVVED_DEVELOPER
#include "draw_effect_probe.h"
#endif

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <tuple>
#include <utility>

namespace rerevved::gpu::guest_d3d
{

std::atomic<NativeDrawDispatcher*> nativeDrawDispatcher{ nullptr };
// Removal waits for zero, so a root never touches a dispatcher its owner already destroyed.
std::atomic<std::uint64_t> nativeDrawInFlight{ 0 };

namespace
{

auto& inFlight = nativeDrawInFlight;

struct Pending;
struct Orphan;

// How a root ends: numbered with its verdict, abandoned by a skipped scope or
// unwind, dropped as a failed direct lowering, or refused uncommitted.
enum class RootEnd
{
    Complete,
    Unwound,
    Discarded,
    Uncommitted,
};
void finishRoot(Pending& pending, RootEnd end) noexcept;

// Per-thread state of the outermost draw hook currently executing Original.
struct Pending
{
    NativeDrawState* owner      = nullptr;
    std::uint32_t    depth      = 0;
    std::uint64_t    generation = 0;
    std::uint32_t    rootStack  = 0;
    bool             unwound    = false;
    // This root holds a draw-effect sample its end must finish; set only in the
    // developer build.
    bool effectProbe = false;
    // A direct lowerer root held after Original until its commit.
    bool                                            held       = false;
    const std::uint8_t*                             base       = nullptr;
    std::uint32_t                                   pendingEnd = 0;
    NativeDrawSubmission                            submission;
    std::array<std::uint32_t, kNativeDrawHookCount> calls{};
    // Allocated when the root is held, so a thread exit queues it without
    // allocating; owned by this root until queued or finished.
    Orphan* spare = nullptr;

    // A thread that exits with a root releases it here, touching no other
    // thread_local: other per-thread state may already be destroyed.
    ~Pending();
};

// A held root whose thread exited before its commit. Teardown only appends
// its node to one FIFO list; a live entry of its own dispatcher refuses it
// (drainOrphans). std::mutex is an SRW lock: locking it allocates nothing.
struct Orphan
{
    NativeDrawState* owner = nullptr;
    Pending          pending;
    Orphan*          next = nullptr;
};

std::mutex orphanMutex;
Orphan*    orphanHead = nullptr;
Orphan*    orphanTail = nullptr;
// Nodes in the list, so a draw entry with none takes no lock.
std::atomic<std::size_t> orphanCount{ 0 };

// Unlinks `owner`'s nodes, oldest first, leaving every other node in place.
// The caller holds orphanMutex.
Orphan* unlinkOrphans(const NativeDrawState* owner) noexcept
{
    Orphan *taken = nullptr, **end = &taken;
    for (Orphan** link = &orphanHead; *link;)
    {
        Orphan* node = *link;
        if (node->owner != owner)
        {
            orphanTail = node;
            link       = &node->next;
            continue;
        }
        *link      = node->next;
        node->next = nullptr;
        orphanCount.fetch_sub(1, std::memory_order_relaxed);
        *end = node;
        end  = &node->next;
    }
    if (!orphanHead)
        orphanTail = nullptr;
    return taken;
}

// Each removal withdraws, publishes, waits and drains as one critical
// section under this lock. Its dispatcher stays in `removing` until drained.
std::timed_mutex                   removalMutex;
std::atomic<NativeDrawDispatcher*> removing{ nullptr };
thread_local bool                  removalHeld = false;

thread_local Pending       active;
thread_local std::uint64_t generations = 0;

constexpr std::size_t slot(NativeDrawHook hook) noexcept
{
    return static_cast<std::size_t>(hook);
}

constexpr std::size_t slot(NativeDrawReason reason) noexcept
{
    return static_cast<std::size_t>(reason);
}

// Route census (EnableNativeDrawCensus); touched only while enabled.
std::atomic<bool>                                                             drawCensusEnabled{ false };
std::array<std::atomic<std::uint64_t>, kNativeDrawHookCount>                  drawCensusRoots{}, drawCensusNested{}, drawCensusUnread{};
std::array<std::array<std::atomic<std::uint64_t>, 256>, kNativeDrawHookCount> drawCensusTile{};

// Three heap lookups and range queries per observed root draw, paid only
// while a dispatcher is installed.
bool readKernelWord(const std::uint8_t*, std::uint32_t address, std::uint32_t& value)
{
    auto* kernel = rex::system::kernel_state();
    if (!kernel || !kernel->memory() || address > 0xFFFFFFFCU)
        return false;
    auto*      memory = kernel->memory();
    auto*      heap   = memory->LookupHeap(address);
    const auto last   = address + 3;
    if (!heap || memory->LookupHeap(last) != heap ||
        !(static_cast<unsigned>(heap->QueryRangeAccess(address, last)) & static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
        return false;
    std::uint32_t raw = 0;
    std::memcpy(&raw, memory->TranslateVirtual<const std::uint8_t*>(address), sizeof(raw));
    value = rex::byte_swap(raw);
    return true;
}

// The entry shadow copy of a direct lowerer root, about 13 draws a frame.
bool readKernelBlock(const std::uint8_t*, std::uint32_t address, std::span<std::uint8_t> bytes)
{
    auto* kernel = rex::system::kernel_state();
    if (!kernel || !kernel->memory() || bytes.empty() || address > 0xFFFFFFFFU - (bytes.size() - 1))
        return false;
    auto*      memory = kernel->memory();
    auto*      heap   = memory->LookupHeap(address);
    const auto last   = static_cast<std::uint32_t>(address + (bytes.size() - 1));
    if (!heap || memory->LookupHeap(last) != heap ||
        !(static_cast<unsigned>(heap->QueryRangeAccess(address, last)) & static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
        return false;
    std::memcpy(bytes.data(), memory->TranslateVirtual<const std::uint8_t*>(address), bytes.size());
    return true;
}

// The word store of a consumed draw: the checks and the big-endian store of a
// guest `stw`, over memory the guest has mapped writable.
bool writableKernel(const std::uint8_t*, std::uint32_t address, std::uint32_t size)
{
    auto* kernel = rex::system::kernel_state();
    if (!kernel || !kernel->memory() || !size || address < 0x10000 || address > 0xFFFFFFFFU - (size - 1))
        return false;
    auto*      memory = kernel->memory();
    auto*      heap   = memory->LookupHeap(address);
    const auto last   = address + size - 1;
    const auto needed = static_cast<unsigned>(rex::memory::PageAccess::kReadWrite);
    return heap && memory->LookupHeap(last) == heap && (static_cast<unsigned>(heap->QueryRangeAccess(address, last)) & needed) == needed;
}

bool writeKernelWord(const std::uint8_t* base, std::uint32_t address, std::uint32_t value)
{
    if (!writableKernel(base, address, 4))
        return false;
    const std::uint32_t raw = rex::byte_swap(value);
    std::memcpy(rex::system::kernel_state()->memory()->TranslateVirtual<std::uint8_t*>(address), &raw, sizeof(raw));
    return true;
}

// Each outer UP wrapper makes exactly one static child call (0x826A35AC and
// 0x826A3020); no other draw hook calls a draw hook.
std::optional<std::pair<NativeDrawHook, std::uint32_t>> expectedChild(NativeDrawHook root) noexcept
{
    switch (root)
    {
        case NativeDrawHook::IndexedUp:
            return std::pair{ NativeDrawHook::IndexedUpLowerer, kIndexedUpLowererReturn };
        case NativeDrawHook::NonindexedUp:
            return std::pair{ NativeDrawHook::NonindexedSink, kNonindexedSinkReturn };
        default:
            return std::nullopt;
    }
}

// A root child entered from its outer call site belongs to an outer that
// began before a dispatcher was installed; it is not a direct caller.
bool orphanChild(NativeDrawHook hook, std::uint32_t caller) noexcept
{
    return (hook == NativeDrawHook::IndexedUpLowerer && caller == kIndexedUpLowererReturn) ||
           (hook == NativeDrawHook::NonindexedSink && caller == kNonindexedSinkReturn);
}

void addCounters(NativeDrawFrameCounters& counters, const std::array<std::uint32_t, kNativeDrawHookCount>& calls, const NativeDrawSubmission& submission, bool unwound)
{
    for (std::size_t i = 0; i < kNativeDrawHookCount; ++i)
        counters.invocations[i] += calls[i];
    counters.nested += submission.nested;
    if (unwound)
        ++counters.abandoned;
}

// One worker thread running posted jobs in order.
class DrawSequencer
{
public:
    explicit DrawSequencer(std::size_t capacity)
    : capacity(capacity ? capacity : 1)
    {
        worker = std::thread([this]
                             {
                                 run();
                             });
    }

    ~DrawSequencer()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_all();
        worker.join();
    }

    DrawSequencer(const DrawSequencer&)            = delete;
    DrawSequencer& operator=(const DrawSequencer&) = delete;

    bool OnWorker() const noexcept
    {
        return std::this_thread::get_id() == worker.get_id();
    }

    // Waits while the queue is full; the worker runs its own posts inline.
    void Post(std::function<void()> job)
    {
        if (OnWorker())
        {
            job();
            return;
        }
        std::unique_lock lock(mutex);
        if (jobs.size() >= capacity)
        {
            const auto started = std::chrono::steady_clock::now();
            room.wait(lock, [&]
                      {
                          return jobs.size() < capacity;
                      });
            stats.fullNanoseconds += elapsed(started);
        }
        jobs.push_back(std::move(job));
        ++stats.posted;
        lock.unlock();
        ready.notify_one();
    }

    // False when `deadline` passed with jobs left.
    bool Drain(std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt)
    {
        if (OnWorker())
            return true;
        std::unique_lock lock(mutex);
        if (jobs.empty() && !running)
            return true;
        const auto started = std::chrono::steady_clock::now();
        const auto done    = [&]
        {
            return jobs.empty() && !running;
        };
        bool drained = true;
        if (deadline)
            drained = idle.wait_until(lock, *deadline, done);
        else
            idle.wait(lock, done);
        ++stats.drains;
        stats.drainNanoseconds += elapsed(started);
        return drained;
    }

    NativeDrawSequencerStats Stats() const
    {
        std::lock_guard lock(mutex);
        return stats;
    }

private:
    static std::uint64_t elapsed(std::chrono::steady_clock::time_point started) noexcept
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    }

    void run()
    {
        // Its jobs run draw callbacks: a removal from one withdraws without
        // waiting on this thread, as from inside a hook.
        nativeDrawThreadActive = true;
        std::unique_lock lock(mutex);
        for (;;)
        {
            ready.wait(lock, [&]
                       {
                           return stopping || !jobs.empty();
                       });
            if (jobs.empty())
                return;
            auto job = std::move(jobs.front());
            jobs.pop_front();
            running = true;
            lock.unlock();
            room.notify_one();
            try
            {
                job();
            }
            catch (...)
            {
            }
            job = nullptr;
            lock.lock();
            running = false;
            ++stats.ran;
            if (jobs.empty())
                idle.notify_all();
        }
    }

    const std::size_t                 capacity;
    mutable std::mutex                mutex;
    std::condition_variable           ready, room, idle;
    std::deque<std::function<void()>> jobs;
    bool                              running = false, stopping = false;
    NativeDrawSequencerStats          stats;
    std::thread                       worker;
};

} // namespace

struct NativeDrawState
{
    using Identity = std::tuple<NativeDrawHook, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;

    NativeDrawDispatchConfig         config;
    mutable std::mutex               mutex;
    std::uint64_t                    sequence = 0;
    NativeDrawFrameCounters          frame, totals;
    std::vector<NativeDrawFirstSeen> firstSeen;
    std::map<Identity, std::size_t>  firstSeenIndex;
    std::uint64_t                    firstSeenDropped = 0;
    std::optional<NativeDrawFailure> failure;

    std::optional<NativeDrawReason> Classify(const NativeDrawSubmission& submission) const;
    void                            Count(NativeDrawSubmission& submission, std::optional<NativeDrawReason> reason, std::optional<NativeDrawFailure>& raised);
    // Posts a deferred root's conclusion to the sequencer, else concludes it here.
    void Finish(Pending& pending, RootEnd end) noexcept;
    // Counts the root and reports its verdict; a deferred root's began, or
    // abandoned, runs first.
    void Conclude(NativeDrawSubmission& submission, const std::array<std::uint32_t, kNativeDrawHookCount>& calls, RootEnd end) noexcept;
    bool ReadBlock(const std::uint8_t* base, std::uint32_t address, std::span<std::uint8_t> bytes) const;

    // Last, so it drains and joins before the state its jobs use goes.
    std::unique_ptr<DrawSequencer> sequencer;
};

struct NativeDrawAccess
{
    static NativeDrawState* State(NativeDrawDispatcher& dispatcher) noexcept
    {
        return dispatcher.state.get();
    }
};

std::optional<NativeDrawReason> NativeDrawState::Classify(const NativeDrawSubmission& submission) const
{
    if (submission.nestingMismatch)
        return NativeDrawReason::NestingMismatch;
    if (submission.hook == NativeDrawHook::AutomaticIndex)
        return NativeDrawReason::AutomaticIndex;
    if (!submission.stateRead)
        return NativeDrawReason::StateUnreadable;
    return config.admit ? config.admit(submission) : std::optional{ NativeDrawReason::NoNativeRoute };
}

// Caller holds the mutex.
void NativeDrawState::Count(NativeDrawSubmission& submission, std::optional<NativeDrawReason> reason, std::optional<NativeDrawFailure>& raised)
{
    submission.sequence = ++sequence;
    if (config.numberOperation)
        submission.operationMark = config.numberOperation();
    submission.frame = frame.frame;
    ++frame.attempted;
    ++totals.attempted;
    if (!reason)
    {
        ++frame.admitted;
        ++totals.admitted;
    }
    else if (config.mode != NativeDrawMode::Strict)
    {
        ++frame.skipped[slot(*reason)];
        ++totals.skipped[slot(*reason)];
    }
    else
    {
        ++frame.rejected[slot(*reason)];
        ++totals.rejected[slot(*reason)];
        if (!failure)
        {
            failure = NativeDrawFailure{ submission, *reason };
            raised  = failure;
        }
    }
}

bool NativeDrawState::ReadBlock(const std::uint8_t* base, std::uint32_t address, std::span<std::uint8_t> bytes) const
{
    if (config.readBlock)
        return config.readBlock(base, address, bytes);
    if (bytes.size() % 4 || address > 0xFFFFFFFFU - bytes.size())
        return false;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4)
    {
        std::uint32_t value = 0;
        if (!config.read(base, static_cast<std::uint32_t>(address + offset), value))
            return false;
        for (unsigned i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }
    return true;
}

void NativeDrawState::Finish(Pending& pending, RootEnd end) noexcept
{
    if (pending.submission.deferral && sequencer)
    {
        try
        {
            struct Ended
            {
                NativeDrawSubmission                            submission;
                std::array<std::uint32_t, kNativeDrawHookCount> calls;
                RootEnd                                         end;
            };

            auto ended = std::make_shared<Ended>(Ended{ pending.submission, pending.calls, end });
            sequencer->Post([this, ended]
                            {
                                Conclude(ended->submission, ended->calls, ended->end);
                            });
            return;
        }
        catch (...)
        {
            // Without a post, the root concludes here once the roots before it have.
            try
            {
                sequencer->Drain();
            }
            catch (...)
            {
            }
        }
    }
    Conclude(pending.submission, pending.calls, end);
}

void NativeDrawState::Conclude(NativeDrawSubmission& submission, const std::array<std::uint32_t, kNativeDrawHookCount>& calls, RootEnd end) noexcept
{
    if (expectedChild(submission.hook) && submission.nested != 1)
        submission.nestingMismatch = true;
    const bool unwound = end == RootEnd::Unwound || end == RootEnd::Discarded;
    if (submission.deferral)
    {
        try
        {
            if (!unwound && config.began)
                config.began(submission);
            else if (unwound && config.abandoned)
                config.abandoned(submission);
        }
        catch (...)
        {
        }
    }
    std::optional<NativeDrawReason> reason;
    if (end == RootEnd::Uncommitted)
        reason = NativeDrawReason::UncommittedSinkDraw;
    else if (!unwound)
    {
        try
        {
            // The commit's build runs before admission reads its result.
            if (submission.commit && config.committed)
                config.committed(submission);
        }
        catch (...)
        {
        }
        try
        {
            reason = Classify(submission);
        }
        catch (...)
        {
            reason = NativeDrawReason::DispatchFailure;
        }
    }
    std::optional<NativeDrawFailure> raised;
    bool                             counted = false;
    std::optional<NativeDrawReason>  verdict;
    try
    {
        std::lock_guard lock(mutex);
        // A failed direct lowering staged nothing the guest publishes; its
        // caller draws through its own fallback instead.
        addCounters(frame, calls, submission, end == RootEnd::Unwound);
        addCounters(totals, calls, submission, end == RootEnd::Unwound);
        // An unwound Original produced no complete guest effect to submit.
        if (unwound)
            return;
        Count(submission, reason, raised);
        counted = true;
        verdict = reason;
        const Identity identity{ submission.hook, submission.caller, submission.vertexShader, submission.pixelShader, submission.declaration };
        if (const auto found = firstSeenIndex.find(identity); found != firstSeenIndex.end())
            ++firstSeen[found->second].occurrences;
        else if (firstSeen.size() < config.firstSeenCapacity)
        {
            firstSeen.push_back({ submission, reason, 1 });
            firstSeenIndex.emplace(identity, firstSeen.size() - 1);
        }
        else
            ++firstSeenDropped;
    }
    catch (...)
    {
        // Bookkeeping never unwinds into guest code; the lost record has its own reason.
        try
        {
            std::lock_guard lock(mutex);
            if (counted)
            {
                if (firstSeen.size() > firstSeenIndex.size())
                    firstSeen.pop_back();
                ++firstSeenDropped;
            }
            else
            {
                Count(submission, NativeDrawReason::DispatchFailure, raised);
                counted = true;
                verdict = NativeDrawReason::DispatchFailure;
            }
        }
        catch (...)
        {
        }
    }
    if (counted && config.recorded)
    {
        try
        {
            config.recorded(submission, verdict);
        }
        catch (...)
        {
        }
    }
    if (!raised)
        return;
    try
    {
        if (config.failed)
            config.failed(*raised);
        else
            REXLOG_ERROR("Native draw dispatch rejected {} from {:#010x}: {}; VS {:#010x}, PS {:#010x}",
                         NativeDrawHookName(raised->submission.hook),
                         raised->submission.caller,
                         NativeDrawReasonName(raised->reason),
                         raised->submission.vertexShader,
                         raised->submission.pixelShader);
    }
    catch (...)
    {
    }
}

namespace
{

Pending::~Pending()
{
    if (!depth)
        return;
    depth        = 0;
    auto* spare  = std::exchange(this->spare, nullptr);
    bool  queued = false;
    if (owner && held && spare)
    {
        // Copies the submission's words and shared pointers; nothing allocates.
        spare->owner         = owner;
        spare->pending       = *this;
        spare->pending.owner = nullptr;
        spare->pending.held  = false;
        try
        {
            std::lock_guard lock(orphanMutex);
            (orphanTail ? orphanTail->next : orphanHead) = spare;
            orphanTail                                   = spare;
            orphanCount.fetch_add(1, std::memory_order_release);
            queued = true;
        }
        catch (...)
        {
        }
    }
    if (!queued)
    {
        delete spare;
        // An unwound root only adds counters under the dispatcher's mutex.
        if (owner)
            owner->Finish(*this, RootEnd::Unwound);
    }
    owner = nullptr;
    // Released after queueing, so removal that waits for it also drains it.
    inFlight.fetch_sub(1, std::memory_order_release);
}

// Refuse owner's queued orphans oldest-first outside list lock, on live
// rootless draw-entry/removal thread. Hold in-flight count and inside-hook
// state across batch, so callback removal returns false immediately.
void drainOrphans(NativeDrawState* owner) noexcept
{
    if (!orphanCount.load(std::memory_order_acquire))
        return;
    Orphan* ordered = nullptr;
    try
    {
        std::lock_guard lock(orphanMutex);
        ordered = unlinkOrphans(owner);
    }
    catch (...)
    {
        return;
    }
    const bool inside      = nativeDrawThreadActive;
    nativeDrawThreadActive = true;
    while (ordered)
    {
        std::unique_ptr<Orphan> orphan(std::exchange(ordered, ordered->next));
        orphan->owner->Finish(orphan->pending, RootEnd::Uncommitted);
    }
    nativeDrawThreadActive = inside;
}

void finishRoot(Pending& pending, RootEnd end) noexcept
{
    pending.depth = 0;
    pending.held  = false;
#if REREVVED_DEVELOPER
    if (std::exchange(pending.effectProbe, false))
        if (auto* probe = drawEffectProbe.load(std::memory_order_relaxed))
            probe->AbandonRoot();
#endif
    delete std::exchange(pending.spare, nullptr);
    if (pending.owner)
        pending.owner->Finish(pending, end);
    pending.owner = nullptr;
    inFlight.fetch_sub(1, std::memory_order_release);
    // Cleared last so a Finish callback that calls Remove sees it is inside a hook.
    nativeDrawThreadActive = false;
}

} // namespace

std::uint32_t NativeDrawScope::EntryStack() const noexcept
{
    return context ? context->r1.u32 : 0;
}

std::uint32_t NativeDrawScope::EntryPcr() const noexcept
{
    return context ? context->r13.u32 : 0;
}

NativeDrawToken BeginNativeDraw(NativeDrawHook hook, const PPCContext& ctx, const std::uint8_t* base) noexcept
{
    auto&      pending    = active;
    const int  exceptions = std::uncaught_exceptions();
    const auto stack      = ctx.r1.u32;
    if (pending.depth)
    {
        // A callee runs on a deeper guest stack. An entry at or above the root's
        // frame means a non-C++ unwind skipped the root's scope, or that a held
        // root's caller moved on without committing.
        if (stack >= pending.rootStack)
            finishRoot(pending, pending.held ? RootEnd::Uncommitted : RootEnd::Unwound);
        else
        {
            // Same-thread nesting folds into the root submission; Original still runs.
            ++pending.depth;
            ++pending.calls[slot(hook)];
            if (drawCensusEnabled.load(std::memory_order_relaxed)) [[unlikely]]
                drawCensusNested[slot(hook)].fetch_add(1, std::memory_order_relaxed);
            auto&      submission = pending.submission;
            const auto caller     = static_cast<std::uint32_t>(ctx.lr);
            if (++submission.nested == 1)
            {
                submission.child       = hook;
                submission.childCaller = caller;
            }
            const auto expected = expectedChild(submission.hook);
            if (!expected || submission.nested != 1 || pending.depth != 2 || expected->first != hook || expected->second != caller)
                submission.nestingMismatch = true;
            return { pending.generation, false, exceptions };
        }
    }
    if (!nativeDrawDispatcher.load(std::memory_order_relaxed))
        return {};
    // Pairs with the store and counter load in RemoveNativeDrawDispatcher.
    inFlight.fetch_add(1, std::memory_order_seq_cst);
    auto* dispatcher = nativeDrawDispatcher.load(std::memory_order_seq_cst);
    if (!dispatcher)
    {
        inFlight.fetch_sub(1, std::memory_order_release);
        return {};
    }
    auto* state = NativeDrawAccess::State(*dispatcher);
    drainOrphans(state);
    pending                    = {};
    pending.owner              = state;
    pending.depth              = 1;
    pending.generation         = ++generations;
    pending.rootStack          = stack;
    pending.base               = base;
    pending.calls[slot(hook)]  = 1;
    auto& submission           = pending.submission;
    submission.hook            = hook;
    submission.caller          = static_cast<std::uint32_t>(ctx.lr);
    submission.thread          = std::this_thread::get_id();
    submission.stack           = stack;
    submission.arguments       = { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 };
    submission.nestingMismatch = orphanChild(hook, submission.caller);
    const auto graphics        = submission.Graphics();
    const auto read            = state->config.read;
#if REREVVED_DEVELOPER
    // Every root of any hook moves the probe's count, so a sample can tell that
    // another draw ran while it was open.
    auto* const effectProbe = drawEffectProbe.load(std::memory_order_relaxed);
    if (effectProbe) [[unlikely]]
        effectProbe->NoteRoot();
#endif
    if (drawCensusEnabled.load(std::memory_order_relaxed)) [[unlikely]]
    {
        // The tiling byte is the top byte of the big-endian word at G+0x2ABC.
        const auto    h    = slot(hook);
        std::uint32_t word = 0;
        drawCensusRoots[h].fetch_add(1, std::memory_order_relaxed);
        if (graphics && graphics <= 0xFFFFFFFFU - kNativeDrawTilingFlagsOffset && read(base, graphics + kNativeDrawTilingFlagsOffset, word))
            drawCensusTile[h][word >> 24].fetch_add(1, std::memory_order_relaxed);
        else
            drawCensusUnread[h].fetch_add(1, std::memory_order_relaxed);
    }
    submission.stateRead = graphics && graphics <= 0xFFFFFFFFU - 0x319C && read(base, graphics + 0x3198, submission.vertexShader) &&
                           read(base, graphics + 0x3194, submission.pixelShader) &&
                           read(base, graphics + 0x2E2C, submission.declaration);
    // The lowerer flushes and zeroes the dirty groups before the commit, so
    // a held root keeps the shadow its draw state was set in.
    if (NativeDrawDefersToCommit(hook, submission.caller) && submission.stateRead && graphics <= 0xFFFFFFFFU - 0x3500)
    {
        try
        {
            auto shadow = std::make_shared<std::vector<std::uint8_t>>(0x3500);
            if (state->ReadBlock(base, graphics, *shadow))
                submission.entryState = std::move(shadow);
        }
        catch (...)
        {
        }
    }
    nativeDrawThreadActive = true;
    if (state->sequencer)
    {
        // A held lowerer root builds at its commit, which follows Original.
        if (state->config.defer && !NativeDrawDefersToCommit(hook, submission.caller))
        {
            try
            {
                submission.deferral = state->config.defer(submission);
            }
            catch (...)
            {
                submission.deferral = nullptr;
            }
        }
        if (!submission.deferral)
        {
            try
            {
                state->sequencer->Drain();
            }
            catch (...)
            {
            }
        }
    }
    if (state->config.began && !submission.deferral)
    {
        try
        {
            state->config.began(submission);
        }
        catch (...)
        {
        }
    }
#if REREVVED_DEVELOPER
    // Last before Original: the sampled copy of G is taken after every observer above.
    if (effectProbe && hook == NativeDrawHook::IndexedBuffer) [[unlikely]]
        pending.effectProbe = effectProbe->BeginRoot({ read, state->config.readBlock }, base, graphics, inFlight);
#endif
    return { pending.generation, true, exceptions };
}

void EndNativeDraw(const NativeDrawToken& token, const PPCContext& ctx) noexcept
{
    auto& pending = active;
    // A stale token belongs to a root already abandoned by a skipped scope.
    if (!pending.depth || token.generation != pending.generation)
        return;
    const bool unwinding = std::uncaught_exceptions() > token.exceptions;
    if (!token.root)
    {
        if (--pending.depth == 1 && pending.submission.nested == 1)
            pending.submission.childResult = ctx.r3.u32;
        pending.unwound |= unwinding;
        return;
    }
    pending.submission.result = ctx.r3.u32;
    const bool unwound        = unwinding || pending.unwound;
    // The root's Original, with every nested draw it made, has returned. A live
    // root always has an owner: it is cleared only with the depth.
#if REREVVED_DEVELOPER
    if (std::exchange(pending.effectProbe, false))
        if (auto* probe = drawEffectProbe.load(std::memory_order_relaxed))
        {
            if (unwound)
                probe->AbandonRoot();
            else
                probe->EndRoot({ pending.owner->config.read, pending.owner->config.readBlock }, pending.base, inFlight);
        }
#endif
    if (!unwound && NativeDrawDefersToCommit(pending.submission.hook, pending.submission.caller))
    {
        // 0x826A3544: a failed reservation returns 0x8007000E and stages nothing.
        if (pending.submission.result)
        {
            finishRoot(pending, RootEnd::Discarded);
            return;
        }
        // 0x826A3524: G+0x3484 holds the staged PM4 end the commit publishes.
        const auto graphics = pending.submission.Graphics();
        if (pending.owner && graphics <= 0xFFFFFFFFU - 0x3488)
            static_cast<void>(pending.owner->config.read(pending.base, graphics + 0x3484, pending.pendingEnd));
        // Without its exit node a held root could not be refused at thread
        // exit, so it is refused now instead.
        pending.spare = new (std::nothrow) Orphan;
        if (!pending.spare)
        {
            finishRoot(pending, RootEnd::Uncommitted);
            return;
        }
        pending.held = true;
        return;
    }
    finishRoot(pending, unwound ? RootEnd::Unwound : RootEnd::Complete);
}

void ObserveNativeDrawCommit(const PPCContext& ctx, const std::uint8_t* base) noexcept
{
    auto& pending = active;
    // A commit after a direct NonindexedSink call, or with no held root, is not ours.
    if (pending.depth != 1 || !pending.held || !pending.owner)
        return;
    auto&      submission = pending.submission;
    const auto graphics   = submission.Graphics();
    // The caller commits from the frame that called the lowerer, on its device.
    if (ctx.r3.u32 != graphics || ctx.r1.u32 != pending.rootStack)
    {
        finishRoot(pending, RootEnd::Uncommitted);
        return;
    }
    const auto       read = pending.owner->config.read;
    NativeDrawCommit commit;
    commit.pendingEnd  = pending.pendingEnd;
    std::uint32_t slot = 0;
    commit.read        = graphics <= 0xFFFFFFFFU - 0x3498 && pending.rootStack <= 0xFFFFFFFFU - 88 &&
                         read(base, graphics + 0x3484, commit.commitEnd) && read(base, graphics + 0x3488, commit.vertexDest) &&
                         read(base, graphics + 0x348C, commit.indexDest) && read(base, graphics + 0x3490, commit.vertexDwords) &&
                         read(base, graphics + 0x3494, commit.indexDwords) && read(base, pending.rootStack + 84, slot) &&
                         read(base, slot, commit.vertexOut) && read(base, submission.arguments[7], commit.indexOut) &&
                         read(base, graphics + 0x3198, commit.vertexShader) && read(base, graphics + 0x3194, commit.pixelShader) &&
                         read(base, graphics + 0x2E2C, commit.declaration);
    submission.commit  = commit;
    // The commit publishes the held draw only while G+0x3484 still ends it.
    finishRoot(pending, commit.read && commit.commitEnd != commit.pendingEnd ? RootEnd::Uncommitted : RootEnd::Complete);
}

bool InstallNativeDrawDispatcher(NativeDrawDispatcher& dispatcher) noexcept
{
    // A withdrawn dispatcher keeps `removing` until a removal drains its
    // orphans; installing another first would let its removal replace it.
    if (removalHeld)
        return false;
    std::unique_lock serial(removalMutex, std::defer_lock);
    try
    {
        if (!serial.try_lock() || removing.load(std::memory_order_seq_cst))
            return false;
    }
    catch (...)
    {
        return false;
    }
    NativeDrawDispatcher* expected = nullptr;
    return nativeDrawDispatcher.compare_exchange_strong(expected, &dispatcher, std::memory_order_seq_cst);
}

namespace detail
{

void (*nativeDrawRemovalPause)() = nullptr;

} // namespace detail

namespace
{

bool hasOrphans(NativeDrawDispatcher* dispatcher) noexcept
{
    if (!dispatcher || !orphanCount.load(std::memory_order_acquire))
        return false;
    const auto* owner = NativeDrawAccess::State(*dispatcher);
    try
    {
        std::lock_guard lock(orphanMutex);
        for (const Orphan* node = orphanHead; node; node = node->next)
            if (node->owner == owner)
                return true;
        return false;
    }
    catch (...)
    {
        return true;
    }
}

// Withdraws the installed dispatcher and leaves it for a removal to drain.
// The caller holds removalMutex, so no other removal can return in between.
void withdrawForDrain() noexcept
{
    if (auto pause = detail::nativeDrawRemovalPause)
        pause();
    if (auto* removed = nativeDrawDispatcher.exchange(nullptr, std::memory_order_seq_cst))
        removing.store(removed, std::memory_order_seq_cst);
}

} // namespace

bool RemoveNativeDrawDispatcher(std::chrono::milliseconds timeout)
{
    // Only the removalMutex holder changes the installed pointer; a hook or
    // drain callback withdraws without waiting.
    if (nativeDrawThreadActive)
    {
        std::unique_lock serial(removalMutex, std::defer_lock);
        if (!removalHeld && serial.try_lock())
            withdrawForDrain();
        return false;
    }
    // A concurrent removal waits, within its own timeout, for this one.
    const auto       deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock serial(removalMutex, std::defer_lock);
    if (!serial.try_lock_until(deadline))
        return false;
    withdrawForDrain();
    while (inFlight.load(std::memory_order_seq_cst))
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    // Every root posted before the withdrawal concludes before the return;
    // past the deadline the withdrawn dispatcher awaits a later removal.
    if (auto* removed = removing.load(std::memory_order_seq_cst); removed && !removed->Drain(deadline))
        return false;
    // Past the deadline its orphans stay queued for a later removal. A
    // refusal callback can still delay the return; ours do not block.
    if (std::chrono::steady_clock::now() >= deadline && hasOrphans(removing.load(std::memory_order_seq_cst)))
        return false;
    // No root is in flight, so every exited thread's orphan is queued; a
    // removal that took the lock after this one finds nothing to drain.
    if (auto* removed = removing.exchange(nullptr, std::memory_order_seq_cst))
    {
        inFlight.fetch_add(1, std::memory_order_seq_cst);
        removalHeld = true;
        drainOrphans(NativeDrawAccess::State(*removed));
        removalHeld = false;
        inFlight.fetch_sub(1, std::memory_order_release);
    }
    return true;
}

NativeDrawRootView CurrentNativeDrawRoot() noexcept
{
    const auto& pending = active;
    if (pending.depth != 1 || !pending.owner)
        return {};
    return { pending.owner->config.read, pending.base, pending.effectProbe, pending.owner->config.write, pending.owner->config.writable };
}

void NoteNativeDrawConsumedChild(const NativeDrawScope& scope) noexcept
{
    auto& pending = active;
    // A scope whose root a later draw abandoned names an older generation.
    if (!pending.owner || scope.Generation() != pending.generation)
        return;
    auto&      submission = pending.submission;
    const auto expected   = expectedChild(submission.hook);
    if (!expected || submission.nested)
        return;
    // As BeginNativeDraw folds a child, which the skipped call would have entered.
    ++pending.calls[slot(expected->first)];
    if (drawCensusEnabled.load(std::memory_order_relaxed)) [[unlikely]]
        drawCensusNested[slot(expected->first)].fetch_add(1, std::memory_order_relaxed);
    // childResult stays zero, the lowerer's success value; nothing reads it.
    submission.nested      = 1;
    submission.child       = expected->first;
    submission.childCaller = expected->second;
}

void EnableNativeDrawCensus(bool enabled) noexcept
{
    drawCensusEnabled.store(enabled, std::memory_order_relaxed);
}

NativeDrawCensus TakeNativeDrawCensus() noexcept
{
    NativeDrawCensus census;
    for (std::size_t h = 0; h < kNativeDrawHookCount; ++h)
    {
        census.roots[h]  = drawCensusRoots[h].exchange(0, std::memory_order_relaxed);
        census.nested[h] = drawCensusNested[h].exchange(0, std::memory_order_relaxed);
        census.unread[h] = drawCensusUnread[h].exchange(0, std::memory_order_relaxed);
        for (std::size_t b = 0; b < 256; ++b)
            census.tile[h][b] = drawCensusTile[h][b].exchange(0, std::memory_order_relaxed);
    }
    return census;
}

std::string FormatNativeDrawCensus(const NativeDrawCensus& census)
{
    std::string fields;
    const auto  name = [](std::size_t h)
    {
        return NativeDrawHookName(static_cast<NativeDrawHook>(h));
    };
    for (std::size_t h = 0; h < kNativeDrawHookCount; ++h)
        fields += std::format(" draw_root_{}={}", name(h), census.roots[h]);
    for (std::size_t h = 0; h < kNativeDrawHookCount; ++h)
        if (census.nested[h])
            fields += std::format(" draw_nested_{}={}", name(h), census.nested[h]);
    for (std::size_t h = 0; h < kNativeDrawHookCount; ++h)
    {
        for (std::size_t b = 0; b < 256; ++b)
            if (census.tile[h][b])
                fields += std::format(" draw_tile_{}_{:02x}={}", name(h), b, census.tile[h][b]);
        if (census.unread[h])
            fields += std::format(" draw_tile_{}_unread={}", name(h), census.unread[h]);
    }
    return fields;
}

const char* NativeDrawHookName(NativeDrawHook hook) noexcept
{
    switch (hook)
    {
        case NativeDrawHook::IndexedBuffer:
            return "indexed_buffer";
        case NativeDrawHook::IndexedUp:
            return "indexed_up";
        case NativeDrawHook::NonindexedUp:
            return "nonindexed_up";
        case NativeDrawHook::IndexedUpLowerer:
            return "indexed_up_lowerer";
        case NativeDrawHook::NonindexedSink:
            return "nonindexed_sink";
        case NativeDrawHook::AutomaticIndex:
            return "automatic_index";
    }
    return "unknown";
}

const char* NativeDrawReasonName(NativeDrawReason reason) noexcept
{
    switch (reason)
    {
        case NativeDrawReason::NoNativeRoute:
            return "no_native_route";
        case NativeDrawReason::NestingMismatch:
            return "nesting_mismatch";
        case NativeDrawReason::AutomaticIndex:
            return "automatic_index";
        case NativeDrawReason::StateUnreadable:
            return "state_unreadable";
        case NativeDrawReason::DispatchFailure:
            return "dispatch_failure";
        case NativeDrawReason::UncommittedSinkDraw:
            return "uncommitted_sink_draw";
    }
    return "unknown";
}

void DrainNativeDraws() noexcept
{
    if (!nativeDrawDispatcher.load(std::memory_order_relaxed))
        return;
    // As a draw entry, so a removal waits for the drain.
    inFlight.fetch_add(1, std::memory_order_seq_cst);
    if (auto* dispatcher = nativeDrawDispatcher.load(std::memory_order_seq_cst))
    {
        try
        {
            dispatcher->Drain();
        }
        catch (...)
        {
        }
    }
    inFlight.fetch_sub(1, std::memory_order_release);
}

NativeDrawDispatcher::NativeDrawDispatcher(NativeDrawDispatchConfig config)
: state(std::make_unique<NativeDrawState>())
{
    state->config = std::move(config);
    if (state->config.sequenced)
        state->sequencer = std::make_unique<DrawSequencer>(state->config.sequencerCapacity);
    if (!state->config.read)
    {
        state->config.read = readKernelWord;
        if (!state->config.readBlock)
            state->config.readBlock = readKernelBlock;
        if (!state->config.write && !state->config.writable)
        {
            state->config.write    = writeKernelWord;
            state->config.writable = writableKernel;
        }
    }
}

NativeDrawDispatcher::~NativeDrawDispatcher()
{
    if (state->sequencer)
        state->sequencer->Drain();
    // An orphan left by a removal that timed out cannot outlive its owner;
    // other dispatchers' nodes keep their place and order.
    auto* self = this;
    removing.compare_exchange_strong(self, nullptr, std::memory_order_seq_cst);
    Orphan* own = nullptr;
    try
    {
        std::lock_guard lock(orphanMutex);
        own = unlinkOrphans(state.get());
    }
    catch (...)
    {
    }
    while (own)
        delete std::exchange(own, own->next);
}

NativeDrawFrameCounters NativeDrawDispatcher::CloseFrame()
{
    std::lock_guard lock(state->mutex);
    auto            closed = state->frame;
    state->frame           = {};
    state->frame.frame     = closed.frame + 1;
    state->totals.frame    = state->frame.frame;
    return closed;
}

NativeDrawFrameCounters NativeDrawDispatcher::CurrentFrame() const
{
    std::lock_guard lock(state->mutex);
    return state->frame;
}

NativeDrawFrameCounters NativeDrawDispatcher::Totals() const
{
    std::lock_guard lock(state->mutex);
    return state->totals;
}

std::vector<NativeDrawFirstSeen> NativeDrawDispatcher::FirstSeen() const
{
    std::lock_guard lock(state->mutex);
    return state->firstSeen;
}

std::uint64_t NativeDrawDispatcher::NumberedThrough() const
{
    std::lock_guard lock(state->mutex);
    return state->sequence;
}

std::uint64_t NativeDrawDispatcher::FirstSeenDropped() const
{
    std::lock_guard lock(state->mutex);
    return state->firstSeenDropped;
}

void NativeDrawDispatcher::Drain()
{
    if (state->sequencer)
        state->sequencer->Drain();
}

bool NativeDrawDispatcher::Drain(std::chrono::steady_clock::time_point deadline)
{
    return !state->sequencer || state->sequencer->Drain(deadline);
}

NativeDrawSequencerStats NativeDrawDispatcher::Sequencer() const
{
    return state->sequencer ? state->sequencer->Stats() : NativeDrawSequencerStats{};
}

std::optional<NativeDrawFailure> NativeDrawDispatcher::Failure() const
{
    std::lock_guard lock(state->mutex);
    return state->failure;
}

} // namespace rerevved::gpu::guest_d3d
