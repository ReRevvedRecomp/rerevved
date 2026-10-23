#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

struct PPCContext;

namespace rerevved::gpu::guest_d3d
{

// VS observer records Original entry/return and guarded scalars without
// claiming generation, cause, order, equality, or coverage.
inline constexpr std::uint32_t kVsReadbackRefreshEntry = 0x82662D28;
inline constexpr std::uint32_t kVsReadbackCopyEntry    = 0x82694480;
inline constexpr std::size_t   kVsReadbackDetails      = 128;
inline constexpr std::size_t   kVsReadbackActive       = 64;
inline constexpr auto          kVsReadbackShutdownWait = std::chrono::milliseconds(10);

// Full 64-bit link register values of the known callers.
inline constexpr std::uint64_t kVsReadbackRefreshPrepLr       = 0x00000000825E1D30ULL;
inline constexpr std::uint64_t kVsReadbackRefreshDescriptorLr = 0x000000008265E270ULL;
inline constexpr std::uint64_t kVsReadbackQueryLr             = 0x0000000082662D64ULL;
inline constexpr std::uint64_t kVsReadbackCopyLr              = 0x0000000082662DFCULL;

// Big-endian guest bytes; the production reader validates the heap range.
using VsReadbackReader = bool (*)(void*, std::uint32_t, std::uint8_t*, std::size_t);

struct VsReadbackAccess
{
    VsReadbackReader reader  = nullptr;
    void*            context = nullptr;
};

enum class VsReadbackFunction : std::uint8_t
{
    Refresh,
    Readback
};

// A readback entry is a query when its entry destination is zero and a copy
// otherwise, whatever the diagnostic readability of its fields.
enum class VsReadbackKind : std::uint8_t
{
    Refresh,
    Query,
    Copy,
    Count
};
inline constexpr std::size_t kVsReadbackKinds = static_cast<std::size_t>(VsReadbackKind::Count);

enum class VsReadbackCaller : std::uint8_t
{
    Prep,
    Descriptor,
    Other,
    Count
};

enum class VsReadbackQState : std::uint8_t
{
    Dirty,
    Clean,
    Unknown,
    Count
};

enum class VsReadbackParent : std::uint8_t
{
    InRefresh,
    OutsideRefresh,
    Unknown,
    Count
};

enum class VsReadbackParentReason : std::uint8_t
{
    None,
    NoActiveRefresh,
    NotNested,
    ThreadUnknown,
    LrMismatch,
    VsUnknown,
    VsMismatch,
    Untracked,
    Count
};

enum class VsReadbackTerminal : std::uint8_t
{
    Returned,
    Abandoned
};

// Refresh validity bits, shared by entry and exit samples.
inline constexpr std::uint32_t kVsReadbackQVs     = 1U << 0; // Q+0x04
inline constexpr std::uint32_t kVsReadbackQBuffer = 1U << 1; // Q+0x08
inline constexpr std::uint32_t kVsReadbackQFlags  = 1U << 2; // Q+0x14
inline constexpr std::uint32_t kVsReadbackODevice = 1U << 3; // O+0x0C
// Readback validity bits: entry fields, then the query exit size word.
inline constexpr std::uint32_t kVsReadbackM        = 1U << 0; // VS+0x36C
inline constexpr std::uint32_t kVsReadbackS        = 1U << 1; // VS+0x370
inline constexpr std::uint32_t kVsReadbackC        = 1U << 2; // VS+0x20, copy only
inline constexpr std::uint32_t kVsReadbackSizeWord = 1U << 0; // BE32 at the saved size pointer
inline constexpr std::uint32_t kVsReadbackQDirty   = 0x80000000U;

// Virtual interval intersections among the metadata, code, output and size word.
inline constexpr std::uint32_t kVsReadbackOverlapOutputCode = 1U << 0;
inline constexpr std::uint32_t kVsReadbackOverlapOutputMeta = 1U << 1;
inline constexpr std::uint32_t kVsReadbackOverlapOutputSize = 1U << 2;
inline constexpr std::uint32_t kVsReadbackOverlapCodeMeta   = 1U << 3;
inline constexpr std::uint32_t kVsReadbackOverlapCodeSize   = 1U << 4;
inline constexpr std::uint32_t kVsReadbackOverlapMetaSize   = 1U << 5;

// Quality bits of one record. They are not disjoint terminal categories.
inline constexpr std::uint32_t kVsReadbackQualityUnreadable      = 1U << 0;
inline constexpr std::uint32_t kVsReadbackQualityRange           = 1U << 1;
inline constexpr std::uint32_t kVsReadbackQualityOverlap         = 1U << 2;
inline constexpr std::uint32_t kVsReadbackQualityPairError       = 1U << 3;
inline constexpr std::uint32_t kVsReadbackQualityCorrelationDrop = 1U << 4;
inline constexpr std::uint32_t kVsReadbackQualityParentUnknown   = 1U << 5;

// Entry values of one call, sampled before Original. Raw registers keep their
// full 64-bit width; guest pointers are the low 32 bits.
struct VsReadbackEntry
{
    VsReadbackFunction function = VsReadbackFunction::Refresh;
    std::uint32_t      thread   = 0;
    std::uint64_t      sp = 0, lr = 0, r3 = 0, r4 = 0, r5 = 0;
    std::uint32_t      object = 0, q = 0, vs = 0, destination = 0, sizePointer = 0;
    // Refresh: Q+4, Q+8, Q+0x14, O+0x0C. Readback: M, S, C in the first three.
    std::array<std::uint32_t, 4> words{};
    std::uint32_t                valid = 0;
    // Failed scalar read attempts and bytes the reader copied while sampling.
    std::uint32_t unreadable = 0;
    std::uint64_t probeBytes = 0;
};

struct VsReadbackExit
{
    std::uint64_t                result = 0;
    std::uint32_t                thread = 0;
    std::array<std::uint32_t, 4> words{};
    std::uint32_t                valid      = 0;
    std::uint32_t                unreadable = 0;
    std::uint64_t                probeBytes = 0;
};

struct VsReadbackInterval
{
    std::uint64_t begin = 0, length = 0;
    // Valid and empty is a valid extent that takes part in no intersection.
    bool valid = false;
};

struct VsReadbackExtents
{
    VsReadbackInterval metadata{}, code{}, output{}, sizeWord{};
    std::uint32_t      sum32   = 0;
    std::uint64_t      sumWide = 0;
    // Header fields all read; headerValid also requires nonwrapping data intervals.
    bool          headerComplete = false, headerValid = false;
    bool          rangeInvalid = false;
    std::uint32_t overlap      = 0;
};

// Pure value arithmetic over the sampled entry; it reads no guest memory.
VsReadbackExtents AnalyzeVsReadbackCopy(const VsReadbackEntry& entry) noexcept;
VsReadbackKind    VsReadbackKindOf(const VsReadbackEntry& entry) noexcept;
VsReadbackCaller  VsReadbackRefreshCaller(std::uint64_t lr) noexcept;

// Everything one finished call retains. Pending calls have no record.
struct VsReadbackDetail
{
    VsReadbackKind         kind     = VsReadbackKind::Refresh;
    VsReadbackTerminal     terminal = VsReadbackTerminal::Returned;
    VsReadbackParent       parent   = VsReadbackParent::Unknown;
    VsReadbackParentReason reason   = VsReadbackParentReason::None;
    std::uint64_t          call = 0, parentCall = 0;
    VsReadbackEntry        entry{};
    VsReadbackExit         exit{};
    VsReadbackExtents      extents{};
    std::uint32_t          quality = 0;
};

struct VsReadbackCounts
{
    std::array<std::uint64_t, kVsReadbackKinds> enter{}, returned{}, abandon{};
    std::array<std::uint64_t, 3>                refreshCaller{};
    std::array<std::uint64_t, 3>                refreshQ{};
    std::uint64_t                               refreshNegativeResult = 0;
    // Index by VsReadbackParent for the query and copy kinds.
    std::array<std::uint64_t, 3> queryParent{}, copyParent{};
    std::uint64_t                querySizeValid = 0, copyHeaderValid = 0;
    std::uint64_t                requestedMetaBytes = 0, requestedCodeBytes = 0, probeBytes = 0;
    std::uint64_t                unreadable = 0, rangeOverflow = 0, overlap = 0, pairError = 0;
    std::uint64_t                correlationDrop = 0, eventDrop = 0, reportingFailures = 0, finalSkipped = 0;
};

struct VsReadbackPendingRefresh
{
    std::uint64_t call = 0, sp = 0;
    std::uint32_t thread = 0, vs = 0;
    bool          vsValid = false;
};

struct VsReadbackReport
{
    // Interval counts reset at Take; cumulative totals and pending state survive it.
    VsReadbackCounts                                        counts{};
    std::array<std::uint64_t, kVsReadbackKinds>             enterTotal{}, returnTotal{}, abandonTotal{}, pending{};
    std::array<VsReadbackDetail, kVsReadbackDetails>        details{};
    std::size_t                                             detailCount = 0;
    std::array<VsReadbackPendingRefresh, kVsReadbackActive> pendingRefresh{};
    std::size_t                                             pendingRefreshCount = 0;
    std::uint64_t                                           untrackedRefresh    = 0;
    // False when a bounded final lock was not acquired: the report is empty.
    bool complete = true;
};

struct VsReadbackTokenState
{
    VsReadbackEntry        entry{};
    VsReadbackKind         kind   = VsReadbackKind::Refresh;
    VsReadbackParent       parent = VsReadbackParent::Unknown;
    VsReadbackParentReason reason = VsReadbackParentReason::None;
    VsReadbackExtents      extents{};
    std::uint64_t          call = 0, parentCall = 0;
    std::uint32_t          quality = 0;
    std::size_t            slot = 0, parentSlot = 0;
    bool                   counted = false, tracked = false, done = false;
};

// One live VS call owns one move-only token; moved-from tokens finish and
// cannot end another call.
struct VsReadbackToken : VsReadbackTokenState
{
    VsReadbackToken()                                  = default;
    VsReadbackToken(const VsReadbackToken&)            = delete;
    VsReadbackToken& operator=(const VsReadbackToken&) = delete;

    VsReadbackToken(VsReadbackToken&& other) noexcept
    : VsReadbackTokenState(other)
    {
        other.Release();
    }

    // Assignment would discard a live call without abandoning it.
    VsReadbackToken& operator=(VsReadbackToken&&) = delete;

private:
    void Release() noexcept
    {
        done    = true;
        counted = false;
        tracked = false;
    }
};

// Fixed storage and value-level recording only; it never reads guest memory.
class VsReadbackProbe
{
public:
    VsReadbackToken Begin(const VsReadbackEntry& entry) noexcept;
    void            End(VsReadbackToken& token, const VsReadbackExit& exit) noexcept;
    void            Abandon(VsReadbackToken& token) noexcept;
    // A shutdown Take waits a bounded time for the recorder lock.
    VsReadbackReport Take(bool shutdown = false) noexcept;
    // Returns an already taken batch whose handoff failed. A bounded call waits
    // at most the shutdown wait for the recorder lock and returns false when it
    // could not keep the batch.
    bool Retain(const VsReadbackCounts& counts, bool bounded = false) noexcept;
    void NoteReportingFailure() noexcept;
    // A final report that did not acquire a lock; the next report carries the count.
    void NoteFinalSkipped() noexcept;

private:
    friend struct VsReadbackProbeTestAccess;

    struct Slot
    {
        bool          active = false;
        std::uint64_t call = 0, sp = 0;
        std::uint32_t thread = 0, vs = 0, children = 0;
        bool          vsValid = false;
    };

    void Finish(VsReadbackToken& token, const VsReadbackExit* exit, VsReadbackTerminal terminal) noexcept;
    void Classify(VsReadbackToken& token) noexcept;
    void Fill(VsReadbackReport& report, VsReadbackCounts& taken) noexcept;

    std::timed_mutex                                 mutex_;
    VsReadbackCounts                                 counts_{};
    VsReadbackCounts                                 retained_{};
    std::array<std::uint64_t, kVsReadbackKinds>      enterTotal_{}, returnTotal_{}, abandonTotal_{}, pending_{};
    std::array<VsReadbackDetail, kVsReadbackDetails> details_{};
    std::size_t                                      detailCount_ = 0;
    std::array<Slot, kVsReadbackActive>              slots_{};
    std::uint64_t                                    untracked_ = 0, call_ = 0;
    std::atomic<std::uint64_t>                       reportingFailures_{ 0 }, finalSkipped_{ 0 };
};

extern std::atomic<std::int8_t> vsReadbackProbeState;
bool                            RefreshVsReadbackProbeRequested() noexcept;

inline bool VsReadbackProbeRequested() noexcept
{
    const auto state = vsReadbackProbeState.load(std::memory_order_relaxed);
    return state < 0 ? RefreshVsReadbackProbeRequested() : state != 0;
}

VsReadbackProbe& PublishedVsReadbackProbe() noexcept;
// Test seam: scopes that name no access read through this one. Null restores
// the production reader.
void SetVsReadbackAccess(const VsReadbackAccess* access) noexcept;

// The reader-level entries sample the named fields through access (null reader:
// production) and record through probe. None writes guest memory.
VsReadbackToken  BeginVsReadbackProbe(VsReadbackProbe& probe, VsReadbackFunction function, const PPCContext& context, const VsReadbackAccess& access = {}) noexcept;
void             EndVsReadbackProbe(VsReadbackProbe& probe, VsReadbackToken& token, const PPCContext& context, const VsReadbackAccess& access = {}) noexcept;
void             AbandonVsReadbackProbe(VsReadbackProbe& probe, VsReadbackToken& token) noexcept;
VsReadbackReport TakeVsReadbackReport(bool shutdown = false) noexcept;

// Surrounds exactly one Original. End is called after a normal return; any
// other exit abandons the call, so an unwind is never a return.
class VsReadbackScope
{
public:
    VsReadbackScope(VsReadbackFunction function, const PPCContext& context) noexcept;
    ~VsReadbackScope();
    VsReadbackScope(const VsReadbackScope&)            = delete;
    VsReadbackScope& operator=(const VsReadbackScope&) = delete;
    void             End(const PPCContext& context) noexcept;

private:
    VsReadbackProbe* probe_;
    VsReadbackToken  token_;
};

std::string FormatVsReadbackCounts(const VsReadbackReport& report);

struct VsReadbackReportOperations
{
    // Null uses the published probe.
    VsReadbackProbe* probe = nullptr;
    // Null uses the runtime logger. Callbacks may throw; the batch is retained.
    void (*log)(bool detail, std::string_view line) = nullptr;
};

// VS timing report waits boundedly for locks and retains skipped count on
// incomplete final output.
std::string ReportVsReadbackProbe(bool shutdown = false, const VsReadbackReportOperations& operations = {}) noexcept;

} // namespace rerevved::gpu::guest_d3d
