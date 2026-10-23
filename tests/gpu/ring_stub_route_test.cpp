// Tests the progress observer, stub runner, and completion ledger through the
// real ring route wrappers. Generated originals cannot run here; fixtures stand
// in and record each call.

#include "developer_checks.hpp"
#include "gpu/completion/completion_ledger.h"
#include "gpu/completion/guest_progress_hooks.h"
#include "gpu/guest_d3d/indexed_buffer_consume.h"
#include "gpu/guest_d3d/ring_stubs.h"
#include "gpu/guest_d3d/set_predication_consume.h"
#include "gpu/guest_gpu_service.h"
#include "guest_import_doubles.hpp"

#if REREVVED_DEVELOPER
#include "gpu/diagnostics/route_trace.h"
#include "gpu/guest_d3d/completion_probe.h"
#include "gpu/guest_d3d/guest_api_census.h"
#include "gpu/guest_d3d/original_residue_probe.h"
#include "gpu/guest_d3d/tiling_probe.h"
#include "gpu/guest_d3d/tiling_restore_consume.h"
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using rex::X_STATUS;

REXCVAR_DECLARE(bool, native_consume_indexed_buffer);
REXCVAR_DECLARE(bool, native_consume_indexed_buffer_tiled);
REXCVAR_DECLARE(bool, native_consume_user_pointer);
REXCVAR_DECLARE(bool, native_consume_user_pointer_tiled);
REXCVAR_DECLARE(bool, native_consume_lowerer);
REXCVAR_DECLARE(bool, native_consume_lowerer_tiled);
REXCVAR_DECLARE(bool, native_consume_set_predication);
REXCVAR_DECLARE(bool, native_consume_set_predication_preswap);
DEV_CVAR_DECLARE(bool, native_set_predication_exit_check);

#if REREVVED_DEVELOPER
REXCVAR_DECLARE(bool, native_consume_tiling_restore);
REXCVAR_DECLARE(bool, native_tiling_restore_exit_check);
REXCVAR_DECLARE(bool, native_ring_consume_publish);
REXCVAR_DECLARE(bool, native_original_residue_probe);
REXCVAR_DECLARE(bool, native_completion_probe);
REXCVAR_DECLARE(bool, native_tiling_quiesce_check);

// Assertions on the developer probes' counters run only where the probes exist.
#define PROBE_INFO(...)  UNSCOPED_INFO(__VA_ARGS__)
#define PROBE_CHECK(...) CHECK(__VA_ARGS__)
#else
#define PROBE_INFO(...)  static_cast<void>(0)
#define PROBE_CHECK(...) static_cast<void>(sizeof(__VA_ARGS__))
#endif
REX_EXTERN(sub_8269B118);
REX_EXTERN(sub_8269C400);
REX_EXTERN(sub_8269BD98);

// Declarations of the wrappers the route and guest API hook sources define.
#define ORIGINAL_New(address) REX_EXTERN(sub_##address);
#define ORIGINAL_Existing(address)
#define ROUTE_ENTRY(address, groups, mode) ORIGINAL_##mode(address)
#include "gpu/guest_d3d/route_entries.inc"
#undef ROUTE_ENTRY
#undef ORIGINAL_Existing
#undef ORIGINAL_New

#define CENSUS_ORIGINAL_Census(address) REX_EXTERN(sub_##address);
#define CENSUS_ORIGINAL_Route(address)
#define CENSUS_ORIGINAL_Raw(address)
#define GUEST_API_ENTRY(address, role, name, hook) CENSUS_ORIGINAL_##hook(address)
#include "gpu/guest_d3d/guest_api_entries.inc"
#include "manual_cases.hpp"
#undef GUEST_API_ENTRY
#undef CENSUS_ORIGINAL_Census
#undef CENSUS_ORIGINAL_Route
#undef CENSUS_ORIGINAL_Raw

REX_EXTERN(sub_826AC0A8);
REX_EXTERN(sub_826AC218);
REX_EXTERN(sub_826AB740);

namespace
{

using namespace rerevved::gpu::guest_d3d;
using namespace rerevved::gpu::guest_d3d::ring_layout;
namespace completion = rerevved::gpu::completion;

// Each fixture Original records its address and the device it was given.
std::vector<std::pair<std::uint32_t, std::uint32_t>> originals;
std::function<void(std::uint32_t, PPCContext&)>      residueBody;
std::function<void(std::uint32_t)>                   residueKick;

void original(std::uint32_t address, PPCContext& ctx)
{
    originals.emplace_back(address, ctx.r3.u32);
    if (residueBody)
        residueBody(address, ctx);
}

std::size_t originalCalls(std::uint32_t address)
{
    std::size_t count = 0;
    for (const auto& [called, device] : originals)
        count += called == address ? 1 : 0;
    return count;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> decodedInterval;

// The EndTiling helpers' fixtures: a bump allocator over a physical-heap
// allocation, the list terminator, the failure reset and 0x8269BD98's
// deferred path, each recording its arguments.
struct Submission
{
    std::uint32_t returnAddress, cursor, physical, dwords, raise, list;
    bool          operator==(const Submission&) const = default;
};

struct TilingFixtures
{
    void reset(rex::memory::Memory* guest, std::uint32_t arena)
    {
        *this  = {};
        memory = guest;
        next   = arena;
    }

    void store(std::uint32_t address, std::uint32_t value) const
    {
        rex::memory::store_and_swap<std::uint32_t>(memory->TranslateVirtual(address), value);
    }

    std::uint32_t load(std::uint32_t address) const
    {
        return rex::memory::load_and_swap<std::uint32_t>(memory->TranslateVirtual(address));
    }

    rex::memory::Memory*                                 memory = nullptr;
    std::uint32_t                                        next   = 0;
    std::optional<std::size_t>                           failAllocation; // 0x8269B818 sets G+0x2ABD bit 0x20
    std::vector<std::pair<std::uint32_t, std::uint32_t>> allocations;    // {dwords, alignment}
    std::vector<std::uint32_t>                           terminated;     // 0x8269B508 lists
    std::vector<std::uint32_t>                           resets;         // 0x8269B118 devices
    std::vector<Submission>                              submissions;    // 0x8269BD98 Originals
    std::vector<std::pair<std::uint32_t, std::uint32_t>> restoreWrites;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> allocationBytes;
    std::uint32_t                                        restoreActive = 0, restoreNext = 0;
    std::uint8_t                                         restoreMode = 0;
};

TilingFixtures tiling;

// 0x8269CD20 also records the link register of each call: the SetPredication consume keeps the
// pre-swap stub's own call, which returns to kSetPredicationPreSwapReturn, on Original.
std::vector<std::uint32_t> setPredicationCallers;

std::function<void(PPCContext&)> setPredicationBody;

void censusOriginal(std::uint32_t address, PPCContext& ctx)
{
    original(address, ctx);
    if (address == kSetPredicationAddress)
    {
        setPredicationCallers.push_back(static_cast<std::uint32_t>(ctx.lr));
        if (setPredicationBody)
            setPredicationBody(ctx);
    }
}

// 0x8269BD98 while the worker owns the ring: the deferred-kick record, a
// three-word token past the cursor and G+0x2B00 += r7.
void fixtureSub8269BD98(std::uint32_t, PPCContext& ctx, std::uint8_t*)
{
    original(0x8269BD98, ctx);
    if (residueBody)
        return;
    tiling.submissions.push_back({ static_cast<std::uint32_t>(ctx.lr), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32 });
    rerevved::gpu::completion::detail::RecordDeferredKick(ctx.r5.u32, ctx.r6.u32);
    const std::uint32_t g = ctx.r3.u32;
    tiling.store(g + rerevved::gpu::guest_d3d::ring_layout::kWorkerPending,
                 tiling.load(g + rerevved::gpu::guest_d3d::ring_layout::kWorkerPending) + ctx.r7.u32);
    ctx.r3.u64 = ctx.r4.u32 + 16;
}

// BeginTiling: G+0x33B0 and G+0x33B4 set, and its BeginTiledList record.
void fixtureSub826AC0A8(std::uint32_t, PPCContext& ctx, std::uint8_t*)
{
    original(0x826AC0A8, ctx);
    tiling.store(ctx.r3.u32 + rerevved::gpu::guest_d3d::ring_layout::kTiling, 1);
    tiling.store(ctx.r3.u32 + rerevved::gpu::guest_d3d::ring_layout::kTilingNext, 1);
    rerevved::gpu::completion::detail::BeginTiledList();
}

void fixtureSub8269B818(std::uint32_t, PPCContext& ctx, std::uint8_t*)
{
    const std::uint32_t g = ctx.r3.u32, dwords = ctx.r4.u32, alignment = ctx.r5.u32;
    if (tiling.failAllocation == tiling.allocations.size())
    {
        auto* mode = tiling.memory->TranslateVirtual<std::uint8_t*>(g + rerevved::gpu::guest_d3d::ring_layout::kModeHigh);
        *mode |= rerevved::gpu::guest_d3d::ring_layout::kModeAllocFailed;
        tiling.allocations.emplace_back(dwords, alignment);
        ctx.r3.u64 = 0;
        return;
    }
    tiling.allocations.emplace_back(dwords, alignment);
    tiling.next = (tiling.next + alignment - 1) & ~(alignment - 1);
    ctx.r3.u64  = tiling.next;
    if (tiling.failAllocation || (*tiling.memory->TranslateVirtual<std::uint8_t*>(g + rerevved::gpu::guest_d3d::ring_layout::kModeHigh) & 0x20))
    {
        const auto marker = 0xA1735D29U ^ dwords;
        tiling.store(tiling.next, marker);
        tiling.allocationBytes.emplace_back(tiling.next, marker);
    }
    tiling.next += 4 * dwords;
}

void fixtureSub8269B508(std::uint32_t, PPCContext& ctx, std::uint8_t*)
{
    tiling.terminated.push_back(ctx.r3.u32);
}

#if REREVVED_DEVELOPER

// Ring helpers log deferred-list, allocation and rollover calls with guest words in
// order.
struct HelperCall
{
    std::uint32_t              address;
    std::uint64_t              lr, r3, r4, r5;
    std::uint32_t              leases;
    std::vector<std::uint32_t> words; // helperWatch at the call
};

std::vector<std::string>                  consumeEvents; // kicks, lock calls and helper calls in order
std::vector<HelperCall>                   helperCalls;
std::vector<std::uint32_t>                helperWatch;   // guest addresses to sample at each helper call
std::function<std::uint32_t(PPCContext&)> growthStorage; // 0x8269C170's return, when set
std::uint64_t                             rolloverResult = 0;

void noteHelper(std::uint32_t address, PPCContext& ctx)
{
    HelperCall call{ address, ctx.lr, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, RingStubLeaseCountForTesting(), {} };
    for (const auto word : helperWatch)
        call.words.push_back(tiling.load(word));
    helperCalls.push_back(std::move(call));
    consumeEvents.push_back(std::format("helper:{:X}", address));
}

// The view adjustment CommandBufferPhysical applies, from the generated rlwinm/addi chain.
std::uint32_t viewAdjusted(std::uint32_t address)
{
    return (address & 0x1FFFFFFF) + ((((address >> 20) & 0xFFF) + 512) & 0x1000);
}

void fixtureSub8269B550(std::uint32_t, PPCContext& ctx, std::uint8_t*)
{
    noteHelper(0x8269B550, ctx);
    const std::uint32_t list = ctx.r3.u32, p = ctx.r4.u32, q = ctx.r5.u32;
    const std::uint32_t old = tiling.load(list + 8);
    const auto          d   = [](std::uint32_t x)
    {
        return viewAdjusted(x) - 0x40000000U;
    };
    tiling.store(p, d(q));
    tiling.store(list + 4, p);
    tiling.store(list + 8, q - 4);
    tiling.store(old, d(p));
}

void fixtureSub8269C170(std::uint32_t, PPCContext& ctx, std::uint8_t*)
{
    noteHelper(0x8269C170, ctx);
    ctx.r3.u64 = growthStorage ? growthStorage(ctx) : ctx.r3.u64;
}

void fixtureSub8269C270(std::uint32_t, PPCContext& ctx, std::uint8_t*)
{
    noteHelper(0x8269C270, ctx);
    ctx.r3.u64 = rolloverResult;
}

#endif

void fixtureSub8269B118(std::uint32_t, PPCContext& ctx, std::uint8_t*)
{
    original(0x8269B118, ctx);
    const auto g = ctx.r3.u32;
    tiling.resets.push_back(g);
    tiling.restoreActive = tiling.load(g + 0x33B0);
    tiling.restoreNext   = tiling.load(g + 0x33B4);
    tiling.restoreMode   = *tiling.memory->TranslateVirtual<std::uint8_t*>(g + 0x2ABD);
    // Independent instruction semantics of 8269B118..8269B150.
    ctx.r11.u64    = tiling.load(g + 13412);
    ctx.r10.u64    = tiling.load(g + 13408);
    ctx.r9.u64     = tiling.load(g + 13416);
    ctx.r8.u64     = ctx.r11.u64 - 160ULL;
    ctx.r7.u64     = tiling.load(g + 13420);
    ctx.r6.u64     = tiling.load(g + 13424);
    ctx.r5.u64     = tiling.load(g + 13428);
    const auto stw = [&](std::uint32_t address, std::uint32_t value)
    {
        tiling.restoreWrites.emplace_back(address, value);
        tiling.store(address, value);
    };
    stw(g + 48, ctx.r10.u32);
    stw(g + 52, ctx.r11.u32);
    stw(g + 14928, ctx.r9.u32);
    stw(g + 14924, ctx.r7.u32);
    stw(g + 14916, ctx.r6.u32);
    stw(g + 14920, ctx.r5.u32);
    stw(g + 56, ctx.r8.u32);
}

} // namespace

// The midasm observer the pre-swap stub reports its decoded interval to.
void ObserveDecodedSwapInterval(PPCRegister& r11, PPCRegister& r31)
{
    decodedInterval = std::pair{ r11.u32, r31.u32 };
}

namespace
{

constexpr std::uint32_t kPhysicalHeap = 0xA0000000;
constexpr std::uint32_t kImagePage    = 0x82000000;
constexpr std::uint32_t kCaller       = 0x82303E40;
constexpr std::uint32_t kStartAt      = 0x2000;

void setupRuntime(rex::Runtime& runtime)
{
    UNSCOPED_INFO("runtime setup succeeds");
    CHECK(runtime.Setup(rex::RuntimeConfig{
              .kernel_init = rex::kernel::InitializeKernel,
              .tool_mode   = true,
          }) == X_STATUS_SUCCESS);
}

constexpr std::uint32_t kAllocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
constexpr std::uint32_t kWritable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;

std::uint32_t allocatePhysical(rex::memory::Memory& memory)
{
    auto*         heap    = memory.LookupHeap(kPhysicalHeap);
    std::uint32_t address = 0;
    UNSCOPED_INFO("a physical allocation succeeds");
    CHECK((heap && heap->Alloc(0x10000, 0x10000, kAllocation, kWritable, false, &address)));
    return address;
}

// The ledger's sink over the runtime's physical memory.
class Sink final : public completion::IGuestProgressSink
{
public:
    explicit Sink(rex::memory::Memory& memory)
    : memory(memory)
    {
    }

    bool WritePhysical(std::uint32_t address, std::uint32_t value) override
    {
        rex::memory::store_and_swap<std::uint32_t>(memory.TranslatePhysical(address), value);
        writes.emplace_back(address, value);
        return true;
    }

    std::optional<std::uint32_t> ReadPhysical(std::uint32_t address) override
    {
        return rex::memory::load_and_swap<std::uint32_t>(memory.TranslatePhysical(address));
    }

    completion::InterruptResult RaiseCallbackInterrupt(std::uint32_t cpu) override
    {
        interrupts.push_back(cpu);
        if (handler)
            handler();
        return completion::InterruptResult::Raised;
    }

    rex::memory::Memory&                                 memory;
    std::function<void()>                                handler; // the guest's source-1 handler
    std::vector<std::uint32_t>                           interrupts;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;
};

class Draws final : public completion::IDrawCompletion
{
public:
    completion::DrawFlush FlushThrough(std::uint64_t) override
    {
        return { ++next, false, {} };
    }

    std::uint64_t Completed() const override
    {
        return held ? 0 : next;
    }

    std::uint64_t next = 0;
    bool          held = false;
};

// The publisher's kick reaches the ledger as 0x8269B900's CP_RB_WPTR store does.
class Services final : public IRingStubServices
{
public:
    explicit Services(std::weak_ptr<completion::CompletionLedger> ledger)
    : ledger(std::move(ledger))
    {
    }

    void Kick(std::uint32_t writePointer) override
    {
        if (residueKick)
            residueKick(writePointer);
        if (const auto target = ledger.lock())
            target->Kick(writePointer);
    }

private:
    std::weak_ptr<completion::CompletionLedger> ledger;
};

// A device as 0x8269C8E0 leaves it, in guest memory the hooks read through
// the kernel, with its progress block not yet allocated.
struct Device
{
    explicit Device(rex::memory::Memory& memory)
    : memory(memory)
    {
        g       = allocatePhysical(memory);
        scratch = allocatePhysical(memory);
        block   = allocatePhysical(memory);
        cell    = scratch + 0x100;
        store(kDeviceGlobal, cell);
        store(cell, g);
        store(g + kScratchBlock, scratch);
        store(g + kRingMask, 0x3FFF);
        store(g + kNextFence, 0x11);
        store(g + kCursor, kStartAt);
        store(g + kCursorLimit, 0x9000);
        store(g + kSwapInterval, 2);
        store(g + kSwapFlags, 0x15U << 23);
        store(g + kCoherencyRange + 4, 0xFFFFFFFF);
        store(g + 0x3460, kStartAt);
    }

    void store(std::uint32_t address, std::uint32_t value)
    {
        rex::memory::store_and_swap<std::uint32_t>(memory.TranslateVirtual(address), value);
    }

    std::uint32_t load(std::uint32_t address)
    {
        return rex::memory::load_and_swap<std::uint32_t>(memory.TranslateVirtual(address));
    }

    std::uint32_t physical(std::uint32_t address) const
    {
        return memory.GetPhysicalAddress(address);
    }

    // The handler 0x8269B450 clears CPU 2's bit in S2+0.
    void clearCpuBit()
    {
        store(scratch + completion::kScratchMaskOffset, load(scratch + completion::kScratchMaskOffset) & ~(1U << completion::kCallbackCpu));
    }

    rex::memory::Memory& memory;
    std::uint32_t        g = 0, scratch = 0, block = 0, cell = 0;
};

// A published ledger and published stub services, as GuestVideoServices
// starts them with native_ring_stubs on.
struct Session
{
    explicit Session(Device& device)
    : device(device)
    {
        ledger->BeginRingEpoch();
        ledger->RegisterWriteback(device.physical(device.block) + completion::kReadPointerOffset);
        completion::PublishGuestProgress(ledger);
        PublishRingStubServices(std::make_shared<Services>(ledger));
        (void)TakeRingStubCounts();
        originals.clear();
    }

    ~Session()
    {
        WithdrawRingStubServices();
        completion::WithdrawGuestProgress();
    }

    Device&                                       device;
    Sink                                          sink{ device.memory };
    Draws                                         draws;
    std::vector<std::string>                      failed;
    std::shared_ptr<completion::CompletionLedger> ledger = std::make_shared<completion::CompletionLedger>(
        completion::LedgerConfig{ NativeDrawMode::Strict, &sink, &draws, [this](const completion::LedgerFailure& failure)
                                  {
                                      failed.push_back(failure.detail);
                                  } });
};

// P1-3: fallback runs Original once while the observed callback still lands.
void wrapperFallbackRunsOriginalFixtureAndLedgerCallback(rex::memory::Memory& memory, std::uint8_t* base)
{
    Device  device(memory);
    Session session(device);
    UNSCOPED_INFO("the stubs are active");
    CHECK(ringStubsActive.load());

    PPCContext ctx{};
    ctx.r3.u64 = device.g;
    ctx.r4.u64 = kStartAt;
    ctx.r5.u64 = 0;
    ctx.r6.u64 = 0x826A8BE8;
    ctx.r7.u64 = 0x55;
    ctx.lr     = kCaller;
    sub_8269BF40(ctx, base);

    UNSCOPED_INFO("the fallback runs the callback emitter's Original once");
    CHECK((originals.size() == 1 && originals[0] == std::pair{ 0x8269BF40U, device.g }));
    const auto counts = TakeRingStubCounts();
    const auto stub   = static_cast<std::size_t>(RingStub::Callback);
    UNSCOPED_INFO("the not-ready device counts stub_callback_original");
    CHECK((counts.original[stub] == 1 && counts.stubbed[stub] == 0));

    session.sink.handler = [&]
    {
        device.clearCpuBit();
    };
    session.ledger->Kick(0x40);
    UNSCOPED_INFO("the ledger raises the observed callback once on CPU 2");
    CHECK((session.ledger->Pump() && session.sink.interrupts.size() == 1 && session.sink.interrupts[0] == completion::kCallbackCpu));
    UNSCOPED_INFO("the ledger writes and retires the fallback's callback slot");
    CHECK((device.load(device.scratch + completion::kScratchParameterOffset) == 0x55 &&
           device.load(device.scratch + completion::kScratchCallbackOffset) == completion::kRetiredCallback));
    UNSCOPED_INFO("the fallback is no ledger failure");
    CHECK(session.failed.empty());
}

// P3: pre-swap reports 0x826A4258's interval; callback runs through its wrapper.
void consumedPreSwapObservesTheDecodedInterval(rex::memory::Memory& memory, std::uint8_t* base)
{
    Device device(memory);
    device.store(device.g + kProgressBlock, device.block);
    Session session(device);
    decodedInterval.reset();
    setPredicationCallers.clear();

    PPCContext ctx{};
    ctx.r3.u64 = device.g;
    ctx.r4.u64 = 0; // a flip request
    ctx.lr     = kCaller;
    sub_826A4150(ctx, base);

    UNSCOPED_INFO("interval 2 is reported as code 2 for the device");
    CHECK(decodedInterval == std::pair{ 2U, device.g });
    UNSCOPED_INFO("only 0x8269CD20 runs as Original");
    CHECK((originalCalls(0x8269CD20) == 1 && originals.size() == 1));
    UNSCOPED_INFO("the stub calls SetPredication at the return address the consume keeps on Original");
    CHECK((setPredicationCallers.size() == 1 && setPredicationCallers[0] == kSetPredicationPreSwapReturn));
    UNSCOPED_INFO("the flip callback advances the cursor by 37 dwords");
    CHECK(device.load(device.g + kCursor) == kStartAt + 148);
    UNSCOPED_INFO("the pre-swap mode bit is set");
    CHECK(memory.TranslateVirtual<std::uint8_t*>(device.g + kModeHigh)[0] == kModePreSwapDone);
    const auto counts = TakeRingStubCounts();
    UNSCOPED_INFO("the pre-swap and its flip callback are stubbed");
    CHECK((counts.stubbed[static_cast<std::size_t>(RingStub::PreSwap)] == 1 && counts.stubbed[static_cast<std::size_t>(RingStub::Callback)] == 1));

    // 0x826A3FB8 retires the flip by clearing S2+4.
    session.sink.handler = [&]
    {
        device.store(device.scratch + completion::kScratchFlipOffset, 0);
        device.clearCpuBit();
    };
    session.ledger->Kick(0x40);
    for (int i = 0; i < 4; ++i)
        session.ledger->Pump();
    const auto snapshot = session.ledger->Snapshot();
    UNSCOPED_INFO("the flip span releases");
    CHECK((session.sink.interrupts.size() == 1 && snapshot.spansReleased == 1 && !snapshot.pending));
    UNSCOPED_INFO("the flip is no ledger failure");
    CHECK(session.failed.empty());
}

constexpr std::uint32_t kEndTilingFlags = 0x04000006;

// Clear-arm transcription of CD20 (g226:12791-12999), independent of the derive helper.
void clearPredicationOriginal(Device& device, PPCContext& ctx, std::uint8_t* base, std::vector<SetPredicationWrite>& writes)
{
    const auto g     = ctx.r3.u32;
    const auto store = [&](std::uint32_t address, std::uint32_t value)
    {
        writes.push_back({ address, value, false });
        device.store(address, value);
    };
    store(ctx.r1.u32 - 8, static_cast<std::uint32_t>(ctx.lr));
    store(ctx.r1.u32 - 16, static_cast<std::uint32_t>(ctx.r31.u64 >> 32));
    store(ctx.r1.u32 - 12, ctx.r31.u32);
    store(ctx.r1.u32 - 96, ctx.r1.u32);
    auto*      tilingByte = device.memory.TranslateVirtual<std::uint8_t*>(g + kModeLow);
    const auto old        = *tilingByte;
    store(g + 0x31AC, 0);
    bool bit = false;
    if (!(old & 8) && !(old & 4) && device.memory.TranslateVirtual<std::uint8_t*>(g + 0x2F9B)[0] == 0)
    {
        if (old & 0x10)
            bit = true;
        else if (old & 0x20)
        {
            bit = true;
            for (std::uint32_t i = 0; i < 5; ++i)
            {
                const auto bound = device.load(g + 0x3098 + 4 * i);
                if (bound && bound != device.load(g + 0x31B8 + 4 * i))
                    bit = false;
            }
        }
    }
    store(g + 0x31A4, 0xFFFFFFFF);
    *tilingByte = static_cast<std::uint8_t>((old & 0xFE) | bit);
    writes.push_back({ g + kModeLow, *tilingByte, true });
    store(g + 0x31A8, 0);
    store(g + 0x2AB4, 0);
    store(g + 0x2AB8, 0);
    if (device.load(g + kCursor) > device.load(g + kCursorLimit))
    {
        const auto lr = ctx.lr;
        ctx.r3.u64    = g;
        ctx.lr        = 0x8269CE60;
        sub_8269C588(ctx, base);
        ctx.lr = lr;
    }
    const auto cursor = device.load(g + kCursor);
    store(cursor + 4, 0xC0006000);
    store(cursor + 8, 0xFFFFFFFF);
    store(g + kCursor, cursor + 8);
    ctx.r3.u64 = cursor + 8;
}

std::vector<std::uint8_t> guestBytes(rex::memory::Memory& memory, std::uint32_t address, std::size_t count = 0x10000)
{
    const auto* first = memory.TranslateVirtual<const std::uint8_t*>(address);
    return { first, first + count };
}

#if REREVVED_DEVELOPER
constexpr std::array<bool, 2> probeStates{ false, true };
#else
constexpr std::array<bool, 1> probeStates{ false };
#endif

void realCensusPreSwapOff([[maybe_unused]] rex::memory::Memory& memory, std::uint8_t* base)
{
    UNSCOPED_INFO("pre-swap consume defaults on");
    CHECK(REXCVAR_GET(native_consume_set_predication_preswap));
    REXCVAR_SET(native_consume_set_predication_preswap, false);
#if REREVVED_DEVELOPER
    EnableGuestApiCensus(true);
#endif
    Device device(memory);
    device.store(device.g + kProgressBlock, device.block);
    const auto ring  = allocatePhysical(memory);
    const auto stack = allocatePhysical(memory);
    const auto pcr   = allocatePhysical(memory);
    device.store(pcr + 0x70, stack + 0x10000);
    device.store(pcr + 0x74, stack);
    Session session(device);
    ringStubsValidatedDevice.store(device.g);
    for (const bool native : { false, true })
        for (const bool existing : { false, true })
            for (const bool probe : probeStates)
            {
                REXCVAR_SET(native_consume_set_predication, existing);
                DEV_CVAR_SET(native_set_predication_exit_check, probe);
                ringStubsActive.store(native);
                std::memset(memory.TranslateVirtual(ring), 0x5A, 0x10000);
                std::memset(memory.TranslateVirtual(stack), 0xA5, 0x10000);
                device.store(device.g + kCursor, ring);
                device.store(device.g + kCursorLimit, ring + 0x100);
                device.store(device.g + kModeLow, 0x20123456);
                device.store(device.g + 0x31AC, 7);
                device.store(device.g + 0x31A4, 0x01234567);
                const auto beforeDevice = guestBytes(memory, device.g);
                const auto beforeRing   = guestBytes(memory, ring);
                const auto beforeStack  = guestBytes(memory, stack);
                PPCContext ctx{};
                ctx.r3.u64                                 = device.g;
                ctx.r1.u64                                 = stack + 0x8000;
                ctx.r13.u64                                = pcr;
                ctx.r31.u64                                = 0x12345678ABCDEF01ULL;
                ctx.lr                                     = kSetPredicationPreSwapReturn;
                const auto                       beforeCtx = ctx;
                std::vector<SetPredicationWrite> writes;
                setPredicationBody = [&](PPCContext& c)
                {
                    clearPredicationOriginal(device, c, base, writes);
                };
                // The 3129fb65 pre-swap route always reached this Original, regardless of the base consume flag.
                censusOriginal(kSetPredicationAddress, ctx);
                const auto expectedDevice  = guestBytes(memory, device.g);
                const auto expectedRing    = guestBytes(memory, ring);
                const auto expectedStack   = guestBytes(memory, stack);
                const auto expectedWrites  = writes;
                const auto expectedContext = ctx;
                std::memcpy(memory.TranslateVirtual(device.g), beforeDevice.data(), beforeDevice.size());
                std::memcpy(memory.TranslateVirtual(ring), beforeRing.data(), beforeRing.size());
                std::memcpy(memory.TranslateVirtual(stack), beforeStack.data(), beforeStack.size());
                ctx = beforeCtx;
                originals.clear();
                writes.clear();
#if REREVVED_DEVELOPER
                TakeSetPredicationConsumeCounts();
                TakeSetPredicationPreSwapCounts();
                TakeSetPredicationExitCounts();
                TakeGuestApiCounts();
#endif
                if (native)
                {
                    SetPredicationPreSwapScope token(ctx, device.g);
                    sub_8269CD20(ctx, base);
                }
                else
                    sub_8269CD20(ctx, base);
                UNSCOPED_INFO("default-off complete guest bytes and Original write log match the old route");
                CHECK((guestBytes(memory, device.g) == expectedDevice && guestBytes(memory, ring) == expectedRing && guestBytes(memory, stack) == expectedStack && writes == expectedWrites));
                UNSCOPED_INFO("default-off observable registers and Original count/order match");
                CHECK((std::memcmp(&ctx, &expectedContext, sizeof(ctx)) == 0 && originalCalls(kSetPredicationAddress) == 1 && originals.size() == 1));
#if REREVVED_DEVELOPER
                const auto counts = TakeSetPredicationConsumeCounts();
                const auto pre    = TakeSetPredicationPreSwapCounts();
                const auto exit   = TakeSetPredicationExitCounts();
                const auto api    = TakeGuestApiCounts();
                for (std::size_t i = 0; i < api.size(); ++i)
                {
                    UNSCOPED_INFO("default-off census role counted once regardless of consume/probe/native flags");
                    CHECK(api[i] == (i == static_cast<std::size_t>(GuestApiRole::RingInternal) ? 1U : 0U));
                }
                UNSCOPED_INFO("default-off never consumed");
                CHECK((counts.consumed == 0 && counts.consumedClear == 0));
                for (std::size_t i = 0; i < kSetPredicationFallbackCount; ++i)
                {
                    UNSCOPED_INFO("default-off aggregate refusal counters unchanged");
                    CHECK(counts.fallback[i] == (native && existing && i == static_cast<std::size_t>(probe ? SetPredicationFallback::Diagnostics : SetPredicationFallback::PreSwap) ? 1U : 0U));
                }
                UNSCOPED_INFO("new counters gated by probe or new flag");
                CHECK((pre.entered == (native && probe ? 1U : 0U) && pre.original == pre.entered && pre.consumed == 0));
                UNSCOPED_INFO("probe-off reads no exit, probe-on partitions exact sample");
                CHECK((exit.ok == (probe ? 1U : 0U) && exit.preSwap.exact == (native && probe ? 1U : 0U)));
#endif
            }
    setPredicationBody = {};
    REXCVAR_SET(native_consume_set_predication, true);
    DEV_CVAR_SET(native_set_predication_exit_check, false);
    ringStubsActive.store(true);
#if REREVVED_DEVELOPER
    EnableGuestApiCensus(false);
#endif
}

void realPreSwapConsumeDifferential(rex::memory::Memory& memory, std::uint8_t* base)
{
    Device device(memory);
    device.store(device.g + kProgressBlock, device.block);
    const auto ring  = allocatePhysical(memory);
    const auto stack = allocatePhysical(memory);
    const auto pcr   = allocatePhysical(memory);
    device.store(pcr + 0x70, stack + 0x10000);
    device.store(pcr + 0x74, stack);
    for (const std::uint32_t flip : { 0U, 0x400U })
        for (const std::uint32_t interval : { 0U, 1U, 2U, 4U, 0x80000000U })
            for (const int limitDelta : { -4, 0, 4, 256 })
            {
                std::vector<std::uint8_t>                              referenceDevice, referenceRing;
                RingStubCounts                                         referenceCounts;
                PPCContext                                             referenceCtx{};
                std::optional<std::pair<std::uint32_t, std::uint32_t>> referenceInterval;
                std::vector<std::pair<std::uint32_t, std::uint32_t>>   referenceEffects;
                for (const bool enabled : { false, true })
                {
                    Session session(device);
                    device.store(device.g + kCursor, ring);
                    device.store(device.g + kCursorLimit, ring + limitDelta);
                    device.store(device.g + kSwapInterval, interval);
                    device.store(device.g + kModeLow, 0x20123456);
                    device.store(device.g + 0x31AC, 7);
                    device.store(device.g + 0x31A4, 0x12345678);
                    device.store(device.g + 0x31A8, 0x55);
                    device.store(device.g + 0x2AB4, 0x66);
                    device.store(device.g + 0x2AB8, 0x77);
                    device.store(device.scratch + completion::kScratchFlipOffset, 0);
                    device.store(device.scratch + completion::kScratchMaskOffset, 0);
                    std::memset(memory.TranslateVirtual(ring), 0x5A, 0x10000);
                    std::vector<SetPredicationWrite> writes;
                    setPredicationBody = [&](PPCContext& c)
                    {
                        clearPredicationOriginal(device, c, base, writes);
                    };
                    REXCVAR_SET(native_consume_set_predication_preswap, enabled);
#if REREVVED_DEVELOPER
                    TakeSetPredicationConsumeCounts();
                    TakeSetPredicationPreSwapCounts();
#endif
                    PPCContext ctx{};
                    ctx.r3.u64  = device.g;
                    ctx.r4.u64  = flip;
                    ctx.r1.u64  = stack + 0x8000;
                    ctx.r13.u64 = pcr;
                    ctx.r31.u64 = 0x12345678ABCDEF01ULL;
                    ctx.lr      = kCaller;
                    decodedInterval.reset();
                    sub_826A4150(ctx, base);
                    const auto counts = TakeRingStubCounts();
#if REREVVED_DEVELOPER
                    const auto pre      = TakeSetPredicationPreSwapCounts();
                    const auto consumed = TakeSetPredicationConsumeCounts();
#endif
                    auto effects = originals;
                    std::erase_if(effects, [](const auto& effect)
                                  {
                                      return effect.first == kSetPredicationAddress;
                                  });
                    if (!enabled)
                    {
                        referenceDevice   = guestBytes(memory, device.g);
                        referenceRing     = guestBytes(memory, ring);
                        referenceCtx      = ctx;
                        referenceInterval = decodedInterval;
                        referenceCounts   = counts;
                        referenceEffects  = effects;
                        UNSCOPED_INFO("off host stub keeps Original and no new counters");
                        CHECK((originalCalls(kSetPredicationAddress) == 1 && DEV_CHECK(pre.entered == 0)));
                    }
                    else
                    {
                        UNSCOPED_INFO("real wrapper consume equals Original device/packets/cursor/registers for both flip forms and all boundaries");
                        CHECK((guestBytes(memory, device.g) == referenceDevice && guestBytes(memory, ring) == referenceRing && std::memcmp(&ctx, &referenceCtx, sizeof(ctx)) == 0));
                        UNSCOPED_INFO("retained flush and callback routes and interval unchanged");
                        CHECK((decodedInterval == referenceInterval && counts.stubbed == referenceCounts.stubbed && counts.original == referenceCounts.original && effects == referenceEffects));
                        const bool flush = limitDelta < 0;
                        UNSCOPED_INFO("Original runs exactly for the above-limit call");
                        CHECK(originalCalls(kSetPredicationAddress) == (flush ? 1U : 0U));
#if REREVVED_DEVELOPER
                        UNSCOPED_INFO("actual Original and consumed accounting at final wrapper decision");
                        CHECK((pre.entered == 1 && pre.consumed == (flush ? 0U : 1U) && pre.original == (flush ? 1U : 0U) && originalCalls(kSetPredicationAddress) == pre.original));
                        UNSCOPED_INFO("above-limit call falls back before replay");
                        CHECK((consumed.consumed == pre.consumed && pre.fallback[static_cast<std::size_t>(SetPredicationFallback::RingFlush)] == (flush ? 1U : 0U)));
#endif
                    }
                    UNSCOPED_INFO("host continuation preserves LR, stack and nonvolatile register");
                    CHECK((ctx.lr == kCaller && ctx.r1.u32 == stack + 0x8000 && ctx.r31.u64 == 0x12345678ABCDEF01ULL));
                    UNSCOPED_INFO("BF40 receives the advanced CD20 cursor exactly once");
                    CHECK(device.load(device.g + kCursor) == ring + 8 + 148);
                    session.sink.handler = [&]
                    {
                        device.store(device.scratch + completion::kScratchFlipOffset, 0);
                        device.clearCpuBit();
                    };
                    session.ledger->Kick(0x40);
                    for (int i = 0; i < 4; ++i)
                        session.ledger->Pump();
                    const auto code = interval == 4 ? 3U : interval == 2         ? 2U
                                                       : interval == 0x80000000U ? 0U
                                                                                 : 1U;
                    UNSCOPED_INFO("packed callback argument retained");
                    CHECK(device.load(device.scratch + completion::kScratchParameterOffset) == ((code << 8) | 0x15U | flip));
                    UNSCOPED_INFO("same callback retirement and no ledger failure");
                    CHECK((session.sink.interrupts.size() == 1 && session.failed.empty()));
                }
            }
    setPredicationBody = {};
    REXCVAR_SET(native_consume_set_predication_preswap, false);
}

void realCensusPreSwapGuards(rex::memory::Memory& memory, std::uint8_t* base)
{
    Device device(memory);
    device.store(device.g + kProgressBlock, device.block);
    const auto ring  = allocatePhysical(memory);
    const auto stack = allocatePhysical(memory);
    const auto pcr   = allocatePhysical(memory);
    device.store(pcr + 0x70, stack + 0x10000);
    device.store(pcr + 0x74, stack);
    Session session(device);
    REXCVAR_SET(native_consume_set_predication_preswap, true);
    std::vector<SetPredicationWrite> writes;
    setPredicationBody = [&](PPCContext& ctx)
    {
        clearPredicationOriginal(device, ctx, base, writes);
    };
    const auto call = [&](bool trusted)
    {
        device.store(device.g + kCursor, ring);
        device.store(device.g + kCursorLimit, ring + 0x100);
        PPCContext ctx{};
        ctx.r3.u64  = device.g;
        ctx.r1.u64  = stack + 0x8000;
        ctx.r13.u64 = pcr;
        ctx.lr      = kSetPredicationPreSwapReturn;
        originals.clear();
#if REREVVED_DEVELOPER
        TakeSetPredicationConsumeCounts();
        TakeSetPredicationPreSwapCounts();
        TakeSetPredicationExitCounts();
#endif
        if (trusted)
        {
            SetPredicationPreSwapScope token(ctx, device.g);
            sub_8269CD20(ctx, base);
        }
        else
            sub_8269CD20(ctx, base);
        UNSCOPED_INFO("guarded real wrapper executes Original once");
        CHECK((originalCalls(kSetPredicationAddress) == 1 && ctx.r3.u64 == ring + 8));
    };
    ringStubsValidatedDevice.store(device.g);
    call(false);
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("new flag never enables Original A4150's LR-only call");
    CHECK((TakeSetPredicationConsumeCounts().fallback[static_cast<std::size_t>(SetPredicationFallback::PreSwap)] == 1 && TakeSetPredicationPreSwapCounts().entered == 0));
#endif
    ringStubsValidatedDevice.store(0);
    call(true);
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("unvalidated device counts scoped stubs refusal");
    CHECK(TakeSetPredicationPreSwapCounts().fallback[static_cast<std::size_t>(SetPredicationFallback::Stubs)] == 1);
#endif
    ringStubsValidatedDevice.store(device.g);
#if REREVVED_DEVELOPER
    REXCVAR_SET(native_set_predication_exit_check, true);
    call(true);
    UNSCOPED_INFO("exit check forces actual Original and exact scoped probe even with consume enabled");
    CHECK((TakeSetPredicationPreSwapCounts().fallback[static_cast<std::size_t>(SetPredicationFallback::Diagnostics)] == 1 && TakeSetPredicationExitCounts().preSwap.exact == 1));
    REXCVAR_SET(native_set_predication_exit_check, false);
    rerevved::gpu::diagnostics::routeTraceEnabled.store(1);
    call(true);
    UNSCOPED_INFO("route diagnostic retains Original");
    CHECK(TakeSetPredicationPreSwapCounts().fallback[static_cast<std::size_t>(SetPredicationFallback::Diagnostics)] == 1);
    rerevved::gpu::diagnostics::routeTraceEnabled.store(0);
#endif
    SetRingStubLeaseProbe([]() noexcept
                          {
                              ringStubsActive.store(false);
                          });
    call(true);
    SetRingStubLeaseProbe(nullptr);
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("withdrawal before lease cannot consume");
    CHECK(TakeSetPredicationPreSwapCounts().fallback[static_cast<std::size_t>(SetPredicationFallback::Stubs)] == 1);
#endif
    ringStubsActive.store(true);
    REXCVAR_SET(native_consume_set_predication, false);
    call(true);
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("base consume gate still required");
    CHECK((TakeSetPredicationPreSwapCounts().consumed == 0 && TakeSetPredicationConsumeCounts().consumed == 0));
#endif
    REXCVAR_SET(native_consume_set_predication, true);
    ringStubsActive.store(false);
    call(true);
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("unpublished dispatch cannot consume");
    CHECK(TakeSetPredicationConsumeCounts().consumed == 0);
#endif
    ringStubsActive.store(true);

#if REREVVED_DEVELOPER
    REXCVAR_SET(native_set_predication_exit_check, true);
    setPredicationBody = [](PPCContext&)
    {
        throw std::runtime_error("unwind fixture");
    };
    try
    {
        call(true);
        UNSCOPED_INFO("Original unwind must propagate from direct census call");
        CHECK(false);
    }
    catch (const std::runtime_error&)
    {
    }
    auto       exits = TakeSetPredicationExitCounts();
    const auto pre   = TakeSetPredicationPreSwapCounts();
    UNSCOPED_INFO("unwound trusted Original cannot count as a match");
    CHECK((pre.entered == 1 && pre.original == 1 && exits.preSwap.unpaired == 1 && exits.preSwap.ok == 0 && exits.preSwap.exact == 0));
    {
        PPCContext                 ctx{};
        SetPredicationPreSwapScope abandoned(ctx, device.g);
    }
    exits = TakeSetPredicationExitCounts();
    UNSCOPED_INFO("abandoned host token cannot count as a match");
    CHECK((exits.preSwap.unpaired == 1 && exits.preSwap.exact == 0));
    REXCVAR_SET(native_set_predication_exit_check, false);
#endif
    setPredicationBody = {};
    REXCVAR_SET(native_consume_set_predication_preswap, false);
}

std::uint64_t stubbed(const RingStubCounts& counts, RingStub stub)
{
    return counts.stubbed[static_cast<std::size_t>(stub)];
}

std::uint64_t fellBack(const RingStubCounts& counts, RingStub stub)
{
    return counts.original[static_cast<std::size_t>(stub)];
}

// Not-ready device keeps EndTiling/BeginTiling Original; precheck counts stay Original.
void endTilingFallsBackWhenTheDeviceIsNotReady(rex::memory::Memory& memory, std::uint8_t* base)
{
    Device  device(memory);
    Session session(device);
    tiling.reset(&memory, allocatePhysical(memory));

    PPCContext ctx{};
    ctx.r3.u64 = device.g;
    ctx.lr     = kCaller;
    sub_826AC0A8(ctx, base);
    ctx.r3.u64 = device.g;
    ctx.r5.u64 = kEndTilingFlags;
    sub_826AC218(ctx, base);
    ctx.r3.u64 = device.g + 0x2AC0;
    sub_826AB740(ctx, base);
    ctx.r3.u64 = 0;
    sub_826AC020(ctx, base);

    UNSCOPED_INFO("EndTiling, BeginTiling, the token handler and the worker kick run as Original");
    CHECK((originalCalls(0x826AC218) == 1 && originalCalls(0x826AC0A8) == 1 && originalCalls(0x826AB740) == 1 && originalCalls(0x826AC020) == 1));
    UNSCOPED_INFO("the fallback runs none of EndTiling's helpers itself");
    CHECK((tiling.allocations.empty() && tiling.terminated.empty() && tiling.submissions.empty()));
    const auto counts = TakeRingStubCounts();
    UNSCOPED_INFO("the not-ready device counts tiling_end_original");
    CHECK((fellBack(counts, RingStub::TilingEnd) == 1 && stubbed(counts, RingStub::TilingEnd) == 0));
    UNSCOPED_INFO("before a device is checked the bracket counts are Original calls");
    CHECK((fellBack(counts, RingStub::TilingBegin) == 1 && fellBack(counts, RingStub::TilingTokens) == 1 && fellBack(counts, RingStub::TilingCallbacks) == 1));
    UNSCOPED_INFO("the fallback is no ledger failure");
    CHECK(session.failed.empty());
}

// EndTiling submits callback before worker fence; B+4 precedes B+0 and RPTR is last.
void endTilingOrdersCallbackThenFenceThroughTheLedger(rex::memory::Memory& memory, std::uint8_t* base)
{
    Device device(memory);
    device.store(device.g + kProgressBlock, device.block);
    device.store(device.g + kRingWriteIndex, 0x10);
    const std::uint32_t list    = allocatePhysical(memory);
    const std::uint32_t segment = allocatePhysical(memory);
    device.store(device.g + kTileList, list);
    Session             session(device);
    const std::uint32_t arena = allocatePhysical(memory);
    tiling.reset(&memory, arena);

    PPCContext ctx{};
    ctx.r3.u64 = device.g;
    ctx.lr     = kCaller;
    sub_826AC0A8(ctx, base);
    // 0x8269C400's tiled segment branch.
    completion::detail::RecordTiledSegment(device.physical(segment), 0x10);

    ctx        = {};
    ctx.r3.u64 = device.g;
    ctx.r5.u64 = kEndTilingFlags;
    ctx.lr     = kCaller;
    sub_826AC218(ctx, base);

    UNSCOPED_INFO("the stub returns 0 to its caller");
    CHECK((ctx.r3.u32 == 0 && ctx.lr == kCaller));
    UNSCOPED_INFO("the callback, fence and token buffers are allocated in order");
    CHECK((tiling.allocations == std::vector<std::pair<std::uint32_t, std::uint32_t>>{ { 148, 32 }, { 40, 32 }, { 48, 4 } }));
    std::vector<std::uint32_t> lists;
    for (const auto offset : kEndTilingLists)
        lists.push_back(device.g + offset);
    UNSCOPED_INFO("the seven lists are terminated in order");
    CHECK(tiling.terminated == lists);
    UNSCOPED_INFO("the callback, fence, EndTiling and publisher Originals do not run");
    CHECK((originalCalls(0x8269BF40) == 0 && originalCalls(0x8269BCA8) == 0 && originalCalls(0x826AC218) == 0 && originalCalls(0x8269B900) == 0));
    // The bump allocator hands out arena, arena + 608 and arena + 768.
    const std::uint32_t fencePhysical = CommandBufferPhysical(arena + 608);
    UNSCOPED_INFO("only the fence buffer's submit runs Original, chained onto the tiled list with its 10 dwords");
    CHECK(tiling.submissions == std::vector<Submission>{ { 0x826AC3E8, arena + 768, fencePhysical, 10, 0, device.g + kTileList } });
    UNSCOPED_INFO("the callback submit raises G+0x2B00 for the worker");
    CHECK(device.load(device.g + kWorkerPending) == 1);
    UNSCOPED_INFO("G+0x33B0 and G+0x33B4 are cleared");
    CHECK((device.load(device.g + kTiling) == 0 && device.load(device.g + kTilingNext) == 0));
    UNSCOPED_INFO("no ring word is written; the fence advances F");
    CHECK((device.load(device.g + kCursor) == kStartAt && device.load(device.g + kNextFence) == 0x13));
    ctx.r3.u64 = device.g + 0x2AC0;
    sub_826AB740(ctx, base);
    const auto counts = TakeRingStubCounts();
    UNSCOPED_INFO("BeginTiling, EndTiling and the token handler are counted for the checked device");
    CHECK((stubbed(counts, RingStub::TilingBegin) == 1 && stubbed(counts, RingStub::TilingEnd) == 1 && stubbed(counts, RingStub::TilingTokens) == 1 &&
           fellBack(counts, RingStub::TilingEnd) == 0));
    UNSCOPED_INFO("the callback and fence are stubbed; the direct submit is stubbed and the deferred one runs Original");
    CHECK((stubbed(counts, RingStub::Callback) == 1 && stubbed(counts, RingStub::Fence) == 1 && stubbed(counts, RingStub::Submit) == 1 &&
           fellBack(counts, RingStub::Submit) == 1));

    session.sink.handler = [&]
    {
        device.clearCpuBit();
    };
    while (session.ledger->Pump())
    {
    }
    const auto                                                 parameter = CommandBufferPhysical(list) + 0xC0000000;
    const auto                                                 scratch   = device.physical(device.scratch);
    const auto                                                 block     = device.physical(device.block);
    const auto                                                 writeback = block + completion::kReadPointerOffset;
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> started{
        { scratch + completion::kScratchCallbackOffset, kWorkerKick },
        { scratch + completion::kScratchParameterOffset, parameter },
        { scratch + completion::kScratchMaskOffset, 1U << completion::kCallbackCpu },
        { scratch + completion::kScratchCallbackOffset, completion::kRetiredCallback },
        { writeback, 0x10 },
    };
    UNSCOPED_INFO("the worker-kick callback raises once on CPU 2 with the tiled list's parameter, then RPTR");
    CHECK((session.sink.writes == started && session.sink.interrupts == std::vector<std::uint32_t>{ completion::kCallbackCpu }));
    UNSCOPED_INFO("the EndTiling fence waits for the worker");
    CHECK(session.ledger->Snapshot().oldestRecord.starts_with("awaiting worker submit"));
    // The guest delivers the callback to 0x826AC020.
    ctx.r3.u64 = parameter;
    sub_826AC020(ctx, base);

    std::thread([&]
                {
                    session.ledger->MarkReplayThread();
                    session.ledger->ReplaySubmit(device.physical(segment), 0x10, 0x826AB890);
                    session.ledger->Kick(0x18);
                    session.ledger->ReplaySubmit(fencePhysical, 10, 0x826AB890);
                    session.ledger->Kick(0x20);
                })
        .join();
    while (session.ledger->Pump())
    {
    }
    // The tile replay is host-only; RPTR passes it only with the deferred fence.
    auto expected = started;
    expected.insert(expected.end(), { { block + completion::kCursorOffset, kStartAt }, { block, 0x11 }, { writeback, 0x20 } });
    const auto snapshot = session.ledger->Snapshot();
    UNSCOPED_INFO("the fence releases after the worker submits its buffer: B+4, then B+0, then RPTR");
    CHECK((session.sink.writes == expected && !snapshot.pending && snapshot.deferredSpans == 1 && snapshot.callbacksReleased == 1));
    const auto delivered = TakeRingStubCounts();
    UNSCOPED_INFO("the delivery is counted and runs as guest code");
    CHECK((stubbed(delivered, RingStub::TilingCallbacks) == 1 && originalCalls(0x826AC020) == 1));
    UNSCOPED_INFO("the bracket is no ledger failure");
    CHECK(session.failed.empty());
}

// Worker ownership chains callback before fence and holds their effects.
void endTilingChainsBothBuffersWhileTheWorkerOwnsTheRing(rex::memory::Memory& memory, std::uint8_t* base)
{
    // Callback uses 35 of 148 reserved dwords with G+0x2B00 + 1; the fence
    // chains behind its token.
    Device device(memory);
    device.store(device.g + kProgressBlock, device.block);
    device.store(device.g + kWorkerPending, 1);
    device.store(device.g + kTiling, 1);
    Session             session(device);
    const std::uint32_t arena = allocatePhysical(memory);
    tiling.reset(&memory, arena);

    PPCContext ctx{};
    ctx.r3.u64 = device.g;
    ctx.r5.u64 = kEndTilingFlags;
    ctx.lr     = kCaller;
    sub_826AC218(ctx, base);

    // The bump allocator hands out arena, arena + 608 and arena + 768.
    const std::uint32_t           callbackBuffer = arena, fenceBuffer = arena + 608, tokenBuffer = arena + 768;
    const std::vector<Submission> expected{
        { 0x826AC3B4, tokenBuffer, CommandBufferPhysical(callbackBuffer), 35, 1, device.g + kPreviousList },
        { 0x826AC3E8, tokenBuffer + 16, CommandBufferPhysical(fenceBuffer), 10, 0, device.g + kTileList },
    };
    UNSCOPED_INFO("the callback buffer is chained with 35 dwords and raise 1, then the fence buffer with 10");
    CHECK(tiling.submissions == expected);
    UNSCOPED_INFO("the guest translation matches the heap's");
    CHECK(CommandBufferPhysical(callbackBuffer) == device.physical(callbackBuffer));
    UNSCOPED_INFO("the bracket closes");
    CHECK((device.load(device.g + kTiling) == 0 && ctx.r3.u32 == 0));
    const auto counts = TakeRingStubCounts();
    UNSCOPED_INFO("both submits take the deferred Original path");
    CHECK((stubbed(counts, RingStub::TilingEnd) == 1 && fellBack(counts, RingStub::Submit) == 2 && stubbed(counts, RingStub::Submit) == 0));
    UNSCOPED_INFO("chaining is no ledger failure");
    CHECK(session.failed.empty());
}

#if REREVVED_DEVELOPER

// 0x8269B118 allocation failure clears bit 0x20, returns r3=1 and emits nothing.
void rawTilingRestoreDispatchAndLease(rex::memory::Memory& memory, std::uint8_t* base)
{
    for (int arm = 0; arm < 8; ++arm)
    {
        Device device(memory);
        if (arm == 7)
        {
            auto*         heap    = memory.LookupHeap(0x40000000);
            std::uint32_t address = 0;
            UNSCOPED_INFO("ordinary virtual device allocation succeeds");
            CHECK((heap && heap->Alloc(0x10000, 0x10000, kAllocation, kWritable, false, &address)));
            device.g = address;
        }
        Session session(device);
        tiling.reset(&memory, allocatePhysical(memory));
        ringStubsValidatedDevice.store(device.g);
        REXCVAR_SET(native_consume_tiling_restore, arm != 0 && arm != 2);
        REXCVAR_SET(native_tiling_restore_exit_check, arm == 2 || arm == 3 || arm == 6);
        if (arm == 2 || arm == 6)
            WithdrawRingStubServices();
        if (arm == 4)
            ringStubsValidatedDevice.store(device.g + 4);
        if (arm == 5)
            SetRingStubLeaseProbe(+[]() noexcept
                                  {
                                      WithdrawRingStubServices();
                                  });
        for (std::uint32_t i = 0; i < 6; ++i)
            device.store(device.g + 0x3460 + i * 4, 0x9137B5D3 + i * 0x1723);
        PPCContext ctx{};
        ctx.r3.u64 = 0x937B51D300000000ULL | device.g;
        ctx.r4.u64 = 0xB35D7129F183C547;
        ctx.lr     = kCaller;
        (void)TakeTilingRestoreCounts();
        sub_8269B118(ctx, base);
        SetRingStubLeaseProbe(nullptr);
        const auto c        = TakeTilingRestoreCounts();
        const bool consumed = arm == 1 || arm == 7;
        UNSCOPED_INFO("raw direct B118 executes exactly one Original or seven replay stores");
        CHECK((originalCalls(0x8269B118) == (consumed ? 0U : 1U) && tiling.restoreWrites.size() == (consumed ? 0U : 7U)));
        UNSCOPED_INFO("raw B118 preserves full incoming r3 r4 and LR");
        CHECK((ctx.r3.u64 == (0x937B51D300000000ULL | device.g) && ctx.r4.u64 == 0xB35D7129F183C547 && ctx.lr == kCaller));
        UNSCOPED_INFO("raw B118 returns exact lwz/addi outputs");
        CHECK((ctx.r11.u64 == 0x9137B5D3U + 0x1723 && ctx.r8.u64 == std::uint64_t{ 0x9137B5D3U + 0x1723 } - 160 &&
               device.load(device.g + 0x38) == 0x9137B5D3U + 0x1723 - 160));
        UNSCOPED_INFO("raw dispatch counter census");
        CHECK((c.enter == c.consumed + c.original && c.enter == (arm ? 1U : 0U) &&
               c.consumed == (consumed ? 1U : 0U) && c.original == (arm && !consumed ? 1U : 0U)));
        UNSCOPED_INFO("checker runs without published ring stubs and wins over consume");
        CHECK(c.exitChecked == (arm == 2 || arm == 3 || arm == 6 ? 1U : 0U));
        UNSCOPED_INFO("checker-only calls invent no consume refusals");
        CHECK(c.fallback[static_cast<std::size_t>(TilingRestoreFallback::ExitCheck)] == (arm == 3 || arm == 6 ? 1U : 0U));
        if (arm == 4)
        {
            UNSCOPED_INFO("lease rechecks same validated device");
            CHECK(c.fallback[static_cast<std::size_t>(TilingRestoreFallback::Device)] == 1);
        }
        if (arm == 5)
        {
            UNSCOPED_INFO("lease withdrawal refuses before any replay");
            CHECK(c.fallback[static_cast<std::size_t>(TilingRestoreFallback::Stubs)] == 1);
        }
    }
    REXCVAR_SET(native_consume_tiling_restore, false);
    REXCVAR_SET(native_tiling_restore_exit_check, false);
}

#endif

// The shipping build forwards B118 to its Original once; the developer build also
// runs it under the default-off tiling restore options.
void endTilingDropsTheBracketWhenAnAllocationFails(rex::memory::Memory& memory, std::uint8_t* base)
{
    constexpr int arms = REREVVED_DEVELOPER ? 3 : 1;
    for (int arm = 0; arm < arms; ++arm)
        for (std::size_t failure = 0; failure < 4; ++failure)
        {
            Device device(memory);
            device.store(device.g + kProgressBlock, device.block);
            device.store(device.g + kTiling, 1);
            device.store(device.g + kTilingNext, 1);
            constexpr std::array<std::uint32_t, 6> saved{ 0x19375281, 0x91A3B5C7, 0xD26E4953, 0x85B36D17, 0x6F291D83, 0xE172C54B };
            for (std::size_t i = 0; i < saved.size(); ++i)
                device.store(device.g + 0x3460 + static_cast<std::uint32_t>(4 * i), saved[i]);
            auto* mode = memory.TranslateVirtual<std::uint8_t*>(device.g + kModeHigh);
            *mode      = failure == 3 ? 0xA5 : 0x85;
            std::vector<std::uint8_t> expected(0x3A80);
            std::memcpy(expected.data(), memory.TranslateVirtual(device.g), expected.size());
            const auto put = [&](std::size_t address, std::uint32_t value)
            {
                expected[address]     = static_cast<std::uint8_t>(value >> 24);
                expected[address + 1] = static_cast<std::uint8_t>(value >> 16);
                expected[address + 2] = static_cast<std::uint8_t>(value >> 8);
                expected[address + 3] = static_cast<std::uint8_t>(value);
            };
            // Independent whole failure exit: active clear, restore, failure clear, full r3=1.
            put(0x33B0, 0);
            put(0x33B4, 0);
            put(0x30, saved[0]);
            put(0x34, saved[1]);
            put(0x3A50, saved[2]);
            put(0x3A4C, saved[3]);
            put(0x3A44, saved[4]);
            put(0x3A48, saved[5]);
            put(0x38, saved[1] - 160U);
            expected[0x2ABD] &= ~0x20U;
            Session    session(device);
            const auto arena = allocatePhysical(memory);
            tiling.reset(&memory, arena);
            if (failure != 3)
                tiling.failAllocation = failure;
#if REREVVED_DEVELOPER
            REXCVAR_SET(native_consume_tiling_restore, arm == 2);
            REXCVAR_SET(native_tiling_restore_exit_check, arm == 1);
            (void)TakeTilingRestoreCounts();
#endif
            PPCContext ctx{};
            ctx.r3.u64 = device.g;
            ctx.r5.u64 = kEndTilingFlags;
            ctx.lr     = kCaller;
            sub_826AC218(ctx, base);
            UNSCOPED_INFO("allocation1/2/3 and preexisting failure match full independent byte exit and full r3=1");
            CHECK((ctx.r3.u64 == 1 && std::memcmp(expected.data(), memory.TranslateVirtual(device.g), expected.size()) == 0));
            UNSCOPED_INFO("all three B818 attempts retain order and arguments");
            CHECK((tiling.allocations == std::vector<std::pair<std::uint32_t, std::uint32_t>>{ { 148, 32 }, { 40, 32 }, { 48, 4 } }));
            std::uint32_t expectedNext = arena;
            for (std::size_t i = 0; i < tiling.allocations.size(); ++i)
                if (i != failure)
                {
                    const auto [words, alignment] = tiling.allocations[i];
                    expectedNext                  = ((expectedNext + alignment - 1) & ~(alignment - 1)) + 4 * words;
                }
            UNSCOPED_INFO("successful earlier allocations are retained without rollback");
            CHECK(tiling.next == expectedNext);
            for (const auto& [address, marker] : tiling.allocationBytes)
            {
                UNSCOPED_INFO("successful allocation bytes survive failure");
                CHECK(tiling.load(address) == marker);
            }
            UNSCOPED_INFO("failure emits no B508 BF40 BCA8 or BD98 work");
            CHECK((tiling.terminated.empty() && tiling.submissions.empty() && originalCalls(0x8269BF40) == 0 && originalCalls(0x8269BCA8) == 0 && originalCalls(0x8269BD98) == 0));
#if REREVVED_DEVELOPER
            const auto tr = TakeTilingRestoreCounts();
            UNSCOPED_INFO("callGuest restore dispatch totals reconcile");
            CHECK((tr.enter == tr.consumed + tr.original && tr.consumed == (arm == 2 ? 1U : 0U) && tr.original == (arm == 1 ? 1U : 0U)));
#endif
            UNSCOPED_INFO("callGuest reaches the sole raw wrapper exactly once");
            CHECK(originalCalls(0x8269B118) == (arm == 2 ? 0U : 1U));
            if (arm != 2)
            {
                UNSCOPED_INFO("active clears precede restore and failure clear follows it");
                CHECK((tiling.restoreActive == 0 && tiling.restoreNext == 0 && (tiling.restoreMode & 0x20) != 0));
                const std::vector<std::pair<std::uint32_t, std::uint32_t>> trace{
                    { device.g + 0x30, saved[0] }, { device.g + 0x34, saved[1] }, { device.g + 0x3A50, saved[2] }, { device.g + 0x3A4C, saved[3] }, { device.g + 0x3A44, saved[4] }, { device.g + 0x3A48, saved[5] }, { device.g + 0x38, saved[1] - 160U }
                };
                UNSCOPED_INFO("Original fixture executes all seven stores in order");
                CHECK(tiling.restoreWrites == trace);
            }
#if REREVVED_DEVELOPER
            UNSCOPED_INFO("checker validates independent failure restore");
            CHECK((tr.exitChecked == (arm == 1 ? 1U : 0U) && tr.exitMismatch == 0 && tr.exitAbiMismatch == 0));
#endif
            const auto counts = TakeRingStubCounts();
            UNSCOPED_INFO("no callback fence or C400 publication is emitted");
            CHECK((stubbed(counts, RingStub::TilingEnd) == 1 && stubbed(counts, RingStub::Callback) == 0 && stubbed(counts, RingStub::Fence) == 0 && stubbed(counts, RingStub::Publish) == 0));
            UNSCOPED_INFO("dropped bracket is no ledger failure");
            CHECK(session.failed.empty());
        }
#if REREVVED_DEVELOPER
    REXCVAR_SET(native_consume_tiling_restore, false);
    REXCVAR_SET(native_tiling_restore_exit_check, false);
#endif
}

// Allocation failure leaves ledger empty; callbacks/fences stay unseen; kick writes RPTR.
void endTilingAllocationFailureLeavesTheLedgerEmpty(rex::memory::Memory& memory, std::uint8_t* base)
{
    Device device(memory);
    device.store(device.g + kProgressBlock, device.block);
    device.store(device.g + kTiling, 1);
    Session session(device);
    tiling.reset(&memory, allocatePhysical(memory));
    tiling.failAllocation = 1;

    PPCContext ctx{};
    ctx.r3.u64 = device.g;
    ctx.r5.u64 = kEndTilingFlags;
    ctx.lr     = kCaller;
    sub_826AC218(ctx, base);

    UNSCOPED_INFO("the dropped bracket returns 1 and runs neither the callback nor the fence emitter");
    CHECK((ctx.r3.u32 == 1 && originalCalls(0x8269BF40) == 0 && originalCalls(0x8269BCA8) == 0 && tiling.submissions.empty()));
    UNSCOPED_INFO("the fence is not advanced and no ring word or worker count changes");
    CHECK((device.load(device.g + kNextFence) == 0x11 && device.load(device.g + kCursor) == kStartAt && device.load(device.g + kWorkerPending) == 0));
    session.sink.handler = [&]
    {
        device.clearCpuBit();
    };
    session.ledger->Kick(0x10);
    while (session.ledger->Pump())
    {
    }
    const auto writeback = device.physical(device.block) + completion::kReadPointerOffset;
    const auto snapshot  = session.ledger->Snapshot();
    UNSCOPED_INFO("the ledger holds no callback or fence for the dropped bracket and fails nothing; the kick writes RPTR alone");
    CHECK((session.sink.writes == std::vector<std::pair<std::uint32_t, std::uint32_t>>{ { writeback, 0x10 } } && session.sink.interrupts.empty() &&
           snapshot.spansReleased == 1 && snapshot.callbacksReleased == 0 && !snapshot.pending && session.failed.empty()));
}

// Missing worker submit holds callback/fence/RPTR; close drops all effects.
void endTilingWorkerSubmitNeverArrives(rex::memory::Memory& memory, std::uint8_t* base)
{
    Device device(memory);
    device.store(device.g + kProgressBlock, device.block);
    device.store(device.g + kWorkerPending, 1);
    device.store(device.g + kTiling, 1);
    Session             session(device);
    const std::uint32_t arena = allocatePhysical(memory);
    tiling.reset(&memory, arena);

    PPCContext ctx{};
    ctx.r3.u64 = device.g;
    ctx.r5.u64 = kEndTilingFlags;
    ctx.lr     = kCaller;
    sub_826AC218(ctx, base);
    UNSCOPED_INFO("both buffers are chained behind the worker");
    CHECK((ctx.r3.u32 == 0 && tiling.submissions.size() == 2));

    session.sink.handler = [&]
    {
        device.clearCpuBit();
    };
    session.ledger->Kick(0x18); // the next main-thread publication queues behind the bracket
    for (int i = 0; i < 4; ++i)
        session.ledger->Pump();
    const auto snapshot = session.ledger->Snapshot();
    UNSCOPED_INFO("no callback, fence or RPTR is written while the worker submits are missing");
    CHECK((session.sink.writes.empty() && session.sink.interrupts.empty() && snapshot.callbacksReleased == 0 && snapshot.pending &&
           snapshot.oldestRecord.starts_with("awaiting worker submit") && session.failed.empty()));

    session.ledger->Close();
    UNSCOPED_INFO("closing drops the bracket without writing");
    CHECK((!session.ledger->Pump() && session.sink.writes.empty()));
    std::thread([&]
                {
                    session.ledger->MarkReplayThread();
                    session.ledger->ReplaySubmit(CommandBufferPhysical(arena), 35, 0x826AB890);
                    session.ledger->Kick(0x20);
                    session.ledger->ReplaySubmit(CommandBufferPhysical(arena + 608), 10, 0x826AB890);
                    session.ledger->Kick(0x28);
                })
        .join();
    UNSCOPED_INFO("a worker submit after the close writes nothing");
    CHECK((!session.ledger->Pump() && session.sink.writes.empty() && session.sink.interrupts.empty()));
}

// Layout mismatch leaves only ledger writes to S2, B, RPTR; helpers run Original.
void layoutMismatchLeavesTheLedgerTheOnlyWriter(rex::memory::Memory& memory, std::uint8_t* base)
{
    Device device(memory);
    device.store(device.g + kProgressBlock, device.block);
    device.store(device.g + kNextFence, 0x12); // F must be odd
    Session session(device);
    UNSCOPED_INFO("the stubs start active");
    CHECK(ringStubsActive.load());

    PPCContext ctx{};
    ctx.r3.u64 = device.g;
    ctx.r4.u64 = kStartAt;
    ctx.r5.u64 = 0;
    ctx.r6.u64 = 0x826A8BE8;
    ctx.r7.u64 = 0x55;
    ctx.lr     = kCaller;
    sub_8269BF40(ctx, base);
    UNSCOPED_INFO("the mismatch stops the stubs");
    CHECK(!ringStubsActive.load());
    ctx        = {};
    ctx.r3.u64 = device.g;
    ctx.lr     = kCaller;
    sub_8269BCA8(ctx, base);

    UNSCOPED_INFO("the callback and fence emitters each run Original once");
    CHECK((originals == std::vector<std::pair<std::uint32_t, std::uint32_t>>{ { 0x8269BF40, device.g }, { 0x8269BCA8, device.g } }));
    const auto counts = TakeRingStubCounts();
    UNSCOPED_INFO("only the first call consulted the stubs; the fence never did");
    CHECK((fellBack(counts, RingStub::Callback) == 1 && stubbed(counts, RingStub::Callback) == 0 && fellBack(counts, RingStub::Fence) == 0 &&
           stubbed(counts, RingStub::Fence) == 0));
    const auto scratch   = device.physical(device.scratch);
    const auto block     = device.physical(device.block);
    const auto writeback = block + completion::kReadPointerOffset;
    UNSCOPED_INFO("neither a stub nor an Original fixture wrote the device, S2 or B");
    CHECK((device.load(device.g + kNextFence) == 0x12 && device.load(device.g + kCursor) == kStartAt && device.load(device.scratch) == 0 &&
           device.load(device.scratch + completion::kScratchCallbackOffset) == 0 && device.load(device.scratch + completion::kScratchParameterOffset) == 0 &&
           device.load(device.block) == 0 && device.load(device.block + completion::kCursorOffset) == 0 && device.load(device.block + completion::kReadPointerOffset) == 0));

    session.sink.handler = [&]
    {
        device.clearCpuBit();
    };
    session.ledger->Kick(0x40);
    while (session.ledger->Pump())
    {
    }
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> expected{
        { scratch + completion::kScratchCallbackOffset, 0x826A8BE8 },
        { scratch + completion::kScratchParameterOffset, 0x55 },
        { scratch + completion::kScratchMaskOffset, 1U << completion::kCallbackCpu },
        { scratch + completion::kScratchCallbackOffset, completion::kRetiredCallback },
        { block + completion::kCursorOffset, kStartAt },
        { block + completion::kFenceOffset, 0x12 },
        { writeback, 0x40 },
    };
    UNSCOPED_INFO("the ledger alone writes the callback slot, B+4, B+0 and RPTR, in order");
    CHECK((session.sink.writes == expected && session.sink.interrupts == std::vector<std::uint32_t>{ completion::kCallbackCpu } && session.failed.empty()));
}

#if REREVVED_DEVELOPER
// The six cvars that decide whether EndTiling observes its list: set off for the
// scope whatever their defaults are, and restored at its end.
struct TileListCvars
{
    TileListCvars()
    : indexed(REXCVAR_GET(native_consume_indexed_buffer))
    , indexedTiled(REXCVAR_GET(native_consume_indexed_buffer_tiled))
    , user(REXCVAR_GET(native_consume_user_pointer))
    , userTiled(REXCVAR_GET(native_consume_user_pointer_tiled))
    , lowerer(REXCVAR_GET(native_consume_lowerer))
    , lowererTiled(REXCVAR_GET(native_consume_lowerer_tiled))
    {
        set(false);
    }

    ~TileListCvars()
    {
        REXCVAR_SET(native_consume_indexed_buffer, indexed);
        REXCVAR_SET(native_consume_indexed_buffer_tiled, indexedTiled);
        REXCVAR_SET(native_consume_user_pointer, user);
        REXCVAR_SET(native_consume_user_pointer_tiled, userTiled);
        REXCVAR_SET(native_consume_lowerer, lowerer);
        REXCVAR_SET(native_consume_lowerer_tiled, lowererTiled);
    }

    // The IndexedBuffer and user-pointer pairs; the lowerer's stay off.
    static void set(bool value)
    {
        REXCVAR_SET(native_consume_indexed_buffer, value);
        REXCVAR_SET(native_consume_indexed_buffer_tiled, value);
        REXCVAR_SET(native_consume_user_pointer, value);
        REXCVAR_SET(native_consume_user_pointer_tiled, value);
        REXCVAR_SET(native_consume_lowerer, false);
        REXCVAR_SET(native_consume_lowerer_tiled, false);
    }

    bool indexed, indexedTiled, user, userTiled, lowerer, lowererTiled;
};

// EndTiling reads the list before termination, once, with published/enabled stubs.
void endTilingObservesTheRecordList(rex::memory::Memory& memory, std::uint8_t* base)
{
    constexpr std::uint32_t kChunk = 0x00200000;
    const TileListCvars     cvars;
    const auto              bracket = [&](std::uint32_t cursor)
    {
        Device device(memory);
        device.store(device.g + kProgressBlock, device.block);
        device.store(device.g + kWorkerPending, 1);
        device.store(device.g + kTiling, 1);
        device.store(device.g + kTileListChunkOffset, kChunk);
        device.store(device.g + kTileListCursorOffset, cursor);
        Session session(device);
        tiling.reset(&memory, allocatePhysical(memory));
        PPCContext ctx{};
        ctx.r3.u64 = device.g;
        ctx.r5.u64 = kEndTilingFlags;
        ctx.lr     = kCaller;
        sub_826AC218(ctx, base);
        UNSCOPED_INFO("the bracket ends");
        CHECK((device.load(device.g + kTiling) == 0 && session.failed.empty()));
    };

    // Every pair on and no published services: EndTiling reads no header and runs Original.
    {
        TileListCvars::set(true);
        Device device(memory);
        device.store(device.g + kTiling, 1);
        device.store(device.g + kTileListChunkOffset, kChunk);
        device.store(device.g + kTileListCursorOffset, kChunk + 4 + 32);
        PPCContext ctx{};
        ctx.r3.u64 = device.g;
        ctx.r5.u64 = kEndTilingFlags;
        ctx.lr     = kCaller;
        UNSCOPED_INFO("no services are published");
        CHECK(!RingStubsPublished());
        (void)TakeTileListCounts();
        const auto before = originalCalls(0x826AC218);
        sub_826AC218(ctx, base);
        UNSCOPED_INFO("EndTiling ran as Original without published services");
        CHECK(originalCalls(0x826AC218) == before + 1);
        const auto unpublished = TakeTileListCounts();
        UNSCOPED_INFO("nothing is observed without published services");
        CHECK((unpublished.records == 0 && unpublished.empty == 0 && unpublished.misaligned == 0));
        TileListCvars::set(false);
    }

    REXCVAR_SET(native_consume_indexed_buffer, true);
    REXCVAR_SET(native_consume_indexed_buffer_tiled, true);
    (void)TakeTileListCounts();
    bracket(kChunk + 4 + 16 * 5);
    bracket(kChunk + 4);
    bracket(kChunk + 4 + 6);
    const auto counts = TakeTileListCounts();
    UNSCOPED_INFO("each EndTiling call counts its list once");
    CHECK((counts.records == 5 && counts.empty == 1 && counts.misaligned == 1));

    // A device the stubs have not checked runs EndTiling as Original; the list is still observed.
    {
        Device device(memory);
        device.store(device.g + kTiling, 1);
        device.store(device.g + kTileListChunkOffset, kChunk);
        device.store(device.g + kTileListCursorOffset, kChunk + 4 + 32);
        Session    session(device);
        PPCContext ctx{};
        ctx.r3.u64 = device.g;
        ctx.r5.u64 = kEndTilingFlags;
        ctx.lr     = kCaller;
        sub_826AC218(ctx, base);
        UNSCOPED_INFO("EndTiling ran as Original");
        CHECK(originalCalls(0x826AC218) == 1);
        const auto observed = TakeTileListCounts();
        UNSCOPED_INFO("the not-ready device's EndTiling is observed too");
        CHECK((observed.records == 2 && observed.empty == 0 && observed.misaligned == 0));
    }

    REXCVAR_SET(native_consume_indexed_buffer_tiled, false);
    bracket(kChunk + 4 + 16);
    const auto off = TakeTileListCounts();
    UNSCOPED_INFO("nothing is observed with the tiled cvar off");
    CHECK((off.records == 0 && off.empty == 0 && off.misaligned == 0));
    REXCVAR_SET(native_consume_indexed_buffer_tiled, true);
    REXCVAR_SET(native_consume_indexed_buffer, false);
    bracket(kChunk + 4 + 16);
    const auto baseOff = TakeTileListCounts();
    UNSCOPED_INFO("nothing is observed with the base cvar off");
    CHECK((baseOff.records == 0 && baseOff.empty == 0 && baseOff.misaligned == 0));
    REXCVAR_SET(native_consume_indexed_buffer_tiled, false);

    // The user-pointer wrappers' tiled consume reads the same list, behind its own pair of cvars.
    REXCVAR_SET(native_consume_user_pointer_tiled, true);
    bracket(kChunk + 4 + 16);
    const auto userBaseOff = TakeTileListCounts();
    UNSCOPED_INFO("nothing is observed with the wrapper base cvar off");
    CHECK((userBaseOff.records == 0 && userBaseOff.empty == 0 && userBaseOff.misaligned == 0));
    // A base cvar and the tiled cvar of the other entry do not pair.
    REXCVAR_SET(native_consume_indexed_buffer, true);
    bracket(kChunk + 4 + 16);
    const auto crossed = TakeTileListCounts();
    UNSCOPED_INFO("the IndexedBuffer base cvar does not pair with the wrapper tiled cvar");
    CHECK((crossed.records == 0 && crossed.empty == 0 && crossed.misaligned == 0));
    REXCVAR_SET(native_consume_indexed_buffer, false);
    REXCVAR_SET(native_consume_user_pointer, true);
    REXCVAR_SET(native_consume_user_pointer_tiled, false);
    REXCVAR_SET(native_consume_indexed_buffer_tiled, true);
    bracket(kChunk + 4 + 16);
    const auto crossedBack = TakeTileListCounts();
    UNSCOPED_INFO("the wrapper base cvar does not pair with the IndexedBuffer tiled cvar");
    CHECK((crossedBack.records == 0 && crossedBack.empty == 0 && crossedBack.misaligned == 0));
    REXCVAR_SET(native_consume_indexed_buffer_tiled, false);
    REXCVAR_SET(native_consume_user_pointer_tiled, true);
    bracket(kChunk + 4 + 16 * 2);
    const auto user = TakeTileListCounts();
    UNSCOPED_INFO("the wrapper cvars observe the list");
    CHECK((user.records == 2 && user.empty == 0 && user.misaligned == 0));
    REXCVAR_SET(native_consume_user_pointer_tiled, false);
    bracket(kChunk + 4 + 16);
    const auto userTiledOff = TakeTileListCounts();
    UNSCOPED_INFO("nothing is observed with the wrapper tiled cvar off");
    CHECK((userTiledOff.records == 0 && userTiledOff.empty == 0 && userTiledOff.misaligned == 0));
    REXCVAR_SET(native_consume_user_pointer, false);

    // The direct lowerer's tiled consume reads the same list, behind its own pair of cvars.
    REXCVAR_SET(native_consume_lowerer_tiled, true);
    bracket(kChunk + 4 + 16);
    const auto lowererBaseOff = TakeTileListCounts();
    UNSCOPED_INFO("nothing is observed with the lowerer base cvar off");
    CHECK((lowererBaseOff.records == 0 && lowererBaseOff.empty == 0 && lowererBaseOff.misaligned == 0));
    REXCVAR_SET(native_consume_user_pointer, true);
    bracket(kChunk + 4 + 16);
    const auto lowererCrossed = TakeTileListCounts();
    UNSCOPED_INFO("the wrapper base cvar does not pair with the lowerer tiled cvar");
    CHECK((lowererCrossed.records == 0 && lowererCrossed.empty == 0 && lowererCrossed.misaligned == 0));
    REXCVAR_SET(native_consume_user_pointer, false);
    REXCVAR_SET(native_consume_lowerer, true);
    bracket(kChunk + 4 + 16 * 3);
    const auto lowerer = TakeTileListCounts();
    UNSCOPED_INFO("the lowerer cvars observe the list");
    CHECK((lowerer.records == 3 && lowerer.empty == 0 && lowerer.misaligned == 0));
    REXCVAR_SET(native_consume_lowerer_tiled, false);
    bracket(kChunk + 4 + 16);
    const auto lowererTiledOff = TakeTileListCounts();
    UNSCOPED_INFO("nothing is observed with the lowerer tiled cvar off");
    CHECK((lowererTiledOff.records == 0 && lowererTiledOff.empty == 0 && lowererTiledOff.misaligned == 0));
    REXCVAR_SET(native_consume_lowerer, false);
}

#endif

#if REREVVED_DEVELOPER
// Compare architectural fields explicitly; PPCContext padding is not state.
std::vector<std::uint64_t> residueContext(const PPCContext& ctx)
{
    std::vector<std::uint64_t> fields;
#define FIELD(n)                    \
    fields.push_back(ctx.r##n.u64); \
    fields.push_back(ctx.f##n.u64)
    FIELD(0);
    FIELD(1);
    FIELD(2);
    FIELD(3);
    FIELD(4);
    FIELD(5);
    FIELD(6);
    FIELD(7);
    FIELD(8);
    FIELD(9);
    FIELD(10);
    FIELD(11);
    FIELD(12);
    FIELD(13);
    FIELD(14);
    FIELD(15);
    FIELD(16);
    FIELD(17);
    FIELD(18);
    FIELD(19);
    FIELD(20);
    FIELD(21);
    FIELD(22);
    FIELD(23);
    FIELD(24);
    FIELD(25);
    FIELD(26);
    FIELD(27);
    FIELD(28);
    FIELD(29);
    FIELD(30);
    FIELD(31);
#undef FIELD
#define CR(n)                       \
    fields.push_back(ctx.cr##n.lt); \
    fields.push_back(ctx.cr##n.gt); \
    fields.push_back(ctx.cr##n.eq); \
    fields.push_back(ctx.cr##n.so)
    CR(0);
    CR(1);
    CR(2);
    CR(3);
    CR(4);
    CR(5);
    CR(6);
    CR(7);
#undef CR
    fields.insert(fields.end(), { ctx.lr, ctx.ctr.u64, ctx.xer.so, ctx.xer.ov, ctx.xer.ca, ctx.reserved.u64, ctx.msr, ctx.fpscr.csr, ctx.vscr_sat, ctx.last_indirect_target });
#define VECTOR(n)                      \
    fields.push_back(ctx.v##n.u64[0]); \
    fields.push_back(ctx.v##n.u64[1])
    VECTOR(0);
    VECTOR(1);
    VECTOR(2);
    VECTOR(3);
    VECTOR(4);
    VECTOR(5);
    VECTOR(6);
    VECTOR(7);
    VECTOR(8);
    VECTOR(9);
    VECTOR(10);
    VECTOR(11);
    VECTOR(12);
    VECTOR(13);
    VECTOR(14);
    VECTOR(15);
    VECTOR(16);
    VECTOR(17);
    VECTOR(18);
    VECTOR(19);
    VECTOR(20);
    VECTOR(21);
    VECTOR(22);
    VECTOR(23);
    VECTOR(24);
    VECTOR(25);
    VECTOR(26);
    VECTOR(27);
    VECTOR(28);
    VECTOR(29);
    VECTOR(30);
    VECTOR(31);
    VECTOR(32);
    VECTOR(33);
    VECTOR(34);
    VECTOR(35);
    VECTOR(36);
    VECTOR(37);
    VECTOR(38);
    VECTOR(39);
    VECTOR(40);
    VECTOR(41);
    VECTOR(42);
    VECTOR(43);
    VECTOR(44);
    VECTOR(45);
    VECTOR(46);
    VECTOR(47);
    VECTOR(48);
    VECTOR(49);
    VECTOR(50);
    VECTOR(51);
    VECTOR(52);
    VECTOR(53);
    VECTOR(54);
    VECTOR(55);
    VECTOR(56);
    VECTOR(57);
    VECTOR(58);
    VECTOR(59);
    VECTOR(60);
    VECTOR(61);
    VECTOR(62);
    VECTOR(63);
    VECTOR(64);
    VECTOR(65);
    VECTOR(66);
    VECTOR(67);
    VECTOR(68);
    VECTOR(69);
    VECTOR(70);
    VECTOR(71);
    VECTOR(72);
    VECTOR(73);
    VECTOR(74);
    VECTOR(75);
    VECTOR(76);
    VECTOR(77);
    VECTOR(78);
    VECTOR(79);
    VECTOR(80);
    VECTOR(81);
    VECTOR(82);
    VECTOR(83);
    VECTOR(84);
    VECTOR(85);
    VECTOR(86);
    VECTOR(87);
    VECTOR(88);
    VECTOR(89);
    VECTOR(90);
    VECTOR(91);
    VECTOR(92);
    VECTOR(93);
    VECTOR(94);
    VECTOR(95);
    VECTOR(96);
    VECTOR(97);
    VECTOR(98);
    VECTOR(99);
    VECTOR(100);
    VECTOR(101);
    VECTOR(102);
    VECTOR(103);
    VECTOR(104);
    VECTOR(105);
    VECTOR(106);
    VECTOR(107);
    VECTOR(108);
    VECTOR(109);
    VECTOR(110);
    VECTOR(111);
    VECTOR(112);
    VECTOR(113);
    VECTOR(114);
    VECTOR(115);
    VECTOR(116);
    VECTOR(117);
    VECTOR(118);
    VECTOR(119);
    VECTOR(120);
    VECTOR(121);
    VECTOR(122);
    VECTOR(123);
    VECTOR(124);
    VECTOR(125);
    VECTOR(126);
    VECTOR(127);
#undef VECTOR
    return fields;
}

void residueRawWrapperInvariance(rex::memory::Memory& memory, std::uint8_t* base)
{
    using F = OriginalResidueFunction;
    using R = OriginalResidueReason;
    using S = OriginalResidueSite;

    struct Case
    {
        bool     publish;
        bool     active;
        bool     cached;
        unsigned layout;
        unsigned high, low, tiled, sink, callback, pending, raise;
        R        reason;
    };

    const std::vector<Case> cases{
        { true, true, true, 0, 0, 0, 0, 0, 0, 9, 0, R::Submit },
        { true, true, true, 0, 0x20, 0x80, 1, 0, 0, 0, 0, R::Starved },
        { true, true, true, 0, 0, 0x80, 1, 0, 0, 0, 0, R::Recording },
        { true, true, true, 0, 0, 0, 1, 0, 0, 0, 0, R::Tiling },
        { false, true, true, 0, 0, 0, 0, 0, 0, 0, 0, R::ServedKick },
        { false, true, true, 0, 2, 0, 0, 0, 0x82600110, 0, 3, R::ServedSilent },
        { false, true, true, 0, 0, 0, 0, 0, 0, 9, 3, R::PendingNonzero },
        { false, true, true, 0, 0, 0, 0, 1, 2, 0, 3, R::PublisherSink },
        { false, true, true, 0, 0, 0, 0, 0, 2, 0, 3, R::PublisherCallback },
        { true, false, false, 0, 0, 0, 0, 0, 0, 9, 0, R::Inactive },
        { false, false, false, 0, 0, 0, 0, 0, 0, 9, 3, R::Inactive },
        { true, true, false, 1, 0, 0, 0, 0, 0, 9, 0, R::NotReady },
        { false, true, false, 1, 0, 0, 0, 0, 0, 9, 3, R::NotReady },
        { true, true, false, 2, 0, 0, 0, 0, 0, 9, 0, R::LayoutMismatch },
        { false, true, false, 2, 0, 0, 0, 0, 0, 9, 3, R::LayoutMismatch },
        { true, true, false, 0, 0x20, 0, 0, 0, 0, 9, 0, R::Starved },
        { false, true, false, 0, 0, 0, 0, 0, 0, 0, 3, R::ServedKick },
        // A cached hit must ignore a subsequently invalid layout word.
        { true, true, true, 2, 0x20, 0, 0, 0, 0, 9, 0, R::Starved },
        { false, true, true, 2, 0, 0, 0, 0, 0, 9, 3, R::PendingNonzero },
    };
    Device     device(memory);
    const auto before = guestBytes(memory, device.g);

    struct Result
    {
        std::vector<std::uint64_t>   context;
        std::vector<std::uint8_t>    bytes;
        std::vector<std::string>     trace;
        RingStubCounts               legacy;
        std::array<std::uint64_t, 8> cp;
        std::string                  tiling;
        std::uint64_t                deferred;
    };

    // Independent fixture effects, including arbitrary Original return/LR and both
    // volatile and nonvolatile registers. These do not model retail guest exits.
    for (const auto& fixture : cases)
    {
        Result baseline;
        for (bool enabled : { false, true })
        {
            std::memcpy(memory.TranslateVirtual(device.g), before.data(), before.size());
            device.store(device.g + kProgressBlock, fixture.layout == 1 ? 0 : device.block);
            device.store(device.g + kRingMask, fixture.layout == 2 ? 6 : 0x3FFF);
            *memory.TranslateVirtual<std::uint8_t*>(device.g + kModeHigh) = static_cast<std::uint8_t>(fixture.high);
            *memory.TranslateVirtual<std::uint8_t*>(device.g + kModeLow)  = static_cast<std::uint8_t>(fixture.low);
            device.store(device.g + kTiling, fixture.tiled);
            device.store(device.g + kCpuFenceSink, fixture.sink);
            device.store(device.g + kRingCallback, fixture.callback);
            device.store(device.g + kWorkerPending, fixture.pending);
            device.store(device.g + kRingWriteIndex, 0x73);
            Session session(device);
            session.draws.held = true;
            if (!fixture.active)
            {
                WithdrawRingStubServices();
                UNSCOPED_INFO("residueRawWrapperInvariance: inactive arm has no published ring services");
                CHECK(!CurrentRingStubServices());
            }
            ringStubsActive.store(fixture.active);
            ringStubsValidatedDevice.store(fixture.cached ? device.g : 0);
            REXCVAR_SET(native_original_residue_probe, enabled);
            RefreshOriginalResidueProbeRequested();
            REXCVAR_SET(native_completion_probe, true);
            RefreshCompletionProbeRequested();
            PublishedCompletionProbe().Take();
            REXCVAR_SET(native_tiling_quiesce_check, true);
            RefreshTilingProbeRequested();
            PublishedTilingProbe().Take();
            TakeOriginalResidueCounts();
            std::vector<std::string> trace;
            residueKick = [&](std::uint32_t pointer)
            {
                trace.push_back("kick:" + std::to_string(pointer));
            };
            residueBody = [&](std::uint32_t address, PPCContext& c)
            {
                trace.push_back(address == 0x8269C400 ? "original_publish" : "original_submit");
                if (address == 0x8269C400 && fixture.reason == R::Submit)
                {
                    c.lr = 0x8269C53C;
                    sub_8269BD98(c, base);
                }
                if (address == 0x8269BD98)
                {
                    trace.push_back("deferred");
                    NoteTilingSubmitArm(TilingArm::Deferred);
                    completion::detail::RecordDeferredKick(0x112200, 7);
                    device.store(device.g + kWorkerPending, 0x35);
                }
                device.store(device.g + kCursor, 0x51712340);
                trace.push_back("store_cursor");
                device.store(device.g + kPreviousList, 0xAB91C018);
                trace.push_back("store_list");
                c.r3.u64  = 0xFEDCBA9876543210ULL;
                c.r0.u64  = 0x1234567887654321ULL;
                c.r12.u64 = 0xABCDEF0112345678ULL;
                c.r31.u64 = 0x91A2B3C4D5E6F708ULL;
                c.lr      = 0xAABBCCDD8269C53CULL;
                c.ctr.u64 = 0x1122334455667788ULL;
                c.cr0.set_raw(9);
                c.cr6.set_raw(6);
                c.xer        = { 1, 0, 1 };
                c.f1.u64     = 0x400921FB54442D18ULL;
                c.f31.u64    = 0xC005BF0A8B145769ULL;
                c.v20.u64[0] = 0x51A891B2C3D4E5F6ULL;
                c.v20.u64[1] = 0x718293A4B5C6D7E8ULL;
            };
            PPCContext ctx{};
            ctx.r3.u64        = device.g;
            ctx.r4.u64        = 0x89ABCDEF00102030ULL;
            ctx.r5.u64        = 0x112200;
            ctx.r6.u64        = 7;
            ctx.r7.u64        = 0xAABBCCDD00000000ULL | fixture.raise;
            ctx.r31.u64       = 0xFEDCBA9812345678ULL;
            ctx.lr            = fixture.publish ? 0x8269C61C : 0x826AC3B4;
            const bool served = fixture.reason == R::ServedKick || fixture.reason == R::ServedSilent;
            if (fixture.reason == R::ServedKick)
            {
                // A held host completion gates only spans with an admitted draw.
                session.ledger->BeginDraw(NativeDrawHook::IndexedUp, kCaller);
                session.ledger->FinishDraw(1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
                session.ledger->Fence(device.physical(device.block), 0x51, 0xA7, 0x112200);
            }
            if (fixture.publish)
                sub_8269C400(ctx, base);
            else
                sub_8269BD98(ctx, base);
            UNSCOPED_INFO("residueRawWrapperInvariance: exactly one selected Original dispatch");
            CHECK(originalCalls(fixture.publish ? 0x8269C400 : 0x8269BD98) == (served ? 0U : 1U));
            UNSCOPED_INFO("residueRawWrapperInvariance: baseline narrowed served/full64 Original return");
            CHECK(ctx.r3.u64 == (served ? 0x00102030ULL : 0xFEDCBA9876543210ULL));
            const auto            counts = TakeOriginalResidueCounts();
            OriginalResidueCounts expected;
            const auto            function = fixture.publish ? F::Publish : F::Submit;
            const auto            site     = fixture.publish ? S::C61C : S::AC3B4;
            if (enabled)
            {
                expected.cells[static_cast<std::size_t>(function)][static_cast<std::size_t>(site)][static_cast<std::size_t>(fixture.reason)] = 1;
                if (fixture.publish && fixture.reason == R::Submit)
                    expected.cells[1][static_cast<std::size_t>(S::C53C)][static_cast<std::size_t>(R::PendingNonzero)] = 1;
            }
            UNSCOPED_INFO("residueRawWrapperInvariance: exact entry LR cells including nested helper");
            CHECK((counts.cells == expected.cells && counts.internalError == 0));
            std::vector<std::string> expectedTrace;
            if (fixture.publish)
            {
                expectedTrace.push_back("original_publish");
                if (fixture.reason == R::Submit)
                    expectedTrace.insert(expectedTrace.end(), { "original_submit", "deferred", "store_cursor", "store_list" });
                expectedTrace.insert(expectedTrace.end(), { "store_cursor", "store_list" });
            }
            else if (!served)
                expectedTrace = { "original_submit", "deferred", "store_cursor", "store_list" };
            else if (fixture.reason == R::ServedKick)
                expectedTrace = { "kick:115" };
            UNSCOPED_INFO("residueRawWrapperInvariance: independent ordered Original/store/Kick sequence");
            CHECK(trace == expectedTrace);
            if (fixture.reason == R::NotReady)
            {
                device.store(device.g + kProgressBlock, device.block);
                device.store(device.g + kWorkerPending, 0);
                ctx.r3.u64 = device.g;
                ctx.lr     = fixture.publish ? 0x8269C61C : 0x826AC3B4;
                if (fixture.publish)
                    sub_8269C400(ctx, base);
                else
                    sub_8269BD98(ctx, base);
                const auto retry = TakeOriginalResidueCounts();
                expected         = {};
                if (enabled)
                    expected.cells[static_cast<std::size_t>(function)][static_cast<std::size_t>(site)][static_cast<std::size_t>(fixture.publish ? R::Submit : R::ServedKick)] = 1;
                UNSCOPED_INFO("residueRawWrapperInvariance: NotReady retries and caches a later Ok");
                CHECK((retry.cells == expected.cells && ringStubsValidatedDevice.load() == device.g && ringStubsActive.load()));
            }
            if (fixture.reason == R::LayoutMismatch)
            {
                UNSCOPED_INFO("residueRawWrapperInvariance: mismatch disables the session");
                CHECK(!ringStubsActive.load());
                ctx.r3.u64 = device.g;
                ctx.lr     = fixture.publish ? 0x8269C61C : 0x826AC3B4;
                if (fixture.publish)
                    sub_8269C400(ctx, base);
                else
                    sub_8269BD98(ctx, base);
                const auto inactive = TakeOriginalResidueCounts();
                expected            = {};
                if (enabled)
                    expected.cells[static_cast<std::size_t>(function)][static_cast<std::size_t>(site)][static_cast<std::size_t>(R::Inactive)] = 1;
                UNSCOPED_INFO("residueRawWrapperInvariance: subsequent call is inactive without reclassification");
                CHECK(inactive.cells == expected.cells);
            }
            if (fixture.reason == R::ServedKick)
            {
                session.ledger->Pump();
                const auto held = session.ledger->Snapshot();
                UNSCOPED_INFO("residueRawWrapperInvariance: admitted draw waits on held host fence");
                CHECK((held.hostTarget != 0 && held.hostCompleted < held.hostTarget));
                UNSCOPED_INFO("residueRawWrapperInvariance: held host completion releases no guest effect");
                CHECK(session.sink.writes.empty());
                session.draws.held = false;
                while (session.ledger->Pump())
                {
                }
                UNSCOPED_INFO("residueRawWrapperInvariance: direct record exactly once before Kick attaches held fence");
                CHECK((device.load(device.block) == 0x51 && session.failed.empty()));
                const auto                                                 block = device.physical(device.block);
                const std::vector<std::pair<std::uint32_t, std::uint32_t>> expectedWrites{
                    { block + completion::kCursorOffset, 0xA7 }, { block + completion::kFenceOffset, 0x51 }, { block + completion::kReadPointerOffset, 0x73 }
                };
                UNSCOPED_INFO("residueRawWrapperInvariance: exact ordered release stores after host completion");
                CHECK(session.sink.writes == expectedWrites);
            }
            const auto                         legacy = TakeRingStubCounts();
            const auto                         c      = PublishedCompletionProbe().Take().counts;
            const std::array<std::uint64_t, 8> cp{ c.adds[0], c.adds[1], c.adds[2], c.addCalls[0], c.addCalls[1], c.addCalls[2], c.d, c.callbacks };
            const auto                         tilingCounts = FormatTilingProbeCounts(PublishedTilingProbe().Take());
            Result                             result{ residueContext(ctx), guestBytes(memory, device.g), trace, legacy, cp, tilingCounts, session.ledger->Snapshot().deferredSpans };
            RingStubCounts                     expectedLegacy;
            if (fixture.active)
            {
                const auto index                                                          = static_cast<std::size_t>(fixture.publish ? RingStub::Publish : RingStub::Submit);
                const bool countedServed                                                  = fixture.publish ? (fixture.reason == R::Submit || fixture.reason == R::Starved) : served;
                (countedServed ? expectedLegacy.stubbed : expectedLegacy.original)[index] = 1;
                if (fixture.publish && fixture.reason == R::Submit)
                    expectedLegacy.original[static_cast<std::size_t>(RingStub::Submit)] = 1;
                if (fixture.reason == R::NotReady)
                    expectedLegacy.stubbed[index] = 1;
            }
            UNSCOPED_INFO("residueRawWrapperInvariance: independently expected legacy counts including inactive omission");
            CHECK((legacy.stubbed == expectedLegacy.stubbed && legacy.original == expectedLegacy.original));
            const auto expectedDeferred = fixture.reason == R::Submit || (!fixture.publish && !served) ? (fixture.reason == R::LayoutMismatch ? 2U : 1U) : 0U;
            UNSCOPED_INFO("residueRawWrapperInvariance: one deferred record per fixture Original");
            CHECK(result.deferred == expectedDeferred);
            if (!enabled)
                baseline = result;
            else
            {
                UNSCOPED_INFO("residueRawWrapperInvariance: off/on explicit PPC/guest bytes/ordered service trace/legacy/CP invariance");
                CHECK((result.context == baseline.context && result.bytes == baseline.bytes && result.trace == baseline.trace && result.legacy.stubbed == baseline.legacy.stubbed && result.legacy.original == baseline.legacy.original && result.cp == baseline.cp && result.tiling == baseline.tiling && result.deferred == baseline.deferred));
            }
            residueBody = {};
            residueKick = {};
        }
    }
    REXCVAR_SET(native_original_residue_probe, false);
    RefreshOriginalResidueProbeRequested();
    REXCVAR_SET(native_completion_probe, false);
    RefreshCompletionProbeRequested();
    REXCVAR_SET(native_tiling_quiesce_check, false);
    RefreshTilingProbeRequested();
}
#endif

void residueReadinessTrace()
{
    class ScriptedMemory final : public IRingStubMemory
    {
    public:
        std::map<std::uint32_t, std::uint32_t>      words;
        std::vector<std::pair<char, std::uint32_t>> trace;

        std::optional<std::uint32_t> Read32(std::uint32_t address) override
        {
            trace.emplace_back('r', address);
            const auto found = words.find(address);
            return found == words.end() ? std::nullopt : std::optional(found->second);
        }

        std::optional<std::uint8_t> Read8(std::uint32_t address) override
        {
            trace.emplace_back('b', address);
            return std::nullopt;
        }

        bool Write32(std::uint32_t, std::uint32_t) override
        {
            UNSCOPED_INFO("residueReadinessTrace: unexpected word write");
            CHECK(false);
            return false;
        }

        bool Write8(std::uint32_t, std::uint8_t) override
        {
            UNSCOPED_INFO("residueReadinessTrace: unexpected byte write");
            CHECK(false);
            return false;
        }

        std::optional<std::uint64_t> Exchange64(std::uint32_t, std::uint64_t) override
        {
            UNSCOPED_INFO("residueReadinessTrace: unexpected exchange");
            CHECK(false);
            return {};
        }

        std::optional<std::uint32_t> Physical(std::uint32_t address) override
        {
            trace.emplace_back('p', address);
            return address;
        }
    };

    class Services final : public IRingStubServices
    {
        void Kick(std::uint32_t) override
        {
            UNSCOPED_INFO("residueReadinessTrace: unexpected Kick");
            CHECK(false);
        }
    };

    constexpr std::uint32_t g = 0x10000, block = 0x20000, scratch = 0x30000, cell = 0x40000;
    using Trace = std::vector<std::pair<char, std::uint32_t>>;
    const Trace notReady{
        { 'r', g }, { 'r', g + 0x5B0C }, { 'r', 0x820009F0 }, { 'r', cell }, { 'r', g + 0x2A90 }, { 'r', g + 0x2A94 }
    };
    Trace ok = notReady;
    ok.insert(ok.end(), { { 'p', block }, { 'r', block + 0x5C }, { 'p', scratch }, { 'r', scratch + 0x1C }, { 'r', g + 0x3A34 }, { 'r', g + 0x2A9C } });
    auto mismatch = ok;
    mismatch.pop_back();
    Trace plainOk;
#if REREVVED_DEVELOPER
    for (bool diagnostic : { false, true })
#else
    for (bool diagnostic : { false })
#endif
    {
        ScriptedMemory memory;
        memory.words = { { g, 0 }, { g + 0x5B0C, 0 }, { 0x820009F0, cell }, { cell, g }, { g + 0x2A90, 0 }, { g + 0x2A94, scratch }, { block + 0x5C, 0 }, { scratch + 0x1C, 0 }, { g + 0x3A34, 0x3FFF }, { g + 0x2A9C, 0x11 } };
        PublishRingStubServices(std::make_shared<Services>());
        ringStubsValidatedDevice.store(0);
        TakeRingStubCounts();
#if REREVVED_DEVELOPER
        OriginalResidueReadiness observed{};
#endif
        const auto check = [&](std::uint32_t device)
        {
#if REREVVED_DEVELOPER
            return CheckRingStubReadinessForTesting(memory, device, RingStub::Publish, 0x8269C61C, diagnostic ? &observed : nullptr);
#else
            return CheckRingStubReadinessForTesting(memory, device, RingStub::Publish, 0x8269C61C);
#endif
        };
        UNSCOPED_INFO("residueReadinessTrace: exact NotReady reads, no classifier and no disable");
        CHECK((!check(g) && memory.trace == notReady && ringStubsActive.load() && ringStubsValidatedDevice.load() == 0));
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("residueReadinessTrace: actual NotReady outcome");
        CHECK((!diagnostic || observed == OriginalResidueReadiness::NotReady));
#endif
        auto counts = TakeRingStubCounts();
        UNSCOPED_INFO("residueReadinessTrace: NotReady counts once");
        CHECK((counts.original[static_cast<std::size_t>(RingStub::Publish)] == 1 && counts.stubbed[static_cast<std::size_t>(RingStub::Publish)] == 0));
        memory.words[g + 0x2A90] = block;
        memory.trace.clear();
        const auto* logger  = rex::GetLoggerRaw(rex::log::core());
        const bool  logging = logger && logger->should_log(spdlog::level::info);
        UNSCOPED_INFO("residueReadinessTrace: later Ok caches the same device");
        CHECK((check(g) && ringStubsValidatedDevice.load() == g));
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("residueReadinessTrace: actual uncached Ok outcome");
        CHECK((!diagnostic || observed == OriginalResidueReadiness::Ok));
#endif
        UNSCOPED_INFO("residueReadinessTrace: independent exact ordered layout reads");
        CHECK((memory.trace.size() == ok.size() + (logging ? 4 : 0) && std::equal(ok.begin(), ok.end(), memory.trace.begin())));
        // The existing logging arguments have compiler-selected evaluation order.
        // Both diagnostic modes must preserve that same four-read suffix exactly.
        if (!diagnostic)
            plainOk = memory.trace;
        else
        {
            UNSCOPED_INFO("residueReadinessTrace: null/present exact Ok logging read trace");
            CHECK(memory.trace == plainOk);
        }
        auto  logReads = Trace(memory.trace.begin() + std::min(ok.size(), memory.trace.size()), memory.trace.end());
        Trace expectedLog{ { 'r', g + 0x2A90 }, { 'r', g + 0x2A94 }, { 'r', g + 0x3A34 }, { 'r', g + 0x2A9C } };
        if (!logging)
            expectedLog.clear();
        std::sort(logReads.begin(), logReads.end());
        std::sort(expectedLog.begin(), expectedLog.end());
        UNSCOPED_INFO("residueReadinessTrace: only existing four logging reads and no Ok count");
        CHECK((logReads == expectedLog && FormatRingStubCounts(TakeRingStubCounts()).empty()));
        memory.words[g + 0x3A34] = 6;
        memory.trace.clear();
        UNSCOPED_INFO("residueReadinessTrace: cached equality has zero fresh layout reads");
        CHECK((check(g) && memory.trace.empty()));
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("residueReadinessTrace: actual cache hit outcome");
        CHECK((!diagnostic || observed == OriginalResidueReadiness::Cached));
#endif
        ringStubsValidatedDevice.store(0);
        UNSCOPED_INFO("residueReadinessTrace: literal zero cache equality is preserved");
        CHECK((check(0) && memory.trace.empty()));
        UNSCOPED_INFO("residueReadinessTrace: exact Mismatch reads and session disable");
        CHECK((!check(g) && memory.trace == mismatch && !ringStubsActive.load()));
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("residueReadinessTrace: actual Mismatch outcome");
        CHECK((!diagnostic || observed == OriginalResidueReadiness::Mismatch));
#endif
        counts = TakeRingStubCounts();
        UNSCOPED_INFO("residueReadinessTrace: Mismatch counts once");
        CHECK((counts.original[static_cast<std::size_t>(RingStub::Publish)] == 1 && counts.stubbed[static_cast<std::size_t>(RingStub::Publish)] == 0));
        WithdrawRingStubServices();
    }
}

// Publish runs Original exactly once however the device classifies the call, and keeps
// the legacy classification counts.
void publishRoutesOriginalOnce(rex::memory::Memory& memory, std::uint8_t* base)
{
    Device  device(memory);
    Session session(device);
    ringStubsValidatedDevice.store(device.g);
    const auto stack   = allocatePhysical(memory) + 0x8000;
    const auto segment = allocatePhysical(memory);
    const auto pair    = allocatePhysical(memory);
    for (unsigned branch = 0; branch < 4; ++branch)
    {
#if REREVVED_DEVELOPER
        REXCVAR_SET(native_original_residue_probe, true);
        RefreshOriginalResidueProbeRequested();
        TakeOriginalResidueCounts();
#endif
        TakeRingStubCounts();
        device.store(device.g + kCursor, segment + 12);
        device.store(device.g + kCursorLimit, segment + 0x1000);
        device.store(device.g + 0x3A50, segment);
        device.store(device.g + 0x3440, pair);
        device.store(device.g + 0x3444, pair + 8);
        device.store(device.g + kTiling, 1);
        *memory.TranslateVirtual<std::uint8_t*>(device.g + kModeHigh) = branch == 1 ? 0x20 : 0;
        *memory.TranslateVirtual<std::uint8_t*>(device.g + kModeLow)  = branch == 2 ? 0x80 : 0;
        PPCContext ctx{};
        ctx.r3.u64     = device.g;
        ctx.r1.u64     = stack;
        ctx.lr         = branch == 3 ? 0x123400008269C61CULL : 0x8269C61CULL;
        unsigned calls = 0;
        residueBody    = [&](std::uint32_t address, PPCContext& out)
        {
            UNSCOPED_INFO("publishRoutesOriginalOnce: selected Original only");
            CHECK(address == 0x8269C400);
            ++calls;
            // Deliberately incomplete sentinel: routing evidence, never an ABI oracle.
            out.r3.u64 = 0x123456789ABCDEF0ULL;
        };
        const auto bytes = guestBytes(memory, device.g);
        sub_8269C400(ctx, base);
        const auto legacy = TakeRingStubCounts();
        UNSCOPED_INFO("publishRoutesOriginalOnce: every branch runs Original exactly once and replays nothing");
        CHECK((calls == 1 && ctx.r3.u64 == 0x123456789ABCDEF0ULL && guestBytes(memory, device.g) == bytes));
        UNSCOPED_INFO("publishRoutesOriginalOnce: legacy classification is unchanged");
        CHECK((legacy.original[static_cast<std::size_t>(RingStub::Publish)] == (branch == 1 ? 0U : 1U) && legacy.stubbed[static_cast<std::size_t>(RingStub::Publish)] == (branch == 1 ? 1U : 0U)));
#if REREVVED_DEVELOPER
        const auto r1fields = FormatOriginalResidueCounts(TakeOriginalResidueCounts());
        UNSCOPED_INFO("publishRoutesOriginalOnce: actual Original dispatch remains visible for tiling");
        CHECK(r1fields.find(" or_publish_calls=1 or_publish_original_dispatch=1") != std::string::npos);
#endif
    }
    residueBody = {};
#if REREVVED_DEVELOPER
    REXCVAR_SET(native_original_residue_probe, false);
    RefreshOriginalResidueProbeRequested();
#endif
}

#if REREVVED_DEVELOPER

// Consume bypasses Publish and Submit Originals; helper fixtures and the lock preserve
// generated order.
std::function<void(PPCContext&)> lockBody;
std::uint64_t                    lockLevel = 0xFEDC000000000003ULL;

void lockAcquireFixture(PPCContext& ctx, std::uint8_t*)
{
    consumeEvents.push_back(std::format("acquire:{:X}:{:X}", ctx.r3.u64, ctx.lr));
    if (lockBody)
        lockBody(ctx);
    ctx.r3.u64 = lockLevel;
}

void lockReleaseFixture(PPCContext& ctx, std::uint8_t*)
{
    consumeEvents.push_back(std::format("release:{:X}:{:X}:{:X}", ctx.r3.u64, ctx.r4.u64, ctx.lr));
}

std::array<std::uint64_t, 20> savedRegisters(const PPCContext& ctx)
{
    return { ctx.r1.u64, ctx.lr, ctx.r14.u64, ctx.r15.u64, ctx.r16.u64, ctx.r17.u64, ctx.r18.u64, ctx.r19.u64, ctx.r20.u64, ctx.r21.u64, ctx.r22.u64, ctx.r23.u64, ctx.r24.u64, ctx.r25.u64, ctx.r26.u64, ctx.r27.u64, ctx.r28.u64, ctx.r29.u64, ctx.r30.u64, ctx.r31.u64 };
}

void seedRegisters(PPCContext& ctx)
{
    ctx.r14.u64 = 0x1400000014000014ULL;
    ctx.r15.u64 = 0x1500000015000015ULL;
    ctx.r16.u64 = 0x1600000016000016ULL;
    ctx.r17.u64 = 0x1700000017000017ULL;
    ctx.r18.u64 = 0x1800000018000018ULL;
    ctx.r19.u64 = 0x1900000019000019ULL;
    ctx.r20.u64 = 0x200000002000002AULL;
    ctx.r21.u64 = 0x210000002100002BULL;
    ctx.r22.u64 = 0x220000002200002CULL;
    ctx.r23.u64 = 0x230000002300002DULL;
    ctx.r24.u64 = 0x240000002400002EULL;
    ctx.r25.u64 = 0x250000002500002FULL;
    ctx.r26.u64 = 0x2600000026000030ULL;
    ctx.r27.u64 = 0x2700000027000031ULL;
    ctx.r28.u64 = 0x2800000028000032ULL;
    ctx.r29.u64 = 0x2900000029000033ULL;
    ctx.r30.u64 = 0x3000000030000034ULL;
    ctx.r31.u64 = 0x3100000031000035ULL;
}

// The value of one " key=N" field; zero when the field is absent.
[[maybe_unused]] std::uint64_t field(const std::string& line, std::string_view key)
{
    const auto token = " " + std::string(key) + "=";
    const auto at    = line.find(token);
    return at == std::string::npos ? 0 : std::stoull(line.substr(at + token.size()));
}

std::string takeResidue()
{
#if REREVVED_DEVELOPER
    return FormatOriginalResidueCounts(TakeOriginalResidueCounts());
#else
    return {};
#endif
}

// Both conservation equations, with no internal accounting error.
void requireConserved([[maybe_unused]] const std::string& line, [[maybe_unused]] std::string_view where)
{
#if REREVVED_DEVELOPER
    const auto publish = field(line, "or_publish_original_dispatch") + field(line, "or_publish_consumed_submit") + field(line, "or_publish_consumed_tiling") + field(line, "or_publish_consumed_starved");
    const auto submit  = field(line, "or_submit_original_dispatch") + field(line, "or_submit_served_kick") + field(line, "or_submit_served_silent") + field(line, "or_submit_consumed_deferred");
    UNSCOPED_INFO(std::string(where) + ": Publish conservation: " + line);
    CHECK(field(line, "or_publish_calls") == publish);
    UNSCOPED_INFO(std::string(where) + ": Submit conservation: " + line);
    CHECK(field(line, "or_submit_calls") == submit);
    UNSCOPED_INFO(std::string(where) + ": no internal accounting error");
    CHECK(field(line, "or_internal_error") == 0);
#endif
}

struct ConsumeRig
{
    ConsumeRig(rex::memory::Memory& guest, std::uint8_t* hostBase)
    : memory(guest)
    , base(hostBase)
    , device(guest)
    , session(device)
    {
        device.store(device.g + kProgressBlock, device.block);
        device.store(device.g + kRingWriteIndex, 0x73);
        ringStubsValidatedDevice.store(device.g);
        ring  = allocatePhysical(guest);
        list  = allocatePhysical(guest);
        stack = allocatePhysical(guest) + 0x8000;
        tiling.reset(&guest, allocatePhysical(guest));
        helperCalls.clear();
        helperWatch.clear();
        growthStorage  = {};
        rolloverResult = 0x13579BDF02468ACEULL;
        consumeEvents.clear();
        lockBody    = {};
        lockLevel   = 0xFEDC000000000003ULL;
        residueBody = {};
        residueKick = [](std::uint32_t pointer)
        {
            consumeEvents.push_back(std::format("kick:{}", pointer));
        };
        REXCVAR_SET(native_ring_consume_publish, true);
#if REREVVED_DEVELOPER
        REXCVAR_SET(native_original_residue_probe, true);
        RefreshOriginalResidueProbeRequested();
        REXCVAR_SET(native_completion_probe, true);
        RefreshCompletionProbeRequested();
        (void)PublishedCompletionProbe().Take();
#endif
        SetRingConsumeLockCallsForTesting(&lockAcquireFixture, &lockReleaseFixture);
        (void)TakeRingStubCounts();
#if REREVVED_DEVELOPER
        (void)TakeOriginalResidueCounts();
#endif
        (void)TakeRingConsumeCounts();
        originals.clear();
        placeRing(0x108, 0x100, 0x1000);
    }

    ~ConsumeRig()
    {
        SetRingConsumeLockCallsForTesting(nullptr, nullptr);
        REXCVAR_SET(native_ring_consume_publish, false);
#if REREVVED_DEVELOPER
        REXCVAR_SET(native_original_residue_probe, false);
        RefreshOriginalResidueProbeRequested();
        REXCVAR_SET(native_completion_probe, false);
        RefreshCompletionProbeRequested();
#endif
        residueKick   = {};
        residueBody   = {};
        lockBody      = {};
        growthStorage = {};
        helperWatch.clear();
    }

    // C, S and the cursor limit as offsets into the command buffer.
    void placeRing(std::uint32_t cursor, std::uint32_t start, std::uint32_t limit)
    {
        device.store(device.g + kCursor, ring + cursor);
        device.store(device.g + kTailStart, ring + start);
        device.store(device.g + kCursorLimit, ring + limit);
    }

    void mode(std::uint8_t high, std::uint8_t low, std::uint32_t tiled)
    {
        *memory.TranslateVirtual<std::uint8_t*>(device.g + kModeHigh) = high;
        *memory.TranslateVirtual<std::uint8_t*>(device.g + kModeLow)  = low;
        device.store(device.g + kTiling, tiled);
    }

    // The worker's list: its tail word points at storage the token link may write.
    void workerList(std::uint32_t head, std::uint32_t tail)
    {
        device.store(head + 8, tail);
    }

    PPCContext context(std::uint32_t lr)
    {
        PPCContext ctx{};
        ctx.r3.u64 = device.g;
        ctx.r1.u64 = stack;
        ctx.lr     = lr;
        seedRegisters(ctx);
        return ctx;
    }

    // An admitted draw that holds its span on a host fence the test releases.
    void holdOnHostDraw()
    {
        session.draws.held = true;
        session.ledger->BeginDraw(NativeDrawHook::IndexedUp, kCaller);
        session.ledger->FinishDraw(1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    }

    std::uint32_t at(std::uint32_t offset) const
    {
        return ring + offset;
    }

    rex::memory::Memory& memory;
    std::uint8_t*        base;
    Device               device;
    Session              session;
    std::uint32_t        ring = 0, list = 0, stack = 0;
};

// Direct Publish: BCA8 stays stubbed; BD98 kicks the existing index only.
void consumePublishSubmitDirect(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    rig.holdOnHostDraw();
    const auto g           = rig.device.g;
    auto       ctx         = rig.context(0x8269C61C);
    const auto registers   = savedRegisters(ctx);
    const auto stackBefore = guestBytes(memory, rig.stack - 0x400, 0x500);
    sub_8269C400(ctx, base);

    UNSCOPED_INFO("direct: no Original runs, neither Publish, the fence emitter nor Submit");
    CHECK(originals.empty());
    // C = ring+0x108: the fence stub returns C+40, Submit gets P = C+44 and returns it whole.
    UNSCOPED_INFO("direct: r3 is Submit's full-width return P");
    CHECK(ctx.r3.u64 == rig.at(0x134));
    UNSCOPED_INFO("direct: r1, LR and r14-r31 are as at entry");
    CHECK(savedRegisters(ctx) == registers);
    UNSCOPED_INFO("direct: no write below or around the stack pointer");
    CHECK(guestBytes(memory, rig.stack - 0x400, 0x500) == stackBefore);
    UNSCOPED_INFO("direct: the tail aligns P: G+0x3A50 = ring+0x140, G+0x30 = ring+0x13C");
    CHECK((rig.device.load(g + kTailStart) == rig.at(0x140) && rig.device.load(g + kCursor) == rig.at(0x13C)));
    UNSCOPED_INFO("direct: add-zero N store keeps 0, F advances by 2");
    CHECK((rig.device.load(g + kWorkerPending) == 0 && rig.device.load(g + kNextFence) == 0x13));
    UNSCOPED_INFO("direct: the fence copies C; the publisher index is unchanged");
    CHECK((rig.device.load(g + kFenceCursorCopy) == rig.at(0x108) && rig.device.load(g + kRingWriteIndex) == 0x73));
    UNSCOPED_INFO("direct: one kick of the existing index and nothing else");
    CHECK(consumeEvents == std::vector<std::string>{ "kick:115" });
    UNSCOPED_INFO("direct: no retained helper runs");
    CHECK(helperCalls.empty());

    const auto r1 = takeResidue();
    PROBE_INFO("direct: Publish counted once as consumed_submit: " + r1);
    PROBE_CHECK((field(r1, "or_publish_calls") == 1 && field(r1, "or_publish_consumed_submit") == 1 && field(r1, "or_publish_original_dispatch") == 0));
    PROBE_INFO("direct: nested Submit counted once as served_kick");
    PROBE_CHECK((field(r1, "or_submit_calls") == 1 && field(r1, "or_submit_served_kick") == 1 && field(r1, "or_submit_original_dispatch") == 0));
    requireConserved(r1, "direct");
    PROBE_INFO("direct: Publish dispatch cell keeps its caller bucket: " + r1);
    PROBE_CHECK(r1.find(" or_publish_site_8269c61c_submit_dispatch_consumed_submit=1") != std::string::npos);
    const auto legacy = TakeRingStubCounts();
    UNSCOPED_INFO("direct: legacy counters keep their meaning");
    CHECK((stubbed(legacy, RingStub::Publish) == 1 && fellBack(legacy, RingStub::Publish) == 0 && stubbed(legacy, RingStub::Submit) == 1 && fellBack(legacy, RingStub::Submit) == 0 && stubbed(legacy, RingStub::Fence) == 1));
#if REREVVED_DEVELOPER
    const auto probe = PublishedCompletionProbe().Take().counts;
    UNSCOPED_INFO("direct: one Stub-path N store pair, no Deferred or Direct pair, balanced");
    CHECK((probe.addCalls[2] == 1 && probe.addCalls[0] == 0 && probe.addCalls[1] == 0 && probe.pairError == 0));
#endif
    const auto rc = TakeRingConsumeCounts();
    UNSCOPED_INFO("direct: no fault, CPU fence, fallback or invariant violation: " + FormatRingConsumeCounts(rc));
    CHECK((rc.fault == 0 && rc.publishOriginalCpuFence == 0 && rc.recordingOriginal == 0 && FormatRingConsumeCounts(rc).find("rc_invariant") == std::string::npos));

    // Completion: the host draw holds the span; nothing is released until it completes,
    // then B+4 is stored before B+0 and RPTR is stored last with the existing index.
    rig.session.ledger->Pump();
    const auto held = rig.session.ledger->Snapshot();
    UNSCOPED_INFO("direct: the admitted draw waits on the held host fence");
    CHECK((held.hostTarget != 0 && held.hostCompleted < held.hostTarget));
    UNSCOPED_INFO("direct: held host completion releases no fence, callback or RPTR, and the consume stores no fence word itself");
    CHECK((rig.session.sink.writes.empty() && rig.device.load(rig.device.block) == 0 && rig.device.load(rig.device.block + completion::kCursorOffset) == 0));
    rig.session.draws.held = false;
    while (rig.session.ledger->Pump())
    {
    }
    const auto                       block = rig.device.physical(rig.device.block);
    const std::vector<std::uint32_t> order{ block + completion::kCursorOffset, block + completion::kFenceOffset, block + completion::kReadPointerOffset };
    std::vector<std::uint32_t>       seen;
    for (const auto& write : rig.session.sink.writes)
        seen.push_back(write.first);
    UNSCOPED_INFO("direct: B+4, then B+0, then RPTR");
    CHECK(seen == order);
    if (seen.size() == 3)
    {
        UNSCOPED_INFO("direct: the fence value and the existing publisher index");
        CHECK((rig.session.sink.writes[1].second == 0x11 && rig.session.sink.writes[2].second == 0x73));
    }
    UNSCOPED_INFO("direct: no ledger failure");
    CHECK(rig.session.failed.empty());
}

// Deferred Publish locks, stores token, links B550 and unlocks.
void consumePublishSubmitDeferred(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    const auto g = rig.device.g;
    rig.device.store(g + kWorkerPending, 9);
    rig.workerList(g + kTileList, rig.list + 0x40);
    const auto p = rig.at(0x134);
    // Sampled at the B550 call: N and the cursor words are still the entry values and
    // the three token words are already stored.
    helperWatch          = { g + kWorkerPending, p + 4, p + 8, p + 12, g + kCursor, g + kTailStart };
    auto       ctx       = rig.context(0x8269C61C);
    const auto registers = savedRegisters(ctx);
    sub_8269C400(ctx, base);

    UNSCOPED_INFO("deferred: no Original runs");
    CHECK(originals.empty());
    UNSCOPED_INFO("deferred: r3 is P+0x10, the rest as at entry");
    CHECK((ctx.r3.u64 == p + 16 && savedRegisters(ctx) == registers));
    UNSCOPED_INFO("deferred: acquire, B550, release with the full acquire result; no kick");
    CHECK((consumeEvents.size() == 3 && consumeEvents[0] == std::format("acquire:{:X}:8269BDD8", g + 0x2B04) && consumeEvents[1] == "helper:8269B550" &&
           consumeEvents[2] == std::format("release:{:X}:{:X}:8269BE2C", g + 0x2B04, lockLevel)));
    UNSCOPED_INFO("deferred: B550 gets list, P and P+0x10 at its return address");
    CHECK((helperCalls.size() == 1 && helperCalls[0].lr == 0x8269BE10 && helperCalls[0].r3 == g + kTileList && helperCalls[0].r4 == p && helperCalls[0].r5 == p + 16));
    if (helperCalls.size() == 1)
    {
        UNSCOPED_INFO("deferred: token stored before B550; N, G+0x30 and G+0x3A50 not yet changed");
        CHECK(helperCalls[0].words == std::vector<std::uint32_t>{ 9, 0x8100000D, viewAdjusted(rig.at(0x100)), 0xC0000000, rig.at(0x108), rig.at(0x100) });
    }
    UNSCOPED_INFO("deferred: N = 9 + 0 is stored after B550");
    CHECK(rig.device.load(g + kWorkerPending) == 9);
    UNSCOPED_INFO("deferred: B550 linked the token onto the worker's list");
    CHECK((rig.device.load(p) == viewAdjusted(p + 16) - 0x40000000U && rig.device.load(g + kTileList + 4) == p && rig.device.load(g + kTileList + 8) == p + 12 && rig.device.load(rig.list + 0x40) == viewAdjusted(p) - 0x40000000U));
    UNSCOPED_INFO("deferred: the tail aligns the deferred return, not the entry cursor or P");
    CHECK((rig.device.load(g + kTailStart) == rig.at(0x160) && rig.device.load(g + kCursor) == rig.at(0x15C)));
    UNSCOPED_INFO("deferred: the publisher index is unchanged");
    CHECK(rig.device.load(g + kRingWriteIndex) == 0x73);

    const auto r1 = takeResidue();
    PROBE_INFO("deferred: Submit's final arm is consumed_deferred: " + r1);
    PROBE_CHECK((field(r1, "or_publish_consumed_submit") == 1 && field(r1, "or_submit_consumed_deferred") == 1 && field(r1, "or_submit_original_dispatch") == 0 && field(r1, "or_submit_served_kick") == 0));
    PROBE_INFO("deferred: pending_nonzero in the nested Submit's bucket: " + r1);
    PROBE_CHECK(r1.find(" or_submit_site_8269c53c_pending_nonzero_dispatch_consumed_deferred=1") != std::string::npos);
    requireConserved(r1, "deferred");
    const auto legacy = TakeRingStubCounts();
    UNSCOPED_INFO("deferred: stub_submit counts the deferred consume");
    CHECK((stubbed(legacy, RingStub::Submit) == 1 && fellBack(legacy, RingStub::Submit) == 0));
#if REREVVED_DEVELOPER
    const auto probe = PublishedCompletionProbe().Take().counts;
    UNSCOPED_INFO("deferred: one Deferred N store pair with an add of zero, balanced");
    CHECK((probe.addCalls[0] == 1 && probe.addCalls[2] == 0 && probe.addCalls[1] == 0 && probe.adds[0] == 0 && probe.pairError == 0));
#endif
    // The deferral is held for the worker: nothing is kicked or released.
    rig.session.ledger->Pump();
    UNSCOPED_INFO("deferred: no kick, no fence release, no RPTR before the worker submits");
    CHECK(rig.session.sink.writes.empty());
    UNSCOPED_INFO("deferred: the record waits for its worker submit");
    CHECK(rig.session.ledger->Snapshot().pending);
    UNSCOPED_INFO("deferred association: the ledger holds exactly one deferred record for its worker");
    CHECK(rig.session.ledger->Snapshot().deferredSpans == 1);
    UNSCOPED_INFO("deferred: no ledger failure");
    CHECK(rig.session.failed.empty());

    // Deferred publish, main kick and worker submit feed completion debt from publisher
    // index and RPTR.
    using Clock                             = rerevved::gpu::CompletionDebtMonitor::Clock;
    const auto                           t0 = Clock::time_point{} + std::chrono::hours(1);
    rerevved::gpu::CompletionDebtMonitor monitor(std::chrono::seconds(2)), neverReleased(std::chrono::seconds(2));
    std::uint64_t                        publications = 0;
    std::uint32_t                        published    = 0;
    residueKick                                       = [&](std::uint32_t pointer)
    {
        consumeEvents.push_back(std::format("kick:{}", pointer));
        ++publications;
        published = pointer;
    };
    const auto rptr     = rig.device.physical(rig.device.block) + completion::kReadPointerOffset;
    const auto rptrSeen = [&]
    {
        std::vector<std::uint32_t> pointers;
        for (const auto& write : rig.session.sink.writes)
            if (write.first == rptr)
                pointers.push_back(write.second);
        return pointers;
    };
    const auto observe = [&](rerevved::gpu::CompletionDebtMonitor& target, std::chrono::milliseconds at)
    {
        const auto pointers = rptrSeen();
        return target.Observe(t0 + at, publications, published, pointers.empty() ? 0U : pointers.back());
    };
    UNSCOPED_INFO("deferred: the deferral publishes nothing, so there is no debt");
    CHECK((rptrSeen().empty() && !observe(monitor, std::chrono::milliseconds(0)) && !observe(neverReleased, std::chrono::milliseconds(0))));

    rig.device.store(g + kWorkerPending, 0);
    rig.placeRing(0x200, 0x1F8, 0x1000);
    consumeEvents.clear();
    auto later = rig.context(0x8269C61C);
    sub_8269C400(later, base);
    UNSCOPED_INFO("deferred: the main Publish kicks the existing index once");
    CHECK(consumeEvents == std::vector<std::string>{ "kick:115" });
    rig.session.ledger->Pump();
    UNSCOPED_INFO("deferred: the main kick publishes the index and releases no RPTR before the worker's submit");
    CHECK((publications == 1 && published == 0x73 && rptrSeen().empty() && rig.device.load(g + kRingWriteIndex) == 0x73));
    UNSCOPED_INFO("deferred: the unreleased index starts the debt clock");
    CHECK((!observe(monitor, std::chrono::milliseconds(100)) && !observe(neverReleased, std::chrono::milliseconds(100))));
    UNSCOPED_INFO("deferred: the deferral is inside the bound");
    CHECK((!observe(monitor, std::chrono::milliseconds(1500)) && !observe(neverReleased, std::chrono::milliseconds(1500))));
    UNSCOPED_INFO("deferred: with no worker submit the debt is reported at the bound");
    CHECK((observe(neverReleased, std::chrono::milliseconds(3000)) && neverReleased.Reported()));
    UNSCOPED_INFO("deferred: the main kick adds no ledger failure");
    CHECK(rig.session.failed.empty());

    std::thread([&]
                {
                    rig.session.ledger->MarkReplayThread();
                    rig.session.ledger->ReplaySubmit(CommandBufferPhysical(rig.at(0x100)), 13, 0x826AB890);
                    rig.session.ledger->Kick(0x73);
                })
        .join();
    while (rig.session.ledger->Pump())
    {
    }
    const auto pointers = rptrSeen();
    UNSCOPED_INFO("deferred: every RPTR store repeats the existing publisher index");
    CHECK((!pointers.empty() && std::all_of(pointers.begin(), pointers.end(), [](std::uint32_t value)
                                            {
                                                return value == 0x73;
                                            })));
    UNSCOPED_INFO("deferred: the worker's release stores the index and clears the debt");
    CHECK((!observe(monitor, std::chrono::milliseconds(1900)) && !monitor.Reported()));
    UNSCOPED_INFO("deferred: no debt is reported afterwards");
    CHECK((!observe(monitor, std::chrono::seconds(30)) && !monitor.Reported()));
    UNSCOPED_INFO("deferred: the fences released and the index is unchanged");
    CHECK((rig.device.load(rig.device.block) != 0 && rig.device.load(g + kRingWriteIndex) == 0x73));
    UNSCOPED_INFO("deferred: the defer, main and worker sequence adds no ledger failure");
    CHECK(rig.session.failed.empty());
}

// Heap ownership defines bounds counts; below-limit foreign allocations count.
void consumeEntryChecksTheAllocation(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    const auto g = rig.device.g;
    // The ring below the limit's allocation, so the cursor is below the limit but not in it.
    const auto low  = std::min(rig.ring, rig.list);
    const auto high = std::max(rig.ring, rig.list);
    rig.ring        = low;
    rig.placeRing(0x108, 0x100, 0x1000);

    rig.device.store(g + kHardLimit, low + 0x1000);
    auto ctx = rig.context(0x8269C61C);
    sub_8269C400(ctx, base);
    auto rc = TakeRingConsumeCounts();
    UNSCOPED_INFO("allocation: C and S inside the limit's allocation count no violation: " + FormatRingConsumeCounts(rc));
    CHECK((originals.empty() && rc.fault == 0 && FormatRingConsumeCounts(rc).find("rc_invariant") == std::string::npos));

    rig.device.store(g + kWorkerPending, 0);
    rig.placeRing(0x108, 0x100, 0x1000);
    rig.device.store(g + kHardLimit, high + 0x1000);
    auto other = rig.context(0x8269C61C);
    sub_8269C400(other, base);
    rc = TakeRingConsumeCounts();
    UNSCOPED_INFO("allocation: C and S below the limit but in another allocation are counted: " + FormatRingConsumeCounts(rc));
    CHECK((rc.invariant[static_cast<std::size_t>(RingInvariant::CursorExtent)] == 1 && rc.invariant[static_cast<std::size_t>(RingInvariant::StartExtent)] == 1));
    UNSCOPED_INFO("allocation: the counts do not change the consume");
    CHECK((originals.empty() && rc.fault == 0));
    (void)takeResidue();
}

// Exercise each Submit arm with r4 high half set and nonzero r7 to expose narrowing.
void consumeSubmitArms(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    const auto g     = rig.device.g;
    const auto token = 0x5555AAAA00000000ULL | rig.at(0x200);
    const auto call  = [&](std::uint64_t add, std::uint32_t lr)
    {
        auto ctx   = rig.context(lr);
        ctx.r4.u64 = token;
        ctx.r5.u64 = 0x7777000000000000ULL | viewAdjusted(rig.at(0x100));
        ctx.r6.u64 = 0x9999000000000000ULL | 13;
        ctx.r7.u64 = add;
        ctx.r8.u64 = 0x3333000000000000ULL | (g + kTileList);
        sub_8269BD98(ctx, base);
        return ctx;
    };
    // Entry N zero, Kick: N += r7 (low 32 bits only), the direct record, the kick.
    auto ctx = call(0xAABBCCDD00000002ULL, 0x826AC3B4);
    UNSCOPED_INFO("submit direct: full-width P returned, no Original");
    CHECK((originals.empty() && ctx.r3.u64 == token));
    UNSCOPED_INFO("submit direct: N = 0 + low32(r7)");
    CHECK(rig.device.load(g + kWorkerPending) == 2);
    UNSCOPED_INFO("submit direct: the existing index is kicked once");
    CHECK(consumeEvents == std::vector<std::string>{ "kick:115" });
    auto r1 = takeResidue();
    PROBE_INFO("submit direct: served_kick: " + r1);
    PROBE_CHECK((field(r1, "or_submit_served_kick") == 1 && field(r1, "or_submit_calls") == 1));
    requireConserved(r1, "submit direct");

    // Add zero still stores N: observed by a store the probe pairs.
    rig.device.store(g + kWorkerPending, 0);
    consumeEvents.clear();
#if REREVVED_DEVELOPER
    (void)PublishedCompletionProbe().Take();
#endif
    ctx = call(0, 0x826AC3B4);
#if REREVVED_DEVELOPER
    auto probe = PublishedCompletionProbe().Take().counts;
    UNSCOPED_INFO("submit direct: the add-zero store is paired");
    CHECK((probe.addCalls[2] == 1 && probe.adds[2] == 0 && probe.pairError == 0));
#endif
    CHECK(ctx.r3.u64 == token);
    (void)takeResidue();

    // Silent: CPU fence mode with no sink and no callback.
    rig.mode(kModeCpuFence, 0, 0);
    consumeEvents.clear();
    ctx = call(0, 0x826AC3B4);
    UNSCOPED_INFO("submit silent: full-width P, no kick");
    CHECK((ctx.r3.u64 == token && consumeEvents.empty()));
    r1 = takeResidue();
    PROBE_INFO("submit silent: served_silent: " + r1);
    PROBE_CHECK((field(r1, "or_submit_served_silent") == 1 && field(r1, "or_submit_served_kick") == 0));
    requireConserved(r1, "submit silent");
    rig.mode(0, 0, 0);
    // The Silent record is sealed on the host: no kick, index or RPTR effect, and a
    // direct submit after it is no unkicked one.
    UNSCOPED_INFO("submit silent: nothing failed, the index stayed and no RPTR was stored");
    CHECK((rig.session.failed.empty() && rig.device.load(g + kRingWriteIndex) == 0x73 && rig.session.sink.writes.empty()));
    consumeEvents.clear();
    ctx = call(0, 0x826AC3B4);
    UNSCOPED_INFO("submit silent: a direct submit after it kicks and fails nothing");
    CHECK((rig.session.failed.empty() && consumeEvents == std::vector<std::string>{ "kick:115" } && ctx.r3.u64 == token));
    (void)takeResidue();

    // Entry N nonzero, locked reread zero: the direct arm, with the lock balanced and
    // the third read of N storing r7.
    rig.device.store(g + kWorkerPending, 9);
    rig.workerList(g + kTileList, rig.list + 0x40);
    consumeEvents.clear();
#if REREVVED_DEVELOPER
    (void)PublishedCompletionProbe().Take();
#endif
    lockBody = [&](PPCContext&)
    {
        rig.device.store(g + kWorkerPending, 0);
    };
    ctx = call(5, 0x826AC3E8);
    UNSCOPED_INFO("locked zero: the direct arm returns P and runs no B550");
    CHECK((ctx.r3.u64 == token && helperCalls.empty()));
    UNSCOPED_INFO("locked zero: acquire, release with the acquire result, then the direct kick");
    CHECK((consumeEvents.size() == 3 && consumeEvents[0] == std::format("acquire:{:X}:8269BDD8", g + 0x2B04) && consumeEvents[1] == std::format("release:{:X}:{:X}:8269BE2C", g + 0x2B04, lockLevel) && consumeEvents[2] == "kick:115"));
    UNSCOPED_INFO("locked zero: N = reread 0 + r7");
    CHECK(rig.device.load(g + kWorkerPending) == 5);
#if REREVVED_DEVELOPER
    probe = PublishedCompletionProbe().Take().counts;
    UNSCOPED_INFO("locked zero: the Stub pair only, no Deferred pair");
    CHECK((probe.addCalls[2] == 1 && probe.addCalls[0] == 0 && probe.pairError == 0));
#endif
    r1 = takeResidue();
    PROBE_INFO("locked zero: reason from the final arm, not from the nonzero entry N: " + r1);
    PROBE_CHECK((field(r1, "or_submit_served_kick") == 1 && field(r1, "or_submit_consumed_deferred") == 0 && r1.find("or_submit_reason_pending_nonzero") == std::string::npos));
    requireConserved(r1, "locked zero");

    // Locked reread nonzero: the deferred arm. The token words carry the unmasked
    // dwords; the return is P+0x10 in full width; N = reread + r7 after B550.
    lockBody = {};
    rig.device.store(g + kWorkerPending, 9);
    consumeEvents.clear();
    helperCalls.clear();
    helperWatch = { g + kWorkerPending, rig.at(0x204), rig.at(0x208), rig.at(0x20C) };
#if REREVVED_DEVELOPER
    (void)PublishedCompletionProbe().Take();
#endif
    {
        auto deferred   = rig.context(0x826AC3B4);
        deferred.r4.u64 = token;
        deferred.r5.u64 = viewAdjusted(rig.at(0x100));
        deferred.r6.u64 = 0x7E000005; // the guest token keeps the bits above 24
        deferred.r7.u64 = 0xAABBCCDD00000003ULL;
        deferred.r8.u64 = g + kTileList;
        sub_8269BD98(deferred, base);
        UNSCOPED_INFO("deferred: full-width P+0x10");
        CHECK(deferred.r3.u64 == token + 16);
        UNSCOPED_INFO("deferred: P+4 unmasked, P+8 the address, P+0xC the terminator");
        CHECK((rig.device.load(rig.at(0x204)) == 0xFF000005 && rig.device.load(rig.at(0x208)) == viewAdjusted(rig.at(0x100)) && rig.device.load(rig.at(0x20C)) == 0xC0000000));
        UNSCOPED_INFO("deferred: B550 sees the full-width token");
        CHECK((helperCalls.size() == 1 && helperCalls[0].r3 == g + kTileList && helperCalls[0].r4 == token && helperCalls[0].r5 == token + 16));
        if (helperCalls.size() == 1)
        {
            UNSCOPED_INFO("deferred: N is still 9 at B550");
            CHECK(helperCalls[0].words[0] == 9);
        }
        UNSCOPED_INFO("deferred: N = reread after B550 + r7");
        CHECK(rig.device.load(g + kWorkerPending) == 12);
        UNSCOPED_INFO("deferred: the lock is released last");
        CHECK((consumeEvents.size() == 3 && consumeEvents[2] == std::format("release:{:X}:{:X}:8269BE2C", g + 0x2B04, lockLevel)));
    }
#if REREVVED_DEVELOPER
    probe = PublishedCompletionProbe().Take().counts;
    UNSCOPED_INFO("deferred: one Deferred pair carrying the add");
    CHECK((probe.addCalls[0] == 1 && probe.adds[0] == 3 && probe.addCalls[2] == 0 && probe.pairError == 0));
#endif
    r1 = takeResidue();
    PROBE_INFO("deferred: consumed_deferred: " + r1);
    PROBE_CHECK((field(r1, "or_submit_consumed_deferred") == 1 && field(r1, "or_submit_calls") == 1));
    requireConserved(r1, "deferred");
}

// Starved and tiling arms of Publish: the common tail only, plus the one tiled segment.
void consumePublishStarvedAndTiling(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    const auto g        = rig.device.g;
    const auto highG    = 0xCAFE000000000000ULL | g;
    const auto listBase = rig.list;
    // Starved wins over recording and tiling. It reaches the tail with R = C+4 and leaves
    // the fence, N and the list alone.
    rig.mode(0x20, 0x80, 1);
    rig.device.store(g + kListEnd, listBase);
    rig.device.store(g + kListLimit, listBase + 0x100);
    auto ctx              = rig.context(0x8269C61C);
    ctx.r3.u64            = highG;
    const auto registers  = savedRegisters(ctx);
    const auto listBefore = guestBytes(memory, listBase, 0x100);
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("starved: no Original, helper, lock or kick");
    CHECK((originals.empty() && helperCalls.empty() && consumeEvents.empty()));
    UNSCOPED_INFO("starved: r3 is the incoming full-width device");
    CHECK((ctx.r3.u64 == highG && savedRegisters(ctx) == registers));
    UNSCOPED_INFO("starved: the tail aligns R = C+4: G+0x3A50 = ring+0x120, G+0x30 = ring+0x11C");
    CHECK((rig.device.load(g + kTailStart) == rig.at(0x120) && rig.device.load(g + kCursor) == rig.at(0x11C)));
    UNSCOPED_INFO("starved: no fence advance and no pending-N change");
    CHECK((rig.device.load(g + kNextFence) == 0x11 && rig.device.load(g + kFenceCursorCopy) == 0 && rig.device.load(g + kWorkerPending) == 0));
    UNSCOPED_INFO("starved: no list word");
    CHECK(guestBytes(memory, listBase, 0x100) == listBefore);
    auto r1 = takeResidue();
    PROBE_INFO("starved: consumed_starved: " + r1);
    PROBE_CHECK((field(r1, "or_publish_consumed_starved") == 1 && field(r1, "or_publish_calls") == 1 && field(r1, "or_submit_calls") == 0));
    requireConserved(r1, "starved");
    PROBE_INFO("starved: reason starved");
    PROBE_CHECK(r1.find(" or_publish_reason_starved=1") != std::string::npos);

    // Tiling, q = 0: C+4 == S, no descriptor read, no segment, no list store.
    rig.mode(0, 0, 1);
    rig.placeRing(0x2C, 0x30, 0x1000);
    rig.device.store(g + kListEnd, 0);
    rig.device.store(g + kListLimit, 0);
    ctx        = rig.context(0x8269C61C);
    ctx.r3.u64 = highG;
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("tiling q=0: no Original, r3 is the incoming device");
    CHECK((originals.empty() && ctx.r3.u64 == highG));
    UNSCOPED_INFO("tiling q=0: tail only, no list word");
    CHECK((rig.device.load(g + kTailStart) == rig.at(0x40) && rig.device.load(g + kCursor) == rig.at(0x3C) && rig.device.load(g + kListEnd) == 0 && guestBytes(memory, listBase, 0x100) == listBefore));
    r1 = takeResidue();
    PROBE_INFO("tiling q=0: consumed_tiling: " + r1);
    PROBE_CHECK((field(r1, "or_publish_consumed_tiling") == 1 && r1.find(" or_publish_reason_tiling=1") != std::string::npos));
    requireConserved(r1, "tiling q=0");

    // Tiling, q = 12: one segment {A(S), 0x8100000C} at E, the cursor E+8, r3 = G+0x3438.
    ctx = rig.context(0x826AC3B4);
    sub_826AC0A8(ctx, base); // BeginTiling: the fixture sets G+0x33B0/G+0x33B4 and starts the ledger's list
    rig.placeRing(0x2C, 0, 0x1000);
    rig.device.store(g + kListEnd, listBase + 0x10);
    rig.device.store(g + kListLimit, listBase + 0x100);
    (void)TakeRingStubCounts();
    (void)takeResidue();
    ResetRingStubLogsForTesting();
    ctx        = rig.context(0x8269C61C);
    ctx.r3.u64 = highG;
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("tiling q=12: a consumed call logs no stub_publish_original warning");
    CHECK(!RingStubOriginalLoggedForTesting(RingStub::Publish));
    UNSCOPED_INFO("tiling q=12: only BeginTiling's Original ran");
    CHECK((originals.size() == 1 && originals[0].first == 0x826AC0A8));
    UNSCOPED_INFO("tiling q=12: r3 is the incoming device + 0x3438");
    CHECK(ctx.r3.u64 == highG + kTileList);
    UNSCOPED_INFO("tiling q=12: A(S) at E+4, the token at E, E+8 at G+0x3440");
    CHECK((rig.device.load(listBase + 0x14) == viewAdjusted(rig.at(0)) && rig.device.load(listBase + 0x10) == 0x8100000C && rig.device.load(g + kListEnd) == listBase + 0x18));
    UNSCOPED_INFO("tiling q=12: the tail aligns R");
    CHECK((rig.device.load(g + kTailStart) == rig.at(0x40) && rig.device.load(g + kCursor) == rig.at(0x3C)));
    r1 = takeResidue();
    PROBE_INFO("tiling q=12: consumed_tiling: " + r1);
    PROBE_CHECK((field(r1, "or_publish_consumed_tiling") == 1 && field(r1, "or_publish_original_dispatch") == 0));
    requireConserved(r1, "tiling q=12");
    const auto legacy = TakeRingStubCounts();
    UNSCOPED_INFO("tiling: stub_publish_original keeps its classification meaning even when consumed");
    CHECK((fellBack(legacy, RingStub::Publish) == 1 && stubbed(legacy, RingStub::Publish) == 0));
    UNSCOPED_INFO("tiling: no ledger failure");
    CHECK(rig.session.failed.empty());
}

// Tiled list growth: the retained helper's exact arguments, return address and return.
void consumePublishGrowth(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    const auto g        = rig.device.g;
    const auto highG    = 0xCAFE000000000000ULL | g;
    const auto listBase = rig.list;
    const auto grown    = listBase + 0x200;
    auto       ctx      = rig.context(0x826AC3B4);
    sub_826AC0A8(ctx, base);
    rig.placeRing(0x2C, 0, 0x1000);
    rig.device.store(g + kListEnd, listBase + 0x10);
    rig.device.store(g + kListLimit, listBase + 0x17); // E+8 is past the limit
    growthStorage = [&](PPCContext&)
    {
        return grown;
    };
    helperWatch          = { g + kListEnd };
    ctx                  = rig.context(0x8269C61C);
    ctx.r3.u64           = highG;
    const auto registers = savedRegisters(ctx);
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("growth: C170 gets the full-width list and returns at 0x8269C4E4");
    CHECK((helperCalls.size() == 1 && helperCalls[0].address == 0x8269C170 && helperCalls[0].r3 == highG + kTileList && helperCalls[0].lr == 0x8269C4E4));
    if (helperCalls.size() == 1)
    {
        UNSCOPED_INFO("growth: the lease is held and the list cursor is still the old E");
        CHECK((helperCalls[0].leases >= 1 && helperCalls[0].words == std::vector<std::uint32_t>{ listBase + 0x10 }));
    }
    UNSCOPED_INFO("growth: the pair lands at the helper's destination, E+8 at G+0x3440");
    CHECK((rig.device.load(grown + 4) == viewAdjusted(rig.at(0)) && rig.device.load(grown) == 0x8100000C && rig.device.load(g + kListEnd) == grown + 8));
    UNSCOPED_INFO("growth: the old E is untouched");
    CHECK((rig.device.load(listBase + 0x10) == 0 && rig.device.load(listBase + 0x14) == 0));
    UNSCOPED_INFO("growth: r3 is the helper's actual return");
    CHECK((ctx.r3.u64 == grown && savedRegisters(ctx) == registers));
    UNSCOPED_INFO("growth: no ledger failure");
    CHECK(rig.session.failed.empty());
}

// Rollover releases Publish's lease before the waiting C270 helper.
void consumePublishRollover(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    const auto g     = rig.device.g;
    const auto highG = 0xCAFE000000000000ULL | g;
    rig.placeRing(0x108, 0x100, 0x120);
    helperWatch = { g + kCursor, g + kTailStart };
    auto ctx    = rig.context(0x8269C61C);
    ctx.r3.u64  = highG;
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("rollover: C270 gets the full-width device, r4 = 0, at 0x8269C568");
    CHECK((helperCalls.size() == 1 && helperCalls[0].address == 0x8269C270 && helperCalls[0].r3 == highG && helperCalls[0].r4 == 0 && helperCalls[0].lr == 0x8269C568));
    if (helperCalls.size() == 1)
    {
        UNSCOPED_INFO("rollover: the Publish lease is released before the helper that can wait");
        CHECK(helperCalls[0].leases == 0);
        UNSCOPED_INFO("rollover: G+0x30 = P-4 is stored first and G+0x3A50 is not");
        CHECK(helperCalls[0].words == std::vector<std::uint32_t>{ rig.at(0x130), rig.at(0x100) });
    }
    UNSCOPED_INFO("rollover: the helper's actual return propagates");
    CHECK(ctx.r3.u64 == rolloverResult);
    UNSCOPED_INFO("rollover: G+0x3A50 stays");
    CHECK(rig.device.load(g + kTailStart) == rig.at(0x100));
    const auto r1 = takeResidue();
    PROBE_INFO("rollover: counted once as consumed_submit: " + r1);
    PROBE_CHECK(field(r1, "or_publish_consumed_submit") == 1);
    requireConserved(r1, "rollover");
    UNSCOPED_INFO("rollover: no ledger failure");
    CHECK(rig.session.failed.empty());
}

// CPU fencing would outrun host draws, so Publish refuses before mutation.
void consumeCpuFenceRefusesBeforeMutation(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    rig.holdOnHostDraw();
    rig.mode(kModeCpuFence, 0, 0);
    const auto before = guestBytes(memory, rig.device.g);
    const auto ring   = guestBytes(memory, rig.ring, 0x400);
    auto       ctx    = rig.context(0x8269C61C);
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("cpu fence: Publish runs Original once and nothing nested");
    CHECK((originalCalls(0x8269C400) == 1 && originalCalls(0x8269BD98) == 0 && originalCalls(0x8269BCA8) == 0));
    UNSCOPED_INFO("cpu fence: no store before the refusal, so B+0 and B+4 stay behind the held host draw");
    CHECK((guestBytes(memory, rig.device.g) == before && guestBytes(memory, rig.ring, 0x400) == ring));
    UNSCOPED_INFO("cpu fence: no consume effect");
    CHECK((consumeEvents.empty() && helperCalls.empty()));
    const auto rc     = TakeRingConsumeCounts();
    const auto fields = FormatRingConsumeCounts(rc);
    UNSCOPED_INFO("cpu fence: counted under its own refusal: " + fields);
    CHECK((rc.publishOriginalCpuFence == 1 && rc.fault == 0 && rc.recordingOriginal == 0));
    UNSCOPED_INFO("cpu fence: the timing-line field: " + fields);
    CHECK((fields.find(" rc_publish_original_cpu_fence=1") != std::string::npos && fields.find("cpu_fence_consumed") == std::string::npos));
    const auto r1 = takeResidue();
    PROBE_INFO("cpu fence: one Original dispatch: " + r1);
    PROBE_CHECK((field(r1, "or_publish_original_dispatch") == 1 && field(r1, "or_publish_consumed_submit") == 0));
    requireConserved(r1, "cpu fence");
    UNSCOPED_INFO("cpu fence: no ledger failure");
    CHECK(rig.session.failed.empty());
}

// No Original checker exists, so PT2 plans every call itself: a plain call is consumed
// and writes the guest tail, a recording call is refused to exactly one Original, and the
// dispatch counts balance.
void consumeWithoutExitChecker(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    const auto g = rig.device.g;
    // Starved reaches the tail with R = C+4 and nothing else.
    rig.mode(0x20, 0, 0);
    auto ctx = rig.context(0x8269C61C);
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("no checker: a planned call is consumed with no Original");
    CHECK(originals.empty());
    UNSCOPED_INFO("no checker: the consume stored the aligned tail: G+0x3A50 = ring+0x120, G+0x30 = ring+0x11C");
    CHECK((rig.device.load(g + kTailStart) == rig.at(0x120) && rig.device.load(g + kCursor) == rig.at(0x11C)));
    auto r1 = takeResidue();
    PROBE_INFO("no checker: one consumed dispatch and no Original dispatch: " + r1);
    PROBE_CHECK((field(r1, "or_publish_calls") == 1 && field(r1, "or_publish_consumed_starved") == 1 && field(r1, "or_publish_original_dispatch") == 0));
    requireConserved(r1, "no checker consumed");
    originals.clear();

    // Recording is refused before planning writes anything: one Original, its own counter.
    rig.mode(0, 0x80, 1);
    const auto before = guestBytes(memory, g);
    ctx               = rig.context(0x8269C61C);
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("no checker: a recording call runs exactly one Original");
    CHECK(originalCalls(0x8269C400) == 1);
    UNSCOPED_INFO("no checker: the refused call wrote nothing");
    CHECK(guestBytes(memory, g) == before);
    const auto refusal = TakeRingConsumeCounts();
    UNSCOPED_INFO("no checker: the refusal counts as recording and as no fault: " + FormatRingConsumeCounts(refusal));
    CHECK((refusal.recordingOriginal == 1 && refusal.fault == 0 && FormatRingConsumeCounts(refusal).find("exit_check") == std::string::npos));
    r1 = takeResidue();
    PROBE_INFO("no checker: one Original dispatch: " + r1);
    PROBE_CHECK((field(r1, "or_publish_original_dispatch") == 1 && field(r1, "or_publish_consumed_starved") == 0));
    requireConserved(r1, "no checker refused");
    originals.clear();
}

// Every fallback is one Original dispatch with its own reason and no consume effect.
void consumeFallbacksAndPrerequisites(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    const auto g              = rig.device.g;
    const auto expectOriginal = [&](std::string_view name, std::uint32_t function, std::string_view reasonField, const std::vector<std::uint8_t>& deviceBefore)
    {
        UNSCOPED_INFO(std::string(name) + ": exactly one Original dispatch");
        CHECK(originalCalls(function) == 1);
        UNSCOPED_INFO(std::string(name) + ": no consume effect");
        CHECK((consumeEvents.empty() && helperCalls.empty()));
        const auto r1 = takeResidue();
        PROBE_INFO(std::string(name) + ": reason " + std::string(reasonField) + " in " + r1);
        PROBE_CHECK(r1.find(reasonField) != std::string::npos);
        requireConserved(r1, name);
        UNSCOPED_INFO(std::string(name) + ": the device is untouched");
        CHECK(guestBytes(memory, g) == deviceBefore);
    };

    // Recording: its decoder is unknown, so the call stays on Original with its own counter.
    rig.mode(0, 0x80, 1);
    auto before = guestBytes(memory, g);
    auto ctx    = rig.context(0x8269C61C);
    sub_8269C400(ctx, base);
    expectOriginal("recording", 0x8269C400, " or_publish_reason_recording=1", before);
    UNSCOPED_INFO("recording: rc_recording_original counts it");
    CHECK(TakeRingConsumeCounts().recordingOriginal == 1);
    originals.clear();

    // Sink: Publish consumes, the nested Submit falls back to its own Original.
    rig.mode(0, 0, 0);
    rig.device.store(g + kCpuFenceSink, 0x82600110);
    ctx = rig.context(0x8269C61C);
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("sink: Publish is consumed and the nested Submit runs one Original");
    CHECK((originalCalls(0x8269C400) == 0 && originalCalls(0x8269BD98) == 1));
    auto r1 = takeResidue();
    PROBE_INFO("sink: nested fallback is Submit's Original with publisher_sink: " + r1);
    PROBE_CHECK((field(r1, "or_publish_consumed_submit") == 1 && field(r1, "or_submit_original_dispatch") == 1 && r1.find(" or_submit_reason_publisher_sink=1") != std::string::npos));
    requireConserved(r1, "sink");
    UNSCOPED_INFO("sink: a nested Original fallback is not a Publish fault");
    CHECK(TakeRingConsumeCounts().fault == 0);
    rig.device.store(g + kCpuFenceSink, 0);
    originals.clear();
    tiling.submissions.clear();

    // A nonzero ring callback without CPU mode.
    rig.device.store(g + kRingCallback, 0x82600110);
    ctx        = rig.context(0x826AC3B4);
    ctx.r7.u64 = 1;
    sub_8269BD98(ctx, base);
    r1 = takeResidue();
    UNSCOPED_INFO("callback: Submit runs Original with publisher_callback: " + r1);
    CHECK(originalCalls(0x8269BD98) == 1);
    PROBE_CHECK((r1.find(" or_submit_reason_publisher_callback=1") != std::string::npos && field(r1, "or_submit_original_dispatch") == 1));
    requireConserved(r1, "callback");
    rig.device.store(g + kRingCallback, 0);
    originals.clear();
    tiling.submissions.clear();

    // The cvar off: today's behavior, Original for Publish.
    REXCVAR_SET(native_ring_consume_publish, false);
    ctx = rig.context(0x8269C61C);
    sub_8269C400(ctx, base);
    r1 = takeResidue();
    UNSCOPED_INFO("cvar off: Original: " + r1);
    CHECK(originalCalls(0x8269C400) == 1);
    PROBE_CHECK((field(r1, "or_publish_original_dispatch") == 1 && field(r1, "or_publish_consumed_submit") == 0));
    UNSCOPED_INFO("cvar off: no rc field");
    CHECK(FormatRingConsumeCounts(TakeRingConsumeCounts()).empty());
    originals.clear();
    tiling.submissions.clear();
    REXCVAR_SET(native_ring_consume_publish, true);

    // No published ledger: the native route is not complete, so nothing is consumed. A
    // Submit with N = 0 is no legacy direct stub either: it runs Original untouched.
    completion::WithdrawGuestProgress();
    ctx = rig.context(0x8269C61C);
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("no ledger: Publish runs Original");
    CHECK(originalCalls(0x8269C400) == 1);
    originals.clear();
    rig.device.store(g + kWorkerPending, 0);
    (void)takeResidue();
    before = guestBytes(memory, g);
    consumeEvents.clear();
    residueBody = [](std::uint32_t, PPCContext& out)
    {
        out.r3.u64 = 0x1234;
    };
    ctx        = rig.context(0x826AC3B4);
    ctx.r7.u64 = 3;
    sub_8269BD98(ctx, base);
    residueBody = {};
    UNSCOPED_INFO("no ledger: Submit with N = 0 runs Original");
    CHECK(originalCalls(0x8269BD98) == 1);
    UNSCOPED_INFO("no ledger: the Submit stub changed neither N nor the publisher");
    CHECK((guestBytes(memory, g) == before && consumeEvents.empty()));
    r1 = takeResidue();
    PROBE_INFO("no ledger: one Original dispatch and no served arm: " + r1);
    PROBE_CHECK((field(r1, "or_submit_original_dispatch") == 1 && field(r1, "or_submit_served_kick") == 0 && field(r1, "or_submit_served_silent") == 0));
    requireConserved(r1, "no ledger");
    originals.clear();
    tiling.submissions.clear();
    completion::PublishGuestProgress(rig.session.ledger);

    // Withdrawn services: the consume wrappers are inactive.
    WithdrawRingStubServices();
    ctx = rig.context(0x8269C61C);
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("inactive: Publish runs Original");
    CHECK((originalCalls(0x8269C400) == 1 && consumeEvents.empty() && helperCalls.empty()));
    PublishRingStubServices(std::make_shared<Services>(rig.session.ledger));
    (void)takeResidue();
}

// Read-only token stores take consumed-fault, release the lock and link nothing.
void consumeDeferredTokenStoreFails(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    const auto g     = rig.device.g;
    const auto token = rig.at(0x200);
    rig.device.store(g + kWorkerPending, 9);
    rig.workerList(g + kTileList, rig.list + 0x40);
    auto* heap = memory.LookupHeap(rig.ring);
    UNSCOPED_INFO("token fault: the ring heap exists");
    CHECK(heap != nullptr);
    if (!heap)
        return;
    lockBody = [&](PPCContext&)
    {
        UNSCOPED_INFO("token fault: the token page turns read-only under the lock");
        CHECK(heap->Protect(rig.ring, 0x10000, rex::memory::kMemoryProtectRead));
    };
    const auto listBefore = guestBytes(memory, rig.list, 0x100);
    const auto deviceList = rig.device.load(g + kTileList + 4);
    auto       ctx        = rig.context(0x826AC3B4);
    ctx.r4.u64            = token;
    ctx.r5.u64            = viewAdjusted(rig.at(0x100));
    ctx.r6.u64            = 13;
    ctx.r7.u64            = 3;
    ctx.r8.u64            = g + kTileList;
    sub_8269BD98(ctx, base);
    UNSCOPED_INFO("token fault: the page is restored");
    CHECK(heap->Protect(rig.ring, 0x10000, kWritable));
    UNSCOPED_INFO("token fault: the deferred return is still P+0x10");
    CHECK(ctx.r3.u64 == token + 16);
    UNSCOPED_INFO("token fault: B550 never links the node");
    CHECK(helperCalls.empty());
    UNSCOPED_INFO("token fault: the lock is acquired and released, balanced");
    CHECK((consumeEvents.size() == 2 && consumeEvents[0] == std::format("acquire:{:X}:8269BDD8", g + 0x2B04) && consumeEvents[1] == std::format("release:{:X}:{:X}:8269BE2C", g + 0x2B04, lockLevel)));
    UNSCOPED_INFO("token fault: the pending add is skipped");
    CHECK(rig.device.load(g + kWorkerPending) == 9);
    UNSCOPED_INFO("token fault: no partial node became reachable");
    CHECK((guestBytes(memory, rig.list, 0x100) == listBefore && rig.device.load(g + kTileList + 4) == deviceList && rig.device.load(rig.ring + 0x200 + 4) == 0));
    const auto rc = TakeRingConsumeCounts();
    UNSCOPED_INFO("token fault: rc_fault counts it: " + FormatRingConsumeCounts(rc));
    CHECK(rc.fault == 1);
    UNSCOPED_INFO("token fault: the strict ledger names the step");
    CHECK((rig.session.failed.size() == 1 && rig.session.failed[0].find("ring_consume_fault: submit_token_store") != std::string::npos));
    const auto r1 = takeResidue();
    PROBE_INFO("token fault: still one consumed_deferred dispatch: " + r1);
    PROBE_CHECK(field(r1, "or_submit_consumed_deferred") == 1);
    requireConserved(r1, "token fault");
    // The strict ledger's failure disabled the stubs for the session; later cases need them.
    PublishRingStubServices(std::make_shared<Services>(rig.session.ledger));
}

// Each unsupported deferred population keeps its own R1 reason, reached through the wrapper.
void consumeSubmitRefusalReasons(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    const auto g = rig.device.g;
    rig.device.store(g + kWorkerPending, 9);
    rig.workerList(g + kTileList, rig.list + 0x40);
    const auto call = [&](std::uint64_t token, std::string_view reasonField, std::string_view name)
    {
        originals.clear();
        tiling.submissions.clear();
        consumeEvents.clear();
        helperCalls.clear();
        (void)takeResidue();
        const auto before = guestBytes(memory, g);
        residueBody       = [](std::uint32_t, PPCContext& out)
        {
            out.r3.u64 = 0x1234;
        };
        auto ctx   = rig.context(0x826AC3B4);
        ctx.r4.u64 = token;
        ctx.r5.u64 = viewAdjusted(rig.at(0x100));
        ctx.r6.u64 = 13;
        ctx.r7.u64 = 1;
        ctx.r8.u64 = g + kTileList;
        sub_8269BD98(ctx, base);
        const auto r1 = takeResidue();
        UNSCOPED_INFO(std::string(name) + ": one Original, no lock, no link");
        CHECK((originalCalls(0x8269BD98) == 1 && consumeEvents.empty() && helperCalls.empty()));
        UNSCOPED_INFO(std::string(name) + ": the device is untouched");
        CHECK(guestBytes(memory, g) == before);
        PROBE_INFO(std::string(name) + ": its own reason " + std::string(reasonField) + ": " + r1);
        PROBE_CHECK((r1.find(std::string(" or_submit_reason_") + std::string(reasonField) + "=1") != std::string::npos && r1.find("or_submit_reason_pending_nonzero") == std::string::npos));
        PROBE_INFO(std::string(name) + ": an Original dispatch: " + r1);
        PROBE_CHECK((field(r1, "or_submit_original_dispatch") == 1 && field(r1, "or_submit_consumed_deferred") == 0));
        requireConserved(r1, name);
    };
    call(0xFFFFFFF8ULL, "token_wrap", "token wrap");
    auto* ringHeap = memory.LookupHeap(rig.ring);
    UNSCOPED_INFO("refusals: the token page turns read-only");
    CHECK((ringHeap && ringHeap->Protect(rig.ring, 0x10000, rex::memory::kMemoryProtectRead)));
    call(rig.at(0x200), "token_storage", "token storage");
    UNSCOPED_INFO("refusals: the token page is restored");
    CHECK((ringHeap && ringHeap->Protect(rig.ring, 0x10000, kWritable)));
    auto* listHeap = memory.LookupHeap(rig.list);
    UNSCOPED_INFO("refusals: the worker list tail turns read-only");
    CHECK((listHeap && listHeap->Protect(rig.list, 0x10000, rex::memory::kMemoryProtectRead)));
    call(rig.at(0x200), "list_storage", "list storage");
    UNSCOPED_INFO("refusals: the worker list is restored");
    CHECK((listHeap && listHeap->Protect(rig.list, 0x10000, kWritable)));
    // The lock word shares G+0x2B00's page, so only the mapping is reachable here.
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("refusals: the lock refusal has its own reason");
    CHECK((SubmitRefusalResidueReason(RingConsumeRefusal::LockStorage) == OriginalResidueReason::LockStorage && SubmitRefusalResidueReason(RingConsumeRefusal::PendingUnwritable) == OriginalResidueReason::RaiseWriteFailed));
#endif
}

// With native_ring_consume_publish off, R1 keeps its base fields; monitors stay idle.
void consumeOffKeepsTheOldReport(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    REXCVAR_SET(native_ring_consume_publish, false);
    (void)takeResidue();
    auto ctx = rig.context(0x8269C61C);
    sub_8269C400(ctx, base);
    ctx = rig.context(0x826AC3B4);
    sub_8269BD98(ctx, base);
    const auto r1 = takeResidue();
    PROBE_INFO("cvar off: the base fields stay: " + r1);
    PROBE_CHECK((r1.find("or_publish_calls=") != std::string::npos && r1.find("or_submit_calls=") != std::string::npos));
    PROBE_INFO("cvar off: no ring consume field in R1: " + r1);
    PROBE_CHECK((r1.find("or_publish_consumed_starved") == std::string::npos && r1.find("or_submit_consumed_deferred") == std::string::npos));
    UNSCOPED_INFO("cvar off: no rc field");
    CHECK(FormatRingConsumeCounts(TakeRingConsumeCounts()).empty());
    originals.clear();
    tiling.submissions.clear();
}

// Consumed EndTiling chains callback and fence buffers under worker-owned ring; RPTR
// advances after worker submit.
void consumeEndTilingBracket(rex::memory::Memory& memory, std::uint8_t* base)
{
    ConsumeRig rig(memory, base);
    const auto g        = rig.device.g;
    const auto listBase = rig.list;
    auto       ctx      = rig.context(kCaller);
    sub_826AC0A8(ctx, base);
    rig.placeRing(0x2C, 0, 0x1000);
    rig.device.store(g + kListEnd, listBase + 0x10);
    rig.device.store(g + kListLimit, listBase + 0x100);
    rig.device.store(g + kTileList, listBase);
    ctx = rig.context(0x8269C61C);
    sub_8269C400(ctx, base);
    UNSCOPED_INFO("bracket: the consumed Publish recorded its tiled segment");
    CHECK(rig.device.load(g + kListEnd) == listBase + 0x18);

    // The worker owns the ring: both buffers are chained.
    rig.device.store(g + kWorkerPending, 1);
    rig.device.store(g + kPreviousList + 8, listBase + 0x200);
    const auto arena = tiling.next;
    (void)TakeRingStubCounts();
    (void)takeResidue();
#if REREVVED_DEVELOPER
    (void)PublishedCompletionProbe().Take();
#endif
    helperCalls.clear();
    consumeEvents.clear();
    ctx        = rig.context(kCaller);
    ctx.r5.u64 = kEndTilingFlags;
    sub_826AC218(ctx, base);

    const auto callbackBuffer = arena, fenceBuffer = arena + 608, tokenBuffer = arena + 768;
    UNSCOPED_INFO("bracket: neither EndTiling, the emitters nor Submit run Original");
    CHECK((ctx.r3.u32 == 0 && originalCalls(0x8269BD98) == 0 && originalCalls(0x826AC218) == 0 && originalCalls(0x8269BF40) == 0 && originalCalls(0x8269BCA8) == 0));
    UNSCOPED_INFO("bracket: both buffers are chained by B550");
    CHECK((helperCalls.size() == 2 && helperCalls[0].lr == 0x8269BE10 && helperCalls[1].lr == 0x8269BE10));
    if (helperCalls.size() == 2)
    {
        UNSCOPED_INFO("bracket: the callback buffer chains onto the previous list");
        CHECK((helperCalls[0].r3 == g + kPreviousList && helperCalls[0].r4 == tokenBuffer && helperCalls[0].r5 == tokenBuffer + 16));
        UNSCOPED_INFO("bracket: the fence buffer chains behind the callback's token onto the tile list");
        CHECK((helperCalls[1].r3 == g + kTileList && helperCalls[1].r4 == tokenBuffer + 16 && helperCalls[1].r5 == tokenBuffer + 32));
    }
    UNSCOPED_INFO("bracket: the callback token carries its 35 dwords");
    CHECK((rig.device.load(tokenBuffer + 4) == 0x81000023 && rig.device.load(tokenBuffer + 8) == CommandBufferPhysical(callbackBuffer) && rig.device.load(tokenBuffer + 12) == 0xC0000000));
    UNSCOPED_INFO("bracket: the fence token carries its 10 dwords");
    CHECK((rig.device.load(tokenBuffer + 20) == 0x8100000A && rig.device.load(tokenBuffer + 24) == CommandBufferPhysical(fenceBuffer)));
    UNSCOPED_INFO("bracket: N = 1 + 1 and the bracket closes");
    CHECK((rig.device.load(g + kWorkerPending) == 2 && rig.device.load(g + kTiling) == 0 && rig.device.load(g + kTilingNext) == 0));
    auto r1 = takeResidue();
    PROBE_INFO("bracket: both Submits are consumed_deferred: " + r1);
    PROBE_CHECK((field(r1, "or_submit_consumed_deferred") == 2 && field(r1, "or_submit_calls") == 2));
    requireConserved(r1, "bracket");
#if REREVVED_DEVELOPER
    const auto probe = PublishedCompletionProbe().Take().counts;
    UNSCOPED_INFO(std::format("bracket: two Deferred N store pairs adding 1 and 0: calls {} {} {} adds {} pairError {}", probe.addCalls[0], probe.addCalls[1], probe.addCalls[2], probe.adds[0], probe.pairError));
    CHECK((probe.addCalls[0] == 2 && probe.adds[0] == 1 && probe.pairError == 0));
#endif
    const auto counts = TakeRingStubCounts();
    UNSCOPED_INFO("bracket: the stubs serve the bracket");
    CHECK((stubbed(counts, RingStub::TilingEnd) == 1 && stubbed(counts, RingStub::Submit) == 2 && fellBack(counts, RingStub::Submit) == 0 && stubbed(counts, RingStub::Callback) == 1 && stubbed(counts, RingStub::Fence) == 1));

    // The worker-kick callback is the ledger's, and the fence waits for the worker.
    rig.session.sink.handler = [&]
    {
        rig.device.clearCpuBit();
    };
    while (rig.session.ledger->Pump())
    {
    }
    const auto scratch = rig.device.physical(rig.device.scratch);
    const auto block   = rig.device.physical(rig.device.block);
    UNSCOPED_INFO("bracket: the callback and the fence wait for the worker");
    CHECK((rig.session.sink.interrupts.empty() && rig.device.load(rig.device.block) == 0 && rig.session.ledger->Snapshot().oldestRecord.starts_with("awaiting worker submit")));

    // The worker walks the previous list, so the callback buffer is submitted first: the
    // worker-kick callback raises once on CPU 2 and the guest delivers it.
    std::thread([&]
                {
                    rig.session.ledger->MarkReplayThread();
                    rig.session.ledger->ReplaySubmit(CommandBufferPhysical(callbackBuffer), 35, 0x826AB890);
                    rig.session.ledger->Kick(0x73);
                })
        .join();
    while (rig.session.ledger->Pump())
    {
    }
    bool kick = false;
    for (const auto& write : rig.session.sink.writes)
        kick = kick || (write.first == scratch + completion::kScratchCallbackOffset && write.second == kWorkerKick);
    UNSCOPED_INFO("bracket: the worker-kick callback raises once on CPU 2");
    CHECK((kick && rig.session.sink.interrupts == std::vector<std::uint32_t>{ completion::kCallbackCpu }));
    ctx        = rig.context(kCaller);
    ctx.r3.u64 = CommandBufferPhysical(listBase) + 0xC0000000;
    sub_826AC020(ctx, base);
    UNSCOPED_INFO("bracket: the callback releases no fence");
    CHECK(rig.device.load(rig.device.block) == 0);
    rig.session.sink.writes.clear();

    // The worker then replays the tiled segment and the fence buffer; only the fence
    // buffer's submit releases B+4, B+0 and RPTR, in that order.
    std::thread([&]
                {
                    rig.session.ledger->MarkReplayThread();
                    rig.session.ledger->ReplaySubmit(CommandBufferPhysical(rig.at(0)), 12, 0x826AB890);
                    rig.session.ledger->Kick(0x73);
                    rig.session.ledger->ReplaySubmit(CommandBufferPhysical(fenceBuffer), 10, 0x826AB890);
                    rig.session.ledger->Kick(0x73);
                })
        .join();
    while (rig.session.ledger->Pump())
    {
    }
    std::vector<std::uint32_t> seen;
    for (const auto& write : rig.session.sink.writes)
        if (write.first == block + completion::kCursorOffset || write.first == block + completion::kFenceOffset || write.first == block + completion::kReadPointerOffset)
            seen.push_back(write.first);
    UNSCOPED_INFO("bracket: B+4, B+0, then RPTR after the worker submits the fence buffer");
    CHECK(seen == std::vector<std::uint32_t>{ block + completion::kCursorOffset, block + completion::kFenceOffset, block + completion::kReadPointerOffset });
    const auto snapshot = rig.session.ledger->Snapshot();
    UNSCOPED_INFO("bracket: the fence was associated with the deferred span");
    CHECK((!snapshot.pending && snapshot.deferredSpans >= 1));
    UNSCOPED_INFO("bracket: no ledger failure");
    CHECK(rig.session.failed.empty());
}

#endif

// The ledger fault channel carries the named detail.
void consumeFaultReachesTheLedger(rex::memory::Memory& memory)
{
    Device  device(memory);
    Session session(device);
    completion::detail::RecordRingConsumeFault("publish_tail_store", 0x8269C61C);
    UNSCOPED_INFO("a consumed fault reaches the strict ledger with its step");
    CHECK((session.failed.size() == 1 && session.failed[0].find("ring_consume_fault: publish_tail_store") != std::string::npos));
}

// The fixture Originals and guest helpers one run of the route cases installs
// through the shared import doubles, and the state those cases leave behind.
class RouteFixtures
{
public:
    RouteFixtures()
    {
        originals.clear();
    }

    ~RouteFixtures()
    {
        residueBody        = nullptr;
        residueKick        = nullptr;
        setPredicationBody = nullptr;
#if REREVVED_DEVELOPER
        growthStorage = nullptr;
#endif
        decodedInterval = std::nullopt;
        tiling          = {};
#if REREVVED_DEVELOPER
        consumeEvents.clear();
        helperCalls.clear();
        helperWatch.clear();
#endif
        setPredicationCallers.clear();
        originals.clear();
        ringStubsActive.store(false);
    }

    RouteFixtures(const RouteFixtures&)            = delete;
    RouteFixtures& operator=(const RouteFixtures&) = delete;

private:
    rerevved::tests::guest_imports::ScopedFallbackImport fallback_{ [](std::uint32_t address, PPCContext& ctx, std::uint8_t*)
                                                                    {
                                                                        censusOriginal(address, ctx);
                                                                    } };
    rerevved::tests::guest_imports::ScopedImport submit_{ 0x8269BD98, fixtureSub8269BD98 };
    rerevved::tests::guest_imports::ScopedImport begin_{ 0x826AC0A8, fixtureSub826AC0A8 };
    rerevved::tests::guest_imports::ScopedImport allocate_{ 0x8269B818, fixtureSub8269B818 };
    rerevved::tests::guest_imports::ScopedImport terminate_{ 0x8269B508, fixtureSub8269B508 };
#if REREVVED_DEVELOPER
    rerevved::tests::guest_imports::ScopedImport link_{ 0x8269B550, fixtureSub8269B550 };
    rerevved::tests::guest_imports::ScopedImport growth_{ 0x8269C170, fixtureSub8269C170 };
    rerevved::tests::guest_imports::ScopedImport rollover_{ 0x8269C270, fixtureSub8269C270 };
#endif
    rerevved::tests::guest_imports::ScopedImport reset_{ 0x8269B118, fixtureSub8269B118 };
};

// Runs every route case in the old order, or only the named one.
void runRoute(std::string_view selected)
{
    RouteFixtures fixtures;
    rex::Runtime  runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();
    UNSCOPED_INFO("the runtime has guest memory");
    REQUIRE(memory != nullptr);
    auto* image = memory->LookupHeap(kImagePage);
    UNSCOPED_INFO("the device global's page is committed");
    REQUIRE((image && image->AllocFixed(kImagePage, 0x10000, 0x10000, kAllocation, kWritable)));
    auto* base = memory->virtual_membase();
    if (selected == "publishRoutesOriginalOnce")
    {
        publishRoutesOriginalOnce(*memory, base);
        return;
    }
#if REREVVED_DEVELOPER
    if (selected == "residueRawWrapperInvariance")
    {
        residueRawWrapperInvariance(*memory, base);
        return;
    }
#endif
    if (selected == "residueReadinessTrace")
    {
        residueReadinessTrace();
        return;
    }
    UNSCOPED_INFO("an unknown case name");
    REQUIRE(selected.empty());
    // The not-ready device must come first: a ready device is checked once.
    wrapperFallbackRunsOriginalFixtureAndLedgerCallback(*memory, base);
    endTilingFallsBackWhenTheDeviceIsNotReady(*memory, base);
    consumedPreSwapObservesTheDecodedInterval(*memory, base);
    realCensusPreSwapOff(*memory, base);
    realPreSwapConsumeDifferential(*memory, base);
    realCensusPreSwapGuards(*memory, base);
    endTilingOrdersCallbackThenFenceThroughTheLedger(*memory, base);
    endTilingChainsBothBuffersWhileTheWorkerOwnsTheRing(*memory, base);
#if REREVVED_DEVELOPER
    rawTilingRestoreDispatchAndLease(*memory, base);
#endif
    endTilingDropsTheBracketWhenAnAllocationFails(*memory, base);
    endTilingAllocationFailureLeavesTheLedgerEmpty(*memory, base);
    endTilingWorkerSubmitNeverArrives(*memory, base);
#if REREVVED_DEVELOPER
    endTilingObservesTheRecordList(*memory, base);
#endif
    layoutMismatchLeavesTheLedgerTheOnlyWriter(*memory, base);
#if REREVVED_DEVELOPER
    residueRawWrapperInvariance(*memory, base);
#endif
    residueReadinessTrace();
    publishRoutesOriginalOnce(*memory, base);
#if REREVVED_DEVELOPER
    // The cvar-off report first: the ring consume field flag is sticky once a consume ran.
    consumeOffKeepsTheOldReport(*memory, base);
    consumePublishSubmitDirect(*memory, base);
    consumePublishSubmitDeferred(*memory, base);
    consumeSubmitArms(*memory, base);
    consumePublishStarvedAndTiling(*memory, base);
    consumePublishGrowth(*memory, base);
    consumePublishRollover(*memory, base);
    consumeCpuFenceRefusesBeforeMutation(*memory, base);
    consumeWithoutExitChecker(*memory, base);
    // The completion probe tracks eight devices for the whole process: the probe-checked
    // cases run first, the cases that need no probe count after them.
    consumeEndTilingBracket(*memory, base);
    consumeDeferredTokenStoreFails(*memory, base);
    consumeSubmitRefusalReasons(*memory, base);
    consumeEntryChecksTheAllocation(*memory, base);
    consumeFallbacksAndPrerequisites(*memory, base);
#endif
    consumeFaultReachesTheLedger(*memory);
}

} // namespace

TEST_CASE("ring_stub_route", "[ctest][ring-stub-route]")
{
    runRoute({});
}

// Runs one named case:
//   ring-stub-route.case <publishRoutesOriginalOnce | residueRawWrapperInvariance | residueReadinessTrace>
TEST_CASE("ring_stub_route_selected", "[.][manual][ring-stub-route]")
{
    const auto& args = rerevved::tests::CurrentManualArgv();
    REQUIRE(args.argc() == 1);
    runRoute(args[1]);
}

REREVVED_MANUAL_HELPER(ringStubRouteCase, "ring-stub-route.case", "ring_stub_route_selected")
