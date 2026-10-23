#include "gpu/completion/completion_ledger.h"
#if REREVVED_DEVELOPER
#include "gpu/guest_d3d/original_residue_probe.h"
#endif
#include "gpu/guest_d3d/ring_stubs.h"
#include "helper_commands.hpp"

#include <rex/ppc/context.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace rerevved::gpu::guest_d3d;
using namespace rerevved::gpu::guest_d3d::ring_layout;
namespace completion = rerevved::gpu::completion;

using Write = std::pair<std::uint32_t, std::uint32_t>;

// Big-endian guest memory made of mapped regions. Virtual 0x4xxxxxxx maps to
// physical address & 0x1FFFFFFF; every word store is logged in order.
class FakeMemory final : public IRingStubMemory
{
public:
    void Map(std::uint32_t base, std::uint32_t size)
    {
        regions.push_back({ base, std::vector<std::uint8_t>(size) });
    }

    std::optional<std::uint32_t> Read32(std::uint32_t address) override
    {
        reads.emplace_back(4, address);
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < 4; ++i)
        {
            const auto byte = Read8(address + i);
            if (!byte)
                return std::nullopt;
            value = (value << 8) | *byte;
        }
        return value;
    }

    std::optional<std::uint8_t> Read8(std::uint32_t address) override
    {
        reads.emplace_back(1, address);
        auto* byte = find(address);
        return byte ? std::optional<std::uint8_t>(*byte) : std::nullopt;
    }

    std::vector<std::pair<unsigned, std::uint32_t>> reads;

    bool Write32(std::uint32_t address, std::uint32_t value) override
    {
        for (std::uint32_t i = 0; i < 4; ++i)
            if (!find(address + i))
                return false;
        for (std::uint32_t i = 0; i < 4; ++i)
            *find(address + i) = static_cast<std::uint8_t>(value >> (24 - 8 * i));
        log.emplace_back(address, value);
        return true;
    }

    bool Write8(std::uint32_t address, std::uint8_t value) override
    {
        auto* byte = find(address);
        if (!byte)
            return false;
        *byte = value;
        log.emplace_back(address, value);
        return true;
    }

    std::optional<std::uint64_t> Exchange64(std::uint32_t address, std::uint64_t value) override
    {
        const auto high = Read32(address);
        const auto low  = Read32(address + 4);
        if ((address & 7) || !high || !low)
            return std::nullopt;
        Write32(address, static_cast<std::uint32_t>(value >> 32));
        Write32(address + 4, static_cast<std::uint32_t>(value));
        return (std::uint64_t(*high) << 32) | *low;
    }

    std::optional<std::uint32_t> Physical(std::uint32_t address) override
    {
        return find(address) ? std::optional<std::uint32_t>(address & 0x1FFFFFFFU) : std::nullopt;
    }

    // A mapped word that cannot be read, or can be read but not written.
    void Hole(std::uint32_t address)
    {
        holes.push_back(address);
    }

    void ReadOnly(std::uint32_t address)
    {
        readOnly.push_back(address);
    }

    bool Writable32(std::uint32_t address) override
    {
        for (std::uint32_t i = 0; i < 4; ++i)
            if (!find(address + i) || std::count(readOnly.begin(), readOnly.end(), address) != 0)
                return false;
        return true;
    }

    std::optional<RingAllocation> Allocation(std::uint32_t address) override
    {
        for (const auto& region : regions)
            if (address >= region.base && address - region.base < region.bytes.size())
                return RingAllocation{ region.base, static_cast<std::uint32_t>(region.bytes.size()) };
        return std::nullopt;
    }

    std::uint32_t Word(std::uint32_t address)
    {
        return Read32(address).value_or(0xDEADDEAD);
    }

    std::uint8_t Byte(std::uint32_t address)
    {
        return Read8(address).value_or(0xEE);
    }

    std::vector<Write> log;

private:
    struct Region
    {
        std::uint32_t             base;
        std::vector<std::uint8_t> bytes;
    };

    std::uint8_t* find(std::uint32_t address)
    {
        for (const auto hole : holes)
            if (address >= hole && address - hole < 4)
                return nullptr;
        for (auto& region : regions)
            if (address >= region.base && address - region.base < region.bytes.size())
                return &region.bytes[address - region.base];
        return nullptr;
    }

    std::vector<Region>        regions;
    std::vector<std::uint32_t> holes, readOnly;
};

constexpr std::uint32_t kG       = 0x40001000;
constexpr std::uint32_t kB       = 0x40010000;
constexpr std::uint32_t kS2      = 0x40011000;
constexpr std::uint32_t kSide    = 0x40012000; // an EndTiling side buffer
constexpr std::uint32_t kCell    = 0x40020000; // *0x820009F0 points here; it holds G
constexpr std::uint32_t kFenceF  = 0x11;
constexpr std::uint32_t kMask    = 0x3FFF;
constexpr std::uint32_t kStartAt = 0x2000;
constexpr std::uint32_t kCaller  = 0x82303E40;

constexpr std::uint32_t physical(std::uint32_t address)
{
    return address & 0x1FFFFFFFU;
}

// A device as 0x8269C8E0 leaves it.
std::unique_ptr<FakeMemory> makeDevice()
{
    auto memory = std::make_unique<FakeMemory>();
    memory->Map(kG, kDeviceEnd);
    memory->Map(kB, 0x100);
    memory->Map(kS2, 0x100);
    memory->Map(kSide, 0x100);
    memory->Map(kCell, 0x10);
    memory->Map(0x82000000, 0x1000);
    memory->Map(0x82F9B000, 0x100);
    memory->Write32(kDeviceGlobal, kCell);
    memory->Write32(kCell, kG);
    memory->Write32(kG + kProgressBlock, kB);
    memory->Write32(kG + kScratchBlock, kS2);
    memory->Write32(kG + kRingMask, kMask);
    memory->Write32(kG + kNextFence, kFenceF);
    memory->Write32(kG + kCursor, kStartAt);
    memory->Write32(kG + kCursorLimit, 0x9000);
    memory->Write32(kG + kFenceLow, 0x7);
    memory->Write32(kG + kRingWriteIndex, 0x40);
    memory->Write32(kG + kCoherencyRange, 0);
    memory->Write32(kG + kCoherencyRange + 4, 0xFFFFFFFF);
    memory->log.clear();
    return memory;
}

// The ledger's sink over the fake guest block: physical stores land in the
// same bytes the stubs and the guest read.
class Sink final : public completion::IGuestProgressSink
{
public:
    explicit Sink(FakeMemory& memory)
    : memory(memory)
    {
    }

    bool WritePhysical(std::uint32_t address, std::uint32_t value) override
    {
        writes.emplace_back(address, value);
        return memory.Write32(address | 0x40000000U, value);
    }

    std::optional<std::uint32_t> ReadPhysical(std::uint32_t address) override
    {
        return memory.Read32(address | 0x40000000U);
    }

    completion::InterruptResult RaiseCallbackInterrupt(std::uint32_t cpu) override
    {
        interrupts.emplace_back(cpu, writes.size());
        if (handler)
            handler();
        return completion::InterruptResult::Raised;
    }

    FakeMemory&           memory;
    std::vector<Write>    writes;
    std::function<void()> handler; // the guest's source-1 handler
    // The CPU of each interrupt and how many stores preceded it.
    std::vector<std::pair<std::uint32_t, std::size_t>> interrupts;
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
        return completed;
    }

    std::uint64_t next = 0, completed = 0;
};

// The stubs' guest block with a ledger on top, as GuestVideoServices runs it.
struct Harness
{
    std::unique_ptr<FakeMemory>  memory = makeDevice();
    Sink                         sink{ *memory };
    Draws                        draws;
    std::vector<std::string>     failed;
    completion::CompletionLedger ledger{ completion::LedgerConfig{ NativeDrawMode::Strict, &sink, &draws, [this](const completion::LedgerFailure& failure)
                                                                   {
                                                                       failed.push_back(failure.detail);
                                                                   } } };
    static constexpr std::uint32_t kRptr = physical(kB) + completion::kReadPointerOffset;

    Harness()
    {
        ledger.BeginRingEpoch();
        ledger.RegisterWriteback(kRptr);
    }

    // What the progress observer records at 0x8269BCA8's entry, before the stub.
    void observeFence()
    {
        ledger.InvalidateState(0x8269BCA8);
        ledger.Fence(physical(kB), memory->Word(kG + kNextFence), memory->Word(kG + kCursor) | (memory->Word(kG + kFenceLow) & 3));
    }

    // What the progress observer records at 0x8269BF40's entry, before the stub.
    void observeCallback(std::uint32_t function, std::uint32_t parameter, std::uint32_t flags, std::optional<std::uint32_t> side = std::nullopt)
    {
        ledger.Callback(physical(kS2), function, parameter, flags, std::nullopt, side);
    }

    // The handler 0x8269B450 clears CPU 2's bit in S2+0.
    void clearCpuBit()
    {
        memory->Write32(kS2 + completion::kScratchMaskOffset, memory->Word(kS2 + completion::kScratchMaskOffset) & ~(1U << kCallbackCpu));
    }

    static constexpr std::uint32_t kCallbackCpu = completion::kCallbackCpu;
};

void layoutCheckClassifiesDevices()
{
    auto memory = makeDevice();
    UNSCOPED_INFO("a seeded device passes");
    CHECK(CheckRingLayout(*memory, kG).verdict == RingLayout::Ok);
    UNSCOPED_INFO("the layout check only reads");
    CHECK(memory->log.empty());

    memory->Write32(kCell, 0);
    UNSCOPED_INFO("an unpublished device global is not ready");
    CHECK(CheckRingLayout(*memory, kG).verdict == RingLayout::NotReady);
    memory->Write32(kCell, kG + 0x100);
    UNSCOPED_INFO("a device global naming another device mismatches");
    CHECK(CheckRingLayout(*memory, kG).verdict == RingLayout::Mismatch);
    memory->Write32(kCell, kG);

    memory->Write32(kG + kProgressBlock, 0);
    UNSCOPED_INFO("an unallocated progress block is not ready");
    CHECK(CheckRingLayout(*memory, kG).verdict == RingLayout::NotReady);
    memory->Write32(kG + kProgressBlock, 0x50000000);
    UNSCOPED_INFO("an unmapped progress block mismatches");
    CHECK(CheckRingLayout(*memory, kG).verdict == RingLayout::Mismatch);
    memory->Write32(kG + kProgressBlock, kB);

    memory->Write32(kG + kRingMask, 0x3FFE);
    UNSCOPED_INFO("a ring mask that is not 2^n - 1 mismatches");
    CHECK(CheckRingLayout(*memory, kG).verdict == RingLayout::Mismatch);
    memory->Write32(kG + kRingMask, kMask);

    memory->Write32(kG + kNextFence, 0x12);
    UNSCOPED_INFO("an even next fence mismatches");
    CHECK(CheckRingLayout(*memory, kG).verdict == RingLayout::Mismatch);
    memory->Write32(kG + kNextFence, kFenceF);

    UNSCOPED_INFO("an unmapped device mismatches");
    CHECK(CheckRingLayout(*memory, 0x50000000).verdict == RingLayout::Mismatch);
    UNSCOPED_INFO("the restored device passes again");
    CHECK(CheckRingLayout(*memory, kG).verdict == RingLayout::Ok);
}

void fenceFieldsLeaveTheBlockToTheLedger()
{
    auto memory = makeDevice();
    UNSCOPED_INFO("the fence fields apply");
    CHECK(ApplyFenceFields(*memory, kG));
    const std::vector<Write> cpu{ { kG + kFenceCursorCopy, kStartAt }, { kG + kFenceLowCopy, 0x7 }, { kG + kNextFence, kFenceF + 2 } };
    UNSCOPED_INFO("the emitter's CPU stores in order: G+0x33B8, G+0x33BC, then F + 2");
    CHECK(memory->log == cpu);
    UNSCOPED_INFO("B is not written outside CPU fence mode");
    CHECK((memory->Word(kB + kFenceOffset) == 0 && memory->Word(kB + kFenceCursorOffset) == 0));

    auto cpuMode = makeDevice();
    cpuMode->Write8(kG + kModeHigh, kModeCpuFence);
    cpuMode->log.clear();
    UNSCOPED_INFO("CPU fence mode applies");
    CHECK(ApplyFenceFields(*cpuMode, kG));
    const std::vector<Write> cpuFence{
        { kG + kFenceCursorCopy, kStartAt },
        { kG + kFenceLowCopy, 0x7 },
        { kB + kFenceOffset, kFenceF },
        { kB + kFenceCursorOffset, kStartAt | 3 },
        { kG + kNextFence, kFenceF + 2 },
    };
    UNSCOPED_INFO("CPU fence mode stores B+0, then B+4, before F advances, as the original does");
    CHECK(cpuMode->log == cpuFence);

    memory->Write32(kG + kProgressBlock, 0x50000000);
    memory->log.clear();
    UNSCOPED_INFO("an unmapped progress block writes nothing");
    CHECK((!ApplyFenceFields(*memory, kG) && memory->log.empty()));
}

// P1-2: the stubbed fence publishes B only through the ledger's release.
void ledgerReleasesTheStubbedFence()
{
    Harness h;
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    h.ledger.FinishDraw(1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    h.observeFence();
    UNSCOPED_INFO("the stub applies the fence fields");
    CHECK(ApplyFenceFields(*h.memory, kG));
    UNSCOPED_INFO("F advances at once, as the emitter's CPU store does");
    CHECK(h.memory->Word(kG + kNextFence) == kFenceF + 2);
    h.ledger.Kick(h.memory->Word(kG + kRingWriteIndex));
    h.ledger.Pump();
    UNSCOPED_INFO("B+0 holds while the draw before the fence has not completed");
    CHECK((h.sink.writes.empty() && h.memory->Word(kB + kFenceOffset) == 0));

    h.draws.completed = 1;
    UNSCOPED_INFO("the span releases once its host work completes");
    CHECK(h.ledger.Pump());
    const std::vector<Write> expected{
        { physical(kB) + completion::kCursorOffset, kStartAt | 3 },
        { physical(kB) + completion::kFenceOffset, kFenceF },
        { Harness::kRptr, 0x40 },
    };
    UNSCOPED_INFO("release stores B+4, then B+0, then RPTR");
    CHECK(h.sink.writes == expected);
    UNSCOPED_INFO("the guest sees fence F in B+0");
    CHECK(h.memory->Word(kB + kFenceOffset) == kFenceF);
    UNSCOPED_INFO("the stubbed fence is no ledger failure");
    CHECK(h.failed.empty());
}

// P2-3: the stubbed callback's S2 stores, CPU-2 dispatch and retirement.
void ledgerRunsTheStubbedCallback()
{
    Harness             h;
    const std::uint32_t function = 0x826AC020, parameter = 0x1234;
    h.observeCallback(function, parameter, 0);
    UNSCOPED_INFO("the stub returns the cursor past 37 dwords");
    CHECK(CallbackCursor(*h.memory, kG, 0x3000, 0) == 0x3000 + 148);
    UNSCOPED_INFO("the callback stub writes nothing");
    CHECK(h.memory->log.empty());

    bool seenAtInterrupt = false;
    h.sink.handler       = [&]
    {
        seenAtInterrupt = h.memory->Word(kS2 + completion::kScratchCallbackOffset) == function &&
                          h.memory->Word(kS2 + completion::kScratchParameterOffset) == parameter &&
                          h.memory->Word(kS2 + completion::kScratchMaskOffset) == completion::kCallbackMask;
        h.clearCpuBit();
    };
    h.ledger.Kick(0x40);
    UNSCOPED_INFO("the callback span releases");
    CHECK(h.ledger.Pump());
    UNSCOPED_INFO("S2+0x10, S2+0x14 and S2+0 hold the callback when the handler runs");
    CHECK(seenAtInterrupt);
    UNSCOPED_INFO("one source-1 interrupt on CPU 2, after the three slot stores");
    CHECK((h.sink.interrupts.size() == 1 && h.sink.interrupts[0].first == completion::kCallbackCpu && h.sink.interrupts[0].second == 3));
    const std::vector<Write> expected{
        { physical(kS2) + completion::kScratchCallbackOffset, function },
        { physical(kS2) + completion::kScratchParameterOffset, parameter },
        { physical(kS2) + completion::kScratchMaskOffset, completion::kCallbackMask },
        { physical(kS2) + completion::kScratchCallbackOffset, completion::kRetiredCallback },
        { Harness::kRptr, 0x40 },
    };
    UNSCOPED_INFO("the slot is retired with 0x0BADF00D once S2+0 clears, before RPTR");
    CHECK(h.sink.writes == expected);
    UNSCOPED_INFO("the guest sees the retired slot");
    CHECK(h.memory->Word(kS2 + completion::kScratchCallbackOffset) == 0x0BADF00D);
}

// P1-3: Fallback retains the callback already observed by the ledger.
void fallbackStillProducesTheCallback()
{
    Harness h;
    h.observeCallback(0x826A8BE8, 0x55, 0);
    h.memory->Write32(kCell, 0);
    h.memory->log.clear();
    UNSCOPED_INFO("an unreadable emitter state falls back to Original");
    CHECK(!CallbackCursor(*h.memory, kG, 0x3000, 0));
    h.memory->Write32(kCell, kG);
    h.sink.handler = [&]
    {
        h.clearCpuBit();
    };
    h.ledger.Kick(0x40);
    UNSCOPED_INFO("the fallback's callback is raised once");
    CHECK((h.ledger.Pump() && h.sink.interrupts.size() == 1));
    UNSCOPED_INFO("the fallback's callback writes and retires its slot");
    CHECK((h.memory->Word(kS2 + completion::kScratchParameterOffset) == 0x55 && h.memory->Word(kS2 + completion::kScratchCallbackOffset) == 0x0BADF00D));
}

// The guest's flip callback satisfies the pre-swap ledger's retirement wait.
void flipWaitsAreSatisfied()
{
    Harness             h;
    const std::uint32_t flip = physical(kS2) + completion::kScratchFlipOffset;
    h.ledger.ScratchWrite(flip, 1, 0x826A419C);
    h.ledger.GpuWait(flip, 1, 0x826A419C);
    h.observeCallback(kFlipCallback, 0x115, 0);
    h.ledger.GpuWait(flip, 0, 0x826A42BC);
    // 0x826A3FB8 and the vblank dequeue retire the flip by clearing S2+4.
    h.sink.handler = [&]
    {
        h.memory->Write32(kS2 + completion::kScratchFlipOffset, 0);
        h.clearCpuBit();
    };
    h.ledger.Kick(0x40);
    for (int i = 0; i < 4; ++i)
        h.ledger.Pump();
    UNSCOPED_INFO("the flip span releases");
    CHECK((h.sink.interrupts.size() == 1 && h.ledger.Snapshot().spansReleased == 1 && !h.ledger.Snapshot().pending));
    UNSCOPED_INFO("the flip request is retired");
    CHECK(h.memory->Word(kS2 + completion::kScratchFlipOffset) == 0);
    UNSCOPED_INFO("SCRATCH_REG1 = 1 is stored first");
    CHECK((!h.sink.writes.empty() && h.sink.writes.front() == Write(flip, 1)));
    UNSCOPED_INFO("the flip is no ledger failure");
    CHECK(h.failed.empty());
}

// EndTiling joins submit's side buffer; callback wakes G+0x2B00's owner.
void workerKickThroughDirectSubmit()
{
    Harness h;
    h.observeCallback(0x826AC020, 0x77, 0x04000007, physical(kSide));
    UNSCOPED_INFO("the submit publishes through a kick");
    CHECK(ClassifyPublisher(*h.memory, kG) == PublisherPath::Kick);
    UNSCOPED_INFO("the direct submit raises G+0x2B00 by r7");
    CHECK((RaiseForDirectSubmit(*h.memory, kG, 1) && h.memory->Word(kG + kWorkerPending) == 1));
    h.ledger.DirectSubmit(physical(kSide), 0x20);
    h.ledger.Kick(0x40);
    h.sink.handler = [&]
    {
        // The worker kick runs the tiles and releases the ring.
        h.memory->Write32(kG + kWorkerPending, 0);
        h.clearCpuBit();
    };
    UNSCOPED_INFO("the worker-kick callback is raised once");
    CHECK((h.ledger.Pump() && h.sink.interrupts.size() == 1));
    UNSCOPED_INFO("the drain's wait for G+0x2B00 ends");
    CHECK(h.memory->Word(kG + kWorkerPending) == 0);
    UNSCOPED_INFO("the side buffer's callback is attributed");
    CHECK(h.failed.empty());
    UNSCOPED_INFO("an idle worker submits directly without a raise");
    CHECK((RaiseForDirectSubmit(*h.memory, kG, 0) && h.memory->Word(kG + kWorkerPending) == 0));
    h.memory->Write32(kG + kWorkerPending, 1);
    UNSCOPED_INFO("a busy worker defers to Original");
    CHECK(!RaiseForDirectSubmit(*h.memory, kG, 0));
}

// P2-1: Stub failure leaves the ledger as sole writer of pending callbacks.
void recoveryKeepsOneWriter()
{
    class Services final : public IRingStubServices
    {
    public:
        void Kick(std::uint32_t) override
        {
            ++kicks;
        }

        int kicks = 0;
    };

    Harness h;
    auto    services = std::make_shared<Services>();
    PublishRingStubServices(services);
    h.observeCallback(0x826A8BE8, 0x66, 0);
    DisableRingStubs("test mismatch");
    DisableRingStubs("second mismatch");
    UNSCOPED_INFO("a mismatch stops the stubs");
    CHECK(!ringStubsActive.load());
    h.sink.handler = [&]
    {
        h.clearCpuBit();
    };
    h.ledger.Kick(0x40);
    UNSCOPED_INFO("the pending callback is raised once after recovery");
    CHECK((h.ledger.Pump() && h.sink.interrupts.size() == 1));
    std::size_t slotStores = 0;
    for (const auto& [address, value] : h.sink.writes)
        slotStores += address == physical(kS2) + completion::kScratchCallbackOffset ? 1 : 0;
    UNSCOPED_INFO("one slot store and one retirement, from the ledger alone");
    CHECK(slotStores == 2);
    WithdrawRingStubServices();
}

void helpersMirrorTheOriginals()
{
    auto memory = makeDevice();
    UNSCOPED_INFO("the ring wait returns the masked end");
    CHECK(RingSpaceWait(*memory, kG, 0x3FF0, 0x20) == 0x10);

    UNSCOPED_INFO("the publication fences and kicks");
    CHECK(ClassifyPublish(*memory, kG) == PublishBranch::Submit);
    memory->Write8(kG + kModeHigh, kModeRingStarved);
    UNSCOPED_INFO("a starved ring takes the tail");
    CHECK(ClassifyPublish(*memory, kG) == PublishBranch::Starved);
    memory->Write8(kG + kModeHigh, 0);
    memory->Write8(kG + kModeLow, kModeLowRecording);
    UNSCOPED_INFO("recording takes the command buffer list");
    CHECK(ClassifyPublish(*memory, kG) == PublishBranch::Recording);
    memory->Write8(kG + kModeLow, 0);
    memory->Write32(kG + kTiling, 1);
    UNSCOPED_INFO("tiling takes the segment list");
    CHECK(ClassifyPublish(*memory, kG) == PublishBranch::Tiling);
    UNSCOPED_INFO("the flush skips coherency while tiling");
    CHECK(!TakeCoherency(*memory, kG));
    memory->Write32(kG + kTiling, 0);

    auto none = TakeCoherency(*memory, kG);
    UNSCOPED_INFO("an empty range emits no barrier");
    CHECK((none && !none->range && !none->extra));
    memory->Write32(kG + kCoherencyRange, 0x00100000);
    memory->Write32(kG + kCoherencyRange + 4, 0x00200000);
    memory->Write8(kG + kModeExtra, kModeExtraCoherent);
    auto both = TakeCoherency(*memory, kG);
    UNSCOPED_INFO("a pending range and the extra bit emit both barriers");
    CHECK((both && both->range && both->extra));
    UNSCOPED_INFO("the range is swapped empty");
    CHECK((memory->Word(kG + kCoherencyRange) == 0 && memory->Word(kG + kCoherencyRange + 4) == 0xFFFFFFFF));

    UNSCOPED_INFO("no CPU fence switch while 0x82F9B0A4 is zero");
    CHECK(!CpuFenceSwitch(*memory, kG));
    memory->Write32(kCpuFenceGlobal, 1);
    UNSCOPED_INFO("the switch waits for F - 2");
    CHECK(CpuFenceSwitch(*memory, kG) == kFenceF - 2);
    UNSCOPED_INFO("then sets CPU fence mode");
    CHECK((SetCpuFenceMode(*memory, kG) && memory->Byte(kG + kModeHigh) == kModeCpuFence));
    UNSCOPED_INFO("the switch happens once");
    CHECK(!CpuFenceSwitch(*memory, kG));
    UNSCOPED_INFO("CPU fence mode publishes nothing without a sink");
    CHECK(ClassifyPublisher(*memory, kG) == PublisherPath::Silent);
    memory->Write8(kG + kModeHigh, 0);
    memory->Write32(kG + kRingCallback, 0x82000100);
    UNSCOPED_INFO("a ring callback runs the publisher as Original");
    CHECK(ClassifyPublisher(*memory, kG) == PublisherPath::Original);
    UNSCOPED_INFO("a ring callback runs the direct writer as Original");
    CHECK(!DirectWriterIsStubbed(*memory, kG, 0x826AB3B0));
    memory->Write32(kG + kRingCallback, 0);
    UNSCOPED_INFO("the replay path's direct writer is stubbed");
    CHECK(DirectWriterIsStubbed(*memory, kG, 0x826AB3B0));
    UNSCOPED_INFO("ring initialization keeps the direct writer");
    CHECK(!DirectWriterIsStubbed(*memory, kG, kRingInitWriterReturn));

    UNSCOPED_INFO("the cursor is within the limit");
    CHECK(CursorPastLimit(*memory, kG) == false);
    memory->Write32(kG + kCursor, 0x9004);
    UNSCOPED_INFO("a cursor past the limit flushes first");
    CHECK(CursorPastLimit(*memory, kG) == true);
}

void diagnosticOutputsPreserveHelperOperations()
{
    for (bool cpu : { false, true })
        for (std::uint32_t pending : { 0U, 7U })
            for (std::uint32_t raise : { 0U, 3U })
            {
                auto plain    = makeDevice();
                auto observed = makeDevice();
                for (auto* memory : { plain.get(), observed.get() })
                {
                    memory->Write8(kG + kModeHigh, cpu ? 2 : 0);
                    memory->Write32(kG + kRingCallback, cpu ? 0x82601234 : 0);
                    memory->Write32(kG + kWorkerPending, pending);
                    memory->log.clear();
                    memory->reads.clear();
                }
                PublishDiagnostic   publish;
                PublisherDiagnostic publisher;
                RaiseDiagnostic     raised;
                UNSCOPED_INFO("Publish diagnostic preserves result and valid zeros");
                CHECK((ClassifyPublish(*plain, kG) == ClassifyPublish(*observed, kG, &publish) && publish.valid == 7 && publish.branch == PublishBranch::Submit));
                UNSCOPED_INFO("Publisher diagnostic preserves CPU callback precedence");
                CHECK((ClassifyPublisher(*plain, kG) == ClassifyPublisher(*observed, kG, &publisher) && publisher.valid == 7 && publisher.path == (cpu ? PublisherPath::Silent : PublisherPath::Kick)));
                const bool expected = pending == 0;
                UNSCOPED_INFO("Raise diagnostic preserves served/refused result");
                CHECK((RaiseForDirectSubmit(*plain, kG, raise) == expected && RaiseForDirectSubmit(*observed, kG, raise, &raised) == expected));
                UNSCOPED_INFO("Raise diagnostic describes the actual single store");
                CHECK((raised.pendingValid && raised.pending == pending && raised.attemptedWrite == (pending == 0 && raise != 0) && raised.writeSucceeded == raised.attemptedWrite));
                UNSCOPED_INFO("null/present diagnostic has identical read order and store attempts");
                CHECK((plain->reads == observed->reads && plain->log == observed->log && plain->log.size() == (pending == 0 && raise != 0 ? 1U : 0U)));
            }
}

// EndTiling helpers against the generated 0x826AC218.
void endTilingHelpersMirrorTheOriginal()
{
    auto memory = makeDevice();
    memory->Write32(kG + kTiling, 1);
    memory->Write32(kG + kCoherencyRange, 0x00100000);
    memory->Write32(kG + kCoherencyRange + 4, 0x00200000);
    UNSCOPED_INFO("the flush still skips coherency while tiling");
    CHECK(!TakeCoherency(*memory, kG));
    auto range = SwapCoherency(*memory, kG);
    UNSCOPED_INFO("EndTiling swaps the pending range while tiling");
    CHECK((range && range->range && !range->extra));
    UNSCOPED_INFO("the range is swapped empty");
    CHECK((memory->Word(kG + kCoherencyRange) == 0 && memory->Word(kG + kCoherencyRange + 4) == 0xFFFFFFFF));

    UNSCOPED_INFO("no tiled list passes 0");
    CHECK(EndTilingCallbackParameter(*memory, kG) == 0U);
    memory->Write32(kG + kTileList, 0xE0010000);
    UNSCOPED_INFO("the list head passes its guest translation plus 0xC0000000");
    CHECK(EndTilingCallbackParameter(*memory, kG) == 0xC0011000U);

    UNSCOPED_INFO("the callback buffer is submitted outside CPU fence mode");
    CHECK(EndTilingSubmitsCallback(*memory, kG) == true);
    memory->Write8(kG + kModeHigh, kModeCpuFence);
    UNSCOPED_INFO("CPU fence mode without a sink skips it");
    CHECK(EndTilingSubmitsCallback(*memory, kG) == false);
    memory->Write32(kG + kCpuFenceSink, 0x82000100);
    UNSCOPED_INFO("a sink submits it in CPU fence mode");
    CHECK(EndTilingSubmitsCallback(*memory, kG) == true);

    memory->Write32(kG + kTilingNext, 1);
    memory->log.clear();
    UNSCOPED_INFO("EndTiling clears G+0x33B0 and G+0x33B4 and nothing else");
    CHECK((EndTilingState(*memory, kG) && memory->Word(kG + kTiling) == 0 && memory->Word(kG + kTilingNext) == 0 && memory->log.size() == 2));
}

static_assert(CommandBufferPhysical(0xE0010000) == 0x00011000 && CommandBufferPhysical(0xA0010000) == 0x00010000 &&
                  CommandBufferPhysical(0x40012000) == 0x00012000,
              "the 0xE0000000 view is offset by 0x1000");
static_assert(CallbackEmitterDwords(NormalizeCallbackFlags(0x04000006 | 1)) == 35 && 35 <= kEndTilingCallbackDwords,
              "EndTiling's callback writes 35 dwords into its 148-dword reservation");

static_assert(CallbackEmitterDwords(6) == 37, "default flags write 37 dwords, 148 bytes");
static_assert(CallbackEmitterDwords(2) == 29 && CallbackEmitterDwords(3) == 27, "interrupt with and without the idle wait");
static_assert(CallbackEmitterDwords(4) == 8 && CallbackEmitterDwords(7) == 35, "retire wait alone and with all bits");
static_assert(NormalizeCallbackFlags(0) == 6 && NormalizeCallbackFlags(1) == 7 && NormalizeCallbackFlags(2) == 2, "flag normalization");
static_assert(RingStubEntry(0x8269C588) && RingStubEntry(0x8269B2A0) && RingStubEntry(0x826AC020) && !RingStubEntry(0x8269C400) &&
                  !RingStubEntry(0x826A4638),
              "route wrappers that dispatch stubs");

void preSwapPlansTheFlip()
{
    auto memory = makeDevice();
    memory->Write32(kG + kSwapFlags, 0x15U << 23);
    memory->Write32(kG + kSwapInterval, 1);
    auto plan = PlanPreSwap(*memory, kG, 0);
    UNSCOPED_INFO("interval 1 is code 1 with the swap flag bits");
    CHECK((plan && plan->parameter == 0x115 && plan->interval == 1));
    memory->Write32(kG + kSwapInterval, 2);
    plan = PlanPreSwap(*memory, kG, 0x40050000);
    UNSCOPED_INFO("interval 2 is code 2 joined with the flip address");
    CHECK((plan && plan->parameter == (0x40050000 | 0x215)));
    memory->Write32(kG + kSwapInterval, 4);
    plan = PlanPreSwap(*memory, kG, 0);
    UNSCOPED_INFO("interval 4 is code 3");
    CHECK((plan && plan->parameter == 0x315 && plan->interval == 3));
    memory->Write32(kG + kSwapInterval, 0x80000000);
    plan = PlanPreSwap(*memory, kG, 0);
    UNSCOPED_INFO("the immediate interval is code 0");
    CHECK((plan && plan->parameter == 0x015 && plan->interval == 0));
    std::string refusal;
    memory->Write32(kG + kSwapInterval, 3);
    UNSCOPED_INFO("an unknown interval is refused");
    CHECK((!PlanPreSwap(*memory, kG, 0, &refusal) && !refusal.empty()));
    memory->Write32(kG + kSwapInterval, 1);
    memory->Write32(kG + kTimingMode, 2);
    UNSCOPED_INFO("timestamp mode 2 is refused");
    CHECK(!PlanPreSwap(*memory, kG, 0));
    memory->log.clear();
    UNSCOPED_INFO("only the pre-swap mode bit is written");
    CHECK((FinishPreSwap(*memory, kG) && memory->Byte(kG + kModeHigh) == kModePreSwapDone && memory->log.size() == 1));
}

void countsFormatTimingFields()
{
    (void)TakeRingStubCounts();
    CountRingStub(RingStub::Fence, true);
    CountRingStub(RingStub::Fence, true);
    CountRingStub(RingStub::DirectWriter, false);
    CountRingStub(RingStub::RingWait, true);
    CountRingStub(RingStub::TilingEnd, true);
    CountRingStub(RingStub::TilingTokens, false);
    const auto fields = FormatRingStubCounts(TakeRingStubCounts());
    UNSCOPED_INFO("timing fields: " + fields);
    CHECK(fields == " stub_direct_writer_original=1 stub_ring_wait=1 stub_fence=2 tiling_end=1 tiling_tokens_original=1");
    UNSCOPED_INFO("a take resets the counts");
    CHECK(FormatRingStubCounts(TakeRingStubCounts()).empty());
}

void servicesGateTheStubs()
{
    class Services final : public IRingStubServices
    {
    public:
        void Kick(std::uint32_t) override
        {
        }
    };

    // The linked ring stub hooks register the runner at startup, so the
    // unregistered state is checked in a child that clears it first.
    UNSCOPED_INFO("without a registered runner nothing is stubbed");
    CHECK(rerevved::tests::RunHelperChild("ring-stubs.no-runner") == 0);
    UNSCOPED_INFO("native_ring_stubs defaults to on");
    CHECK(RingStubsRequested());
    UNSCOPED_INFO("nothing is published before the services start");
    CHECK(!RingStubsPublished());
    auto services = std::make_shared<Services>();
    PublishRingStubServices(services);
    UNSCOPED_INFO("publication activates the stubs");
    CHECK((ringStubsActive.load() && RingStubsPublished() && CurrentRingStubServices() == services));
    DisableRingStubs("test mismatch");
    UNSCOPED_INFO("a mismatch withdraws the stubs from the consumes too");
    CHECK(!RingStubsPublished());
    WithdrawRingStubServices();
    UNSCOPED_INFO("withdrawal clears the services");
    CHECK((!ringStubsActive.load() && !RingStubsPublished() && !CurrentRingStubServices()));
}

#if REREVVED_DEVELOPER

// native_ring_consume_publish: the planners and ordered stores, against values worked
// out by hand from the generated instructions of 0x8269C400 and 0x8269BD98.
constexpr std::uint32_t kRing = 0x40030000; // command buffer dwords
constexpr std::uint32_t kList = 0x40040000; // tiled segment list storage

std::unique_ptr<FakeMemory> makeConsumeDevice()
{
    auto memory = makeDevice();
    memory->Map(kRing, 0x2000);
    memory->Map(kList, 0x1000);
    memory->Map(0xE0000000, 0x1000);
    return memory;
}

void setPublishFields(FakeMemory& memory, std::uint32_t cursor, std::uint32_t start)
{
    memory.Write32(kG + kCursor, cursor);
    memory.Write32(kG + kTailStart, start);
    memory.log.clear();
    memory.reads.clear();
}

bool readAddress(const FakeMemory& memory, std::uint32_t address)
{
    return std::any_of(memory.reads.begin(),
                       memory.reads.end(),
                       [&](const auto& read)
                       {
                           return read.second == address;
                       });
}

void publishPlanSubmitAndStarved()
{
    auto memory = makeConsumeDevice();
    setPublishFields(*memory, kRing + 0x108, kRing + 0x100);
    PublishConsumePlan plan;
    UNSCOPED_INFO("submit plan: no refusal");
    CHECK(PlanPublishConsume(*memory, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::None);
    UNSCOPED_INFO("submit plan: C, S, R = C + 4 and A(S)");
    CHECK((plan.cursor == kRing + 0x108 && plan.start == kRing + 0x100 && plan.next == std::uint64_t{ kRing } + 0x10C && plan.physical == 0x30100));
    UNSCOPED_INFO("submit plan: reads no list word");
    CHECK((!readAddress(*memory, kG + kListEnd) && !readAddress(*memory, kG + kListLimit)));
    UNSCOPED_INFO("planning writes nothing");
    CHECK(memory->log.empty());
    // The 0xE0000000 view adds the remap page: A = 0x100 + 0x1000.
    setPublishFields(*memory, 0xE0000108, 0xE0000100);
    UNSCOPED_INFO("starved plan: A(S) includes the view adjustment");
    CHECK((PlanPublishConsume(*memory, kG, PublishBranch::Starved, plan) == RingConsumeRefusal::None && plan.physical == 0x1100));
}

void publishPlanTilingCounts()
{
    auto               memory = makeConsumeDevice();
    PublishConsumePlan plan;
    // q = S32(U32(R - S)) >> 2: R - S = 0x30, q = 12.
    setPublishFields(*memory, kRing + 0x2C, kRing);
    memory->Write32(kG + kListEnd, kList);
    memory->Write32(kG + kListLimit, kList + 0x100);
    memory->reads.clear();
    UNSCOPED_INFO("tiling plan: q = 12 with the list words");
    CHECK((PlanPublishConsume(*memory, kG, PublishBranch::Tiling, plan) == RingConsumeRefusal::None && plan.count == 12 && plan.listEnd == kList && plan.listLimit == kList + 0x100 && !plan.growth));
    // Negative q is an arithmetic shift: R - S = -12, q = -3.
    setPublishFields(*memory, kRing + 0x100, kRing + 0x110);
    UNSCOPED_INFO("tiling plan: negative q is an arithmetic shift");
    CHECK((PlanPublishConsume(*memory, kG, PublishBranch::Tiling, plan) == RingConsumeRefusal::None && plan.count == -3));
    // q = 0: no list read at all, even when the list words are gone.
    setPublishFields(*memory, kRing + 0x2C, kRing + 0x30);
    memory->Hole(kG + kListEnd);
    memory->Hole(kG + kListLimit);
    memory->reads.clear();
    UNSCOPED_INFO("tiling plan: q zero needs no list storage");
    CHECK((PlanPublishConsume(*memory, kG, PublishBranch::Tiling, plan) == RingConsumeRefusal::None && plan.count == 0));
    UNSCOPED_INFO("tiling plan: q zero reads no list word");
    CHECK((!readAddress(*memory, kG + kListEnd) && !readAddress(*memory, kG + kListLimit)));
    // q != 0 refuses without them.
    setPublishFields(*memory, kRing + 0x2C, kRing);
    UNSCOPED_INFO("tiling plan: unreadable list head refuses");
    CHECK(PlanPublishConsume(*memory, kG, PublishBranch::Tiling, plan) == RingConsumeRefusal::ListHead);
}

void publishPlanTilingGrowth()
{
    auto               memory = makeConsumeDevice();
    PublishConsumePlan plan;
    setPublishFields(*memory, kRing + 0x2C, kRing);
    memory->Write32(kG + kListEnd, kList + 0x10);
    // E + 8 == limit is not growth; E + 8 > limit is.
    memory->Write32(kG + kListLimit, kList + 0x18);
    UNSCOPED_INFO("tiling plan: E + 8 at the limit does not grow");
    CHECK((PlanPublishConsume(*memory, kG, PublishBranch::Tiling, plan) == RingConsumeRefusal::None && !plan.growth));
    memory->Write32(kG + kListLimit, kList + 0x17);
    UNSCOPED_INFO("tiling plan: E + 8 past the limit grows");
    CHECK((PlanPublishConsume(*memory, kG, PublishBranch::Tiling, plan) == RingConsumeRefusal::None && plan.growth));
    // The growth helper returns its own destination, so E's storage is not required.
    memory->ReadOnly(kList + 0x10);
    UNSCOPED_INFO("tiling plan: growth needs no storage at the old E");
    CHECK(PlanPublishConsume(*memory, kG, PublishBranch::Tiling, plan) == RingConsumeRefusal::None);
    memory->Write32(kG + kListLimit, kList + 0x100);
    UNSCOPED_INFO("tiling plan: an unwritable E refuses without growth");
    CHECK(PlanPublishConsume(*memory, kG, PublishBranch::Tiling, plan) == RingConsumeRefusal::ListStorage);
}

void publishPlanRefusals()
{
    auto               memory = makeConsumeDevice();
    PublishConsumePlan plan;
    setPublishFields(*memory, kRing + 0x2C, kRing);
    UNSCOPED_INFO("plan: the recording variant is refused");
    CHECK(PlanPublishConsume(*memory, kG, PublishBranch::Recording, plan) == RingConsumeRefusal::Recording);
    UNSCOPED_INFO("plan: an unclassified branch is refused as possible recording");
    CHECK(PlanPublishConsume(*memory, kG, PublishBranch::Unreadable, plan) == RingConsumeRefusal::Unclassified);
    memory->Hole(kG + kCursor);
    UNSCOPED_INFO("plan: unreadable C");
    CHECK(PlanPublishConsume(*memory, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::Cursor);
    auto start = makeConsumeDevice();
    start->Hole(kG + kTailStart);
    UNSCOPED_INFO("plan: unreadable S");
    CHECK(PlanPublishConsume(*start, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::Start);
    auto limit = makeConsumeDevice();
    limit->Hole(kG + kCursorLimit);
    UNSCOPED_INFO("plan: unreadable limit");
    CHECK(PlanPublishConsume(*limit, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::Limit);
    auto tail = makeConsumeDevice();
    tail->ReadOnly(kG + kTailStart);
    UNSCOPED_INFO("plan: unwritable G+0x3A50");
    CHECK(PlanPublishConsume(*tail, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::TailStorage);
    auto cursor = makeConsumeDevice();
    cursor->ReadOnly(kG + kCursor);
    UNSCOPED_INFO("plan: unwritable G+0x30");
    CHECK(PlanPublishConsume(*cursor, kG, PublishBranch::Starved, plan) == RingConsumeRefusal::TailStorage);
    auto list = makeConsumeDevice();
    list->Write32(kG + kTailStart, kRing);
    list->Write32(kG + kCursor, kRing + 0x2C);
    list->Write32(kG + kListEnd, kList);
    list->Hole(kG + kListLimit);
    UNSCOPED_INFO("plan: unreadable list limit");
    CHECK(PlanPublishConsume(*list, kG, PublishBranch::Tiling, plan) == RingConsumeRefusal::ListLimit);

    // The Submit branch's fence would complete on the CPU ahead of the host draws, so the
    // plan refuses it; the branches that emit no fence are unaffected.
    auto cpu = makeConsumeDevice();
    setPublishFields(*cpu, kRing + 0x2C, kRing);
    cpu->Write8(kG + kModeHigh, kModeCpuFence);
    UNSCOPED_INFO("plan: CPU fence mode refuses the Submit branch");
    CHECK(PlanPublishConsume(*cpu, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::CpuFence);
    UNSCOPED_INFO("plan: CPU fence mode leaves Starved alone");
    CHECK(PlanPublishConsume(*cpu, kG, PublishBranch::Starved, plan) == RingConsumeRefusal::None);
    cpu->Write32(kG + kCpuFenceSink, 0x82600110);
    UNSCOPED_INFO("plan: a sink keeps the fence off the CPU path");
    CHECK(PlanPublishConsume(*cpu, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::None);

#if REREVVED_DEVELOPER
    // Each Submit refusal reports its own R1 reason.
    UNSCOPED_INFO("refusal reasons: lock, token, wrap and list storage have their own");
    CHECK((SubmitRefusalResidueReason(RingConsumeRefusal::PendingUnwritable) == OriginalResidueReason::RaiseWriteFailed && SubmitRefusalResidueReason(RingConsumeRefusal::LockStorage) == OriginalResidueReason::LockStorage &&
           SubmitRefusalResidueReason(RingConsumeRefusal::TokenStorage) == OriginalResidueReason::TokenStorage && SubmitRefusalResidueReason(RingConsumeRefusal::TokenWrap) == OriginalResidueReason::TokenWrap &&
           SubmitRefusalResidueReason(RingConsumeRefusal::ListLink) == OriginalResidueReason::ListStorage));
#endif
}

void publishTailMatchesTheOriginal()
{
    auto memory = makeConsumeDevice();
    memory->Write32(kG + kCursorLimit, kRing + 0x1000);
    memory->log.clear();
    // The deferred Submit return P + 0x10 = kRing + 0x144 aligns to 0x160; G+0x3A50 is
    // stored first and G+0x30 = a - 4 second.
    UNSCOPED_INFO("tail: in-limit return stores");
    CHECK(StorePublishTail(*memory, kG, std::uint64_t{ kRing } + 0x144) == PublishTail::Stored);
    UNSCOPED_INFO("tail: G+0x3A50 = a then G+0x30 = a - 4");
    CHECK(memory->log == std::vector<Write>{ { kG + kTailStart, kRing + 0x160 }, { kG + kCursor, kRing + 0x15C } });
    memory->log.clear();
    UNSCOPED_INFO("tail: direct return stores");
    CHECK(StorePublishTail(*memory, kG, std::uint64_t{ kRing } + 0x134) == PublishTail::Stored);
    UNSCOPED_INFO("tail: direct return aligns to 0x140");
    CHECK(memory->log == std::vector<Write>{ { kG + kTailStart, kRing + 0x140 }, { kG + kCursor, kRing + 0x13C } });
    // Past the limit only G+0x30 = r28 - 4 is stored.
    memory->Write32(kG + kCursorLimit, kRing + 0x100);
    memory->log.clear();
    UNSCOPED_INFO("tail: a return past the limit rolls over");
    CHECK(StorePublishTail(*memory, kG, std::uint64_t{ kRing } + 0x144) == PublishTail::Rollover);
    UNSCOPED_INFO("tail: rollover stores only G+0x30 = r28 - 4");
    CHECK(memory->log == std::vector<Write>{ { kG + kCursor, kRing + 0x140 } });
    // The 32-bit wrap: 0xFFFFFFF1 + 0x1F is 0x10 at 32 bits and aligns to 0.
    memory->Write32(kG + kCursorLimit, 0x9000);
    memory->log.clear();
    UNSCOPED_INFO("tail: the aligned sum wraps at 32 bits");
    CHECK((StorePublishTail(*memory, kG, 0xFFFFFFF1ULL) == PublishTail::Stored && memory->log == std::vector<Write>{ { kG + kTailStart, 0 }, { kG + kCursor, 0xFFFFFFFC } }));
    // The high half of the full-width return never reaches the stores.
    memory->log.clear();
    UNSCOPED_INFO("tail: full-width high bits are dropped");
    CHECK((StorePublishTail(*memory, kG, 0xABCD000000000040ULL) == PublishTail::Stored && memory->log == std::vector<Write>{ { kG + kTailStart, 0x40 }, { kG + kCursor, 0x3C } }));
    memory->Hole(kG + kCursorLimit);
    memory->log.clear();
    UNSCOPED_INFO("tail: an unreadable limit stores nothing");
    CHECK((StorePublishTail(*memory, kG, 0x40) == PublishTail::Failed && memory->log.empty()));
}

void tilingSegmentStoresInOrder()
{
    auto memory = makeConsumeDevice();
    memory->log.clear();
    UNSCOPED_INFO("segment: stores succeed");
    CHECK(StoreTilingSegment(*memory, kG, kList + 0x10, 0x30100, 12));
    UNSCOPED_INFO("segment: A(S) at E+4, the token at E, then E+8 at G+0x3440");
    CHECK(memory->log == std::vector<Write>{ { kList + 0x14, 0x30100 }, { kList + 0x10, 0x8100000C }, { kG + kListEnd, kList + 0x18 } });
    // The guest token is not masked to 24 bits; the ledger's record is.
    memory->log.clear();
    UNSCOPED_INFO("segment: the guest token keeps the high bits of q");
    CHECK((StoreTilingSegment(*memory, kG, kList, 0x30100, 0x7E000005) && memory->log[1] == Write{ kList, 0xFF000005 }));
    memory->log.clear();
    UNSCOPED_INFO("segment: a negative q stores its two's complement");
    CHECK((StoreTilingSegment(*memory, kG, kList, 0x30100, -3) && memory->log[1] == Write{ kList, 0xFFFFFFFD }));
    memory->Hole(kList + 4);
    memory->log.clear();
    UNSCOPED_INFO("segment: a failed first store stops the sequence");
    CHECK((!StoreTilingSegment(*memory, kG, kList, 0x30100, 12) && memory->log.empty()));
}

void submitPlanRefusals()
{
    auto                memory = makeConsumeDevice();
    const std::uint32_t token  = kRing + 0x200;
    // Entry N zero needs only N to be writable: the lock, token and list may be absent.
    UNSCOPED_INFO("submit plan: entry N zero needs only N");
    CHECK(PlanSubmitConsume(*memory, kG, 0xF0000000, 0xF0000000, 0) == RingConsumeRefusal::None);
    memory->ReadOnly(kG + kWorkerPending);
    UNSCOPED_INFO("submit plan: unwritable N");
    CHECK(PlanSubmitConsume(*memory, kG, token, kList, 0) == RingConsumeRefusal::PendingUnwritable);
    auto locked = makeConsumeDevice();
    locked->Write32(kList + 8, kList + 0x40);
    UNSCOPED_INFO("submit plan: nonzero entry N checks lock, token and list");
    CHECK(PlanSubmitConsume(*locked, kG, token, kList, 5) == RingConsumeRefusal::None);
    locked->ReadOnly(kG + kSubmitLock);
    UNSCOPED_INFO("submit plan: unwritable lock");
    CHECK(PlanSubmitConsume(*locked, kG, token, kList, 5) == RingConsumeRefusal::LockStorage);
    auto wrap = makeConsumeDevice();
    wrap->Write32(kList + 8, kList + 0x40);
    UNSCOPED_INFO("submit plan: a token buffer that wraps the address space");
    CHECK(PlanSubmitConsume(*wrap, kG, 0xFFFFFFF4, kList, 5) == RingConsumeRefusal::TokenWrap);
    for (const std::uint32_t offset : { 4U, 8U, 0xCU })
    {
        auto word = makeConsumeDevice();
        word->Write32(kList + 8, kList + 0x40);
        word->ReadOnly(token + offset);
        UNSCOPED_INFO("submit plan: each token word must be writable");
        CHECK(PlanSubmitConsume(*word, kG, token, kList, 5) == RingConsumeRefusal::TokenStorage);
    }
    for (const std::uint32_t word : { kList + 4, kList + 8, token, kList + 0x40 })
    {
        auto link = makeConsumeDevice();
        link->Write32(kList + 8, kList + 0x40);
        link->ReadOnly(word);
        UNSCOPED_INFO("submit plan: B550's four stores must be writable");
        CHECK(PlanSubmitConsume(*link, kG, token, kList, 5) == RingConsumeRefusal::ListLink);
    }
    auto tail = makeConsumeDevice();
    tail->Hole(kList + 8);
    UNSCOPED_INFO("submit plan: an unreadable list tail");
    CHECK(PlanSubmitConsume(*tail, kG, token, kList, 5) == RingConsumeRefusal::ListLink);
}

void pendingAndTokenStores()
{
    auto memory = makeConsumeDevice();
    memory->Write32(kG + kWorkerPending, 9);
    memory->log.clear();
    UNSCOPED_INFO("pending: an add of zero still stores");
    CHECK((StorePendingAdd(*memory, kG, 0) == std::optional<std::uint32_t>(9) && memory->log == std::vector<Write>{ { kG + kWorkerPending, 9 } }));
    memory->log.clear();
    UNSCOPED_INFO("pending: the add wraps at 32 bits");
    CHECK((StorePendingAdd(*memory, kG, 0xFFFFFFFF) == std::optional<std::uint32_t>(8) && memory->log == std::vector<Write>{ { kG + kWorkerPending, 8 } }));
    memory->Hole(kG + kWorkerPending);
    memory->log.clear();
    UNSCOPED_INFO("pending: an unreadable N stores nothing");
    CHECK((!StorePendingAdd(*memory, kG, 1) && memory->log.empty()));
    auto token = makeConsumeDevice();
    token->log.clear();
    UNSCOPED_INFO("token: stores succeed");
    CHECK(StoreDeferredToken(*token, kRing + 0x200, 0x30100, 0x7E000005));
    UNSCOPED_INFO("token: P+4 unmasked, P+8 the address, P+0xC the terminator, in order");
    CHECK(token->log == std::vector<Write>{ { kRing + 0x204, 0xFF000005 }, { kRing + 0x208, 0x30100 }, { kRing + 0x20C, 0xC0000000 } });
}

void cpuFenceModeAndFencePredicate()
{
    auto memory = makeConsumeDevice();
    UNSCOPED_INFO("cpu fence: off by default");
    CHECK(!FenceRunsInCpuMode(*memory, kG));
    memory->Write8(kG + kModeHigh, kModeCpuFence);
    UNSCOPED_INFO("cpu fence: bit 0x02 without a sink");
    CHECK(FenceRunsInCpuMode(*memory, kG));
    memory->Write32(kG + kCpuFenceSink, 0x82600110);
    UNSCOPED_INFO("cpu fence: a sink keeps the fence off the CPU path");
    CHECK(!FenceRunsInCpuMode(*memory, kG));
    memory->Hole(kG + kCpuFenceSink);
    UNSCOPED_INFO("cpu fence: unreadable fields are not CPU mode");
    CHECK(!FenceRunsInCpuMode(*memory, kG));
    // (next - requested) >= (next - completed), measured back from F.
    UNSCOPED_INFO("fence predicate: ordinary order");
    CHECK((RingFenceSatisfied(0x13, 0x11, 0x11) && RingFenceSatisfied(0x13, 0x11, 0x13) && !RingFenceSatisfied(0x13, 0x13, 0x11)));
    UNSCOPED_INFO("fence predicate: survives wrap");
    CHECK((RingFenceSatisfied(0x3, 0xFFFFFFFF, 0xFFFFFFFF) && !RingFenceSatisfied(0x3, 0xFFFFFFFF, 0xFFFFFFF9) && RingFenceSatisfied(0x3, 0xFFFFFFFF, 0x1)));
}

void consumeNamesAndCounts()
{
    UNSCOPED_INFO("native_ring_consume_publish is off by default");
    CHECK((!RingConsumeCvar() && !RingConsumePublishRequested()));
    // Every refusal has a distinct nonempty name.
    for (std::size_t i = 0; i < kRingConsumeRefusalNames.size(); ++i)
    {
        UNSCOPED_INFO("refusal names are nonempty");
        CHECK(!kRingConsumeRefusalNames[i].empty());
        for (std::size_t j = i + 1; j < kRingConsumeRefusalNames.size(); ++j)
        {
            UNSCOPED_INFO("refusal names are distinct");
            CHECK(kRingConsumeRefusalNames[i] != kRingConsumeRefusalNames[j]);
        }
    }
    (void)TakeRingConsumeCounts();
    CountRingConsumeFault("unit_step");
    CountRingConsumeCpuFence();
    CountRingConsumeRecording();
    const auto fields = FormatRingConsumeCounts(TakeRingConsumeCounts());
    UNSCOPED_INFO("consume fields: " + fields);
    CHECK(fields == " rc_fault=1 rc_publish_original_cpu_fence=1 rc_recording_original=1");
    UNSCOPED_INFO("a take resets the consume counts");
    CHECK(FormatRingConsumeCounts(TakeRingConsumeCounts()).empty());
}

// The entry invariants count each field separately, and legitimate states count none.
void entryInvariantsCountPerField()
{
    const auto invariants = []
    {
        const auto counts = TakeRingConsumeCounts();
        return counts.invariant;
    };
    using Counts = std::array<std::uint64_t, kRingInvariants>;
    auto memory  = makeConsumeDevice();
    memory->Write32(kG + kNextFence, 0x11);
    memory->Write32(kG + kWorkerPending, 0);
    PublishConsumePlan plan;
    setPublishFields(*memory, kRing + 0x108, kRing + 0x100);
    (void)TakeRingConsumeCounts();
    UNSCOPED_INFO("a consistent entry counts no violation");
    CHECK((PlanPublishConsume(*memory, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::None && CheckPublishEntry(*memory, kG, plan) == 0 && invariants() == Counts{}));

    // A wrapped or reset cursor and a worker-owned ring are legitimate: N nonzero is no violation.
    memory->Write32(kG + kWorkerPending, 9);
    setPublishFields(*memory, kRing + 0x20, kRing + 0x24);
    UNSCOPED_INFO("S == C + 4 and a nonzero pending count are legitimate");
    CHECK((PlanPublishConsume(*memory, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::None && CheckPublishEntry(*memory, kG, plan) == 0 && CheckSubmitEntry(*memory, kG) == 0 && invariants() == Counts{}));

    // S past C + 4.
    setPublishFields(*memory, kRing + 0x20, kRing + 0x28);
    UNSCOPED_INFO("S past C + 4 is one violation");
    CHECK((PlanPublishConsume(*memory, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::None && CheckPublishEntry(*memory, kG, plan) == 1));
    auto counts = invariants();
    UNSCOPED_INFO("cursor_order alone");
    CHECK((counts[static_cast<std::size_t>(RingInvariant::CursorOrder)] == 1 && std::accumulate(counts.begin(), counts.end(), std::uint64_t{ 0 }) == 1));

    // The hard limit G+0x34 bounds the command buffer; zero means the device has set none.
    memory->Write32(kG + kHardLimit, kRing + 0x100);
    setPublishFields(*memory, kRing + 0x108, kRing + 0x100);
    UNSCOPED_INFO("a cursor past the hard limit is one violation");
    CHECK((PlanPublishConsume(*memory, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::None && CheckPublishEntry(*memory, kG, plan) == 1));
    counts = invariants();
    UNSCOPED_INFO("cursor_extent alone for a cursor past the hard limit");
    CHECK((counts[static_cast<std::size_t>(RingInvariant::CursorExtent)] == 1 && std::accumulate(counts.begin(), counts.end(), std::uint64_t{ 0 }) == 1));
    setPublishFields(*memory, kRing + 0x20, kRing + 0x24);
    UNSCOPED_INFO("a cursor inside the hard limit is no violation");
    CHECK((PlanPublishConsume(*memory, kG, PublishBranch::Submit, plan) == RingConsumeRefusal::None && CheckPublishEntry(*memory, kG, plan) == 0 && invariants() == Counts{}));
    PublishConsumePlan pastStart = plan;
    pastStart.start              = kRing + 0x104;
    pastStart.next               = std::uint64_t{ kRing } + 0x108;
    UNSCOPED_INFO("S and C past the hard limit are two violations");
    CHECK(CheckPublishEntry(*memory, kG, pastStart) == 2);
    counts = invariants();
    UNSCOPED_INFO("start_extent and cursor_extent past the hard limit");
    CHECK((counts[static_cast<std::size_t>(RingInvariant::StartExtent)] == 1 && counts[static_cast<std::size_t>(RingInvariant::CursorExtent)] == 1));

    // Mapped storage below the hard limit that is not the command buffer's allocation counts
    // too. S never exceeds C + 4, so a cursor outside the allocation takes its start with it.
    PublishConsumePlan unrelated = plan;
    unrelated.cursor             = kG + 0x100;
    unrelated.next               = std::uint64_t{ kG } + 0x104;
    unrelated.start              = kG + 0x100;
    UNSCOPED_INFO("C and S in unrelated mapped storage are two violations");
    CHECK(CheckPublishEntry(*memory, kG, unrelated) == 2);
    counts = invariants();
    UNSCOPED_INFO("cursor_extent and start_extent for a cursor and start outside the command buffer allocation");
    CHECK((counts[static_cast<std::size_t>(RingInvariant::CursorExtent)] == 1 && counts[static_cast<std::size_t>(RingInvariant::StartExtent)] == 1 && std::accumulate(counts.begin(), counts.end(), std::uint64_t{ 0 }) == 2));
    unrelated.cursor = kRing + 0x20;
    unrelated.next   = std::uint64_t{ kRing } + 0x24;
    UNSCOPED_INFO("S in unrelated mapped storage is one violation");
    CHECK(CheckPublishEntry(*memory, kG, unrelated) == 1);
    counts = invariants();
    UNSCOPED_INFO("start_extent alone for a start outside the command buffer allocation");
    CHECK((counts[static_cast<std::size_t>(RingInvariant::StartExtent)] == 1 && std::accumulate(counts.begin(), counts.end(), std::uint64_t{ 0 }) == 1));
    unrelated.start = kRing + 0x20;
    UNSCOPED_INFO("C and S inside the command buffer allocation count nothing");
    CHECK((CheckPublishEntry(*memory, kG, unrelated) == 0 && invariants() == Counts{}));
    unrelated.cursor = kG + 0x100;
    unrelated.next   = std::uint64_t{ kG } + 0x104;
    unrelated.start  = kG + 0x100;
    memory->Write32(kG + kHardLimit, 0);
    UNSCOPED_INFO("with no hard limit there is no allocation to check against");
    CHECK((CheckPublishEntry(*memory, kG, unrelated) == 0 && invariants() == Counts{}));

    // C and S outside the mapped storage.
    PublishConsumePlan unmapped;
    unmapped.branch = PublishBranch::Submit;
    unmapped.cursor = 0x40090000;
    unmapped.start  = 0x40090000;
    unmapped.next   = std::uint64_t{ 0x40090000 } + 4;
    UNSCOPED_INFO("unmapped C and S are two violations");
    CHECK(CheckPublishEntry(*memory, kG, unmapped) == 2);
    counts = invariants();
    UNSCOPED_INFO("cursor_extent and start_extent");
    CHECK((counts[static_cast<std::size_t>(RingInvariant::CursorExtent)] == 1 && counts[static_cast<std::size_t>(RingInvariant::StartExtent)] == 1));

    // An even or unreadable F, and an unreadable N, on Submit's own check.
    memory->Write32(kG + kNextFence, 0x12);
    UNSCOPED_INFO("an even F is one violation");
    CHECK(CheckSubmitEntry(*memory, kG) == 1);
    memory->Hole(kG + kNextFence);
    memory->Hole(kG + kWorkerPending);
    UNSCOPED_INFO("an unreadable F and N are two violations");
    CHECK(CheckSubmitEntry(*memory, kG) == 2);
    counts = invariants();
    UNSCOPED_INFO("fence_parity and pending_unreadable");
    CHECK((counts[static_cast<std::size_t>(RingInvariant::FenceParity)] == 2 && counts[static_cast<std::size_t>(RingInvariant::PendingUnreadable)] == 1));

    // The list cursor past its capacity matters only for a tiled segment with q != 0.
    memory = makeConsumeDevice();
    memory->Write32(kG + kNextFence, 0x11);
    PublishConsumePlan tiled;
    tiled.branch    = PublishBranch::Tiling;
    tiled.cursor    = kRing + 0x2C;
    tiled.start     = kRing;
    tiled.next      = std::uint64_t{ kRing } + 0x30;
    tiled.count     = 12;
    tiled.listEnd   = kList + 0x20;
    tiled.listLimit = kList + 0x10;
    (void)TakeRingConsumeCounts();
    UNSCOPED_INFO("list_capacity for a segment");
    CHECK((CheckPublishEntry(*memory, kG, tiled) == 1 && invariants()[static_cast<std::size_t>(RingInvariant::ListCapacity)] == 1));
    // The growth helper provides the capacity, so a cursor it will fix is no violation.
    tiled.growth = true;
    UNSCOPED_INFO("list_capacity: the growth helper runs");
    CHECK((CheckPublishEntry(*memory, kG, tiled) == 0 && invariants() == Counts{}));
    // E + 8 is a full-width sum: a 32-bit wrap that skips the helper is a violation.
    tiled.growth    = false;
    tiled.listEnd   = 0xFFFFFFFC;
    tiled.listLimit = 0xFFFFFFFF;
    UNSCOPED_INFO("list_capacity: E + 8 wraps without the growth helper");
    CHECK((CheckPublishEntry(*memory, kG, tiled) == 1 && invariants()[static_cast<std::size_t>(RingInvariant::ListCapacity)] == 1));
    tiled.growth = true;
    UNSCOPED_INFO("list_capacity: the growth helper covers the wrap");
    CHECK((CheckPublishEntry(*memory, kG, tiled) == 0 && invariants() == Counts{}));
    tiled.growth    = false;
    tiled.listEnd   = kList + 0x20;
    tiled.listLimit = kList + 0x10;
    tiled.count     = 0;
    UNSCOPED_INFO("q == 0 reads no list state");
    CHECK((CheckPublishEntry(*memory, kG, tiled) == 0 && invariants() == Counts{}));
    const auto fields = FormatRingConsumeCounts([&]
                                                {
                                                    NoteRingInvariant(RingInvariant::FenceParity, kG, 2);
                                                    return TakeRingConsumeCounts();
                                                }());
    UNSCOPED_INFO("invariant field name: " + fields);
    CHECK(fields == " rc_invariant_fence_parity=1");
}

#endif

} // namespace

TEST_CASE("ring_stubs", "[ctest][ring-stubs]")
{
    layoutCheckClassifiesDevices();
    fenceFieldsLeaveTheBlockToTheLedger();
    ledgerReleasesTheStubbedFence();
    ledgerRunsTheStubbedCallback();
    fallbackStillProducesTheCallback();
    flipWaitsAreSatisfied();
    workerKickThroughDirectSubmit();
    recoveryKeepsOneWriter();
    helpersMirrorTheOriginals();
    endTilingHelpersMirrorTheOriginal();
    preSwapPlansTheFlip();
    countsFormatTimingFields();
    servicesGateTheStubs();
    diagnosticOutputsPreserveHelperOperations();
#if REREVVED_DEVELOPER
    publishPlanSubmitAndStarved();
    publishPlanTilingCounts();
    publishPlanTilingGrowth();
    publishPlanRefusals();
    publishTailMatchesTheOriginal();
    tilingSegmentStoresInOrder();
    submitPlanRefusals();
    pendingAndTokenStores();
    cpuFenceModeAndFencePredicate();
    consumeNamesAndCounts();
    entryInvariantsCountPerField();
#endif
}

// Child of servicesGateTheStubs: exits 0 when an unregistered runner stubs nothing.
REREVVED_HELPER_COMMAND(ringStubsNoRunner, "ring-stubs.no-runner")
{
    (void)args;
    PPCContext ctx{};
    rerevved::gpu::guest_d3d::RegisterRingStubRunner(nullptr);
    return rerevved::gpu::guest_d3d::RunRingStub(0x8269C588, ctx, nullptr) ? 1 : 0;
}
