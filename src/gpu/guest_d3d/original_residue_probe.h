#pragma once

#include "ring_stubs.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rerevved::gpu::guest_d3d
{

enum class OriginalResidueFunction : std::uint8_t
{
    Publish,
    Submit,
    Count,
};

enum class OriginalResidueSite : std::uint8_t
{
    C61C,
    C53C,
    C614,
    AC368,
    AC3B4,
    AC3E8,
    Other,
    Count,
};

enum class OriginalResidueReason : std::uint8_t
{
    Inactive,
    NotReady,
    LayoutMismatch,
    Unreadable,
    Starved,
    Recording,
    Tiling,
    Submit,
    PublisherUnreadable,
    PublisherSink,
    PublisherCallback,
    PendingUnreadable,
    PendingNonzero,
    RaiseWriteFailed,
    ServedKick,
    ServedSilent,
    LockStorage,
    TokenStorage,
    TokenWrap,
    ListStorage,
    Count,
};

enum class OriginalResidueReadiness : std::uint8_t
{
    Cached,
    Ok,
    NotReady,
    Mismatch,
};

inline constexpr std::size_t kOriginalResidueFunctions = static_cast<std::size_t>(OriginalResidueFunction::Count);
inline constexpr std::size_t kOriginalResidueSites     = static_cast<std::size_t>(OriginalResidueSite::Count);
inline constexpr std::size_t kOriginalResidueReasons   = static_cast<std::size_t>(OriginalResidueReason::Count);
using OriginalResidueMatrix                            = std::array<std::array<std::array<std::uint64_t, kOriginalResidueReasons>, kOriginalResidueSites>, kOriginalResidueFunctions>;

enum class OriginalResidueDispatch : std::uint8_t
{
    Original,
    ServedKick,
    ServedSilent,
    ConsumedTiling,
    ConsumedSubmit,
    ConsumedStarved,
    ConsumedDeferred,
    Count,
    Classified = Count,
};
inline constexpr std::size_t kOriginalResidueDispatches = static_cast<std::size_t>(OriginalResidueDispatch::Count);

struct OriginalResidueCounts
{
    OriginalResidueMatrix                                         cells{};
    std::array<OriginalResidueMatrix, kOriginalResidueDispatches> dispatch{};
    bool                                                          explicitDispatch    = false;
    bool                                                          ringConsume         = false;
    std::uint64_t                                                 internalError       = 0;
    std::uint64_t                                                 lateDecisions       = 0;
    std::uint64_t                                                 reportingFailures   = 0;
    std::uint64_t                                                 writersAdmitted     = 0;
    std::uint64_t                                                 skippedFinalReports = 0;
    bool                                                          closed              = false;
};

namespace detail
{

inline constexpr auto kOriginalResidueShutdownWait = std::chrono::milliseconds(10);
// Exposed so tests can model a writer suspended after admission.
extern std::atomic<std::uint64_t> originalResidueAdmissions;

} // namespace detail

extern std::atomic<std::int8_t> originalResidueProbeState;
bool                            RefreshOriginalResidueProbeRequested() noexcept;

inline bool OriginalResidueProbeRequested() noexcept
{
    const auto state = originalResidueProbeState.load(std::memory_order_relaxed);
    return state < 0 ? RefreshOriginalResidueProbeRequested() : state != 0;
}

OriginalResidueSite   OriginalResidueCaller(OriginalResidueFunction function, std::uint64_t lr) noexcept;
OriginalResidueReason OriginalResidueReadyReason(OriginalResidueReadiness readiness) noexcept;
OriginalResidueReason OriginalResiduePublishReason(PublishBranch branch) noexcept;
OriginalResidueReason OriginalResidueSubmitReason(const PublisherDiagnostic& publisher, const RaiseDiagnostic& raise, bool raised) noexcept;
// One terminal wrapper decision, before Original dispatch or the served return.
// No guest access, allocation, logging or locks; invalid enums count an error.
void NoteOriginalResidueDecision(OriginalResidueFunction function, std::uint64_t lr, OriginalResidueReason reason, OriginalResidueDispatch dispatch = OriginalResidueDispatch::Classified) noexcept;
// native_ring_consume_publish is requested: the report appends its dispatch fields.
void                  MarkOriginalResidueRingConsume() noexcept;
OriginalResidueCounts TakeOriginalResidueCounts() noexcept;
std::string           FormatOriginalResidueCounts(const OriginalResidueCounts& counts);
// Closes admission with a bounded wait; remaining writers mark the final batch
// incomplete. Later decisions increment lateDecisions, sampled by each drain.
void CloseOriginalResidueProbe() noexcept;

struct OriginalResidueShutdownCounts
{
    OriginalResidueCounts residue;
    RingStubCounts        legacy;
};

// Final callers close probe admission first; guest stragglers may still run.
OriginalResidueShutdownCounts TakeOriginalResidueShutdownCounts() noexcept;

struct OriginalResidueReportOperations
{
    std::string (*formatResidue)(const OriginalResidueCounts&) = FormatOriginalResidueCounts;
    std::string (*formatLegacy)(const RingStubCounts&)         = FormatRingStubCounts;
    // Null uses the runtime logger. Callbacks may throw; unreported counts persist.
    void (*log)(bool legacy, std::string_view fields) = nullptr;
};

// Logs before clearing either periodic or final batches. A busy final reporter
// returns an incomplete marker and retains a skipped-report count for retry.
std::string ReportOriginalResidueProbe(bool shutdown = false, const OriginalResidueReportOperations& operations = {}) noexcept;

} // namespace rerevved::gpu::guest_d3d
