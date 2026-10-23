#include "original_residue_probe.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <format>
#include <mutex>
#include <string_view>
#include <thread>

REXCVAR_DEFINE_BOOL(native_original_residue_probe, false, "ReRevved", "Observe Publish and Submit wrapper decisions; or_* fields describe reasons and full64 caller buckets")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{
namespace
{

using Function = OriginalResidueFunction;
using Reason   = OriginalResidueReason;
using Site     = OriginalResidueSite;
using Dispatch = OriginalResidueDispatch;
std::array<std::array<std::array<std::array<std::atomic<std::uint64_t>, kOriginalResidueReasons>, kOriginalResidueSites>, kOriginalResidueFunctions>, kOriginalResidueDispatches> dispatchCells{};
std::atomic<bool>                                                                                                                                                                 explicitDispatch{ false };
std::atomic<bool>                                                                                                                                                                 ringConsume{ false };
using detail::originalResidueAdmissions;
std::atomic<std::uint64_t>                                      internalError{ 0 };
constexpr std::uint64_t                                         kClosed = std::uint64_t{ 1 } << 63;
std::atomic<std::uint64_t>                                      lateDecisions{ 0 }, reportingFailures{ 0 }, skippedFinalReports{ 0 };
std::timed_mutex                                                reportMutex;
OriginalResidueShutdownCounts                                   pendingReport;
constexpr std::array<std::string_view, kOriginalResidueReasons> reasonNames{
    "inactive", "not_ready", "layout_mismatch", "unreadable", "starved", "recording", "tiling", "submit", "publisher_unreadable", "publisher_sink", "publisher_callback", "pending_unreadable", "pending_nonzero", "raise_write_failed", "served_kick", "served_silent", "lock_storage", "token_storage", "token_wrap", "list_storage"
};
constexpr std::array<std::string_view, kOriginalResidueSites> siteNames{
    "8269c61c", "8269c53c", "8269c614", "826ac368", "826ac3b4", "826ac3e8", "other"
};

bool validReason(Function function, Reason reason) noexcept
{
    if (reason <= Reason::LayoutMismatch)
        return true;
    return function == Function::Publish ? reason >= Reason::Unreadable && reason <= Reason::Submit : reason >= Reason::PublisherUnreadable && reason < Reason::Count;
}

void retain(const OriginalResidueCounts& counts) noexcept
{
    auto& pending = pendingReport.residue;
    for (std::size_t f = 0; f < kOriginalResidueFunctions; ++f)
        for (std::size_t s = 0; s < kOriginalResidueSites; ++s)
            for (std::size_t r = 0; r < kOriginalResidueReasons; ++r)
                pending.cells[f][s][r] += counts.cells[f][s][r];
    for (std::size_t d = 0; d < kOriginalResidueDispatches; ++d)
        for (std::size_t f = 0; f < kOriginalResidueFunctions; ++f)
            for (std::size_t s = 0; s < kOriginalResidueSites; ++s)
                for (std::size_t r = 0; r < kOriginalResidueReasons; ++r)
                    pending.dispatch[d][f][s][r] += counts.dispatch[d][f][s][r];
    pending.explicitDispatch = pending.explicitDispatch || counts.explicitDispatch;
    pending.ringConsume      = pending.ringConsume || counts.ringConsume;
    pending.internalError += counts.internalError;
    pending.lateDecisions += counts.lateDecisions;
    pending.reportingFailures += counts.reportingFailures;
    pending.writersAdmitted = counts.writersAdmitted;
    pending.skippedFinalReports += counts.skippedFinalReports;
    pending.closed = pending.closed || counts.closed;
}

void retain(const RingStubCounts& counts) noexcept
{
    for (std::size_t i = 0; i < kRingStubCount; ++i)
    {
        pendingReport.legacy.stubbed[i] += counts.stubbed[i];
        pendingReport.legacy.original[i] += counts.original[i];
    }
    for (std::size_t i = 0; i < kPresentOutcomeCount; ++i)
        pendingReport.legacy.present[i] += counts.present[i];
}

void logReport(const OriginalResidueReportOperations& operations, bool legacy, std::string_view fields, bool shutdown = true)
{
    if (operations.log)
        operations.log(legacy, fields);
    else if (legacy)
        REXLOG_INFO("Native ring stubs: shutdown{}", fields);
    else if (shutdown)
        REXLOG_INFO("Native original residue probe: shutdown{}", fields);
    else
        REXLOG_INFO("Native original residue probe: periodic{}", fields);
}

} // namespace

std::atomic<std::int8_t>   originalResidueProbeState{ -1 };
std::atomic<std::uint64_t> detail::originalResidueAdmissions{ 0 };

bool RefreshOriginalResidueProbeRequested() noexcept
{
    const bool requested = REXCVAR_GET(native_original_residue_probe);
    originalResidueProbeState.store(requested ? 1 : 0, std::memory_order_relaxed);
    return requested;
}

OriginalResidueSite OriginalResidueCaller(Function function, std::uint64_t lr) noexcept
{
    if (function == Function::Publish)
        return lr == 0x000000008269C61CULL ? Site::C61C : Site::Other;
    if (function == Function::Submit)
    {
        switch (lr)
        {
            case 0x000000008269C53CULL:
                return Site::C53C;
            case 0x000000008269C614ULL:
                return Site::C614;
            case 0x00000000826AC368ULL:
                return Site::AC368;
            case 0x00000000826AC3B4ULL:
                return Site::AC3B4;
            case 0x00000000826AC3E8ULL:
                return Site::AC3E8;
            default:
                break;
        }
    }
    return Site::Other;
}

OriginalResidueReason OriginalResidueReadyReason(OriginalResidueReadiness readiness) noexcept
{
    switch (readiness)
    {
        case OriginalResidueReadiness::NotReady:
            return Reason::NotReady;
        case OriginalResidueReadiness::Mismatch:
            return Reason::LayoutMismatch;
        default:
            return Reason::Count;
    }
}

OriginalResidueReason OriginalResiduePublishReason(PublishBranch branch) noexcept
{
    switch (branch)
    {
        case PublishBranch::Unreadable:
            return Reason::Unreadable;
        case PublishBranch::Starved:
            return Reason::Starved;
        case PublishBranch::Recording:
            return Reason::Recording;
        case PublishBranch::Tiling:
            return Reason::Tiling;
        case PublishBranch::Submit:
            return Reason::Submit;
    }
    return Reason::Count;
}

OriginalResidueReason OriginalResidueSubmitReason(const PublisherDiagnostic& publisher, const RaiseDiagnostic& raise, bool raised) noexcept
{
    if (publisher.valid != 7)
        return Reason::PublisherUnreadable;
    if (publisher.sink != 0)
        return Reason::PublisherSink;
    if (!(publisher.high & ring_layout::kModeCpuFence) && publisher.callback != 0)
        return Reason::PublisherCallback;
    if (!raise.pendingValid)
        return Reason::PendingUnreadable;
    if (raise.pending != 0)
        return Reason::PendingNonzero;
    if (!raised)
        return Reason::RaiseWriteFailed;
    return publisher.path == PublisherPath::Kick ? Reason::ServedKick : Reason::ServedSilent;
}

void NoteOriginalResidueDecision(Function function, std::uint64_t lr, Reason reason, Dispatch dispatch) noexcept
{
    auto state = originalResidueAdmissions.load(std::memory_order_relaxed);
    do
    {
        if (state & kClosed)
        {
            lateDecisions.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    } while (!originalResidueAdmissions.compare_exchange_weak(state, state + 1, std::memory_order_acquire, std::memory_order_relaxed));
    if (function >= Function::Count || reason >= Reason::Count || !validReason(function, reason))
    {
        internalError.fetch_add(1, std::memory_order_relaxed);
        originalResidueAdmissions.fetch_sub(1, std::memory_order_release);
        return;
    }
    const auto site     = OriginalResidueCaller(function, lr);
    const bool supplied = dispatch != Dispatch::Classified;
    if (!supplied)
        dispatch = function == Function::Submit && reason == Reason::ServedKick ? Dispatch::ServedKick : function == Function::Submit && reason == Reason::ServedSilent ? Dispatch::ServedSilent
                                                                                                                                                                        : Dispatch::Original;
    const bool validDispatch = (dispatch == Dispatch::Original && reason != Reason::ServedKick && reason != Reason::ServedSilent) ||
                               (function == Function::Publish && ((dispatch == Dispatch::ConsumedTiling && reason == Reason::Tiling) || (dispatch == Dispatch::ConsumedSubmit && reason == Reason::Submit) || (dispatch == Dispatch::ConsumedStarved && reason == Reason::Starved))) ||
                               (function == Function::Submit && ((dispatch == Dispatch::ServedKick && reason == Reason::ServedKick) || (dispatch == Dispatch::ServedSilent && reason == Reason::ServedSilent) || (dispatch == Dispatch::ConsumedDeferred && reason == Reason::PendingNonzero)));
    if (!validDispatch || dispatch >= Dispatch::Count)
    {
        internalError.fetch_add(1, std::memory_order_relaxed);
        originalResidueAdmissions.fetch_sub(1, std::memory_order_release);
        return;
    }
    if (supplied)
        explicitDispatch.store(true, std::memory_order_relaxed);
    dispatchCells[static_cast<std::size_t>(dispatch)][static_cast<std::size_t>(function)][static_cast<std::size_t>(site)][static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    originalResidueAdmissions.fetch_sub(1, std::memory_order_release);
}

void MarkOriginalResidueRingConsume() noexcept
{
    ringConsume.store(true, std::memory_order_relaxed);
}

void CloseOriginalResidueProbe() noexcept
{
    if (!OriginalResidueProbeRequested())
        return;
    originalResidueAdmissions.fetch_or(kClosed, std::memory_order_acq_rel);
    const auto deadline = std::chrono::steady_clock::now() + detail::kOriginalResidueShutdownWait;
    while ((originalResidueAdmissions.load(std::memory_order_acquire) & ~kClosed) != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
}

OriginalResidueCounts TakeOriginalResidueCounts() noexcept
{
    OriginalResidueCounts out;
    // Sample before the exchanges: a writer completing during the drain cannot
    // make a partial snapshot appear complete.
    const auto state    = originalResidueAdmissions.load(std::memory_order_acquire);
    out.closed          = (state & kClosed) != 0;
    out.writersAdmitted = state & ~kClosed;
    for (std::size_t d = 0; d < kOriginalResidueDispatches; ++d)
        for (std::size_t f = 0; f < kOriginalResidueFunctions; ++f)
            for (std::size_t s = 0; s < kOriginalResidueSites; ++s)
                for (std::size_t r = 0; r < kOriginalResidueReasons; ++r)
                {
                    out.dispatch[d][f][s][r] = dispatchCells[d][f][s][r].exchange(0, std::memory_order_relaxed);
                    out.cells[f][s][r] += out.dispatch[d][f][s][r];
                }
    out.explicitDispatch    = explicitDispatch.load(std::memory_order_relaxed);
    out.ringConsume         = ringConsume.load(std::memory_order_relaxed);
    out.internalError       = internalError.exchange(0, std::memory_order_relaxed);
    out.lateDecisions       = lateDecisions.exchange(0, std::memory_order_relaxed);
    out.reportingFailures   = reportingFailures.exchange(0, std::memory_order_relaxed);
    out.skippedFinalReports = skippedFinalReports.exchange(0, std::memory_order_relaxed);
    return out;
}

std::string FormatOriginalResidueCounts(const OriginalResidueCounts& counts)
{
    std::array<std::array<std::uint64_t, kOriginalResidueReasons>, kOriginalResidueFunctions> reasons{};
    std::array<std::uint64_t, kOriginalResidueFunctions>                                      calls{};
    for (std::size_t f = 0; f < kOriginalResidueFunctions; ++f)
        for (std::size_t s = 0; s < kOriginalResidueSites; ++s)
            for (std::size_t r = 0; r < kOriginalResidueReasons; ++r)
            {
                reasons[f][r] += counts.cells[f][s][r];
                calls[f] += counts.cells[f][s][r];
            }
    const auto                                                                                   kick   = reasons[1][static_cast<std::size_t>(Reason::ServedKick)];
    const auto                                                                                   silent = reasons[1][static_cast<std::size_t>(Reason::ServedSilent)];
    std::array<std::array<std::uint64_t, kOriginalResidueFunctions>, kOriginalResidueDispatches> dispatched{};
    for (std::size_t d = 0; d < kOriginalResidueDispatches; ++d)
        for (std::size_t f = 0; f < kOriginalResidueFunctions; ++f)
            for (std::size_t s = 0; s < kOriginalResidueSites; ++s)
                for (std::size_t r = 0; r < kOriginalResidueReasons; ++r)
                    dispatched[d][f] += counts.dispatch[d][f][s][r];
    std::string fields = std::format(" or_scope=wrapper_decision or_publish_calls={} or_publish_original_dispatch={} or_submit_calls={} or_submit_original_dispatch={} or_submit_served_kick={} or_submit_served_silent={} or_internal_error={}", calls[0], dispatched[0][0], calls[1], dispatched[0][1], kick, silent, counts.internalError);
    if (counts.explicitDispatch)
        fields += std::format(" or_publish_consumed_tiling={} or_publish_consumed_submit={}", dispatched[3][0], dispatched[4][0]);
    // Only with native_ring_consume_publish: the PT reporting keeps its fields otherwise.
    if (counts.ringConsume)
        fields += std::format(" or_publish_consumed_starved={} or_submit_consumed_deferred={}", dispatched[5][0], dispatched[6][1]);
    constexpr std::array<std::string_view, 2> functions{ "publish", "submit" };
    fields += std::format(" or_closed={} or_late_decisions={} or_reporting_failures={}", counts.closed ? 1 : 0, counts.lateDecisions, counts.reportingFailures);
    fields += std::format(" or_writers_admitted={} or_final_incomplete={} or_final_skipped={}", counts.writersAdmitted, (counts.closed && counts.writersAdmitted != 0) || counts.skippedFinalReports != 0 ? 1 : 0, counts.skippedFinalReports);
    for (std::size_t f = 0; f < kOriginalResidueFunctions; ++f)
        for (std::size_t r = 0; r < kOriginalResidueReasons; ++r)
        {
            if (reasons[f][r])
                fields += std::format(" or_{}_reason_{}={}", functions[f], reasonNames[r], reasons[f][r]);
            for (std::size_t s = 0; s < kOriginalResidueSites; ++s)
                if (counts.cells[f][s][r])
                    fields += std::format(" or_{}_site_{}_{}={}", functions[f], siteNames[s], reasonNames[r], counts.cells[f][s][r]);
            if (counts.explicitDispatch)
                for (std::size_t s = 0; s < kOriginalResidueSites; ++s)
                    for (std::size_t d = 0; d < kOriginalResidueDispatches; ++d)
                        if (counts.dispatch[d][f][s][r])
                            fields += std::format(" or_{}_site_{}_{}_dispatch_{}={}", functions[f], siteNames[s], reasonNames[r], std::array<std::string_view, 7>{ "original", "served_kick", "served_silent", "consumed_tiling", "consumed_submit", "consumed_starved", "consumed_deferred" }[d], counts.dispatch[d][f][s][r]);
        }
    return fields;
}

OriginalResidueShutdownCounts TakeOriginalResidueShutdownCounts() noexcept
{
    if (!OriginalResidueProbeRequested())
        return {};
    OriginalResidueShutdownCounts out;
    out.residue = TakeOriginalResidueCounts();
    out.legacy  = TakeRingStubCounts();
    return out;
}

std::string ReportOriginalResidueProbe(bool shutdown, const OriginalResidueReportOperations& operations) noexcept
{
    if (!OriginalResidueProbeRequested())
        return {};
    try
    {
        std::unique_lock lock(reportMutex, std::defer_lock);
        if (shutdown)
        {
            if (!lock.try_lock_for(detail::kOriginalResidueShutdownWait))
            {
                skippedFinalReports.fetch_add(1, std::memory_order_relaxed);
                reportingFailures.fetch_add(1, std::memory_order_relaxed);
                // No callbacks or drains while a stalled reporter owns its batch.
                return std::format(" or_final_incomplete=1 or_final_skipped=1 or_writers_admitted={}", originalResidueAdmissions.load(std::memory_order_acquire) & ~kClosed);
            }
        }
        else
            lock.lock();
        if (shutdown)
        {
            const auto counts = TakeOriginalResidueShutdownCounts();
            retain(counts.residue);
            retain(counts.legacy);
            auto       fields       = operations.formatResidue(pendingReport.residue);
            const auto legacyFields = operations.formatLegacy(pendingReport.legacy);
            logReport(operations, false, fields);
            pendingReport.residue = {};
            logReport(operations, true, legacyFields);
            pendingReport.legacy = {};
            return fields;
        }
        retain(TakeOriginalResidueCounts());
        auto fields = operations.formatResidue(pendingReport.residue);
        logReport(operations, false, fields, false);
        pendingReport.residue = {};
        return fields;
    }
    catch (...)
    {
        reportingFailures.fetch_add(1, std::memory_order_relaxed);
    }
    return {};
}

} // namespace rerevved::gpu::guest_d3d
