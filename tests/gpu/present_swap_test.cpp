#include "gpu/guest_d3d/present_swap.h"
#include "gpu/guest_d3d/ring_stubs.h"
#include "manual_cases.hpp"

#include <rex/ppc/context.h>
#include <rex/system/interfaces/video_delegate.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The hook adapter the generated code calls (present_hook.cpp).
bool ReplaceVdSwapCall(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r9, PPCRegister& r10, PPCRegister& r30, PPCRegister& r31);

namespace
{

using namespace rerevved::gpu::guest_d3d;
using rex::system::DetachedVideoSwap;
using rex::system::IDetachedVideoDelegate;

// The runtime pieces present_hook.cpp reaches for, scripted for the adapter
// cases: the observer and the NoteRingStub report.
struct AdapterRecord
{
    std::uint32_t observed = 0, notedStubbed = 0, notedOriginal = 0;
};

AdapterRecord adapter;

} // namespace

void ObserveRendererSwapSource(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)
{
    ++adapter.observed;
}

namespace rerevved::gpu::guest_d3d
{

IRingStubMemory* GuestRingMemory() noexcept
{
    return nullptr;
}

void NoteRingStub(RingStub stub, bool stubbed, std::uint32_t, std::string_view) noexcept
{
    if (stub != RingStub::Present)
        return;
    ++(stubbed ? adapter.notedStubbed : adapter.notedOriginal);
}

} // namespace rerevved::gpu::guest_d3d

namespace
{

constexpr std::uint32_t kR1 = 0x7000F000, kR3 = 0x70010000, kR4 = 0x40000000, kR9 = 0x40001000, kR10 = 0x40001004;
constexpr std::uint32_t kWidthPointer = 0x40002000, kHeightPointer = 0x40002004;
constexpr std::uint32_t kFetch1 = 0x82345ABC, kFrontVirtual = 0x82345000, kFrontPhysical = 0x1ABCD000;
constexpr std::uint32_t kFormat = 0x1414, kColorSpace = 2, kWidth = 1280, kHeight = 720;

constexpr PresentSwapArguments kArguments{ kR1, kR3, kR4, kR9, kR10 };

// Guest words by address; a missing address is unreadable. Every read is
// logged, and no write is ever expected.
class FakeMemory final : public IRingStubMemory
{
public:
    std::map<std::uint32_t, std::uint32_t> words;
    std::map<std::uint32_t, std::uint32_t> physicals;
    std::vector<std::uint32_t>             reads;
    std::vector<std::uint32_t>             physicalReads;
    std::optional<std::uint32_t>           throwAt;
    std::uint32_t                          writes = 0;

    FakeMemory()
    {
        words[kR1 + 0x54]     = kWidthPointer;
        words[kR1 + 0x5C]     = kHeightPointer;
        words[kWidthPointer]  = kWidth;
        words[kHeightPointer] = kHeight;
        words[kR9]            = kFormat;
        words[kR10]           = kColorSpace;
        const std::array<std::uint32_t, 6> fetch{ 0x11110001, kFetch1, 0x33330003, 0x44440004, 0x55550005, 0x66660006 };
        for (std::uint32_t i = 0; i < 6; ++i)
            words[kR4 + 4 * i] = fetch[i];
        physicals[kFrontVirtual] = kFrontPhysical;
    }

    std::optional<std::uint32_t> Read32(std::uint32_t address) override
    {
        reads.push_back(address);
        if (throwAt && *throwAt == address)
            throw std::runtime_error("scripted memory failure");
        const auto at = words.find(address);
        if (at == words.end())
            return std::nullopt;
        return at->second;
    }

    std::optional<std::uint8_t> Read8(std::uint32_t) override
    {
        return std::nullopt;
    }

    bool Write32(std::uint32_t, std::uint32_t) override
    {
        ++writes;
        return false;
    }

    bool Write8(std::uint32_t, std::uint8_t) override
    {
        ++writes;
        return false;
    }

    std::optional<std::uint64_t> Exchange64(std::uint32_t, std::uint64_t) override
    {
        ++writes;
        return std::nullopt;
    }

    std::optional<std::uint32_t> Physical(std::uint32_t address) override
    {
        physicalReads.push_back(address);
        const auto at = physicals.find(address);
        if (at == physicals.end())
            return std::nullopt;
        return at->second;
    }
};

class FakeDelegate final : public IDetachedVideoDelegate
{
public:
    std::vector<DetachedVideoSwap> swaps;
    std::vector<std::string>*      events   = nullptr;
    bool                           throwStd = false, throwOther = false;

    void SetInterruptCallback(std::uint32_t, std::uint32_t) override
    {
    }

    void InitializeRingBuffer(std::uint32_t, std::uint32_t) override
    {
    }

    void EnableReadPointerWriteBack(std::uint32_t, std::uint32_t) override
    {
    }

    void OnSwap(const DetachedVideoSwap& swap) override
    {
        if (events)
            events->push_back("onswap");
        swaps.push_back(swap);
        if (throwStd)
            throw std::runtime_error("scripted OnSwap failure");
        if (throwOther)
            throw 7;
    }
};

class FakeHost final : public IPresentSwapHost
{
public:
    FakeMemory                                         memory;
    std::shared_ptr<FakeDelegate>                      delegate = std::make_shared<FakeDelegate>();
    bool                                               runtime = true, hasMemory = true, hasDelegate = true;
    bool                                               throwServices = false, throwObserver = false, throwOther = false;
    std::vector<std::string>                           events;
    std::uint32_t                                      observed = 0, notedConsumed = 0, notedOriginal = 0;
    std::vector<std::pair<std::uint64_t, std::string>> logs;

    FakeHost()
    {
        delegate->events = &events;
    }

    PresentSwapServices Services() override
    {
        events.push_back("services");
        if (throwServices)
            throw std::runtime_error("scripted services failure");
        PresentSwapServices services;
        services.memory   = hasMemory ? &memory : nullptr;
        services.runtime  = runtime;
        services.delegate = hasDelegate ? delegate : nullptr;
        return services;
    }

    void ObserveSource() override
    {
        events.push_back("observe");
        ++observed;
        if (throwObserver)
            throw std::runtime_error("scripted observer failure");
        if (throwOther)
            throw 9;
    }

    void Note(bool consumed, std::string_view) noexcept override
    {
        ++(consumed ? notedConsumed : notedOriginal);
    }

    void LogCommitFailure(std::uint64_t total, std::string_view what) noexcept override
    {
        logs.emplace_back(total, std::string(what));
    }
};

RingStubCounts drain()
{
    return TakeRingStubCounts();
}

std::uint64_t at(const RingStubCounts& counts, PresentOutcome outcome)
{
    return counts.present[static_cast<std::size_t>(outcome)];
}

std::uint64_t refusedTotal(const RingStubCounts& counts)
{
    return at(counts, PresentOutcome::RefusedRuntime) + at(counts, PresentOutcome::RefusedDelegate) + at(counts, PresentOutcome::RefusedArgs) +
           at(counts, PresentOutcome::RefusedException);
}

// True when exactly the named outcome beside Enter was counted, once.
bool onlyOutcome(const RingStubCounts& counts, PresentOutcome outcome)
{
    for (std::size_t i = 0; i < kPresentOutcomeCount; ++i)
    {
        const auto expected = (i == static_cast<std::size_t>(PresentOutcome::Enter) || i == static_cast<std::size_t>(outcome)) ? 1U : 0U;
        if (counts.present[i] != expected)
            return false;
    }
    return true;
}

// A refusal: Original runs once, so the hook returns false with no effect.
void requireRefusal(FakeHost& host, PresentOutcome outcome, std::string_view what)
{
    (void)drain();
    const bool        hook   = ReplacePresentSwap(host, kArguments);
    const auto        counts = drain();
    const std::string label(what);
    UNSCOPED_INFO(label + " returns false so Original runs");
    CHECK(!hook);
    UNSCOPED_INFO(label + " never calls the observer");
    CHECK(host.observed == 0);
    UNSCOPED_INFO(label + " never calls OnSwap");
    CHECK(host.delegate->swaps.empty());
    UNSCOPED_INFO(label + " counts its one outcome");
    CHECK(onlyOutcome(counts, outcome));
    UNSCOPED_INFO(label + " reports one Original");
    CHECK((host.notedOriginal == 1 && host.notedConsumed == 0));
    UNSCOPED_INFO(label + " writes no guest memory");
    CHECK(host.memory.writes == 0);
}

void testConsume()
{
    FakeHost host;
    (void)drain();
    const bool hook   = ReplacePresentSwap(host, kArguments);
    const auto counts = drain();
    UNSCOPED_INFO("a consumed swap returns true");
    CHECK(hook);
    UNSCOPED_INFO("the preflight precedes the observer, then OnSwap");
    CHECK(host.events == std::vector<std::string>{ "services", "observe", "onswap" });
    UNSCOPED_INFO("OnSwap is called once");
    CHECK(host.delegate->swaps.size() == 1);
    UNSCOPED_INFO("a consumed swap counts enter and consumed only");
    CHECK(onlyOutcome(counts, PresentOutcome::Consumed));
    UNSCOPED_INFO("a consumed swap reports one stub_present");
    CHECK((host.notedConsumed == 1 && host.notedOriginal == 0));
    UNSCOPED_INFO("a consumed swap logs nothing");
    CHECK(host.logs.empty());
    UNSCOPED_INFO("a consumed swap writes no guest memory");
    CHECK(host.memory.writes == 0);
    UNSCOPED_INFO("the timing fields name the counts");
    CHECK(FormatRingStubCounts(counts) == " present_enter=1 present_consumed=1");
}

void testDecode()
{
    FakeMemory memory;
    const auto swap = DecodePresentSwap(memory, kArguments);
    UNSCOPED_INFO("the scripted swap decodes");
    CHECK(swap.has_value());
    if (!swap)
        return;
    UNSCOPED_INFO("the reservation is r3");
    CHECK(swap->reservation == kR3);
    UNSCOPED_INFO("the virtual frontbuffer masks fetch[1] with 0xFFFFF000");
    CHECK(swap->frontbuffer_virtual == kFrontVirtual);
    UNSCOPED_INFO("the physical frontbuffer is the mapping");
    CHECK(swap->frontbuffer_physical == kFrontPhysical);
    UNSCOPED_INFO("the mapping is asked for the masked virtual page");
    CHECK(memory.physicalReads == std::vector<std::uint32_t>{ kFrontVirtual });
    UNSCOPED_INFO("fetch dwords other than [1] are copied");
    CHECK((swap->fetch[0] == 0x11110001 && swap->fetch[2] == 0x33330003 && swap->fetch[3] == 0x44440004 && swap->fetch[4] == 0x55550005 && swap->fetch[5] == 0x66660006));
    UNSCOPED_INFO("fetch[1] keeps its low 12 bits and takes the physical page");
    CHECK(swap->fetch[1] == ((kFetch1 & 0xFFFU) | kFrontPhysical));
    UNSCOPED_INFO("format and color space are read through r9 and r10");
    CHECK((swap->texture_format == kFormat && swap->color_space == kColorSpace));
    UNSCOPED_INFO("width and height are read through r1+0x54 and r1+0x5C");
    CHECK((swap->width == kWidth && swap->height == kHeight));
    const std::set<std::uint32_t> read(memory.reads.begin(), memory.reads.end());
    const std::set<std::uint32_t> expected{ kR4, kR4 + 4, kR4 + 8, kR4 + 12, kR4 + 16, kR4 + 20, kR9, kR10, kR1 + 0x54, kR1 + 0x5C, kWidthPointer, kHeightPointer };
    UNSCOPED_INFO("decode reads exactly the six fetch dwords, format, color space and the two size slots");
    CHECK(read == expected);
    UNSCOPED_INFO("decode writes nothing");
    CHECK(memory.writes == 0);

    // The width slot is r1+0x54 and the height slot r1+0x5C, not the reverse.
    FakeMemory swapped;
    std::swap(swapped.words[kR1 + 0x54], swapped.words[kR1 + 0x5C]);
    const auto reversed = DecodePresentSwap(swapped, kArguments);
    UNSCOPED_INFO("exchanging the slot pointers exchanges width and height");
    CHECK((reversed && reversed->width == kHeight && reversed->height == kWidth));

    // The low page bits are not part of the mapping lookup.
    FakeMemory low;
    low.words[kR4 + 4] = kFetch1 | 0xFFF;
    const auto lowSwap = DecodePresentSwap(low, kArguments);
    UNSCOPED_INFO("the low twelve bits ride through the rewrite");
    CHECK((lowSwap && lowSwap->frontbuffer_virtual == kFrontVirtual && lowSwap->fetch[1] == (0xFFFU | kFrontPhysical)));
}

void testRefusals()
{
    {
        FakeHost host;
        host.hasMemory = false;
        requireRefusal(host, PresentOutcome::RefusedRuntime, "no memory");
    }
    {
        FakeHost host;
        host.runtime = false;
        requireRefusal(host, PresentOutcome::RefusedRuntime, "no kernel or owner");
    }
    {
        FakeHost host;
        host.hasDelegate = false;
        requireRefusal(host, PresentOutcome::RefusedDelegate, "no delegate");
    }
    {
        // Without memory the delegate is not asked about: the runtime reason wins.
        FakeHost host;
        host.hasMemory   = false;
        host.hasDelegate = false;
        requireRefusal(host, PresentOutcome::RefusedRuntime, "no memory and no delegate");
    }
    // Each unreadable argument refuses the swap.
    const std::vector<std::pair<std::string, std::uint32_t>> unreadable{
        { "fetch[0]", kR4 },
        { "fetch[1]", kR4 + 4 },
        { "fetch[5]", kR4 + 20 },
        { "format", kR9 },
        { "color space", kR10 },
        { "width slot", kR1 + 0x54 },
        { "height slot", kR1 + 0x5C },
        { "width", kWidthPointer },
        { "height", kHeightPointer },
    };
    for (const auto& [label, address] : unreadable)
    {
        FakeHost host;
        host.memory.words.erase(address);
        requireRefusal(host, PresentOutcome::RefusedArgs, "unreadable " + label);
    }
    {
        FakeHost host;
        host.memory.physicals.clear();
        requireRefusal(host, PresentOutcome::RefusedArgs, "unmapped frontbuffer");
    }
    // A refusal is repeatable: Original ran, so the next call decides afresh.
    FakeHost host;
    host.hasDelegate = false;
    UNSCOPED_INFO("a refusal does not stick");
    CHECK((!ReplacePresentSwap(host, kArguments) && !ReplacePresentSwap(host, kArguments)));
    host.hasDelegate = true;
    UNSCOPED_INFO("a later complete call consumes");
    CHECK(ReplacePresentSwap(host, kArguments));
    (void)drain();
}

void testPreflightException()
{
    {
        FakeHost host;
        host.throwServices = true;
        requireRefusal(host, PresentOutcome::RefusedException, "services exception");
    }
    {
        FakeHost host;
        host.memory.throwAt = kR4 + 8;
        requireRefusal(host, PresentOutcome::RefusedException, "memory exception");
    }
    {
        FakeHost host;
        host.memory.throwAt = kHeightPointer;
        requireRefusal(host, PresentOutcome::RefusedException, "late memory exception");
    }
}

void testCommitFailure()
{
    ResetPresentSwapLogForTesting();
    {
        FakeHost host;
        host.throwObserver = true;
        (void)drain();
        const bool hook   = ReplacePresentSwap(host, kArguments);
        const auto counts = drain();
        UNSCOPED_INFO("an observer exception still returns true");
        CHECK(hook);
        UNSCOPED_INFO("the observer ran and OnSwap did not");
        CHECK((host.observed == 1 && host.delegate->swaps.empty()));
        UNSCOPED_INFO("an observer exception counts a commit failure only");
        CHECK(onlyOutcome(counts, PresentOutcome::CommitFailure));
        UNSCOPED_INFO("a commit failure is neither consumed nor refused");
        CHECK((at(counts, PresentOutcome::Consumed) == 0 && refusedTotal(counts) == 0));
        UNSCOPED_INFO("a commit failure reports no Original");
        CHECK(host.notedOriginal == 0);
        UNSCOPED_INFO("the first failure is logged with its text");
        CHECK((host.logs.size() == 1 && host.logs[0].first == 1 && host.logs[0].second == "scripted observer failure"));
    }
    {
        FakeHost host;
        host.delegate->throwStd = true;
        (void)drain();
        const bool hook   = ReplacePresentSwap(host, kArguments);
        const auto counts = drain();
        UNSCOPED_INFO("an OnSwap exception still returns true");
        CHECK(hook);
        UNSCOPED_INFO("the observer and OnSwap both ran");
        CHECK((host.observed == 1 && host.delegate->swaps.size() == 1));
        UNSCOPED_INFO("an OnSwap exception counts a commit failure only");
        CHECK(onlyOutcome(counts, PresentOutcome::CommitFailure));
        UNSCOPED_INFO("a commit failure reports neither stub_present nor Original");
        CHECK((host.notedOriginal == 0 && host.notedConsumed == 0));
        UNSCOPED_INFO("the second failure is logged with its count");
        CHECK((host.logs.size() == 1 && host.logs[0].first == 2 && host.logs[0].second == "scripted OnSwap failure"));
    }
    {
        FakeHost host;
        host.delegate->throwOther = true;
        (void)drain();
        const bool hook   = ReplacePresentSwap(host, kArguments);
        const auto counts = drain();
        UNSCOPED_INFO("a non-standard exception from OnSwap returns true");
        CHECK(hook);
        UNSCOPED_INFO("a non-standard exception counts a commit failure");
        CHECK(onlyOutcome(counts, PresentOutcome::CommitFailure));
        UNSCOPED_INFO("the third failure is not logged");
        CHECK(host.logs.empty());
    }
    {
        FakeHost host;
        host.throwOther = true;
        (void)drain();
        const bool hook   = ReplacePresentSwap(host, kArguments);
        const auto counts = drain();
        UNSCOPED_INFO("a non-standard observer exception returns true");
        CHECK(hook);
        UNSCOPED_INFO("it counts a commit failure");
        CHECK(onlyOutcome(counts, PresentOutcome::CommitFailure));
        UNSCOPED_INFO("the fourth failure is logged as an unknown exception");
        CHECK((host.logs.size() == 1 && host.logs[0].first == 4 && host.logs[0].second == "unknown exception"));
    }
    // Failures five to eight log only at eight.
    FakeHost host;
    host.throwObserver = true;
    for (int i = 0; i < 4; ++i)
    {
        UNSCOPED_INFO("every failure returns true");
        CHECK(ReplacePresentSwap(host, kArguments));
    }
    UNSCOPED_INFO("the log is paced at powers of two");
    CHECK((host.logs.size() == 1 && host.logs[0].first == 8));
    (void)drain();
    ResetPresentSwapLogForTesting();
}

void testInvariant()
{
    ResetPresentSwapLogForTesting();
    (void)drain();
    std::uint32_t notedConsumed = 0, notedOriginal = 0;
    const auto    run = [&](FakeHost& host)
    {
        const bool hook = ReplacePresentSwap(host, kArguments);
        notedConsumed += host.notedConsumed;
        notedOriginal += host.notedOriginal;
        return hook;
    };
    std::uint32_t trueCalls = 0, falseCalls = 0;
    const auto    tally = [&](bool hook)
    {
        ++(hook ? trueCalls : falseCalls);
    };
    for (int i = 0; i < 3; ++i)
    {
        FakeHost host;
        tally(run(host));
    }
    for (int i = 0; i < 2; ++i)
    {
        FakeHost host;
        host.hasMemory = false;
        tally(run(host));
    }
    {
        FakeHost host;
        host.runtime = false;
        tally(run(host));
    }
    {
        FakeHost host;
        host.hasDelegate = false;
        tally(run(host));
    }
    for (int i = 0; i < 2; ++i)
    {
        FakeHost host;
        host.memory.words.erase(kR9);
        tally(run(host));
    }
    {
        FakeHost host;
        host.throwServices = true;
        tally(run(host));
    }
    {
        FakeHost host;
        host.throwObserver = true;
        tally(run(host));
    }
    for (int i = 0; i < 2; ++i)
    {
        FakeHost host;
        host.delegate->throwStd = true;
        tally(run(host));
    }
    const auto counts = drain();
    UNSCOPED_INFO("every call is counted on entry");
    CHECK(at(counts, PresentOutcome::Enter) == 13);
    UNSCOPED_INFO("three consumed");
    CHECK(at(counts, PresentOutcome::Consumed) == 3);
    UNSCOPED_INFO("three commit failures");
    CHECK(at(counts, PresentOutcome::CommitFailure) == 3);
    UNSCOPED_INFO("the refusals count by reason");
    CHECK((at(counts, PresentOutcome::RefusedRuntime) == 3 && at(counts, PresentOutcome::RefusedDelegate) == 1 && at(counts, PresentOutcome::RefusedArgs) == 2 &&
           at(counts, PresentOutcome::RefusedException) == 1));
    UNSCOPED_INFO("enter equals consumed plus commit failures plus the refusals");
    CHECK(at(counts, PresentOutcome::Enter) == at(counts, PresentOutcome::Consumed) + at(counts, PresentOutcome::CommitFailure) + refusedTotal(counts));
    UNSCOPED_INFO("stub_present_original equals the refusal sum");
    CHECK(notedOriginal == refusedTotal(counts));
    UNSCOPED_INFO("stub_present equals the consumed count");
    CHECK(notedConsumed == at(counts, PresentOutcome::Consumed));
    UNSCOPED_INFO("Original runs exactly for the refusals");
    CHECK((falseCalls == refusedTotal(counts) && trueCalls == at(counts, PresentOutcome::Consumed) + at(counts, PresentOutcome::CommitFailure)));
    UNSCOPED_INFO("a take resets the outcome counts");
    CHECK(FormatRingStubCounts(drain()).empty());
    ResetPresentSwapLogForTesting();
}

void testReport()
{
    RingStubCounts counts;
    counts.stubbed[static_cast<std::size_t>(RingStub::Present)]                = 4;
    counts.original[static_cast<std::size_t>(RingStub::Present)]               = 2;
    counts.present[static_cast<std::size_t>(PresentOutcome::Enter)]            = 7;
    counts.present[static_cast<std::size_t>(PresentOutcome::Consumed)]         = 4;
    counts.present[static_cast<std::size_t>(PresentOutcome::CommitFailure)]    = 1;
    counts.present[static_cast<std::size_t>(PresentOutcome::RefusedRuntime)]   = 1;
    counts.present[static_cast<std::size_t>(PresentOutcome::RefusedDelegate)]  = 0;
    counts.present[static_cast<std::size_t>(PresentOutcome::RefusedArgs)]      = 1;
    counts.present[static_cast<std::size_t>(PresentOutcome::RefusedException)] = 0;
    UNSCOPED_INFO("the outcome fields follow the stub fields, nonzero only");
    CHECK(FormatRingStubCounts(counts) ==
          " stub_present=4 stub_present_original=2 present_enter=7 present_consumed=4 present_commit_failure=1 present_refused_runtime=1 present_refused_args=1");
    UNSCOPED_INFO("the outcome names");
    CHECK((kPresentOutcomeNames[static_cast<std::size_t>(PresentOutcome::RefusedException)] == "present_refused_exception" && kPresentOutcomeCount == 7));
}

void testInactive()
{
    PPCRegister r1{}, r3{}, r4{}, r9{}, r10{}, r30{}, r31{};
    r1.u32 = kR1;
    (void)drain();
    adapter = {};
    ringStubsActive.store(false);
    UNSCOPED_INFO("inactive stubs run Original");
    CHECK(!ReplaceVdSwapCall(r1, r3, r4, r9, r10, r30, r31));
    const auto inactive = drain();
    UNSCOPED_INFO("inactive stubs count nothing");
    CHECK(FormatRingStubCounts(inactive).empty());
    UNSCOPED_INFO("inactive stubs observe and report nothing");
    CHECK((adapter.observed == 0 && adapter.notedStubbed == 0 && adapter.notedOriginal == 0));

    // Active stubs without a kernel refuse the runtime once.
    ringStubsActive.store(true);
    const bool hook = ReplaceVdSwapCall(r1, r3, r4, r9, r10, r30, r31);
    ringStubsActive.store(false);
    const auto active = drain();
    UNSCOPED_INFO("active stubs without a runtime run Original");
    CHECK(!hook);
    UNSCOPED_INFO("the missing runtime is counted once");
    CHECK(onlyOutcome(active, PresentOutcome::RefusedRuntime));
    UNSCOPED_INFO("the missing runtime reports one Original and no observation");
    CHECK((adapter.observed == 0 && adapter.notedOriginal == 1 && adapter.notedStubbed == 0));
}

using CaseTable = std::array<std::pair<const char*, void (*)()>, 8>;

const CaseTable kCases{
    CaseTable::value_type{ "testConsume", testConsume },
    CaseTable::value_type{ "testDecode", testDecode },
    CaseTable::value_type{ "testRefusals", testRefusals },
    CaseTable::value_type{ "testPreflightException", testPreflightException },
    CaseTable::value_type{ "testCommitFailure", testCommitFailure },
    CaseTable::value_type{ "testInvariant", testInvariant },
    CaseTable::value_type{ "testReport", testReport },
    CaseTable::value_type{ "testInactive", testInactive }
};

// Runs every table entry, or only the entry named `selected`.
unsigned runCases(std::string_view selected)
{
    unsigned ran = 0;
    for (const auto& [name, run] : kCases)
        if (selected.empty() || selected == name)
        {
            INFO("case " << name);
            run();
            ++ran;
        }
    return ran;
}

} // namespace

TEST_CASE("present_swap", "[ctest][present-swap][timeout-60]")
{
    runCases({});
}

// Runs one named case:
//   present-swap.case <testName>
TEST_CASE("present_swap_selected", "[.][manual][present-swap]")
{
    const auto& args = rerevved::tests::CurrentManualArgv();
    REQUIRE(args.argc() == 1);
    UNSCOPED_INFO("an unknown case name");
    REQUIRE(runCases(args[1]) == 1);
}

REREVVED_MANUAL_HELPER(presentSwapCase, "present-swap.case", "present_swap_selected")
