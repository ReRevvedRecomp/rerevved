#include "gpu/guest_d3d/completion_probe.h"
#include "gpu/guest_d3d/tiling_probe.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <array>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DECLARE(bool, native_completion_probe);

void CompletionConstructed(PPCRegister&, PPCRegister&);
void CompletionDeferredBegin();
void CompletionDeferredEnd(PPCRegister&, PPCRegister&, PPCRegister&);
void CompletionDirectBegin();
void CompletionDirectEnd(PPCRegister&, PPCRegister&, PPCRegister&);
void CompletionRetireBegin();
void CompletionRetireEnd(PPCRegister&, PPCRegister&);
void CompletionOptional347C(PPCRegister&, PPCRegister&, PPCRegister&);
void CompletionOptional40A0(PPCRegister&, PPCRegister&, PPCRegister&);
void CompletionOptional409C(PPCRegister&, PPCRegister&, PPCRegister&);

using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint32_t kDevice = 0x40010000;

CompletionSnapshot snapshot(std::uint32_t n = 0)
{
    CompletionSnapshot result;
    result.valid    = (std::uint64_t{ 1 } << kCompletionWords) - 1;
    result.words[0] = 1;
    result.words[4] = n;
    return result;
}

std::uint64_t begin(CompletionProbe& probe, std::uint32_t pc, const CompletionSnapshot& state, std::uint32_t thread = 1)
{
    return probe.Begin(pc, kDevice, thread, state, probe.Sample());
}

void end(CompletionProbe& probe, std::uint64_t id, const CompletionSnapshot& state)
{
    probe.End(id, state, probe.Sample(), 0);
}

void add(CompletionProbe& probe, CompletionAdd path, std::uint32_t amount)
{
    probe.StoreBegin();
    probe.StoreEnd(0x8269BE1C, kDevice, 2, path, amount, amount);
}

void retire(CompletionProbe& probe)
{
    probe.StoreBegin();
    probe.StoreEnd(0x826ABB34, kDevice, 3, CompletionAdd::Direct, 1, 0, true);
}

struct Memory
{
    std::map<std::uint32_t, std::uint32_t> words;
    std::uint32_t                          reads = 0;

    static bool Read(void* context, std::uint32_t address, std::uint8_t* bytes, std::size_t size)
    {
        auto& memory = *static_cast<Memory*>(context);
        ++memory.reads;
        const auto word = memory.words.find(address);
        if (word == memory.words.end() || size != 4)
            return false;
        for (std::size_t i = 0; i < 4; ++i)
            bytes[i] = static_cast<std::uint8_t>(word->second >> (24 - i * 8));
        return true;
    }
};

bool faultingReader(void* context, std::uint32_t, std::uint8_t*, std::size_t)
{
    ++*static_cast<unsigned*>(context);
    throw std::runtime_error("reader fault");
}

void testReads()
{
    unsigned faults = 0;
    UNSCOPED_INFO("off path never invokes faulting reader");
    REQUIRE((!ReadCompletionWord(false, faultingReader, &faults, 0x1000) && faults == 0));
    UNSCOPED_INFO("reader faults remain unreadable");
    REQUIRE((!ReadCompletionWord(true, faultingReader, &faults, 0x1000) && faults == 1));
    Memory memory;
    memory.words[0x1000] = 0x12345678;
    UNSCOPED_INFO("big endian read");
    REQUIRE(ReadCompletionWord(true, Memory::Read, &memory, 0x1000) == 0x12345678);
    const auto reads = memory.reads;
    UNSCOPED_INFO("disabled read absent");
    REQUIRE(!ReadCompletionWord(false, Memory::Read, &memory, 0x1000));
    UNSCOPED_INFO("checked address addition");
    REQUIRE(!ReadCompletionWord(true, Memory::Read, &memory, UINT32_MAX - 1, 4));
    UNSCOPED_INFO("checked word end");
    REQUIRE(!ReadCompletionWord(true, Memory::Read, &memory, UINT32_MAX - 2));
    UNSCOPED_INFO("null base is absent");
    REQUIRE(!ReadCompletionWord(true, Memory::Read, &memory, 0, 4));
    UNSCOPED_INFO("disabled and overflowing reads never call reader");
    REQUIRE(memory.reads == reads);
    for (const auto offset : kCompletionOffsets)
        memory.words[kDevice + offset] = 0;
    memory.words[kDevice + 0x2A9C] = 3;
    auto state                     = ReadCompletionSnapshot(Memory::Read, &memory, kDevice);
    UNSCOPED_INFO("null B and S2 are absent without faults");
    REQUIRE((state.unreadable == 0 && !state.Has(29) && !state.Has(30) && !state.Has(31)));
    memory.words[kDevice + 0x2A90] = 0x1000;
    memory.words[kDevice + 0x2A94] = 0x2000;
    memory.words[0x2010]           = 0x826AC020;
    state                          = ReadCompletionSnapshot(Memory::Read, &memory, kDevice);
    UNSCOPED_INFO("each pointer chain read is guarded and independently valid");
    REQUIRE((state.words[29] == 0x12345678 && state.words[30] == 0x826AC020 && !state.Has(31) && state.unreadable == 1));
    memory.words[kDevice + 0x2A90] = UINT32_MAX - 1;
    memory.words.erase(kDevice + 0x2B00);
    state = ReadCompletionSnapshot(Memory::Read, &memory, kDevice);
    CompletionProbe probe;
    probe.Observe(1, kDevice, 1, state);
    const auto report = probe.Take();
    UNSCOPED_INFO("unreadable words are counted without fabricating N");
    REQUIRE((report.counts.unreadable == 3 && !report.records[0].snapshot.Has(4) && report.counts.pOdd == 1));
}

void testBalanceAcrossTake()
{
    UNSCOPED_INFO("unsigned wrapping addition");
    REQUIRE(CompletionResidual(UINT32_MAX, 0, 1, 0) == 0);
    UNSCOPED_INFO("unsigned wrapping retirement");
    REQUIRE(CompletionResidual(0, UINT32_MAX, 0, 1) == 0);
    CompletionProbe probe;
    const auto      id = begin(probe, 0x826AC218, snapshot(7));
    add(probe, CompletionAdd::Deferred, 0);
    add(probe, CompletionAdd::Direct, 3);
    add(probe, CompletionAdd::Stub, 2);
    const auto first = probe.Take();
    UNSCOPED_INFO("zero additions count calls and path sums separately");
    REQUIRE((first.counts.addCalls == std::array<std::uint64_t, 3>{ 1, 1, 1 } && first.counts.adds == std::array<std::uint64_t, 3>{ 0, 3, 2 }));
    UNSCOPED_INFO("Take preserves active bracket");
    REQUIRE(first.counts.unmatched == 1);
    retire(probe);
    end(probe, id, snapshot(11));
    const auto second = probe.Take();
    UNSCOPED_INFO("cumulative totals survive Take with carry-in N");
    REQUIRE((second.counts.balanceOk == 1 && second.counts.balanceBad == 0 && second.counts.balanceIndeterminate == 0 && second.counts.d == 1));
    UNSCOPED_INFO("recorded residual is zero");
    REQUIRE(second.records[second.size - 1].payload[4] == 0);
    UNSCOPED_INFO("closed bracket matches");
    REQUIRE(second.counts.unmatched == 0);
}

void testCarryAndBadBalance()
{
    CompletionProbe probe;
    auto            id = begin(probe, 0x8269C810, snapshot(1));
    end(probe, id, snapshot(1));
    probe.Take();
    retire(probe);
    id = begin(probe, 0x8269C810, snapshot());
    end(probe, id, snapshot());
    auto report = probe.Take();
    UNSCOPED_INFO("consecutive C810 exits reconcile carried retirement across drains");
    REQUIRE((report.counts.balanceOk == 2 && report.counts.c810Enter == 1 && report.counts.c810Exit == 1));
    UNSCOPED_INFO("carry has its own record");
    REQUIRE(std::string(report.records[report.size - 1].kind) == "carry");
    id = begin(probe, 0x826AC218, snapshot());
    end(probe, id, snapshot(2));
    report = probe.Take();
    UNSCOPED_INFO("unexplained N movement is bad");
    REQUIRE((report.counts.balanceBad == 1 && report.records[report.size - 1].payload[4] == 2));
}

void testIndeterminate()
{
    CompletionProbe probe;
    auto            id     = begin(probe, 0x826AC218, snapshot());
    auto            nested = begin(probe, 0x826AC218, snapshot(), 2);
    end(probe, nested, snapshot());
    end(probe, id, snapshot());
    UNSCOPED_INFO("overlapping brackets are indeterminate");
    REQUIRE(probe.Take().counts.balanceIndeterminate == 2);
    id                = begin(probe, 0x826AC218, snapshot());
    const auto submit = begin(probe, 0x8269BD98, snapshot());
    add(probe, CompletionAdd::Deferred, 1);
    end(probe, submit, snapshot(1));
    end(probe, id, snapshot(1));
    UNSCOPED_INFO("a normal nested submission does not invalidate its bracket");
    REQUIRE(probe.Take().counts.balanceOk == 1);
    id = begin(probe, 0x826AC218, snapshot());
    probe.Reset(kDevice);
    end(probe, id, snapshot());
    auto report = probe.Take();
    UNSCOPED_INFO("watchdog zero is not retirement");
    REQUIRE((report.counts.balanceIndeterminate == 1 && report.counts.d == 0 && report.counts.watchdogReset == 1));
    id = begin(probe, 0x826AC218, snapshot());
    probe.Observe(1, kDevice, 1, snapshot(), {}, true);
    end(probe, id, snapshot());
    report = probe.Take();
    UNSCOPED_INFO("epochs never merge");
    REQUIRE((report.counts.balanceIndeterminate == 1 && report.counts.epochAmbiguous == 1));
    const auto stamp = probe.Sample();
    add(probe, CompletionAdd::Direct, 0);
    id = probe.Begin(0x826AC218, kDevice, 1, snapshot(), stamp);
    end(probe, id, snapshot());
    UNSCOPED_INFO("store during sampling invalidates endpoint");
    REQUIRE(probe.Take().counts.balanceIndeterminate == 1);
    probe.StoreBegin();
    id = begin(probe, 0x826AC218, snapshot());
    probe.StoreCancel();
    end(probe, id, snapshot());
    UNSCOPED_INFO("in-flight store invalidates endpoint");
    REQUIRE(probe.Take().counts.balanceIndeterminate == 1);
    auto missing = snapshot();
    missing.valid &= ~(std::uint64_t{ 1 } << 4);
    missing.unreadable = 1;
    id                 = begin(probe, 0x826AC218, missing);
    end(probe, id, snapshot());
    report = probe.Take();
    UNSCOPED_INFO("unreadable N cannot balance as zero");
    REQUIRE((report.counts.balanceIndeterminate == 1 && report.counts.unreadable == 1 && report.counts.balanceOk == 0));
    UNSCOPED_INFO("unreadable start N and residual have invalid payload bits");
    REQUIRE((report.records[report.size - 1].payloadValid & ((1U << 0) | (1U << 4))) == 0);
}

void testSampling()
{
    CompletionProbe probe;
    auto            state = snapshot();
    state.words[1]        = 2;
    state.words[9]        = 0x1000;
    state.words[5]        = 0x82010000;
    probe.Observe(1, kDevice, 1, state, {}, false, "callback");
    state.words[0]  = 0;
    state.words[1]  = 4;
    state.words[9]  = 0x2000;
    state.words[10] = 1;
    state.words[3]  = 0x3000;
    state.words[5]  = 0x82020000;
    probe.Observe(2, kDevice, 1, state);
    probe.Observe(3, kDevice, 1, state);
    const auto report = probe.Take();
    UNSCOPED_INFO("P parity and zero are sampled");
    REQUIRE((report.counts.pOdd == 1 && report.counts.pEven == 2 && report.counts.pZero == 2));
    UNSCOPED_INFO("same values are samples rather than invented writes");
    REQUIRE((report.counts.maskNonzero == 3 && report.counts.maskChange == 1 && report.counts.slotChange[0] == 1 && report.counts.slotSamples[0] == 3));
    UNSCOPED_INFO("queue D0 and cursor activity and callbacks count");
    REQUIRE((report.counts.queueNonzero == 5 && report.counts.queueCursorChange == 1 && report.counts.queueSegmentChange == 1 && report.counts.s2Change == 1 && report.counts.callbacks == 1));
    const auto text = FormatCompletionProbeCounts(report.counts);
    UNSCOPED_INFO("format declares sampled coverage");
    REQUIRE((text.find("cp_slot_coverage=sampled") != std::string::npos && text.find("cp_mask_change=1") != std::string::npos));
}

void testBounds()
{
    UNSCOPED_INFO("snapshot validity lookup is bounded");
    REQUIRE((!snapshot().Has(kCompletionWords) && !snapshot().Has(100)));
    CompletionProbe probe;
    const auto      id = begin(probe, 0x826AC218, snapshot());
    for (std::size_t i = 0; i < kCompletionRecords + 3; ++i)
        probe.Observe(1, kDevice, 1, snapshot());
    add(probe, CompletionAdd::Direct, 1);
    end(probe, id, snapshot(1));
    auto report = probe.Take();
    UNSCOPED_INFO("records are bounded with explicit overflow");
    REQUIRE((report.size == kCompletionRecords && report.counts.eventDrop == 6));
    UNSCOPED_INFO("log cap preserves counters and invalidates lost balance");
    REQUIRE((report.counts.pOdd == kCompletionRecords + 5 && report.counts.adds[1] == 1 && report.counts.balanceIndeterminate == 1));
    for (std::size_t i = 0; i < kCompletionCalls; ++i)
    {
        UNSCOPED_INFO("call fits");
        REQUIRE(begin(probe, 0x8269BD98, snapshot()) != 0);
    }
    UNSCOPED_INFO("call overflow is rejected");
    REQUIRE(begin(probe, 0x8269BD98, snapshot()) == 0);
    report = probe.Take();
    UNSCOPED_INFO("call overflow is visible without overwriting pairs");
    REQUIRE((report.counts.eventDrop == 1 && report.counts.pairError == 1 && report.counts.unmatched == kCompletionCalls));
}

void testAllocationBounds()
{
    CompletionProbe probe;
    for (std::size_t i = 0; i < kCompletionCalls + 1; ++i)
        probe.AllocationBegin(1, 0x826A6AB8, kDevice, 2, 4, 128);
    probe.AllocationEnd(1, 0x826A6B1C, 0, 0, 0, 0);
    const auto report = probe.Take();
    UNSCOPED_INFO("lost allocator nesting freezes pairing without popping an unrelated call");
    REQUIRE((report.counts.eventDrop == 1 && report.counts.pairError == 1 && report.counts.unmatched == kCompletionCalls));
}

void testAllocations()
{
    CompletionProbe probe;
    probe.AllocationBegin(1, 0x8269B81C, kDevice, 0x826AC2F0, 0x40000001, 128);
    probe.AllocationBegin(1, 0x826A6AB8, kDevice, 0x8269B888, 4, 128, 0x40020000, 1);
    probe.AllocationTarget(1, 0x82012340, 0x40030000, 64, { 0x40030000, 64, 0x82012340 });
    probe.AllocationEnd(1, 0x826A6B1C, 0x1EBB0100, 0x1EBB0100, 0x1EBB0000, 0x600000);
    probe.AllocationEnd(1, 0x8269B8EC, 0x1EBB0100, 0x1EBB0100, 0x1EBB0000, 0x600000);
    auto report = probe.Take();
    UNSCOPED_INFO("suballocation hit does not identify extent owner");
    REQUIRE((report.counts.extentHit == 2 && report.counts.extentOwnerJoined == 0 && report.size == 1));
    const auto& record = report.records[0];
    UNSCOPED_INFO("original dwords, modulo bytes, clamped bytes and actual target survive nesting");
    REQUIRE((record.payload[0] == 0x826AC2F0 && record.payload[1] == 0x40000001 && record.payload[2] == 64 && record.snapshot.words[3] == 4 && record.payload[9] == 0x82012340));
    probe.AllocationBegin(2, 0x826A6AB8, kDevice, 3, 0x600000, 128, 0x40020000);
    probe.AllocationTarget(2, 0x82012340, 0x40030000, 0x600000, { 1, 2, 3 });
    probe.AllocationEnd(2, 0x8269B8EC, 0, 0, 0, 0);
    UNSCOPED_INFO("wrong return never pops another allocator");
    REQUIRE(probe.Take().counts.pairError == 1);
    probe.AllocationEnd(2, 0x826A6B1C, 0x1EBB0000, 0x1EBB0000, 0x1EBB0000, 0x600000);
    report = probe.Take();
    UNSCOPED_INFO("whole region request is distinct from hit");
    REQUIRE((report.counts.extentOwnerJoined == 1 && report.counts.unmatched == 0));
    probe.AllocationBegin(3, 0x826A6AB8, kDevice, 3, std::nullopt, 128, 4);
    probe.AllocationTarget(3, 5, 6, std::nullopt, { std::nullopt, 2, 3 });
    probe.AllocationEnd(3, 0x826A6B1C, 0x1EBB0000, 0x1EBB0000, 0, 0);
    report = probe.Take();
    UNSCOPED_INFO("unreadable requests and region queries stay unqualified");
    REQUIRE((report.counts.unreadable == 4 && report.counts.extentOwnerJoined == 0));
}

void testFailedConstruction()
{
    completionProbeState.store(1);
    auto& probe = PublishedCompletionProbe();
    probe.Take();
    probe.Observe(1, kDevice, 1, snapshot(), {}, true);
    const auto  epoch = probe.Take().records[0].epoch;
    PPCRegister device{}, size{};
    size.u64 = 0x5E00;
    CompletionConstructed(device, size);
    const auto failed = probe.Take();
    UNSCOPED_INFO("failed construction retains null identity");
    REQUIRE((failed.size == 1 && failed.records[0].device == 0 && failed.records[0].pc == 0x8268D2AC && std::string(failed.records[0].kind) == "construction"));
    UNSCOPED_INFO("failed construction retains size and null result without a device snapshot");
    REQUIRE((failed.records[0].payload[0] == 0x5E00 && failed.records[0].payload[1] == 0 && failed.records[0].snapshot.valid == 0));
    UNSCOPED_INFO("failed construction never attempts global device fallback");
    REQUIRE(failed.counts.unreadable == 0);
    probe.Observe(2, kDevice, 1, snapshot());
    UNSCOPED_INFO("failed construction preserves existing device epoch");
    REQUIRE(probe.Take().records[0].epoch == epoch);
    device.u64 = kDevice;
    CompletionConstructed(device, size);
    const auto constructed = probe.Take();
    UNSCOPED_INFO("successful construction advances its own device epoch");
    REQUIRE((constructed.size == 1 && constructed.records[0].device == kDevice && constructed.records[0].epoch == epoch + 1));
    completionProbeState.store(0);
}

void testOptional409CPayload()
{
    completionProbeState.store(1);
    PublishedCompletionProbe().Take();
    PPCRegister device{}, target{}, argument{};
    device.u64   = kDevice;
    target.u64   = 0x82012340;
    argument.u64 = 0x40020000;
    CompletionOptional409C(device, target, argument);
    const auto report = PublishedCompletionProbe().Take();
    UNSCOPED_INFO("409C adapter records its callback");
    REQUIRE((report.size == 1 && report.counts.callbacks == 1 && report.records[0].pc == 0x826A40B4));
    UNSCOPED_INFO("unreadable 409C payload retains target and argument with no valid words");
    REQUIRE((report.records[0].payload[0] == target.u32 && report.records[0].payload[1] == argument.u32 && report.records[0].payload[8] == 0));
    UNSCOPED_INFO("409C attempts all six payload words including argument plus 20");
    REQUIRE(report.counts.unreadable == kCompletionOffsets.size() + 6);
    completionProbeState.store(0);
}

void testAdapters()
{
    completionProbeState.store(1);
    PublishedCompletionProbe().Take();
    PPCRegister g{}, amount{}, n{}, w{};
    g.u64      = kDevice;
    amount.u64 = 0;
    n.u64      = 10;
    w.u64      = kDevice + 0x2AC0;
    CompletionDeferredEnd(g, amount, n);
    CompletionRetireEnd(w, n);
    UNSCOPED_INFO("branch targets bypassing stores count no event");
    REQUIRE(PublishedCompletionProbe().Take().size == 0);
    CompletionDeferredBegin();
    CompletionDeferredEnd(g, amount, n);
    CompletionDirectBegin();
    amount.u64 = 2;
    CompletionDirectEnd(g, amount, n);
    ObserveCompletionStubBegin();
    ObserveCompletionStubEnd(kDevice, 2, 3, false);
    ObserveCompletionStubBegin();
    ObserveCompletionStubEnd(kDevice, 2, 3, true);
    CompletionRetireBegin();
    CompletionRetireEnd(w, n);
    const auto report = PublishedCompletionProbe().Take();
    UNSCOPED_INFO("both Original arms, zero-add, successful stub and final retirement count exactly once");
    REQUIRE((report.counts.addCalls == std::array<std::uint64_t, 3>{ 1, 1, 1 } && report.counts.adds == std::array<std::uint64_t, 3>{ 0, 2, 3 } && report.counts.d == 1));
    UNSCOPED_INFO("unreadable host reread and worker snapshots count");
    REQUIRE(report.counts.unreadable > 0);
    PPCRegister target{}, argument{}, fp{};
    target.u64   = 0x82012340;
    argument.u64 = kDevice;
    fp.u64       = 0x3FF0000000000000;
    CompletionOptional347C(target, amount, fp);
    CompletionOptional40A0(g, target, argument);
    CompletionOptional409C(g, target, argument);
    const auto callbacks = PublishedCompletionProbe().Take();
    UNSCOPED_INFO("actual optional callback adapters count while failed payload reads stay unreadable");
    REQUIRE((callbacks.counts.callbacks == 3 && callbacks.counts.unreadable > 0));
    UNSCOPED_INFO("callback target and raw f1 bits are retained");
    REQUIRE((callbacks.records[0].payload[0] == 0x82012340 && callbacks.records[0].payload[2] == 0x3FF00000));
    ObserveCompletionReset(kDevice);
    UNSCOPED_INFO("watchdog zero store remains in flight until its continuation");
    REQUIRE(PublishedCompletionProbe().Sample().stores == 1);
    ObserveCompletionResetEnd();
    const auto reset = PublishedCompletionProbe().Take();
    UNSCOPED_INFO("watchdog pairing closes without a retirement");
    REQUIRE((reset.counts.watchdogReset == 1 && reset.counts.d == 0 && reset.counts.unmatched == 0));
    completionProbeState.store(0);
}

void testOffAndInvariance()
{
    UNSCOPED_INFO("probe is default off");
    REQUIRE(!RefreshCompletionProbeRequested());
    tilingProbeState.store(0);
    PublishedCompletionProbe().Take();
    PPCContext actual{};
    std::memset(&actual, 0x5A, sizeof(actual));
    actual.r3.u64 = kDevice;
    PPCContext expected{};
    std::memcpy(&expected, &actual, sizeof(actual));
    {
        CompletionScope scope(0x8269C810, actual);
        scope.Stubbed();
        ObserveCompletionSample(1, UINT32_MAX, 1);
        ObserveCompletionStoreBegin(1);
        ObserveCompletionStoreEnd(1, UINT32_MAX, 1, CompletionAdd::Direct, 1, 1);
        ObserveCompletionStubBegin();
        ObserveCompletionStubEnd(UINT32_MAX, 1, 1, true);
        ObserveCompletionReset(UINT32_MAX);
        ObserveCompletionCallback(1, UINT32_MAX, UINT32_MAX, UINT32_MAX, 6);
        ObserveCompletionAllocationBegin(1, 1, UINT32_MAX, 1, 1, 1);
        ObserveCompletionAllocationTarget(1, 1, 1, UINT32_MAX, UINT32_MAX);
        ObserveCompletionAllocationEnd(1, 1, UINT32_MAX);
        UNSCOPED_INFO("off reporter is empty");
        REQUIRE(ReportCompletionProbe().empty());
    }
    auto report = PublishedCompletionProbe().Take();
    UNSCOPED_INFO("off observers do no reporting or reads");
    REQUIRE((report.size == 0 && report.counts.unreadable == 0 && report.counts.watchdogReset == 0));
    UNSCOPED_INFO("off probe preserves complete PPC context and independent flag");
    REQUIRE((std::memcmp(&actual, &expected, sizeof(actual)) == 0 && !TilingProbeRequested()));
    for (const bool stub : { false, true })
    {
        std::memcpy(&actual, &expected, sizeof(actual));
        PPCContext unobserved{};
        std::memcpy(&unobserved, &expected, sizeof(expected));
        std::uint32_t calls    = 0;
        const auto    selected = [&](PPCContext& context)
        {
            ++calls;
            context.r3.u64 = stub ? 7 : 9;
        };
        selected(unobserved);
        completionProbeState.store(1);
        {
            CompletionScope scope(0x8269C810, actual);
            selected(actual);
            if (stub)
                scope.Stubbed();
        }
        UNSCOPED_INFO("enabled scope preserves Original/stub count, result and all PPC state");
        REQUIRE((calls == 2 && std::memcmp(&actual, &unobserved, sizeof(actual)) == 0));
    }
    report = PublishedCompletionProbe().Take();
    UNSCOPED_INFO("unreadable enabled endpoints never fabricate balanced zero");
    REQUIRE((report.counts.c810Enter == 2 && report.counts.c810Exit == 2 && report.counts.unreadable > 0 && report.counts.balanceOk == 0));
    completionProbeState.store(0);
}

} // namespace

TEST_CASE("completion_probe", "[ctest][completion-probe][timeout-60]")
{
    testReads();
    testBalanceAcrossTake();
    testCarryAndBadBalance();
    testIndeterminate();
    testSampling();
    testAllocationBounds();
    testBounds();
    testAllocations();
    testOffAndInvariance();
    testFailedConstruction();
    testOptional409CPayload();
    testAdapters();
}
