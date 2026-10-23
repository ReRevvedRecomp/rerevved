#include "gpu/guest_d3d/original_residue_probe.h"
#include "manual_cases.hpp"

#include <rex/cvar.h>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

REXCVAR_DECLARE(bool, native_original_residue_probe);

namespace
{

using namespace rerevved::gpu::guest_d3d;
using Function            = OriginalResidueFunction;
using Reason              = OriginalResidueReason;
using Site                = OriginalResidueSite;
constexpr std::uint32_t g = 0x10000;

struct Operation
{
    char          kind;
    unsigned      width;
    std::uint32_t address, value;
    bool          success;
    bool          operator==(const Operation&) const = default;
};

// Byte-backed BE memory: setup bypasses the operation log, failed writes do not.
class Memory final : public IRingStubMemory
{
public:
    std::map<std::uint32_t, std::uint8_t> bytes;
    std::set<std::uint32_t>               absent;
    bool                                  failWrite = false;
    std::vector<Operation>                trace;

    void word(std::uint32_t address, std::uint32_t value)
    {
        for (unsigned i = 0; i < 4; ++i)
            bytes[address + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }

    std::optional<std::uint32_t> Read32(std::uint32_t address) override
    {
        std::uint32_t value = 0;
        bool          valid = !absent.contains(address);
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto found = bytes.find(address + i);
            valid            = valid && found != bytes.end();
            value            = (value << 8) | (found == bytes.end() ? 0 : found->second);
        }
        trace.push_back({ 'r', 4, address, valid ? value : 0, valid });
        return valid ? std::optional(value) : std::nullopt;
    }

    std::optional<std::uint8_t> Read8(std::uint32_t address) override
    {
        const auto found = bytes.find(address);
        const bool valid = !absent.contains(address) && found != bytes.end();
        const auto value = valid ? found->second : std::uint8_t{ 0 };
        trace.push_back({ 'r', 1, address, value, valid });
        return valid ? std::optional(value) : std::nullopt;
    }

    bool Write32(std::uint32_t address, std::uint32_t value) override
    {
        trace.push_back({ 'w', 4, address, value, !failWrite });
        if (!failWrite)
            word(address, value);
        return !failWrite;
    }

    bool Write8(std::uint32_t, std::uint8_t) override
    {
        UNSCOPED_INFO("unexpected byte write");
        CHECK(false);
        return false;
    }

    std::optional<std::uint64_t> Exchange64(std::uint32_t, std::uint64_t) override
    {
        UNSCOPED_INFO("unexpected exchange");
        CHECK(false);
        return {};
    }

    std::optional<std::uint32_t> Physical(std::uint32_t) override
    {
        UNSCOPED_INFO("unexpected translation");
        CHECK(false);
        return {};
    }
};

std::uint64_t cell(const OriginalResidueCounts& counts, Function f, Site s, Reason r)
{
    return counts.cells[static_cast<std::size_t>(f)][static_cast<std::size_t>(s)][static_cast<std::size_t>(r)];
}

std::uint64_t total(const OriginalResidueCounts& counts)
{
    std::uint64_t result = 0;
    for (const auto& function : counts.cells)
        for (const auto& site : function)
            for (auto reason : site)
                result += reason;
    return result;
}

void add(OriginalResidueCounts& target, const OriginalResidueCounts& source)
{
    for (std::size_t f = 0; f < kOriginalResidueFunctions; ++f)
        for (std::size_t s = 0; s < kOriginalResidueSites; ++s)
            for (std::size_t r = 0; r < kOriginalResidueReasons; ++r)
                target.cells[f][s][r] += source.cells[f][s][r];
    for (std::size_t d = 0; d < kOriginalResidueDispatches; ++d)
        for (std::size_t f = 0; f < kOriginalResidueFunctions; ++f)
            for (std::size_t s = 0; s < kOriginalResidueSites; ++s)
                for (std::size_t r = 0; r < kOriginalResidueReasons; ++r)
                    target.dispatch[d][f][s][r] += source.dispatch[d][f][s][r];
    target.explicitDispatch = target.explicitDispatch || source.explicitDispatch;
    target.ringConsume      = target.ringConsume || source.ringConsume;
    target.internalError += source.internalError;
    target.lateDecisions += source.lateDecisions;
    target.reportingFailures += source.reportingFailures;
    target.writersAdmitted = source.writersAdmitted;
    target.skippedFinalReports += source.skippedFinalReports;
    target.closed = target.closed || source.closed;
}

void add(RingStubCounts& target, const RingStubCounts& source)
{
    for (std::size_t i = 0; i < kRingStubCount; ++i)
    {
        target.stubbed[i] += source.stubbed[i];
        target.original[i] += source.original[i];
    }
}

void testPublishTruthTable()
{
    constexpr std::array<PublishBranch, 8> branches{
        PublishBranch::Submit, PublishBranch::Tiling, PublishBranch::Recording, PublishBranch::Recording, PublishBranch::Starved, PublishBranch::Starved, PublishBranch::Starved, PublishBranch::Starved
    };
    constexpr std::array<Reason, 8> reasons{
        Reason::Submit, Reason::Tiling, Reason::Recording, Reason::Recording, Reason::Starved, Reason::Starved, Reason::Starved, Reason::Starved
    };
    for (unsigned valid = 0; valid < 8; ++valid)
        for (unsigned flags = 0; flags < 8; ++flags)
            for (unsigned unrelated : { 0U, 1U, 0x42U })
            {
                Memory         baseline;
                const auto     high        = static_cast<std::uint8_t>((flags & 4 ? 0x20 : 0) | unrelated);
                const auto     low         = static_cast<std::uint8_t>((flags & 2 ? 0x80 : 0) | unrelated);
                const unsigned tiling      = flags & 1;
                baseline.bytes[g + 0x2ABD] = high;
                baseline.bytes[g + 0x2ABC] = low;
                baseline.word(g + 0x33B0, tiling);
                constexpr std::array<std::uint32_t, 3> addresses{ g + 0x2ABD, g + 0x2ABC, g + 0x33B0 };
                for (unsigned i = 0; i < 3; ++i)
                    if (!(valid & (1U << i)))
                        baseline.absent.insert(addresses[i]);
                auto                         observed = baseline;
                PublishDiagnostic            diagnostic;
                const auto                   plain    = ClassifyPublish(baseline, g);
                const auto                   with     = ClassifyPublish(observed, g, &diagnostic);
                const auto                   expected = valid == 7 ? branches[flags] : PublishBranch::Unreadable;
                const std::vector<Operation> trace{
                    { 'r', 1, addresses[0], valid & 1 ? high : 0U, (valid & 1) != 0 },
                    { 'r', 1, addresses[1], valid & 2 ? low : 0U, (valid & 2) != 0 },
                    { 'r', 4, addresses[2], valid & 4 ? tiling : 0U, (valid & 4) != 0 }
                };
                UNSCOPED_INFO("independent intersection/validity branch");
                CHECK((plain == expected && with == expected && diagnostic.branch == expected));
                UNSCOPED_INFO("exact observed Publish values");
                CHECK((diagnostic.valid == valid && diagnostic.high == (valid & 1 ? high : 0) && diagnostic.low == (valid & 2 ? low : 0) && diagnostic.tiling == (valid & 4 ? tiling : 0)));
                UNSCOPED_INFO("null/present diagnostic exact high/low/tiling trace");
                CHECK((baseline.trace == trace && observed.trace == trace && baseline.bytes == observed.bytes));
                const auto reason = valid == 7 ? reasons[flags] : Reason::Unreadable;
                UNSCOPED_INFO("independent Publish reason");
                CHECK(OriginalResiduePublishReason(with) == reason);
                NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, OriginalResiduePublishReason(with));
                const auto counts = TakeOriginalResidueCounts();
                UNSCOPED_INFO("exactly one matching Publish cell");
                CHECK((total(counts) == 1 && cell(counts, Function::Publish, Site::C61C, reason) == 1));
            }
}

void testPublisherRaiseTruthTable()
{
    for (unsigned valid = 0; valid < 8; ++valid)
        for (unsigned sink : { 0U, 0x82501111U })
            for (unsigned callback : { 0U, 0x82602222U })
                for (unsigned high : { 0U, 2U, 0x42U })
                    for (bool nValid : { false, true })
                        for (unsigned pending : { 0U, 0x319U })
                            for (unsigned raise : { 0U, 3U })
                                for (bool failure : { false, true })
                                {
                                    Memory baseline;
                                    baseline.word(g + 0x541C, sink);
                                    baseline.word(g + 0x5B0C, callback);
                                    baseline.bytes[g + 0x2ABD] = static_cast<std::uint8_t>(high);
                                    baseline.word(g + 0x2B00, pending);
                                    baseline.failWrite = failure;
                                    constexpr std::array<std::uint32_t, 3> addresses{ g + 0x541C, g + 0x5B0C, g + 0x2ABD };
                                    for (unsigned i = 0; i < 3; ++i)
                                        if (!(valid & (1U << i)))
                                            baseline.absent.insert(addresses[i]);
                                    if (!nValid)
                                        baseline.absent.insert(g + 0x2B00);
                                    auto                observed = baseline;
                                    PublisherDiagnostic diagnostic;
                                    RaiseDiagnostic     raiseDiagnostic;
                                    const auto          plain       = ClassifyPublisher(baseline, g);
                                    const auto          with        = ClassifyPublisher(observed, g, &diagnostic);
                                    const bool          supported   = valid == 7 && sink == 0 && ((high & 2) || callback == 0);
                                    const auto          path        = !supported ? PublisherPath::Original : (high & 2) ? PublisherPath::Silent
                                                                                                                        : PublisherPath::Kick;
                                    bool                plainRaised = false, observedRaised = false;
                                    if (plain != PublisherPath::Original)
                                        plainRaised = RaiseForDirectSubmit(baseline, g, raise);
                                    if (with != PublisherPath::Original)
                                        observedRaised = RaiseForDirectSubmit(observed, g, raise, &raiseDiagnostic);
                                    const bool attempted = supported && nValid && pending == 0 && raise != 0;
                                    const bool served    = supported && nValid && pending == 0 && (!raise || !failure);
                                    Reason     reason;
                                    if (valid != 7)
                                        reason = Reason::PublisherUnreadable;
                                    else if (sink)
                                        reason = Reason::PublisherSink;
                                    else if (!(high & 2) && callback)
                                        reason = Reason::PublisherCallback;
                                    else if (!nValid)
                                        reason = Reason::PendingUnreadable;
                                    else if (pending)
                                        reason = Reason::PendingNonzero;
                                    else if (raise && failure)
                                        reason = Reason::RaiseWriteFailed;
                                    else
                                        reason = high & 2 ? Reason::ServedSilent : Reason::ServedKick;
                                    std::vector<Operation> trace{
                                        { 'r', 4, addresses[0], valid & 1 ? sink : 0U, (valid & 1) != 0 },
                                        { 'r', 4, addresses[1], valid & 2 ? callback : 0U, (valid & 2) != 0 },
                                        { 'r', 1, addresses[2], valid & 4 ? high : 0U, (valid & 4) != 0 }
                                    };
                                    if (supported)
                                        trace.push_back({ 'r', 4, g + 0x2B00, nValid ? pending : 0, nValid });
                                    if (attempted)
                                        trace.push_back({ 'w', 4, g + 0x2B00, raise, !failure });
                                    UNSCOPED_INFO("independent Publisher/N/write outcome");
                                    CHECK((plain == path && with == path && plainRaised == served && observedRaised == served));
                                    UNSCOPED_INFO("exact observed Publisher values");
                                    CHECK((diagnostic.valid == valid && diagnostic.path == path && diagnostic.sink == (valid & 1 ? sink : 0) && diagnostic.callback == (valid & 2 ? callback : 0) && diagnostic.high == (valid & 4 ? high : 0)));
                                    UNSCOPED_INFO("one actual write attempt and its result");
                                    CHECK((raiseDiagnostic.pendingValid == (supported && nValid) && raiseDiagnostic.pending == (supported && nValid ? pending : 0) && raiseDiagnostic.attemptedWrite == attempted && raiseDiagnostic.writeSucceeded == (attempted && !failure)));
                                    UNSCOPED_INFO("null/present exact ordered reads/writes, no retry or refusal store");
                                    CHECK((baseline.trace == trace && observed.trace == trace && baseline.bytes == observed.bytes));
                                    UNSCOPED_INFO("independent terminal Submit reason");
                                    CHECK(OriginalResidueSubmitReason(diagnostic, raiseDiagnostic, observedRaised) == reason);
                                    NoteOriginalResidueDecision(Function::Submit, 0x826AC3B4, OriginalResidueSubmitReason(diagnostic, raiseDiagnostic, observedRaised));
                                    const auto counts = TakeOriginalResidueCounts();
                                    UNSCOPED_INFO("exactly one matching Submit cell");
                                    CHECK((total(counts) == 1 && cell(counts, Function::Submit, Site::AC3B4, reason) == 1));
                                }
}

void testSitesAndFormatter()
{
    constexpr std::array<std::uint64_t, 6> lrs{ 0x8269C61C, 0x8269C53C, 0x8269C614, 0x826AC368, 0x826AC3B4, 0x826AC3E8 };
    constexpr std::array<Site, 6>          sites{ Site::C61C, Site::C53C, Site::C614, Site::AC368, Site::AC3B4, Site::AC3E8 };
    for (std::size_t i = 0; i < lrs.size(); ++i)
    {
        const auto function = i == 0 ? Function::Publish : Function::Submit;
        UNSCOPED_INFO("independent full64 LR table");
        CHECK(OriginalResidueCaller(function, lrs[i]) == sites[i]);
        for (auto alias : std::array<std::uint64_t, 3>{ lrs[i] - 4, lrs[i] | 0x1234567800000000ULL, 0x81234567 })
        {
            UNSCOPED_INFO("callPC/unknown/high-half belongs to other");
            CHECK(OriginalResidueCaller(function, alias) == Site::Other);
        }
        NoteOriginalResidueDecision(function, lrs[i], Reason::Inactive);
    }
    UNSCOPED_INFO("sites are function specific");
    CHECK((OriginalResidueCaller(Function::Submit, lrs[0]) == Site::Other && OriginalResidueCaller(Function::Publish, lrs[1]) == Site::Other));
    NoteOriginalResidueDecision(Function::Publish, 0, Reason::Submit);
    NoteOriginalResidueDecision(Function::Submit, 0, Reason::ServedKick);
    NoteOriginalResidueDecision(Function::Submit, 0, Reason::ServedSilent);
    NoteOriginalResidueDecision(Function::Submit, 0, Reason::PendingNonzero);
    NoteOriginalResidueDecision(Function::Count, 0, Reason::Inactive);
    NoteOriginalResidueDecision(Function::Publish, 0, Reason::ServedKick);
    NoteOriginalResidueDecision(Function::Submit, 0, static_cast<Reason>(255));
    const auto counts = TakeOriginalResidueCounts();
    UNSCOPED_INFO("invalid function/reason never indexes storage");
    CHECK((total(counts) == 10 && counts.internalError == 3));
    const auto text = FormatOriginalResidueCounts(counts);
    for (const auto expected : { " or_scope=wrapper_decision", " or_publish_calls=2", " or_publish_original_dispatch=2", " or_submit_calls=8", " or_submit_original_dispatch=6", " or_submit_served_kick=1", " or_submit_served_silent=1", " or_internal_error=3", " or_publish_site_8269c61c_inactive=1", " or_submit_site_other_pending_nonzero=1", " or_submit_reason_pending_nonzero=1" })
    {
        UNSCOPED_INFO("exact formatter identity and wrapper vocabulary");
        CHECK(text.find(expected) != std::string::npos);
    }
    const auto empty = TakeOriginalResidueCounts();
    UNSCOPED_INFO("second drain is empty");
    CHECK((total(empty) == 0 && empty.internalError == 0));
    UNSCOPED_INFO("enabled zero totals are explicit");
    CHECK(FormatOriginalResidueCounts(empty).find("or_publish_calls=0") != std::string::npos);
}

void testDisabled()
{
    UNSCOPED_INFO("cvar defaults false");
    CHECK((!REXCVAR_GET(native_original_residue_probe) && !RefreshOriginalResidueProbeRequested()));
    CountRingStub(RingStub::Publish, true);
    UNSCOPED_INFO("disabled reporting has no fields");
    CHECK((ReportOriginalResidueProbe().empty() && ReportOriginalResidueProbe(true).empty()));
    UNSCOPED_INFO("disabled shutdown does no diagnostic work");
    CHECK(total(TakeOriginalResidueShutdownCounts().residue) == 0);
    UNSCOPED_INFO("disabled shutdown preserves legacy counters");
    CHECK(TakeRingStubCounts().stubbed[static_cast<std::size_t>(RingStub::Publish)] == 1);
    REXCVAR_SET(native_original_residue_probe, true);
    UNSCOPED_INFO("cached flag does not become runtime mutable");
    CHECK(!OriginalResidueProbeRequested());
    UNSCOPED_INFO("test refresh enables observation");
    CHECK(RefreshOriginalResidueProbeRequested());
    REXCVAR_SET(native_original_residue_probe, false);
    RefreshOriginalResidueProbeRequested();
}

void testShutdownAndConcurrentDrain()
{
    REXCVAR_SET(native_original_residue_probe, true);
    RefreshOriginalResidueProbeRequested();
    constexpr auto publish = static_cast<std::size_t>(RingStub::Publish);
    constexpr auto submit  = static_cast<std::size_t>(RingStub::Submit);
    // Both timing cases use the same production final exchange as the reporter.
    for (bool timing : { false, true })
    {
        OriginalResidueCounts residue;
        RingStubCounts        legacy;
        NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Inactive);
        NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Starved);
        CountRingStub(RingStub::Publish, true);
        if (timing)
        {
            add(residue, TakeOriginalResidueCounts());
            add(legacy, TakeRingStubCounts());
        }
        NoteOriginalResidueDecision(Function::Submit, 0x826AC3E8, Reason::PendingNonzero);
        CountRingStub(RingStub::Submit, false);
        NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Submit);
        CountRingStub(RingStub::Publish, true);
        const auto final = TakeOriginalResidueShutdownCounts();
        add(residue, final.residue);
        add(legacy, final.legacy);
        UNSCOPED_INFO("no-timing/after-last-timing exact residue cells");
        CHECK((total(residue) == 4 && cell(residue, Function::Publish, Site::C61C, Reason::Inactive) == 1 && cell(residue, Function::Publish, Site::C61C, Reason::Starved) == 1 && cell(residue, Function::Publish, Site::C61C, Reason::Submit) == 1 && cell(residue, Function::Submit, Site::AC3E8, Reason::PendingNonzero) == 1));
        UNSCOPED_INFO("no-timing/after-last-timing separate final legacy totals");
        CHECK((legacy.stubbed[publish] == 2 && legacy.original[submit] == 1 && legacy.original[publish] == 0 && legacy.stubbed[submit] == 0));
        UNSCOPED_INFO("legacy reconciliation subtracts inactive");
        CHECK(total(residue) - cell(residue, Function::Publish, Site::C61C, Reason::Inactive) == legacy.stubbed[publish] + legacy.original[submit]);
        const auto second = TakeOriginalResidueShutdownCounts();
        UNSCOPED_INFO("empty second shutdown duplicates neither family");
        CHECK((total(second.residue) == 0 && second.residue.internalError == 0 && FormatRingStubCounts(second.legacy).empty()));
    }
    constexpr unsigned       threads = 3, iterations = 4000;
    std::atomic<unsigned>    done{ 0 };
    std::vector<std::thread> producers;
    for (unsigned t = 0; t < threads; ++t)
        producers.emplace_back([&]
                               {
                                   for (unsigned i = 0; i < iterations; ++i)
                                   {
                                       NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Submit);
                                       CountRingStub(RingStub::Publish, true);
                                       NoteOriginalResidueDecision(Function::Submit, 0x8269C53C, Reason::ServedKick);
                                       CountRingStub(RingStub::Submit, true);
                                       NoteOriginalResidueDecision(Function::Submit, 0x100000008269C53CULL, Reason::Inactive);
                                   }
                                   done.fetch_add(1, std::memory_order_release);
                               });
    OriginalResidueCounts sum;
    RingStubCounts        legacy;
    do
    {
        add(sum, TakeOriginalResidueCounts());
        add(legacy, TakeRingStubCounts());
        std::this_thread::yield();
    } while (done.load(std::memory_order_acquire) != threads);
    for (auto& producer : producers)
        producer.join();
    const auto final = TakeOriginalResidueShutdownCounts();
    add(sum, final.residue);
    add(legacy, final.legacy);
    constexpr std::uint64_t expected = threads * iterations;
    OriginalResidueCounts   exact;
    exact.cells[0][static_cast<std::size_t>(Site::C61C)][static_cast<std::size_t>(Reason::Submit)]     = expected;
    exact.cells[1][static_cast<std::size_t>(Site::C53C)][static_cast<std::size_t>(Reason::ServedKick)] = expected;
    exact.cells[1][static_cast<std::size_t>(Site::Other)][static_cast<std::size_t>(Reason::Inactive)]  = expected;
    UNSCOPED_INFO("every generated concurrent decision appears once in its exact cell");
    CHECK((sum.cells == exact.cells && sum.internalError == 0));
    UNSCOPED_INFO("concurrent independent legacy totals");
    CHECK((legacy.stubbed[publish] == expected && legacy.stubbed[submit] == expected && legacy.original[publish] == 0 && legacy.original[submit] == 0));
    UNSCOPED_INFO("whole-run cross-family normalized identity");
    CHECK(total(sum) - cell(sum, Function::Submit, Site::Other, Reason::Inactive) == legacy.stubbed[publish] + legacy.stubbed[submit]);
    UNSCOPED_INFO("concurrent final counts are not double reported");
    CHECK((total(TakeOriginalResidueShutdownCounts().residue) == 0 && FormatRingStubCounts(TakeRingStubCounts()).empty()));
    REXCVAR_SET(native_original_residue_probe, false);
    RefreshOriginalResidueProbeRequested();
}

enum class ReportFailure
{
    None,
    ResidueFormat,
    LegacyFormat,
    ResidueLog,
    LegacyLog
};

ReportFailure                             reportFailure = ReportFailure::None;
std::vector<std::pair<bool, std::string>> reported;

std::string reportResidue(const OriginalResidueCounts& counts)
{
    if (reportFailure == ReportFailure::ResidueFormat)
        throw std::runtime_error("residue format failure");
    return FormatOriginalResidueCounts(counts);
}

std::string reportLegacy(const RingStubCounts& counts)
{
    if (reportFailure == ReportFailure::LegacyFormat)
        throw std::runtime_error("legacy format failure");
    return FormatRingStubCounts(counts);
}

void reportLog(bool legacy, std::string_view fields)
{
    if (reportFailure == (legacy ? ReportFailure::LegacyLog : ReportFailure::ResidueLog))
        throw std::runtime_error("log failure");
    reported.emplace_back(legacy, fields);
}

void testReportingFailures()
{
    REXCVAR_SET(native_original_residue_probe, true);
    RefreshOriginalResidueProbeRequested();
    TakeOriginalResidueShutdownCounts();
    const OriginalResidueReportOperations operations{ reportResidue, reportLegacy, reportLog };
    for (const auto failure : { ReportFailure::ResidueFormat, ReportFailure::LegacyFormat, ReportFailure::ResidueLog, ReportFailure::LegacyLog })
    {
        reported.clear();
        NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Starved);
        NoteOriginalResidueDecision(Function::Count, 0, Reason::Inactive);
        CountRingStub(RingStub::Publish, true);
        reportFailure = failure;
        UNSCOPED_INFO("reporting exceptions do not escape");
        CHECK(ReportOriginalResidueProbe(true, operations).empty());
        const bool partial = failure == ReportFailure::LegacyLog;
        UNSCOPED_INFO("both families format before logging, only successful logs commit");
        CHECK(reported.size() == (partial ? 1U : 0U));
        if (partial)
        {
            UNSCOPED_INFO("first successful family contains its exact batch");
            CHECK((!reported[0].first && reported[0].second.find(" or_publish_calls=1 ") != std::string::npos && reported[0].second.find(" or_internal_error=1 ") != std::string::npos));
        }

        // New producer work between failure and retry must join the retained batch.
        NoteOriginalResidueDecision(Function::Submit, 0x826AC3E8, Reason::PendingNonzero);
        CountRingStub(RingStub::Submit, false);
        reportFailure    = ReportFailure::None;
        const auto retry = ReportOriginalResidueProbe(true, operations);
        UNSCOPED_INFO("retry retains unreported residue exactly once and merges new decisions");
        CHECK((retry.find(partial ? " or_publish_calls=0 " : " or_publish_calls=1 ") != std::string::npos && retry.find(" or_submit_calls=1 ") != std::string::npos && retry.find(partial ? " or_internal_error=0 " : " or_internal_error=1 ") != std::string::npos));
        UNSCOPED_INFO("retry reports the reporting failure");
        CHECK(retry.find(" or_reporting_failures=1") != std::string::npos);
        UNSCOPED_INFO("retry retains unreported legacy exactly once");
        CHECK((reported.size() == (partial ? 3U : 2U) && reported.back().first && reported.back().second == " stub_publish=1 stub_submit_original=1"));
        const auto empty = ReportOriginalResidueProbe(true, operations);
        UNSCOPED_INFO("successful retry clears only reported batches");
        CHECK((empty.find(" or_publish_calls=0 ") != std::string::npos && empty.find(" or_submit_calls=0 ") != std::string::npos && empty.find(" or_reporting_failures=0") != std::string::npos && reported.back().second.empty()));
    }

    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Submit);
    reportFailure = ReportFailure::ResidueFormat;
    UNSCOPED_INFO("periodic formatting failure returns no partial fields");
    CHECK(ReportOriginalResidueProbe(false, operations).empty());
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Submit);
    reportFailure       = ReportFailure::None;
    const auto periodic = ReportOriginalResidueProbe(false, operations);
    UNSCOPED_INFO("periodic retry retains drained cells and counts its failure");
    CHECK((periodic.find(" or_publish_calls=2 ") != std::string::npos && periodic.find(" or_publish_site_8269c61c_submit=2") != std::string::npos && periodic.find(" or_reporting_failures=1") != std::string::npos));
    UNSCOPED_INFO("periodic success clears its batch");
    CHECK(total(TakeOriginalResidueCounts()) == 0);
    REXCVAR_SET(native_original_residue_probe, false);
    RefreshOriginalResidueProbeRequested();
}

void testCloseAndLateDecisions()
{
    REXCVAR_SET(native_original_residue_probe, true);
    RefreshOriginalResidueProbeRequested();
    TakeOriginalResidueShutdownCounts();
    constexpr unsigned       threads = 3, iterations = 4000;
    std::atomic<unsigned>    ready{ 0 };
    std::atomic<bool>        resume{ false };
    std::vector<std::thread> stragglers;
    for (unsigned t = 0; t < threads; ++t)
        stragglers.emplace_back([&]
                                {
                                    for (unsigned i = 0; i < iterations; ++i)
                                        NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Submit);
                                    ready.fetch_add(1, std::memory_order_release);
                                    while (!resume.load(std::memory_order_acquire))
                                        std::this_thread::yield();
                                    for (unsigned i = 0; i < iterations; ++i)
                                        NoteOriginalResidueDecision(Function::Submit, 0x826AC3E8, Reason::PendingNonzero);
                                });
    while (ready.load(std::memory_order_acquire) != threads)
        std::this_thread::yield();
    CloseOriginalResidueProbe();
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Starved);
    const auto final = TakeOriginalResidueShutdownCounts();
    UNSCOPED_INFO("close freezes admitted cells and counts post-close decisions");
    CHECK((final.residue.closed && total(final.residue) == threads * iterations && cell(final.residue, Function::Publish, Site::C61C, Reason::Submit) == threads * iterations && final.residue.lateDecisions == 1));
    resume.store(true, std::memory_order_release);
    for (auto& straggler : stragglers)
        straggler.join();
    CloseOriginalResidueProbe();
    const auto late = TakeOriginalResidueShutdownCounts();
    UNSCOPED_INFO("live guest stragglers remain explicit late decisions after final drain");
    CHECK((late.residue.closed && total(late.residue) == 0 && late.residue.lateDecisions == threads * iterations));
    const auto fields = FormatOriginalResidueCounts(late.residue);
    UNSCOPED_INFO("closed boundary and late decisions appear in reports");
    CHECK(fields.find(" or_closed=1 or_late_decisions=12000 or_reporting_failures=0") != std::string::npos);
    const auto empty = TakeOriginalResidueShutdownCounts();
    UNSCOPED_INFO("repeated close preserves boundary without duplicating late counts");
    CHECK((empty.residue.closed && total(empty.residue) == 0 && empty.residue.lateDecisions == 0));
    NoteOriginalResidueDecision(Function::Submit, 0x826AC3E8, Reason::PendingNonzero);
    const OriginalResidueReportOperations operations{ reportResidue, reportLegacy, reportLog };
    reported.clear();
    const auto report = ReportOriginalResidueProbe(true, operations);
    UNSCOPED_INFO("final reporter exposes the closed boundary and sampled late count");
    CHECK((report.find(" or_closed=1 or_late_decisions=1 or_reporting_failures=0") != std::string::npos && reported.size() == 2 && reported[0].second == report));
    REXCVAR_SET(native_original_residue_probe, false);
    RefreshOriginalResidueProbeRequested();
}

void testPeriodicHandoffFailure()
{
    REXCVAR_SET(native_original_residue_probe, true);
    RefreshOriginalResidueProbeRequested();
    TakeOriginalResidueShutdownCounts();
    reported.clear();
    const OriginalResidueReportOperations operations{ reportResidue, reportLegacy, reportLog };
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Submit);
    reportFailure = ReportFailure::ResidueLog;
    UNSCOPED_INFO("failed periodic handoff returns no committed fields");
    CHECK((ReportOriginalResidueProbe(false, operations).empty() && reported.empty()));
    NoteOriginalResidueDecision(Function::Submit, 0x826AC3E8, Reason::PendingNonzero);
    reportFailure    = ReportFailure::None;
    const auto retry = ReportOriginalResidueProbe(false, operations);
    UNSCOPED_INFO("periodic reporter owns the successful logging handoff");
    CHECK((reported.size() == 1 && !reported[0].first && reported[0].second == retry));
    UNSCOPED_INFO("periodic handoff retry retains the batch and failure exactly once");
    CHECK((retry.find(" or_publish_calls=1 ") != std::string::npos && retry.find(" or_submit_calls=1 ") != std::string::npos && retry.find(" or_reporting_failures=1 ") != std::string::npos));
    const auto empty = ReportOriginalResidueProbe(false, operations);
    UNSCOPED_INFO("successful periodic log clears its committed batch");
    CHECK((empty.find(" or_publish_calls=0 ") != std::string::npos && empty.find(" or_submit_calls=0 ") != std::string::npos && empty.find(" or_reporting_failures=0 ") != std::string::npos));
    REXCVAR_SET(native_original_residue_probe, false);
    RefreshOriginalResidueProbeRequested();
}

// Deadline failure releases stalled threads.
constexpr auto deadlineGuard = std::chrono::milliseconds(250);

struct HeldPeriodicReport
{
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    entered  = false;
    bool                    released = false;
};

HeldPeriodicReport* heldPeriodicReport = nullptr;

void holdPeriodicLog(bool legacy, std::string_view fields)
{
    {
        std::unique_lock lock(heldPeriodicReport->mutex);
        heldPeriodicReport->entered = true;
        heldPeriodicReport->changed.notify_all();
        heldPeriodicReport->changed.wait(lock, []
                                         {
                                             return heldPeriodicReport->released;
                                         });
    }
    reportLog(legacy, fields);
}

void testFinalReportLockDeadline()
{
    REXCVAR_SET(native_original_residue_probe, true);
    RefreshOriginalResidueProbeRequested();
    TakeOriginalResidueShutdownCounts();
    reported.clear();
    HeldPeriodicReport held;
    heldPeriodicReport = &held;
    const OriginalResidueReportOperations operations{ reportResidue, reportLegacy, holdPeriodicLog };
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Starved);
    std::thread periodic([&]
                         {
                             ReportOriginalResidueProbe(false, operations);
                         });
    {
        std::unique_lock lock(held.mutex);
        UNSCOPED_INFO("periodic callback holds the reporting lock");
        CHECK(held.changed.wait_for(lock, deadlineGuard, [&]
                                    {
                                        return held.entered;
                                    }));
    }
    CountRingStub(RingStub::Submit, false);
    std::promise<std::string> finalResult;
    auto                      finalReady = finalResult.get_future();

    std::thread final([&]
                      {
                          finalResult.set_value(ReportOriginalResidueProbe(true, operations));
                      });

    const bool bounded = finalReady.wait_for(deadlineGuard) == std::future_status::ready;
    UNSCOPED_INFO("held reporting lock cannot block final report beyond the deadline guard");
    CHECK(bounded);
    // Release before joining so removing the try-lock deadline produces a failure.
    {
        std::lock_guard lock(held.mutex);
        held.released = true;
    }
    held.changed.notify_all();
    periodic.join();
    final.join();
    const auto marker = finalReady.get();
    UNSCOPED_INFO("busy final report returns an explicit incomplete marker");
    CHECK(marker.find(" or_final_incomplete=1 or_final_skipped=1 ") != std::string::npos);
    const auto retry = ReportOriginalResidueProbe(true, operations);
    UNSCOPED_INFO("skipped final report is counted and reported as incomplete on retry");
    CHECK((retry.find(" or_final_incomplete=1 or_final_skipped=1") != std::string::npos && retry.find(" or_reporting_failures=1 ") != std::string::npos));
    UNSCOPED_INFO("lock timeout leaves the periodic batch and final legacy drain intact");
    CHECK((reported.size() == 3 && reported[0].second.find(" or_publish_calls=1 ") != std::string::npos && retry.find(" or_publish_calls=0 ") != std::string::npos && reported.back().second == " stub_submit_original=1"));
    const auto empty = ReportOriginalResidueProbe(true, operations);
    UNSCOPED_INFO("successful final log clears skipped-report accounting");
    CHECK((empty.find(" or_final_incomplete=0 or_final_skipped=0") != std::string::npos && empty.find(" or_reporting_failures=0 ") != std::string::npos));
    heldPeriodicReport = nullptr;
    REXCVAR_SET(native_original_residue_probe, false);
    RefreshOriginalResidueProbeRequested();
}

void testStuckWriterDeadline()
{
    REXCVAR_SET(native_original_residue_probe, true);
    RefreshOriginalResidueProbeRequested();
    TakeOriginalResidueShutdownCounts();
    reported.clear();
    const OriginalResidueReportOperations operations{ reportResidue, reportLegacy, reportLog };
    // Model preemption between the production admission CAS and its decrement.
    detail::originalResidueAdmissions.fetch_add(1, std::memory_order_acquire);
    CountRingStub(RingStub::Publish, true);
    std::promise<std::string> finalResult;
    auto                      finalReady = finalResult.get_future();

    std::thread final([&]
                      {
                          CloseOriginalResidueProbe();
                          finalResult.set_value(ReportOriginalResidueProbe(true, operations));
                      });

    const bool bounded = finalReady.wait_for(deadlineGuard) == std::future_status::ready;
    UNSCOPED_INFO("stuck admitted writer cannot block shutdown beyond the deadline guard");
    CHECK(bounded);
    // Keep the writer admitted until the bounded reporter has sampled it.
    detail::originalResidueAdmissions.fetch_sub(1, std::memory_order_release);
    final.join();
    const auto fields = finalReady.get();
    UNSCOPED_INFO("final batch reports the exact stuck writer count and incomplete marker");
    CHECK(fields.find(" or_writers_admitted=1 or_final_incomplete=1 or_final_skipped=0") != std::string::npos);
    UNSCOPED_INFO("stuck writer still permits both final batches to be logged");
    CHECK((reported.size() == 2 && reported[0].second == fields && reported[1].second == " stub_publish=1"));
    CloseOriginalResidueProbe();
    const auto complete = ReportOriginalResidueProbe(true, operations);
    UNSCOPED_INFO("released writer permits a complete final report without duplicate legacy counts");
    CHECK((complete.find(" or_writers_admitted=0 or_final_incomplete=0 or_final_skipped=0") != std::string::npos && reported.back().second.empty()));
    REXCVAR_SET(native_original_residue_probe, false);
    RefreshOriginalResidueProbeRequested();
}

void testPublishDispatch()
{
    detail::originalResidueAdmissions.store(0);
    TakeOriginalResidueCounts();
    using D = OriginalResidueDispatch;
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Tiling, D::Original);
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Tiling, D::ConsumedTiling);
    NoteOriginalResidueDecision(Function::Publish, 0x123400008269C61C, Reason::Tiling, D::Original);
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Submit, D::ConsumedSubmit);
    const auto result = TakeOriginalResidueCounts();
    const auto fields = FormatOriginalResidueCounts(result);
    UNSCOPED_INFO("reason and full64 site classification survives actual dispatch");
    CHECK((total(result) == 4 && cell(result, Function::Publish, Site::C61C, Reason::Tiling) == 2 && cell(result, Function::Publish, Site::Other, Reason::Tiling) == 1));
    UNSCOPED_INFO("independent mixed actual dispatch identity");
    CHECK((fields.find(" or_publish_calls=4 or_publish_original_dispatch=2") != std::string::npos && fields.find(" or_publish_consumed_tiling=1 or_publish_consumed_submit=1") != std::string::npos));
    UNSCOPED_INFO("per-reason site and dispatch identity");
    CHECK((fields.find(" or_publish_site_8269c61c_tiling_dispatch_original=1") != std::string::npos && fields.find(" or_publish_site_8269c61c_tiling_dispatch_consumed_tiling=1") != std::string::npos));
    UNSCOPED_INFO("dispatch second drain empty");
    CHECK(total(TakeOriginalResidueCounts()) == 0);
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Starved, D::ConsumedTiling);
    UNSCOPED_INFO("impossible consumed branch is rejected");
    CHECK(TakeOriginalResidueCounts().internalError == 1);
}

// Starved/deferred Submit carry reason-specific fields and conserve outcomes.
void testConsumedStarvedAndDeferred()
{
    detail::originalResidueAdmissions.store(0);
    TakeOriginalResidueCounts();
    using D = OriginalResidueDispatch;
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Starved, D::ConsumedStarved);
    NoteOriginalResidueDecision(Function::Submit, 0x8269C53C, Reason::PendingNonzero, D::ConsumedDeferred);
    // Without the ring consume request the report carries the old PT fields only.
    const auto before = TakeOriginalResidueCounts();
    UNSCOPED_INFO("cvar off: no ring consume fields");
    CHECK((!before.ringConsume && FormatOriginalResidueCounts(before).find("or_publish_consumed_starved") == std::string::npos && FormatOriginalResidueCounts(before).find("or_submit_consumed_deferred") == std::string::npos));
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Starved, D::ConsumedStarved);
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Starved, D::Original);
    NoteOriginalResidueDecision(Function::Submit, 0x8269C53C, Reason::PendingNonzero, D::ConsumedDeferred);
    NoteOriginalResidueDecision(Function::Submit, 0x8269C53C, Reason::PendingNonzero, D::Original);
    NoteOriginalResidueDecision(Function::Submit, 0x826AC3B4, Reason::ServedKick, D::ServedKick);
    MarkOriginalResidueRingConsume();
    const auto result = TakeOriginalResidueCounts();
    const auto fields = FormatOriginalResidueCounts(result);
    UNSCOPED_INFO("consumed starved and deferred are valid outcomes");
    CHECK((result.internalError == 0 && total(result) == 5));
    UNSCOPED_INFO("starved and deferred fields: " + fields);
    CHECK((fields.find(" or_publish_calls=2 or_publish_original_dispatch=1") != std::string::npos && fields.find(" or_publish_consumed_starved=1 or_submit_consumed_deferred=1") != std::string::npos));
    UNSCOPED_INFO("Submit conservation: calls = original + served + deferred: " + fields);
    CHECK(fields.find(" or_submit_calls=3 or_submit_original_dispatch=1 or_submit_served_kick=1") != std::string::npos);
    UNSCOPED_INFO("per-site dispatch identity: " + fields);
    CHECK((fields.find(" or_publish_site_8269c61c_starved_dispatch_consumed_starved=1") != std::string::npos && fields.find(" or_submit_site_8269c53c_pending_nonzero_dispatch_consumed_deferred=1") != std::string::npos));
    // Each consumed outcome is rejected for any other reason or function.
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Submit, D::ConsumedStarved);
    NoteOriginalResidueDecision(Function::Publish, 0x8269C61C, Reason::Starved, D::ConsumedDeferred);
    NoteOriginalResidueDecision(Function::Submit, 0x8269C53C, Reason::ServedKick, D::ConsumedDeferred);
    NoteOriginalResidueDecision(Function::Submit, 0x8269C53C, Reason::PendingNonzero, D::ConsumedStarved);
    UNSCOPED_INFO("consumed starved and deferred are rejected for any other reason or function");
    CHECK(TakeOriginalResidueCounts().internalError == 4);
}

// The VdSwap outcome counts reach the shutdown line, and a failed log keeps them for the retry.
void testPresentOutcomesInShutdown()
{
    REXCVAR_SET(native_original_residue_probe, true);
    RefreshOriginalResidueProbeRequested();
    TakeOriginalResidueShutdownCounts();
    const OriginalResidueReportOperations operations{ reportResidue, reportLegacy, reportLog };
    reported.clear();
    CountPresentOutcome(PresentOutcome::Enter);
    CountPresentOutcome(PresentOutcome::Consumed);
    ReportOriginalResidueProbe(true, operations);
    UNSCOPED_INFO("present outcomes join the shutdown line");
    CHECK((reported.size() == 2 && reported.back().first && reported.back().second == " present_enter=1 present_consumed=1"));

    reported.clear();
    CountPresentOutcome(PresentOutcome::Enter);
    CountPresentOutcome(PresentOutcome::RefusedArgs);
    reportFailure = ReportFailure::LegacyLog;
    ReportOriginalResidueProbe(true, operations);
    reportFailure = ReportFailure::None;
    CountPresentOutcome(PresentOutcome::Enter);
    CountPresentOutcome(PresentOutcome::Consumed);
    reported.clear();
    ReportOriginalResidueProbe(true, operations);
    UNSCOPED_INFO("a failed log retains the present outcomes for the retry");
    CHECK((reported.size() == 2 && reported.back().first && reported.back().second == " present_enter=2 present_consumed=1 present_refused_args=1"));
    UNSCOPED_INFO("the shutdown report drains the present outcomes");
    CHECK(FormatRingStubCounts(TakeRingStubCounts()).empty());
    REXCVAR_SET(native_original_residue_probe, false);
    RefreshOriginalResidueProbeRequested();
}

using CaseTable = std::array<std::pair<const char*, void (*)()>, 13>;

const CaseTable kCases{
    CaseTable::value_type{ "testDisabled", testDisabled },
    CaseTable::value_type{ "testPublishTruthTable", testPublishTruthTable },
    CaseTable::value_type{ "testPublisherRaiseTruthTable", testPublisherRaiseTruthTable },
    CaseTable::value_type{ "testSitesAndFormatter", testSitesAndFormatter },
    CaseTable::value_type{ "testPublishDispatch", testPublishDispatch },
    CaseTable::value_type{ "testConsumedStarvedAndDeferred", testConsumedStarvedAndDeferred },
    CaseTable::value_type{ "testShutdownAndConcurrentDrain", testShutdownAndConcurrentDrain },
    CaseTable::value_type{ "testReportingFailures", testReportingFailures },
    CaseTable::value_type{ "testPeriodicHandoffFailure", testPeriodicHandoffFailure },
    CaseTable::value_type{ "testFinalReportLockDeadline", testFinalReportLockDeadline },
    CaseTable::value_type{ "testCloseAndLateDecisions", testCloseAndLateDecisions },
    CaseTable::value_type{ "testStuckWriterDeadline", testStuckWriterDeadline },
    CaseTable::value_type{ "testPresentOutcomesInShutdown", testPresentOutcomesInShutdown }
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

TEST_CASE("original_residue_probe", "[ctest][original-residue-probe][timeout-60]")
{
    runCases({});
}

// Runs one named case:
//   original-residue-probe.case <testName>
TEST_CASE("original_residue_probe_selected", "[.][manual][original-residue-probe]")
{
    const auto& args = rerevved::tests::CurrentManualArgv();
    REQUIRE(args.argc() == 1);
    UNSCOPED_INFO("an unknown case name");
    REQUIRE(runCases(args[1]) == 1);
}

REREVVED_MANUAL_HELPER(originalResidueProbeCase, "original-residue-probe.case", "original_residue_probe_selected")
