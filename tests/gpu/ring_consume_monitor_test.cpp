// native_ring_consume_publish: the wait monitor, the watchdog poll and the supplemental
// counters, over an injected clock. Entry invariants are checked next to the planners in
// ring_stubs_test.cpp, which owns a fake guest memory.

#include "gpu/guest_d3d/ring_stubs.h"

#include <rex/cvar.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

REXCVAR_DECLARE(bool, native_ring_consume_publish);

namespace
{

using namespace rerevved::gpu::guest_d3d;

std::atomic<std::uint64_t> fakeNow{ 5'000'000 };

std::uint64_t fakeClock() noexcept
{
    return fakeNow.load();
}

constexpr std::uint64_t kBound = 2'000'000; // the two-second debt bound, in microseconds

class Services final : public IRingStubServices
{
public:
    void Kick(std::uint32_t) override
    {
    }
};

// The monitor is live while the services are published and the cvar is on.
struct Session
{
    explicit Session(bool enabled = true)
    {
        REXCVAR_SET(native_ring_consume_publish, enabled);
        PublishRingStubServices(std::make_shared<Services>());
        SetRingConsumeClock(&fakeClock);
        fakeNow = 5'000'000;
        // A final take also clears what an earlier lifecycle change latched.
        (void)TakeRingConsumeCounts(true);
    }

    ~Session()
    {
        WithdrawRingStubServices();
        SetRingConsumeClock(nullptr);
        REXCVAR_SET(native_ring_consume_publish, false);
    }
};

const RingWaitStats& stats(const RingConsumeCounts& counts, RingWaitKind kind)
{
    return counts.wait[static_cast<std::size_t>(kind)];
}

void scopesAreInertWithoutTheCvar()
{
    Session session(false);
    {
        const RingWaitScope scope(RingWaitKind::Fence, 0x1000, 0x8269C61C, 0x13);
        fakeNow += 10 * kBound;
        UNSCOPED_INFO("off: the watchdog sees no scope");
        CHECK(PollRingWaits(kBound).empty());
    }
    const auto counts = TakeRingConsumeCounts();
    UNSCOPED_INFO("off: a scope counts nothing");
    CHECK((stats(counts, RingWaitKind::Fence).calls == 0 && stats(counts, RingWaitKind::Fence).active == 0));
    UNSCOPED_INFO("off: no field is formatted");
    CHECK(FormatRingConsumeCounts(counts).empty());
}

void scopesCountCallsDurationsAndActive()
{
    Session session;
    {
        const RingWaitScope fence(RingWaitKind::Fence, 0x1000, 0x8269C61C, 0x13);
        const auto          open = TakeRingConsumeCounts();
        UNSCOPED_INFO("an open scope is active and not yet a completed call");
        CHECK((stats(open, RingWaitKind::Fence).active == 1 && stats(open, RingWaitKind::Fence).calls == 0));
        fakeNow += 250;
    }
    {
        const RingWaitScope fence(RingWaitKind::Fence, 0x1000, 0x8269C61C, 0x15);
        fakeNow += 100;
    }
    {
        const RingWaitScope space(RingWaitKind::RingSpace, 0x1000, 0x8269B1F0);
        fakeNow += 7;
    }
    {
        const RingWaitScope drain(RingWaitKind::Drain, 0x1000, 0x8269C868);
        fakeNow += 40;
    }
    const auto  counts = TakeRingConsumeCounts();
    const auto& fence  = stats(counts, RingWaitKind::Fence);
    UNSCOPED_INFO("fence waits: calls, total and maximum microseconds");
    CHECK((fence.calls == 2 && fence.totalUs == 350 && fence.maxUs == 250 && fence.active == 0));
    UNSCOPED_INFO("ring-space waits are counted apart");
    CHECK((stats(counts, RingWaitKind::RingSpace).calls == 1 && stats(counts, RingWaitKind::RingSpace).totalUs == 7));
    UNSCOPED_INFO("drain waits are counted apart");
    CHECK((stats(counts, RingWaitKind::Drain).calls == 1 && stats(counts, RingWaitKind::Drain).maxUs == 40));
    const auto fields = FormatRingConsumeCounts(counts);
    UNSCOPED_INFO("fence fields: " + fields);
    CHECK(fields.find(" rc_wait_fence_calls=2 rc_wait_fence_total_us=350 rc_wait_fence_max_us=250 rc_wait_fence_active=0") != std::string::npos);
    UNSCOPED_INFO("ringspace and drain fields: " + fields);
    CHECK((fields.find(" rc_wait_ringspace_calls=1") != std::string::npos && fields.find(" rc_wait_drain_calls=1") != std::string::npos));
    UNSCOPED_INFO("a second take is empty");
    CHECK(FormatRingConsumeCounts(TakeRingConsumeCounts()).empty());
}

void reentryAndThreadsKeepOneCountEach()
{
    Session session;
    {
        const RingWaitScope outer(RingWaitKind::Fence, 0x1000, 0x8269C61C, 0x13);
        const RingWaitScope inner(RingWaitKind::Fence, 0x1000, 0x8269C868, 0x13);
        std::atomic<bool>   open{ false }, release{ false };
        std::thread         other(
            [&]
            {
                const RingWaitScope scope(RingWaitKind::Fence, 0x1000, 0x82303E40, 0x15);
                open = true;
                while (!release)
                    std::this_thread::yield();
            });
        while (!open)
            std::this_thread::yield();
        UNSCOPED_INFO("reentry and a second thread are three active waits");
        CHECK(stats(TakeRingConsumeCounts(), RingWaitKind::Fence).active == 3);
        release = true;
        other.join();
        UNSCOPED_INFO("the finished thread leaves two");
        CHECK(stats(TakeRingConsumeCounts(), RingWaitKind::Fence).active == 2);
        fakeNow += 5;
    }
    const auto counts = TakeRingConsumeCounts();
    UNSCOPED_INFO("the two scopes still open at the earlier takes complete one call each");
    CHECK((stats(counts, RingWaitKind::Fence).active == 0 && stats(counts, RingWaitKind::Fence).calls == 2));
}

void watchdogReportsAtTheBoundaryOnce()
{
    Session             session;
    const RingWaitScope fence(RingWaitKind::Fence, 0x2000, 0x8269C61C, 0x13);
    fakeNow += kBound - 1;
    UNSCOPED_INFO("one microsecond short of the bound is silent");
    CHECK(PollRingWaits(kBound).empty());
    fakeNow += 1;
    const auto reports = PollRingWaits(kBound);
    UNSCOPED_INFO("the bound itself reports");
    CHECK(reports.size() == 1);
    if (reports.size() == 1)
    {
        const auto& report = reports[0];
        UNSCOPED_INFO("the report names kind, device, caller, fence and elapsed time");
        CHECK((report.kind == RingWaitKind::Fence && report.device == 0x2000 && report.caller == 0x8269C61C && report.requested == 0x13 && report.elapsedUs == kBound));
        UNSCOPED_INFO("the report carries the thread and generation");
        CHECK((report.thread != 0 && report.generation != 0));
    }
    fakeNow += kBound;
    UNSCOPED_INFO("a scope is reported once");
    CHECK(PollRingWaits(kBound).empty());
    const auto counts = TakeRingConsumeCounts();
    UNSCOPED_INFO("rc_watchdog_fence counts the report");
    CHECK((counts.watchdogFence == 1 && counts.watchdogDrain == 0));
    UNSCOPED_INFO("the watchdog field is formatted");
    CHECK(FormatRingConsumeCounts(counts).find(" rc_watchdog_fence=1") != std::string::npos);
}

void watchdogCoversDrainsAndSkipsRingSpace()
{
    Session             session;
    const RingWaitScope drain(RingWaitKind::Drain, 0x2000, 0x8269C868);
    const RingWaitScope space(RingWaitKind::RingSpace, 0x2000, 0x8269B1F0);
    fakeNow += 3 * kBound;
    const auto reports = PollRingWaits(kBound);
    UNSCOPED_INFO("only the drain is reported; a ring-space wait has no watchdog");
    CHECK((reports.size() == 1 && reports[0].kind == RingWaitKind::Drain));
    const auto counts = TakeRingConsumeCounts();
    UNSCOPED_INFO("rc_watchdog_drain counts it");
    CHECK((counts.watchdogDrain == 1 && counts.watchdogFence == 0));
}

void watchdogStaysSilentWhenIdle()
{
    Session session;
    fakeNow += 100 * kBound;
    UNSCOPED_INFO("idle: no scope, no report");
    CHECK(PollRingWaits(kBound).empty());
    {
        const RingWaitScope fence(RingWaitKind::Fence, 0x2000, 0x8269C61C, 0x13);
        fakeNow += kBound / 2;
    }
    fakeNow += 100 * kBound;
    UNSCOPED_INFO("a completed wait is never reported afterwards");
    CHECK(PollRingWaits(kBound).empty());
    const auto counts = TakeRingConsumeCounts();
    UNSCOPED_INFO("no watchdog count without a stalled scope");
    CHECK((counts.watchdogFence == 0 && counts.watchdogDrain == 0));
}

void lifecycleChangeResetsTheTable()
{
    Session session;
    auto    scope = std::make_unique<RingWaitScope>(RingWaitKind::Fence, 0x2000, 0x8269C61C, 0x13);
    fakeNow += 3 * kBound;
    // Publishing again starts a new generation: the old scope is neither reported
    // nor left counted as active.
    PublishRingStubServices(std::make_shared<Services>());
    UNSCOPED_INFO("a new generation drops the old scope from the table");
    CHECK(PollRingWaits(kBound).empty());
    UNSCOPED_INFO("the active count is reset with the generation");
    CHECK(stats(TakeRingConsumeCounts(), RingWaitKind::Fence).active == 0);
    scope.reset();
    const auto closed = TakeRingConsumeCounts();
    UNSCOPED_INFO("the old scope's exit completes its call and cannot underflow the active count");
    CHECK((stats(closed, RingWaitKind::Fence).active == 0 && stats(closed, RingWaitKind::Fence).calls == 1));
    {
        const RingWaitScope fresh(RingWaitKind::Fence, 0x2000, 0x8269C61C, 0x15);
        fakeNow += kBound;
        UNSCOPED_INFO("the table works again in the new generation");
        CHECK(PollRingWaits(kBound).size() == 1);
    }
    WithdrawRingStubServices();
    UNSCOPED_INFO("withdrawal stops the monitor");
    CHECK(!RingConsumePublishRequested());
    const RingWaitScope inert(RingWaitKind::Fence, 0x2000, 0x8269C61C, 0x13);
    UNSCOPED_INFO("a scope after withdrawal counts nothing");
    CHECK(stats(TakeRingConsumeCounts(), RingWaitKind::Fence).active == 0);
}

void cvarOffLifecycleTouchesNoMonitorState()
{
    Session session;
    auto    scope = std::make_unique<RingWaitScope>(RingWaitKind::Fence, 0x2000, 0x8269C61C, 0x13);
    REXCVAR_SET(native_ring_consume_publish, false);
    WithdrawRingStubServices();
    PublishRingStubServices(std::make_shared<Services>());
    UNSCOPED_INFO("cvar off: publishing and withdrawing leave the monitor alone");
    CHECK(stats(TakeRingConsumeCounts(), RingWaitKind::Fence).active == 1);
    REXCVAR_SET(native_ring_consume_publish, true);
    PublishRingStubServices(std::make_shared<Services>());
    UNSCOPED_INFO("cvar on: publishing starts a new generation");
    CHECK(stats(TakeRingConsumeCounts(), RingWaitKind::Fence).active == 0);
    scope.reset();
    (void)TakeRingConsumeCounts(true);
}

std::vector<std::string>* reportSink = nullptr;

void withdrawalKeepsIncompleteEvidenceForTheFinalReport()
{
    Session                     session;
    std::vector<std::string>    logged;
    RingConsumeReportOperations ok;
    reportSink = &logged;
    ok.log     = [](std::string_view fields)
    {
        reportSink->emplace_back(fields);
    };
    // A wait stranded at withdrawal: the reset forgets the table, the report does not.
    auto wait = std::make_unique<RingWaitScope>(RingWaitKind::Fence, 0x2000, 0x8269C61C, 0x13);
    fakeNow += 30;
    WithdrawRingStubServices();
    UNSCOPED_INFO("withdrawal clears the active count");
    CHECK(stats(TakeRingConsumeCounts(), RingWaitKind::Fence).active == 0);
    const auto fields = ReportRingConsume(ok);
    UNSCOPED_INFO("the final report after withdrawal still says a wait was open: " + fields);
    CHECK(fields.find(" rc_final_incomplete=1") != std::string::npos);
    wait.reset();
    logged.clear();
    UNSCOPED_INFO("the note is cleared by the final report");
    CHECK(ReportRingConsume(ok).find("rc_final_incomplete") == std::string::npos);

    // A disabled session keeps it the same way.
    PublishRingStubServices(std::make_shared<Services>());
    wait = std::make_unique<RingWaitScope>(RingWaitKind::Drain, 0x2000, 0x8269C868);
    DisableRingStubs("test");
    UNSCOPED_INFO("disabling keeps the incomplete evidence");
    CHECK(ReportRingConsume(ok).find(" rc_final_incomplete=1") != std::string::npos);
    wait.reset();
    (void)ReportRingConsume(ok);
}

std::unique_ptr<RingWaitScope> reusedFirst, reusedSecond;
std::uint64_t                  firstStart = 0;

void reuseSlotAtTheClaim(std::size_t)
{
    // The first wait ends and a second one takes its slot at the same clock value, between
    // the watchdog's last check of the slot and its claim.
    reusedFirst.reset();
    fakeNow      = firstStart;
    reusedSecond = std::make_unique<RingWaitScope>(RingWaitKind::Fence, 0x3000, 0x8269C61C, 0x15);
}

void slotReuseDuringAPollIsNotMarkedReported()
{
    Session session;
    firstStart  = fakeNow;
    reusedFirst = std::make_unique<RingWaitScope>(RingWaitKind::Fence, 0x2000, 0x8269C61C, 0x13);
    fakeNow += 3 * kBound;
    SetRingPollClaimHookForTesting(&reuseSlotAtTheClaim);
    const auto raced = PollRingWaits(kBound);
    SetRingPollClaimHookForTesting(nullptr);
    UNSCOPED_INFO("a slot reused before the claim reports nothing for the old wait");
    CHECK(raced.empty());
    UNSCOPED_INFO("and counts no watchdog for it");
    CHECK(TakeRingConsumeCounts().watchdogFence == 0);
    fakeNow          = firstStart + 3 * kBound;
    const auto later = PollRingWaits(kBound);
    UNSCOPED_INFO("the new wait in the same slot is still reported");
    CHECK((later.size() == 1 && later[0].device == 0x3000 && later[0].requested == 0x15));
    UNSCOPED_INFO("and only once");
    CHECK(PollRingWaits(kBound).empty());
    reusedSecond.reset();
}

void tableOverflowStaysCounted()
{
    Session                                     session;
    std::vector<std::unique_ptr<RingWaitScope>> scopes;
    for (int i = 0; i < 100; ++i)
        scopes.push_back(std::make_unique<RingWaitScope>(RingWaitKind::Fence, 0x2000, 0x8269C61C, 0x13));
    UNSCOPED_INFO("scopes beyond the table are still counted active");
    CHECK(stats(TakeRingConsumeCounts(), RingWaitKind::Fence).active == 100);
    scopes.clear();
    UNSCOPED_INFO("and complete cleanly");
    CHECK(stats(TakeRingConsumeCounts(), RingWaitKind::Fence).active == 0);
}

void finalReportKeepsIncompleteEvidence()
{
    Session                  session;
    std::vector<std::string> logged;
    // A wait still open at shutdown marks the final report incomplete.
    auto wait = std::make_unique<RingWaitScope>(RingWaitKind::Drain, 0x2000, 0x8269C868);
    fakeNow += 30;
    {
        RingConsumeReportOperations failing;
        failing.log = [](std::string_view)
        {
            throw std::runtime_error("log failed");
        };
        const auto fields = ReportRingConsume(failing);
        UNSCOPED_INFO("a failed log returns no fields");
        CHECK(fields.empty());
    }
    wait.reset();
    RingConsumeReportOperations      ok;
    static std::vector<std::string>* sink = nullptr;
    sink                                  = &logged;
    ok.log                                = [](std::string_view fields)
    {
        sink->emplace_back(fields);
    };
    const auto fields = ReportRingConsume(ok);
    UNSCOPED_INFO("the retry logs once");
    CHECK((logged.size() == 1 && logged[0] == fields));
    UNSCOPED_INFO("the retained evidence still says a wait was open: " + fields);
    CHECK(fields.find(" rc_final_incomplete=1") != std::string::npos);
    UNSCOPED_INFO("the retry carries the counts the failed log kept: " + fields);
    CHECK(fields.find(" rc_wait_drain_calls=1") != std::string::npos);
    // A consumed call still running at shutdown is incomplete too.
    {
        const RingConsumeCall call;
        logged.clear();
        const auto open = ReportRingConsume(ok);
        UNSCOPED_INFO("a consumed call still running is incomplete: " + open);
        CHECK(open.find(" rc_final_incomplete=1") != std::string::npos);
    }
    logged.clear();
    UNSCOPED_INFO("a clean shutdown reports no field");
    CHECK(ReportRingConsume(ok).empty());
}

void faultsAndSupplementalCounts()
{
    Session session;
    CountRingConsumeFault("publish_tail_store");
    CountRingConsumeFault("publish_tail_store");
    CountRingConsumeCpuFence();
    CountRingConsumeRecording();
    CountRingConsumeRecording();
    NoteRingInvariant(RingInvariant::FenceParity, 0x2000, 0x12);
    NoteRingInvariant(RingInvariant::ListCapacity, 0x2000, 0x40, 0x38);
    const auto counts = TakeRingConsumeCounts();
    UNSCOPED_INFO("supplemental counters");
    CHECK((counts.fault == 2 && counts.publishOriginalCpuFence == 1 && counts.recordingOriginal == 2));
    const auto fields = FormatRingConsumeCounts(counts);
    UNSCOPED_INFO("fields in order: " + fields);
    CHECK(fields == " rc_fault=2 rc_publish_original_cpu_fence=1 rc_recording_original=2 rc_invariant_fence_parity=1 rc_invariant_list_capacity=1");
    RingConsumeCounts a, b;
    a.fault           = 1;
    a.wait[0].maxUs   = 5;
    b.fault           = 2;
    b.wait[0].maxUs   = 9;
    b.finalIncomplete = 1;
    MergeRingConsumeCounts(a, b);
    UNSCOPED_INFO("merging sums counters and keeps the maximum and the incomplete mark");
    CHECK((a.fault == 3 && a.wait[0].maxUs == 9 && a.finalIncomplete == 1));
}

} // namespace

TEST_CASE("ring_consume_monitor", "[ctest][ring-consume-monitor][timeout-60]")
{
    scopesAreInertWithoutTheCvar();
    scopesCountCallsDurationsAndActive();
    reentryAndThreadsKeepOneCountEach();
    watchdogReportsAtTheBoundaryOnce();
    watchdogCoversDrainsAndSkipsRingSpace();
    watchdogStaysSilentWhenIdle();
    lifecycleChangeResetsTheTable();
    cvarOffLifecycleTouchesNoMonitorState();
    withdrawalKeepsIncompleteEvidenceForTheFinalReport();
    slotReuseDuringAPollIsNotMarkedReported();
    tableOverflowStaysCounted();
    finalReportKeepsIncompleteEvidence();
    faultsAndSupplementalCounts();
}
