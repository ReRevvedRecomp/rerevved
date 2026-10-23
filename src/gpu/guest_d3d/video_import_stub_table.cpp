#include "video_import_stub_table.h"

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

REXCVAR_DEFINE_BOOL(native_video_hsio_stub,
                    false,
                    "ReRevved",
                    "Skip only the VdIsHSIOTrainingSucceeded import at 0x826A65A4 in the owner 0x826A63E8 and return 1 in the full r3 as the SDK export does, when "
                    "ring stub services are published, the native detached video route and the retained SDK export are present and the call has the owner's "
                    "frame and registers; the owner's cache store and diagnostics still run")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_video_hsio_exit_check,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native: force the HSIO training import to run and compare its result, effects and continuation with the SDK "
                    "contract after every call; observes only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_video_edram_worker_stub,
                    false,
                    "ReRevved",
                    "Skip only the VdRetrainEDRAMWorker import at 0x826B053C in the owner 0x826B0508 and return 0 in the full r3 as the SDK export does, when "
                    "ring stub services are published, the native detached video route and the retained SDK export are present and the call has the owner's "
                    "frame and registers; the owner's retained retrain and critical section still run")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_video_edram_worker_exit_check,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native: force the EDRAM worker import to run and compare its result, arguments and continuation with the SDK "
                    "contract after every call; observes only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

namespace
{

using R = VideoImportStubRefusal;

constexpr std::size_t indexOf(VideoImportId which) noexcept
{
    return static_cast<std::size_t>(which);
}

constexpr std::uint32_t low32(std::uint64_t value) noexcept
{
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t kOkLogLimit = 16;

// One import's event counters, gauge and bounds. The two imports share nothing.
struct Counters
{
    std::atomic<std::uint64_t>                                        enter{ 0 }, readyAtEntry{ 0 }, consumed{ 0 }, original{ 0 };
    std::array<std::atomic<std::uint64_t>, kVideoImportRefusalCount>  refusal{};
    std::atomic<std::uint64_t>                                        exitOk{ 0 }, exitUnreadable{ 0 }, exitUnpaired{ 0 }, reentry{ 0 }, abandoned{ 0 }, correlationLost{ 0 }, reportingFailed{ 0 }, finalSkipped{ 0 };
    std::array<std::atomic<std::uint64_t>, kVideoImportMismatchCount> mismatch{};
    // Started calls whose observations have not finished. A gauge: it survives a drain.
    std::atomic<std::uint64_t> pending{ 0 };
    std::atomic<std::uint64_t> nextInvocation{ 1 };
    std::atomic<std::uint32_t> pendingLimit{ kVideoImportMaxPending };
    std::atomic<std::uint32_t> okLogged{ 0 };
    // An incomplete observation or a failed report that no drain forgets.
    std::atomic<bool> unresolved{ false };
};

Counters counters[kVideoImportCount];

std::atomic<std::uint64_t>                    nextGeneration{ 1 };
std::timed_mutex                              reportMutex;
std::atomic<const VideoImportStubOperations*> testOperations{ nullptr };
std::atomic<void (*)()>                       reserveProbe{ nullptr };

// Each import has one live call per thread; nested or unpaired records are
// inconclusive and keep weak delegate/service references.
struct Record
{
    bool                      active = false, poisoned = false, replaced = false, providerValid = false, readyAtEntry = false, identityChanged = false;
    bool                      frameValid = false, cacheEntryValid = false, cacheEffectValid = false, resultValid = false, effectValid = false, preValid = false, postValid = false;
    bool                      preStackValid = false, postStackValid = false;
    bool                      returnBad = false, effectBad = false, continuationBad = false, lrBad = false;
    std::uint8_t              stage      = 0;
    const VideoImportStubRow* row        = nullptr;
    std::uint64_t             invocation = 0, generation = 0, hostThread = 0, spFull = 0, gFull = 0, guestThread = 0;
    std::uint64_t             entryR3 = 0, entryR11 = 0, entryR29 = 0, entryR30 = 0, entryLr = 0;
    std::uint64_t             resultFull = 0, effectR11 = 0, postR3 = 0, postR11 = 0, postR30 = 0, postLr = 0;
    std::uint32_t             pc = 0, cacheEntry = 0, cacheEffect = 0, effectCr0 = 0, postCr0 = 0;
    std::uint64_t             pre[6]{}, post[5]{}; // worker r3..r8 and r4..r8
    std::uint32_t             preStack[2]{}, postStack[2]{};
    VideoImportIdentity       identity;
    std::weak_ptr<const void> delegateBinding, ringBinding;
};

thread_local Record        records[kVideoImportCount];
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
// before it is used, and the row names the import whose dispatcher entry is read.
VideoImportEnvironment productionEnvironment(const VideoImportStubRow& row)
{
    VideoImportEnvironment environment;
    auto*                  kernel  = rex::system::kernel_state();
    auto*                  runtime = kernel ? kernel->emulator() : nullptr;
    if (!runtime)
        return environment;
    environment.runtime           = true;
    environment.identity.runtime  = addressOf(runtime);
    environment.graphics          = runtime->graphics_system() != nullptr;
    const auto delegate           = runtime->detached_video_delegate();
    environment.delegate          = delegate != nullptr;
    environment.identity.delegate = addressOf(delegate.get());
    environment.delegateOwner     = delegate;
    if (auto* dispatcher = runtime->function_dispatcher())
    {
        environment.dispatcher      = true;
        environment.target          = dispatcher->GetFunction(row.importAddress);
        environment.identity.target = addressOf(reinterpret_cast<const void*>(environment.target));
    }
    const auto ring           = CurrentRingStubServices();
    environment.ring          = ring != nullptr;
    environment.identity.ring = addressOf(ring.get());
    environment.ringOwner     = ring;
    return environment;
}

const VideoImportStubOperations& productionOperations()
{
    static const VideoImportStubOperations operations{
        { readKernel32 },
        productionEnvironment,
        [](std::string_view message)
        {
            REXLOG_INFO("{}", message);
        },
    };
    return operations;
}

const VideoImportStubOperations& activeOperations()
{
    const auto* test = testOperations.load(std::memory_order_acquire);
    return test ? *test : productionOperations();
}

bool stubRequested(VideoImportId which) noexcept
{
    return which == VideoImportId::Hsio ? VideoHsioStubRequested() : VideoEdramWorkerStubRequested();
}

bool exitCheckRequested(VideoImportId which) noexcept
{
    return which == VideoImportId::Hsio ? VideoHsioExitCheckRequested() : VideoEdramWorkerExitCheckRequested();
}

bool importActive(VideoImportId which) noexcept
{
    return which == VideoImportId::Hsio ? VideoHsioActive() : VideoEdramWorkerActive();
}

std::string_view prefixOf(VideoImportId which) noexcept
{
    return which == VideoImportId::Hsio ? "video_hsio_" : "video_edram_worker_";
}

// The row's readiness policy for the device in r31.
bool rowReady(const VideoImportStubRow& row, const VideoImportStubCall& call) noexcept
{
    switch (row.readiness)
    {
        case VideoImportReadiness::RingStubsReady:
            return RingStubsReady(low32(call.r31));
    }
    return false;
}

// The route the lease and the probe describe: services, not the provider.
bool environmentAdmits(const VideoImportEnvironment& environment) noexcept
{
    return environment.runtime && !environment.graphics && environment.delegate && environment.ring && environment.identity.runtime != 0 && environment.identity.delegate != 0 && environment.identity.ring != 0;
}

bool providerAdmits(const VideoImportEnvironment& environment, const VideoImportStubRow& row) noexcept
{
    return environment.dispatcher && environment.target != nullptr && environment.target == row.wrapper;
}

bool rowIsCanonical(const VideoImportStubRow& row) noexcept
{
    const auto* table = FindVideoImportStubRow(row.callPC, row.import);
    return table && table->truePC == row.truePC && table->fullR3 == row.fullR3 && table->predicate == row.predicate && table->importAddress == row.importAddress && table->wrapper == row.wrapper &&
           table->frameBytes == row.frameBytes && table->readiness == row.readiness;
}

// The frame [SP, SP + frameBytes) is aligned, not zero and does not wrap the 32-bit address space.
bool frameLayoutAllows(const VideoImportStubRow& row, std::uint32_t sp) noexcept
{
    return sp != 0 && (sp & 15U) == 0 && std::uint64_t{ sp } + row.frameBytes <= 0x100000000ULL;
}

void readFrame(const VideoImportStubRow& row, std::uint32_t sp, const VideoImportStubMemory& memory, VideoImportEntrySnapshot& snapshot)
{
    snapshot.frameAttempted = true;
    snapshot.frameValid     = false;
    for (std::uint32_t i = 0; i < row.frameBytes / 4; ++i)
        if (!memory.read32(sp + 4 * i, snapshot.frame[i]))
            return;
    snapshot.frameValid = true;
}

void readCache(const VideoImportStubRow& row, const VideoImportStubMemory& memory, VideoImportEntrySnapshot& snapshot)
{
    if (row.import != VideoImportId::Hsio)
        return;
    snapshot.cacheAttempted = true;
    snapshot.cacheValid     = memory.read32(kVideoHsioCacheAddress, snapshot.cache);
}

bool sameOwner(const std::weak_ptr<const void>& binding, const std::shared_ptr<const void>& current) noexcept
{
    return !binding.expired() && !binding.owner_before(current) && !current.owner_before(binding);
}

// The publication the entry took its lease on, and the row's readiness when the
// entry had it, against the present state. Neither needs the lease: a loss only
// makes the pair unusable.
bool servicesStable(const Record& r) noexcept
{
    return RingStubsPublished() && (!r.readyAtEntry || RingStubsReady(low32(r.gFull)));
}

// The integers, the weak ownership and the services of the entry against a current probe.
bool bindingStable(const Record& r, const VideoImportEnvironment& environment) noexcept
{
    return environment.identity == r.identity && sameOwner(r.delegateBinding, environment.delegateOwner) && sameOwner(r.ringBinding, environment.ringOwner) && servicesStable(r);
}

constexpr std::uint64_t add64(std::uint64_t base, std::int64_t offset) noexcept
{
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(base) + offset);
}

void noteUnresolved(Counters& c) noexcept
{
    c.unresolved.store(true, std::memory_order_relaxed);
}

void countReportingFailure(Counters& c) noexcept
{
    c.reportingFailed.fetch_add(1, std::memory_order_relaxed);
    noteUnresolved(c);
}

std::string hexOf(std::uint64_t value)
{
    return std::format("{:016X}", value);
}

void logRecord(const VideoImportStubOperations& operations, VideoImportId which, const Record& r, std::string_view verdict) noexcept
{
    auto& c = counters[indexOf(which)];
    try
    {
        if (!operations.log)
            return;
        if (verdict == "ok" && c.okLogged.fetch_add(1, std::memory_order_relaxed) >= kOkLogLimit)
            return;
        const bool hsio = which == VideoImportId::Hsio;
        auto       line = std::format("Native video {} check:", hsio ? "HSIO" : "EDRAM worker");
        const auto put  = [&](std::string_view name, const auto& value)
        {
            line += std::format(" {}={}", name, value);
        };
        const auto put64 = [&](std::string_view name, std::uint64_t value)
        {
            line += std::format(" {}={:016X}", name, value);
        };
        const auto put32 = [&](std::string_view name, std::uint32_t value)
        {
            line += std::format(" {}={:08X}", name, value);
        };
        const auto flag = [&](std::string_view name, bool value)
        {
            put(name, value ? 1 : 0);
        };
        put("import", hsio ? "hsio" : "edram_worker");
        put32("call_pc", r.row ? r.row->callPC : 0);
        put32("true_pc", r.row ? r.row->truePC : 0);
        put32("pc", r.pc);
        put64("thread", r.guestThread);
        put("host_thread", r.hostThread);
        put("invocation", r.invocation);
        put32("sp", low32(r.spFull));
        put64("sp_full", r.spFull);
        put32("g", low32(r.gFull));
        put64("g_full", r.gFull);
        put64("entry_r3", r.entryR3);
        put64("entry_r11", r.entryR11);
        put64("entry_r29", r.entryR29);
        put64("entry_r30", r.entryR30);
        put64("entry_lr", r.entryLr);
        put64("runtime_id", r.identity.runtime);
        put64("delegate_id", r.identity.delegate);
        put64("ring_service_id", r.identity.ring);
        put("service_generation", r.generation);
        put64("provider_target", r.identity.target);
        flag("provider_valid", r.providerValid && !r.identityChanged);
        flag("ready_at_entry", r.readyAtEntry);
        flag("replaced", r.replaced);
        flag("frame_valid", r.frameValid);
        flag("result_valid", r.resultValid);
        put64("result_full", r.resultFull);
        put("return_lr_valid", 0);
        flag("effect_valid", r.effectValid);
        flag("post_valid", r.postValid);
        put64("post_r3", r.postR3);
        put64("post_lr", r.postLr);
        flag("identity_changed", r.identityChanged);
        if (hsio)
        {
            put32("cache_entry", r.cacheEntry);
            put32("cache_effect", r.cacheEffect);
            put64("effect_r11", r.effectR11);
            put32("effect_cr0", r.effectCr0);
            put64("post_r11", r.postR11);
            put32("post_cr0", r.postCr0);
        }
        else
        {
            for (std::size_t i = 0; i < 6; ++i)
                put64(std::format("pre_r{}", 3 + i), r.pre[i]);
            put32("pre_stack80", r.preStack[0]);
            put32("pre_stack84", r.preStack[1]);
            for (std::size_t i = 0; i < 5; ++i)
                put64(std::format("post_r{}", 4 + i), r.post[i]);
            put32("post_stack80", r.postStack[0]);
            put32("post_stack84", r.postStack[1]);
        }
        put("verdict", verdict);
        operations.log(line);
    }
    catch (...)
    {
        countReportingFailure(c);
    }
}

// Ends the thread's record: inconclusive when `conclusive` is false, the
// record was poisoned, the provider was not valid at the entry or an identity
// changed; never exit_ok then.
void finishRecord(const VideoImportStubOperations& operations, VideoImportId which, bool conclusive) noexcept
{
    auto& c = counters[indexOf(which)];
    auto& r = records[indexOf(which)];
    try
    {
        auto finished = r;
        r             = {};
        c.pending.fetch_sub(1, std::memory_order_relaxed);
        if (!conclusive || finished.poisoned || finished.identityChanged || !finished.providerValid)
        {
            c.exitUnpaired.fetch_add(1, std::memory_order_relaxed);
            noteUnresolved(c);
            logRecord(operations, which, finished, "unpaired");
            return;
        }
        const bool bad[kVideoImportMismatchCount]{ finished.returnBad, finished.effectBad, finished.continuationBad, finished.lrBad };
        bool       any = false;
        for (std::size_t i = 0; i < kVideoImportMismatchCount; ++i)
            if (bad[i])
            {
                c.mismatch[i].fetch_add(1, std::memory_order_relaxed);
                any = true;
            }
        // A difference survives every drain: a later final is never complete.
        if (any)
            noteUnresolved(c);
        const bool hsio       = which == VideoImportId::Hsio;
        const bool unreadable = !finished.frameValid || (hsio ? !finished.cacheEntryValid || !finished.cacheEffectValid : !finished.preStackValid || !finished.postStackValid);
        if (unreadable)
        {
            c.exitUnreadable.fetch_add(1, std::memory_order_relaxed);
            noteUnresolved(c);
        }
        else if (!any)
            c.exitOk.fetch_add(1, std::memory_order_relaxed);
        logRecord(operations, which, finished, unreadable ? "unreadable" : any ? "mismatch"
                                                                               : "ok");
    }
    catch (...)
    {
        r = {};
    }
}

// Closes a stale record of this thread, or refuses a call nested below a live
// one. Runs before the lease so that no log is written under it.
std::optional<R> prepareRecord(const VideoImportStubOperations& operations, VideoImportId which, std::uint32_t sp) noexcept
{
    auto& c = counters[indexOf(which)];
    auto& r = records[indexOf(which)];
    try
    {
        ++touches;
        if (r.active)
        {
            if (sp < low32(r.spFull))
            {
                // Below the live frame: nested, so neither call can be paired.
                r.poisoned = true;
                c.reentry.fetch_add(1, std::memory_order_relaxed);
                return R::Reentrant;
            }
            // The earlier frame is gone; its observations can no longer arrive.
            c.abandoned.fetch_add(1, std::memory_order_relaxed);
            finishRecord(operations, which, false);
        }
        return std::nullopt;
    }
    catch (...)
    {
        r = {};
        return R::Unpaired;
    }
}

// The thread's observation record for this call, or why there is none. Neither
// allocates nor logs.
std::optional<R> armRecord(VideoImportId which, const VideoImportStubRow& row, const VideoImportStubCall& call, const VideoImportEnvironment& environment, bool providerValid, bool ready) noexcept
{
    auto& c = counters[indexOf(which)];
    auto& r = records[indexOf(which)];
    // Reserve the slot first: a separate check and increment would let
    // concurrent entries pass the bound together.
    auto pending = c.pending.load(std::memory_order_relaxed);
    do
    {
        if (pending >= c.pendingLimit.load(std::memory_order_relaxed))
            return R::Unpaired;
        if (auto* probe = reserveProbe.load(std::memory_order_relaxed))
            probe();
    } while (!c.pending.compare_exchange_weak(pending, pending + 1, std::memory_order_relaxed));
    r                 = {};
    r.active          = true;
    r.row             = &row;
    r.invocation      = c.nextInvocation.fetch_add(1, std::memory_order_relaxed);
    r.generation      = nextGeneration.fetch_add(1, std::memory_order_relaxed);
    r.hostThread      = std::hash<std::thread::id>{}(std::this_thread::get_id());
    r.spFull          = call.r1;
    r.gFull           = call.r31;
    r.guestThread     = call.r13;
    r.entryR3         = call.r3;
    r.entryR11        = call.r11;
    r.entryR29        = call.r29;
    r.entryR30        = call.r30;
    r.entryLr         = call.lr;
    r.identity        = environment.identity;
    r.delegateBinding = environment.delegateOwner;
    r.ringBinding     = environment.ringOwner;
    r.providerValid   = providerValid;
    r.readyAtEntry    = ready;
    return std::nullopt;
}

// The live record for a boundary after the register identity, the order and
// the bindings pass; null, with the reason counted, otherwise. Takes no LR.
Record* openBoundary(const VideoImportStubOperations& operations, VideoImportId which, const VideoImportObservation& o, std::uint8_t stage, bool recordRequired) noexcept
{
    auto& c = counters[indexOf(which)];
    auto& r = records[indexOf(which)];
    ++touches;
    if (!r.active)
    {
        if (recordRequired)
        {
            c.correlationLost.fetch_add(1, std::memory_order_relaxed);
            c.exitUnpaired.fetch_add(1, std::memory_order_relaxed);
            noteUnresolved(c);
        }
        return nullptr;
    }
    if (r.spFull != o.r1 || r.gFull != o.r31 || r.guestThread != o.r13 || r.stage != stage)
    {
        r.poisoned = true;
        c.correlationLost.fetch_add(1, std::memory_order_relaxed);
        c.exitUnpaired.fetch_add(1, std::memory_order_relaxed);
        noteUnresolved(c);
        return nullptr;
    }
    r.stage = static_cast<std::uint8_t>(stage + 1);
    // A bound identity that moved since the entry makes the pair unusable. A
    // call that was already unusable at the entry needs no second probe.
    if (r.providerValid && !r.identityChanged)
    {
        const auto environment = operations.environment ? operations.environment(*r.row) : VideoImportEnvironment{};
        r.identityChanged      = !bindingStable(r, environment);
    }
    return &r;
}

// Poisons the live record when an observation failed unexpectedly.
void poisonLive(VideoImportId which) noexcept
{
    auto& r = records[indexOf(which)];
    if (r.active)
        r.poisoned = true;
}

// The observers share this gate: the worker's owner bypass, then inactive
// observers with no live record.
bool observerHasWork(VideoImportId which, const VideoImportObservation& o) noexcept
{
    if (which == VideoImportId::EdramWorker && low32(o.r29) == 0)
        return false;
    if (!importActive(which) && counters[indexOf(which)].pending.load(std::memory_order_relaxed) == 0)
        return false;
    return importActive(which) || records[indexOf(which)].active;
}

} // namespace

bool VideoHsioPredicate(const VideoImportStubCall& call) noexcept
{
    return call.r30 == kVideoHsioR30 && call.r11 == 0;
}

bool VideoEdramWorkerPredicate(const VideoImportStubCall& call) noexcept
{
    return call.r3 == 0 && low32(call.r29) != 0;
}

const VideoImportStubRow* FindVideoImportStubRow(std::uint32_t callPC, VideoImportId import) noexcept
{
    for (const auto& row : kVideoImportStubRows)
        if (row.callPC == callPC && row.import == import)
            return &row;
    return nullptr;
}

bool VideoHsioStubRequested() noexcept
{
    return RingStubsPublished() && REXCVAR_GET(native_video_hsio_stub);
}

bool VideoHsioExitCheckRequested() noexcept
{
    return REXCVAR_GET(native_video_hsio_exit_check);
}

bool VideoHsioActive() noexcept
{
    return VideoHsioExitCheckRequested() || VideoHsioStubRequested();
}

bool VideoHsioStubReportingRequested() noexcept
{
    return REXCVAR_GET(native_video_hsio_stub) || REXCVAR_GET(native_video_hsio_exit_check);
}

bool VideoEdramWorkerStubRequested() noexcept
{
    return RingStubsPublished() && REXCVAR_GET(native_video_edram_worker_stub);
}

bool VideoEdramWorkerExitCheckRequested() noexcept
{
    return REXCVAR_GET(native_video_edram_worker_exit_check);
}

bool VideoEdramWorkerActive() noexcept
{
    return VideoEdramWorkerExitCheckRequested() || VideoEdramWorkerStubRequested();
}

bool VideoEdramWorkerStubReportingRequested() noexcept
{
    return REXCVAR_GET(native_video_edram_worker_stub) || REXCVAR_GET(native_video_edram_worker_exit_check);
}

bool VideoImportStubsReportingRequested() noexcept
{
    return VideoHsioStubReportingRequested() || VideoEdramWorkerStubReportingRequested();
}

std::optional<VideoImportStubRefusal> DeriveVideoImportStub(const VideoImportStubRow& row, const VideoImportStubCall& call, const VideoImportStubState& state, const VideoImportStubMemory& memory, VideoImportStubPlan& plan)
{
    plan = {};
    if (!rowIsCanonical(row))
        return R::Pc;
    if (!(state.lease && state.ready && environmentAdmits(state.environment)))
        return R::Services;
    if (!providerAdmits(state.environment, row))
        return R::Provider;
    if (state.diagnostics)
        return R::Diagnostics;

    // The owner's frame: the raw SP keeps its high word; the whole interval
    // the row names is read before anything is armed.
    const std::uint32_t sp = low32(call.r1), device = low32(call.r31);
    if (!frameLayoutAllows(row, sp))
        return R::Stack;
    readFrame(row, sp, memory, plan.snapshot);
    if (!plan.snapshot.frameValid)
        return R::Stack;

    if (!row.predicate(call))
        return R::Shape;

    readCache(row, memory, plan.snapshot);
    if (row.import == VideoImportId::Hsio && !plan.snapshot.cacheValid)
        return R::Unreadable;

    if (state.record)
        return *state.record;

    plan.row      = &row;
    plan.call     = call;
    plan.fullR3   = row.fullR3;
    plan.sp       = sp;
    plan.device   = device;
    plan.identity = state.environment.identity;
    return std::nullopt;
}

void ApplyVideoImportStub(const VideoImportStubPlan& plan, std::uint64_t& r3) noexcept
{
    r3 = plan.fullR3;
}

bool ReplaceVideoImportStubCall(std::uint32_t callPC, VideoImportId import, const VideoImportStubCall& call, std::uint64_t& r3) noexcept
{
    if (!importActive(import)) [[likely]]
        return false;
    return ReplaceVideoImportStubCallWith(activeOperations(), callPC, import, call, r3);
}

bool ReplaceVideoImportStubCallWith(const VideoImportStubOperations& operations, std::uint32_t callPC, VideoImportId import, const VideoImportStubCall& call, std::uint64_t& r3) noexcept
{
    auto& c = counters[indexOf(import)];
    // Held from the identity reads through the full r3 assignment; a withdrawal
    // waits for it. It ends when this function returns, before the generated
    // code runs Original.
    std::optional<RingStubLease> lease;
    VideoImportStubPlan          plan;
    bool                         entered = false, consume = false;
    try
    {
        const bool stub = stubRequested(import), check = exitCheckRequested(import);
        if (!stub && !check)
            return false;
        c.enter.fetch_add(1, std::memory_order_relaxed);
        entered         = true;
        const auto* row = FindVideoImportStubRow(callPC, import);
        if (!row)
        {
            c.refusal[static_cast<std::size_t>(R::Pc)].fetch_add(1, std::memory_order_relaxed);
            c.original.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto prior = prepareRecord(operations, import, low32(call.r1));
        lease.emplace();
        VideoImportStubState state;
        state.lease       = static_cast<bool>(*lease);
        state.diagnostics = check;
        if (state.lease)
        {
            state.environment = operations.environment(*row);
            state.ready       = rowReady(*row, call);
        }
        if (state.ready)
            c.readyAtEntry.fetch_add(1, std::memory_order_relaxed);
        const bool providerValid = state.lease && environmentAdmits(state.environment) && providerAdmits(state.environment, *row);
        state.record             = prior ? prior : armRecord(import, *row, call, state.environment, providerValid, state.ready);
        if (stub)
        {
            const auto refusal = DeriveVideoImportStub(*row, call, state, operations.memory, plan);
            if (!refusal)
                consume = true;
            else
                c.refusal[static_cast<std::size_t>(*refusal)].fetch_add(1, std::memory_order_relaxed);
        }
        if (!state.record && providerValid)
        {
            // The entry words the record needs, when the derivation did not read them.
            auto& snapshot = plan.snapshot;
            auto& r        = records[indexOf(import)];
            if (!snapshot.frameAttempted && frameLayoutAllows(*row, low32(call.r1)))
                readFrame(*row, low32(call.r1), operations.memory, snapshot);
            if (!snapshot.cacheAttempted)
                readCache(*row, operations.memory, snapshot);
            r.frameValid      = snapshot.frameValid;
            r.cacheEntryValid = snapshot.cacheValid;
            r.cacheEntry      = snapshot.cache;
        }
        if (!consume)
            c.original.fetch_add(1, std::memory_order_relaxed);
    }
    catch (...)
    {
        // Nothing has been written yet; the import runs.
        if (entered && !consume)
            c.original.fetch_add(1, std::memory_order_relaxed);
        consume = false;
    }
    if (!consume)
        return false;
    // The commit: nothing below can throw, so a consume is never undone.
    if (operations.beforeCommit)
        operations.beforeCommit();
    ApplyVideoImportStub(plan, r3);
    if (auto& r = records[indexOf(import)]; r.active)
        r.replaced = true;
    c.consumed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ObserveVideoImportReturn(VideoImportId import, const VideoImportObservation& observation) noexcept
{
    if (!observerHasWork(import, observation)) [[likely]]
        return;
    try
    {
        auto* r = openBoundary(activeOperations(), import, observation, 0, true);
        if (!r)
            return;
        if (r->poisoned || r->identityChanged)
            return;
        r->pc          = r->row->truePC;
        r->resultFull  = observation.r3;
        r->resultValid = true;
        r->returnBad   = observation.r3 != r->row->fullR3;
    }
    catch (...)
    {
        poisonLive(import);
    }
}

void ObserveVideoImportEffect(VideoImportId import, const VideoImportObservation& observation) noexcept
{
    if (!observerHasWork(import, observation)) [[likely]]
        return;
    try
    {
        const auto& operations = activeOperations();
        auto*       r          = openBoundary(operations, import, observation, 1, false);
        if (!r)
            return;
        if (r->poisoned || r->identityChanged)
            return;
        if (import == VideoImportId::Hsio)
        {
            r->pc               = kVideoHsioEffectPC;
            r->effectR11        = observation.r11;
            r->effectCr0        = observation.cr0;
            r->cacheEffectValid = operations.memory.read32(kVideoHsioCacheAddress, r->cacheEffect);
            r->effectValid      = r->cacheEffectValid;
            // The cache is zero, r11 is zero and CR0 is equal only; the summary-overflow bit is retained.
            r->effectBad = observation.r11 != 0 || (observation.cr0 & 0xE) != 0x2 || (r->cacheEffectValid && r->cacheEffect != 0);
            return;
        }
        r->pc = kVideoWorkerPrePC;
        const std::uint64_t* regs[6]{ &observation.r3, &observation.r4, &observation.r5, &observation.r6, &observation.r7, &observation.r8 };
        for (std::size_t i = 0; i < 6; ++i)
            r->pre[i] = *regs[i];
        r->preStackValid = operations.memory.read32(low32(r->spFull) + kVideoWorkerStackOffsetA, r->preStack[0]) && operations.memory.read32(low32(r->spFull) + kVideoWorkerStackOffsetB, r->preStack[1]);
        r->preValid      = true;
        r->effectValid   = r->preStackValid;
        r->effectBad     = r->preStackValid && (r->preStack[0] != 0 || r->preStack[1] != 0);
        // The retained arguments of the owner's retrain: signed addi of the raw SP.
        r->continuationBad = observation.r3 != r->entryR30 || observation.r4 != add64(r->spFull, kVideoWorkerStackOffsetA) || observation.r5 != 4096 || observation.r6 != add64(r->spFull, kVideoWorkerStackOffsetB) ||
                             observation.r7 != add64(r->spFull, kVideoWorkerArgOffset) || observation.r8 != 2048 || observation.r29 != r->entryR29 || observation.r30 != r->entryR30;
    }
    catch (...)
    {
        poisonLive(import);
    }
}

void ObserveVideoImportPost(VideoImportId import, const VideoImportObservation& observation, std::uint64_t lr) noexcept
{
    if (!observerHasWork(import, observation)) [[likely]]
        return;
    auto& c = counters[indexOf(import)];
    auto& r = records[indexOf(import)];
    try
    {
        const auto& operations = activeOperations();
        ++touches;
        if (!r.active)
        {
            // The HSIO owner reaches this point on its cached path with no call. A
            // worker call that passed the bypass test has a record, so a missing one is
            // a lost or repeated boundary.
            if (import == VideoImportId::EdramWorker)
            {
                c.correlationLost.fetch_add(1, std::memory_order_relaxed);
                c.exitUnpaired.fetch_add(1, std::memory_order_relaxed);
                noteUnresolved(c);
            }
            return;
        }
        if (r.spFull != observation.r1 || r.gFull != observation.r31 || r.guestThread != observation.r13)
        {
            r.poisoned = true;
            c.correlationLost.fetch_add(1, std::memory_order_relaxed);
            c.exitUnpaired.fetch_add(1, std::memory_order_relaxed);
            noteUnresolved(c);
            return;
        }
        r.pc = import == VideoImportId::Hsio ? kVideoHsioPostPC : kVideoWorkerPostPC;
        if (r.stage != 2)
        {
            // An earlier observation was lost or reordered: the pair is incomplete.
            c.correlationLost.fetch_add(1, std::memory_order_relaxed);
            finishRecord(operations, import, false);
            return;
        }
        if (r.providerValid && !r.identityChanged)
        {
            const auto environment = operations.environment ? operations.environment(*r.row) : VideoImportEnvironment{};
            r.identityChanged      = !bindingStable(r, environment);
        }
        r.postLr    = lr;
        r.postValid = true;
        if (import == VideoImportId::Hsio)
        {
            r.postR3  = observation.r3;
            r.postR11 = observation.r11;
            r.postR30 = observation.r30;
            r.postCr0 = observation.cr0;
            // The retained mode register is the one the entry carried.
            r.continuationBad = observation.r30 != r.entryR30;
        }
        else
        {
            r.postR3 = observation.r3;
            const std::uint64_t regs[5]{ observation.r4, observation.r5, observation.r6, observation.r7, observation.r8 };
            for (std::size_t i = 0; i < 5; ++i)
                r.post[i] = regs[i];
            r.postR30        = observation.r30;
            r.postStackValid = operations.memory.read32(low32(r.spFull) + kVideoWorkerStackOffsetA, r.postStack[0]) && operations.memory.read32(low32(r.spFull) + kVideoWorkerStackOffsetB, r.postStack[1]);
            r.lrBad          = lr != kVideoWorkerPostPC;
        }
        finishRecord(operations, import, true);
    }
    catch (...)
    {
        poisonLive(import);
    }
}

namespace
{

VideoImportStubCounts takeCounts(VideoImportId which, bool final) noexcept
{
    auto&                 c = counters[indexOf(which)];
    VideoImportStubCounts counts;
    counts.enter        = c.enter.exchange(0, std::memory_order_relaxed);
    counts.readyAtEntry = c.readyAtEntry.exchange(0, std::memory_order_relaxed);
    counts.consumed     = c.consumed.exchange(0, std::memory_order_relaxed);
    counts.original     = c.original.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kVideoImportRefusalCount; ++i)
        counts.refusal[i] = c.refusal[i].exchange(0, std::memory_order_relaxed);
    counts.exitOk = c.exitOk.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kVideoImportMismatchCount; ++i)
        counts.mismatch[i] = c.mismatch[i].exchange(0, std::memory_order_relaxed);
    counts.exitUnreadable  = c.exitUnreadable.exchange(0, std::memory_order_relaxed);
    counts.exitUnpaired    = c.exitUnpaired.exchange(0, std::memory_order_relaxed);
    counts.reentry         = c.reentry.exchange(0, std::memory_order_relaxed);
    counts.abandoned       = c.abandoned.exchange(0, std::memory_order_relaxed);
    counts.correlationLost = c.correlationLost.exchange(0, std::memory_order_relaxed);
    counts.reportingFailed = c.reportingFailed.exchange(0, std::memory_order_relaxed);
    counts.pending         = c.pending.load(std::memory_order_relaxed);
    counts.unresolved      = c.unresolved.load(std::memory_order_relaxed);
    if (final)
    {
        // A skipped final is retained for the next final, not lost to a timing drain.
        counts.finalSkipped = c.finalSkipped.exchange(0, std::memory_order_relaxed);
        counts.final        = true;
    }
    return counts;
}

std::string formatCounts(VideoImportId which, const VideoImportStubCounts& counts)
{
    const auto prefix = prefixOf(which);
    auto       fields = std::format(" {0}enter={1} {0}ready_at_entry={2} {0}consumed={3} {0}original={4}", prefix, counts.enter, counts.readyAtEntry, counts.consumed, counts.original);
    for (std::size_t i = 0; i < kVideoImportRefusalCount; ++i)
        fields += std::format(" {}refusal_{}={}", prefix, kVideoImportRefusalNames[i], counts.refusal[i]);
    fields += std::format(" {}exit_ok={}", prefix, counts.exitOk);
    for (std::size_t i = 0; i < kVideoImportMismatchCount; ++i)
        fields += std::format(" {}exit_mismatch_{}={}", prefix, kVideoImportMismatchNames[i], counts.mismatch[i]);
    fields += std::format(" {0}exit_unreadable={1} {0}exit_unpaired={2} {0}reentry={3} {0}abandoned={4} {0}correlation_lost={5} {0}reporting_failed={6} {0}pending={7}",
                          prefix,
                          counts.exitUnreadable,
                          counts.exitUnpaired,
                          counts.reentry,
                          counts.abandoned,
                          counts.correlationLost,
                          counts.reportingFailed,
                          counts.pending);
    if (counts.final)
    {
        const bool incomplete = counts.pending != 0 || counts.finalSkipped != 0 || counts.reportingFailed != 0 || counts.unresolved;
        fields += std::format(" {0}final_incomplete={1} {0}final_skipped={2}", prefix, incomplete ? 1 : 0, counts.finalSkipped);
    }
    return fields;
}

// The final line of one import: logged outside every other lock.
void logFinal(VideoImportId which, const VideoImportReportOperations& operations, const std::string& fields) noexcept
{
    try
    {
        if (operations.log)
            operations.log(fields);
        else
            REXLOG_INFO("Native video {}: shutdown{}", which == VideoImportId::Hsio ? "HSIO" : "EDRAM worker", fields);
    }
    catch (...)
    {
        countReportingFailure(counters[indexOf(which)]);
    }
}

// Drains one import with the report lock held.
std::string reportLocked(VideoImportId which, bool shutdown, const VideoImportReportOperations& operations) noexcept
{
    try
    {
        const auto counts = takeCounts(which, shutdown);
        auto       fields = operations.format ? operations.format(which, counts) : formatCounts(which, counts);
        if (shutdown)
            logFinal(which, operations, fields);
        return fields;
    }
    catch (...)
    {
        countReportingFailure(counters[indexOf(which)]);
    }
    return {};
}

// The skipped final of an import whose lock stayed busy: its outstanding
// calls, and the skip kept for the next final.
std::string skippedFinal(VideoImportId which, const VideoImportReportOperations& operations) noexcept
{
    auto& c = counters[indexOf(which)];
    c.finalSkipped.fetch_add(1, std::memory_order_relaxed);
    noteUnresolved(c);
    const auto prefix = prefixOf(which);
    auto       fields = std::format(" {0}pending={1} {0}final_incomplete=1 {0}final_skipped=1", prefix, c.pending.load(std::memory_order_relaxed));
    logFinal(which, operations, fields);
    return fields;
}

// Reports the requested imports among `only` (or both) under one lock.
std::string reportImports(bool first, bool second, bool shutdown, const VideoImportReportOperations& operations) noexcept
{
    const bool  wanted[kVideoImportCount]{ first, second };
    std::string fields;
    try
    {
        std::unique_lock lock(reportMutex, std::defer_lock);
        if (shutdown)
        {
            // A drain on another thread may still hold the lock; the final waits only briefly.
            if (!lock.try_lock_for(kVideoImportShutdownWait))
            {
                for (std::size_t i = 0; i < kVideoImportCount; ++i)
                    if (wanted[i])
                        fields += skippedFinal(static_cast<VideoImportId>(i), operations);
                return fields;
            }
        }
        else
            lock.lock();
        for (std::size_t i = 0; i < kVideoImportCount; ++i)
            if (wanted[i])
                fields += reportLocked(static_cast<VideoImportId>(i), shutdown, operations);
    }
    catch (...)
    {
    }
    return fields;
}

} // namespace

VideoImportStubCounts TakeVideoHsioStubCounts(bool final) noexcept
{
    return takeCounts(VideoImportId::Hsio, final);
}

VideoImportStubCounts TakeVideoEdramWorkerStubCounts(bool final) noexcept
{
    return takeCounts(VideoImportId::EdramWorker, final);
}

std::string FormatVideoHsioStubCounts(const VideoImportStubCounts& counts)
{
    return formatCounts(VideoImportId::Hsio, counts);
}

std::string FormatVideoEdramWorkerStubCounts(const VideoImportStubCounts& counts)
{
    return formatCounts(VideoImportId::EdramWorker, counts);
}

namespace detail
{

std::timed_mutex& VideoImportReportMutex() noexcept
{
    return reportMutex;
}

} // namespace detail

std::string ReportVideoHsioStub(bool shutdown, const VideoImportReportOperations& operations) noexcept
{
    if (!VideoHsioStubReportingRequested())
        return {};
    return reportImports(true, false, shutdown, operations);
}

std::string ReportVideoEdramWorkerStub(bool shutdown, const VideoImportReportOperations& operations) noexcept
{
    if (!VideoEdramWorkerStubReportingRequested())
        return {};
    return reportImports(false, true, shutdown, operations);
}

std::string ReportVideoImportStubs(bool shutdown, const VideoImportReportOperations& operations) noexcept
{
    return reportImports(VideoHsioStubReportingRequested(), VideoEdramWorkerStubReportingRequested(), shutdown, operations);
}

void SetVideoImportOperationsForTesting(const VideoImportStubOperations* operations) noexcept
{
    testOperations.store(operations, std::memory_order_release);
}

std::uint64_t VideoImportThreadTouchesForTesting() noexcept
{
    return touches;
}

void SetVideoImportPendingLimitForTesting(VideoImportId import, std::uint32_t limit) noexcept
{
    counters[indexOf(import)].pendingLimit.store(limit, std::memory_order_relaxed);
}

void SetVideoImportReserveProbeForTesting(void (*probe)()) noexcept
{
    reserveProbe.store(probe, std::memory_order_relaxed);
}

void ResetVideoImportLogBudgetForTesting() noexcept
{
    for (auto& c : counters)
        c.okLogged.store(0, std::memory_order_relaxed);
}

void ResetVideoImportStateForTesting() noexcept
{
    for (auto& c : counters)
    {
        c.enter.store(0);
        c.readyAtEntry.store(0);
        c.consumed.store(0);
        c.original.store(0);
        for (auto& v : c.refusal)
            v.store(0);
        c.exitOk.store(0);
        c.exitUnreadable.store(0);
        c.exitUnpaired.store(0);
        c.reentry.store(0);
        c.abandoned.store(0);
        c.correlationLost.store(0);
        c.reportingFailed.store(0);
        c.finalSkipped.store(0);
        for (auto& v : c.mismatch)
            v.store(0);
        c.pending.store(0);
        c.nextInvocation.store(1);
        c.pendingLimit.store(kVideoImportMaxPending);
        c.okLogged.store(0);
        c.unresolved.store(false);
    }
    for (auto& r : records)
        r = {};
}

VideoImportEnvironment ProductionVideoImportEnvironmentForTesting(const VideoImportStubRow& row)
{
    return productionEnvironment(row);
}

bool ProductionVideoImportRead32ForTesting(std::uint32_t address, std::uint32_t& value) noexcept
{
    return readKernel32(address, value);
}

} // namespace rerevved::gpu::guest_d3d
