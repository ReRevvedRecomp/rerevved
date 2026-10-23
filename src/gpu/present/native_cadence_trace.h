#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "native_presentation.h"

// The cadence trace is a developer recorder; a shipping build has no hooks.
#if REREVVED_DEVELOPER

namespace rerevved::gpu
{

// Cadence trace keeps swap attempts, output publications, and distinct shown
// frames separate; unknown links stay unknown.
inline constexpr std::uint64_t kCadenceUnknown       = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::size_t   kCadenceTraceCapacity = 65536;

enum class CadenceBoundary : std::uint8_t
{
    SessionOpen,
    SwapAttempt,
    SwapAssembled,
    Publish,
    Present,
    SessionClose
};

const char* CadenceBoundaryName(CadenceBoundary boundary) noexcept;

struct CadenceEvent
{
    std::uint64_t   sequence = 0; // assigned by the trace, in append order
    CadenceBoundary boundary = CadenceBoundary::SessionOpen;
    // Nanoseconds since the process epoch; beginNs is the start of a present attempt.
    std::uint64_t ns      = 0;
    std::uint64_t beginNs = kCadenceUnknown;
    // Guest swap sequence, present frame ID and output lease token are three
    // identity domains; each boundary fills only the ones its owner holds.
    std::uint64_t swapSequence      = kCadenceUnknown;
    std::uint64_t frameId           = kCadenceUnknown;
    std::uint64_t leaseToken        = kCadenceUnknown;
    std::uint64_t surfaceGeneration = kCadenceUnknown;
    // Publish: gamma generation. Present: native code.
    std::uint64_t aux = 0;
    // Static string: assembler status name or present outcome name.
    const char* outcome       = "";
    bool        repeat        = false;
    bool        newFrame      = false;
    bool        showsNewFrame = false;
};

struct CadenceSummary
{
    std::uint64_t windowNs = 0;
    std::uint64_t events   = 0;
    std::uint64_t dropped  = 0;
    // Arrivals at swap entry, counted before the draw drain and the sequence.
    std::uint64_t swapAttempts = 0;
    // Assembled results; an arrival without one at close is unfinished.
    std::uint64_t swapsAssembled  = 0;
    std::uint64_t swapsUnfinished = 0;
    std::uint64_t swapFailed      = 0;
    std::uint64_t publications    = 0;
    std::uint64_t presentResults  = 0;
    std::uint64_t shownFrames     = 0;
    // Present polls that returned no frame or a suspended surface; not stored.
    std::uint64_t idlePolls = 0;
    // Rates over the whole trace window, in events per second: session open to
    // close, so loading and shutdown dilute them.
    double swapAttemptRate = 0, publicationRate = 0, shownFrameRate = 0;
    // Intervals between consecutive distinct shown frames, nearest rank.
    std::uint64_t intervalCount = 0;
    std::uint64_t p50Ns = 0, p95Ns = 0, p99Ns = 0;
    // False once any event was dropped or any swap arrival has no assembled
    // result: counts are lower bounds and an interval may span frames that
    // were never recorded.
    bool complete = true;
};

class NativeCadenceTrace
{
public:
    explicit NativeCadenceTrace(std::size_t capacity = kCadenceTraceCapacity);

    // Appends under a short mutex; a full trace counts the event as dropped.
    void Record(CadenceEvent event);
    void CountIdlePoll() noexcept;
    // Appends the close event outside the capacity and refuses later events.
    void Close(std::uint64_t ns);

    CadenceSummary Summarize() const;
    void           Write(std::ostream& out) const;

private:
    CadenceSummary summarizeLocked() const;

    mutable std::mutex         mutex_;
    std::vector<CadenceEvent>  events_;
    std::size_t                capacity_;
    std::uint64_t              next_    = 0;
    std::uint64_t              dropped_ = 0;
    std::atomic<std::uint64_t> idlePolls_{ 0 };
    bool                       closed_ = false;
};

// NoFrame and Suspended pumps present nothing; the trace counts them instead.
bool CadencePresentAttempted(const PresentResult& result) noexcept;
// Frame ID, surface generation and the new-frame verdict come from the result;
// the lease token stays unknown because PresentResult does not carry one.
CadenceEvent MakeCadencePresentEvent(std::uint64_t beginNs, std::uint64_t endNs, const PresentResult& result) noexcept;

namespace detail
{

inline std::atomic<bool> cadenceEnabled{ false };

void Append(const CadenceEvent& event);
void AppendPresent(std::uint64_t beginNs, const PresentResult& result);

} // namespace detail

// Hooks cost one relaxed load when native_cadence_trace is empty.
inline bool NativeCadenceEnabled() noexcept
{
    return detail::cadenceEnabled.load(std::memory_order_relaxed);
}

// Nanoseconds since the process-local steady_clock epoch.
std::uint64_t NativeCadenceNowNs() noexcept;

// Creates the process trace and its new file; the file is written by Close.
bool OpenNativeCadenceTrace(const std::filesystem::path& path, std::string& error);
// Appends the close event, writes the file and disables the hooks. Idempotent.
bool CloseNativeCadenceTrace(std::string& error);

namespace cadence
{

// Zero when off, so a caller can capture a start time unconditionally.
inline std::uint64_t Now() noexcept
{
    return NativeCadenceEnabled() ? NativeCadenceNowNs() : 0;
}

// Records a swap arrival at entry, before the draw drain, so a swap stalled in
// the drain at close is still counted. The sequence does not exist yet and
// stays unknown; SwapAssembled carries it.
inline void SwapArrived()
{
    if (!NativeCadenceEnabled())
        return;
    CadenceEvent event;
    event.boundary = CadenceBoundary::SwapAttempt;
    event.ns       = NativeCadenceNowNs();
    event.outcome  = "arrived";
    detail::Append(event);
}

// For a caller that already tested NativeCadenceEnabled and wants one load.
inline void RecordSwapAssembled(std::uint64_t swapSequence, const char* status)
{
    CadenceEvent event;
    event.boundary     = CadenceBoundary::SwapAssembled;
    event.ns           = NativeCadenceNowNs();
    event.swapSequence = swapSequence;
    event.outcome      = status;
    detail::Append(event);
}

inline void SwapAssembled(std::uint64_t swapSequence, const char* status)
{
    if (!NativeCadenceEnabled())
        return;
    RecordSwapAssembled(swapSequence, status);
}

// The assembler does not know which swap produced a lease, so the swap link stays unknown.
inline void Publish(std::uint64_t leaseToken, std::uint64_t gammaGeneration)
{
    if (!NativeCadenceEnabled())
        return;
    CadenceEvent event;
    event.boundary   = CadenceBoundary::Publish;
    event.ns         = NativeCadenceNowNs();
    event.leaseToken = leaseToken;
    event.aux        = gammaGeneration;
    event.outcome    = "published";
    detail::Append(event);
}

inline const char* PresentOutcomeName(PresentOutcome outcome) noexcept
{
    switch (outcome)
    {
        case PresentOutcome::Presented:
            return "presented";
        case PresentOutcome::Occluded:
            return "occluded";
        case PresentOutcome::NoFrame:
            return "no_frame";
        case PresentOutcome::Suspended:
            return "suspended";
        case PresentOutcome::OutOfDate:
            return "out_of_date";
        case PresentOutcome::DeviceLost:
            return "device_lost";
        case PresentOutcome::Timeout:
            return "timeout";
        case PresentOutcome::Failed:
            return "failed";
    }
    return "unknown";
}

// Called with every pump result before NativeAppTrace deduplicates it. A poll
// that presented nothing is only counted, so an idle canvas cannot fill the trace.
inline void Present(std::uint64_t beginNs, const PresentResult& result)
{
    if (!NativeCadenceEnabled())
        return;
    detail::AppendPresent(beginNs, result);
}

} // namespace cadence

} // namespace rerevved::gpu

#endif // REREVVED_DEVELOPER
