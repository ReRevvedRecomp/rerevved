#include "native_cadence_trace.h"

#if REREVVED_DEVELOPER

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <ostream>
#include <string_view>

namespace rerevved::gpu
{

namespace
{

using Clock = std::chrono::steady_clock;

// Fixed on first use, which OpenNativeCadenceTrace forces before enabling the hooks.
Clock::time_point Epoch() noexcept
{
    static const Clock::time_point epoch = Clock::now();
    return epoch;
}

struct ProcessTrace
{
    std::filesystem::path path;
    NativeCadenceTrace    trace;
    bool                  written = false;
};

std::mutex processMutex;
// Never freed: a hook that loaded the pointer before Close may still be appending.
ProcessTrace*              processTrace = nullptr;
std::atomic<ProcessTrace*> processLive{ nullptr };

void WriteId(std::ostream& out, const char* key, std::uint64_t value)
{
    out << key << " = ";
    if (value == kCadenceUnknown)
        out << "'unknown'";
    else
        out << value;
    out << '\n';
}

void WriteBool(std::ostream& out, const char* key, bool value)
{
    out << key << " = " << (value ? "true" : "false") << '\n';
}

std::string Rate(double value)
{
    char text[40];
    std::snprintf(text, sizeof text, "%.3f", value);
    return text;
}

} // namespace

const char* CadenceBoundaryName(CadenceBoundary boundary) noexcept
{
    switch (boundary)
    {
        case CadenceBoundary::SessionOpen:
            return "session_open";
        case CadenceBoundary::SwapAttempt:
            return "swap_attempt";
        case CadenceBoundary::SwapAssembled:
            return "swap_assembled";
        case CadenceBoundary::Publish:
            return "publish";
        case CadenceBoundary::Present:
            return "present";
        case CadenceBoundary::SessionClose:
            return "session_close";
    }
    return "unknown";
}

bool CadencePresentAttempted(const PresentResult& result) noexcept
{
    return result.outcome != PresentOutcome::NoFrame && result.outcome != PresentOutcome::Suspended;
}

CadenceEvent MakeCadencePresentEvent(std::uint64_t beginNs, std::uint64_t endNs, const PresentResult& result) noexcept
{
    CadenceEvent event;
    event.boundary          = CadenceBoundary::Present;
    event.beginNs           = beginNs;
    event.ns                = endNs;
    event.frameId           = result.frameId;
    event.surfaceGeneration = result.surfaceGeneration == 0 ? kCadenceUnknown : result.surfaceGeneration;
    event.aux               = static_cast<std::uint32_t>(result.nativeCode);
    event.outcome           = cadence::PresentOutcomeName(result.outcome);
    event.repeat            = result.repeat;
    event.newFrame          = result.newFrame;
    event.showsNewFrame     = ShowsNewFrame(result);
    return event;
}

NativeCadenceTrace::NativeCadenceTrace(std::size_t capacity)
: capacity_(capacity)
{
    // One spare slot keeps the close event outside the bound.
    events_.reserve(capacity_ + 1);
}

void NativeCadenceTrace::Record(CadenceEvent event)
{
    std::lock_guard lock(mutex_);
    if (closed_)
        return;
    event.sequence = next_++;
    if (events_.size() >= capacity_)
    {
        ++dropped_;
        return;
    }
    events_.push_back(event);
}

void NativeCadenceTrace::CountIdlePoll() noexcept
{
    idlePolls_.fetch_add(1, std::memory_order_relaxed);
}

void NativeCadenceTrace::Close(std::uint64_t ns)
{
    std::lock_guard lock(mutex_);
    if (closed_)
        return;
    CadenceEvent event;
    event.boundary = CadenceBoundary::SessionClose;
    event.ns       = ns;
    event.outcome  = "closed";
    event.sequence = next_++;
    events_.push_back(event);
    closed_ = true;
}

CadenceSummary NativeCadenceTrace::Summarize() const
{
    std::lock_guard lock(mutex_);
    return summarizeLocked();
}

CadenceSummary NativeCadenceTrace::summarizeLocked() const
{
    CadenceSummary summary;
    summary.events    = events_.size();
    summary.dropped   = dropped_;
    summary.idlePolls = idlePolls_.load(std::memory_order_relaxed);
    summary.complete  = dropped_ == 0;
    if (!events_.empty())
    {
        // The first event opens the window; the close event, or the latest of an open trace, ends it.
        std::uint64_t end = events_.front().ns;
        for (const auto& event : events_)
            end = std::max(end, event.ns);
        summary.windowNs = end - events_.front().ns;
    }
    std::vector<std::uint64_t> shownAt;
    for (const auto& event : events_)
    {
        switch (event.boundary)
        {
            case CadenceBoundary::SwapAttempt:
                ++summary.swapAttempts;
                break;
            case CadenceBoundary::SwapAssembled:
                ++summary.swapsAssembled;
                if (std::string_view(event.outcome) == "failed")
                    ++summary.swapFailed;
                break;
            case CadenceBoundary::Publish:
                ++summary.publications;
                break;
            case CadenceBoundary::Present:
                ++summary.presentResults;
                if (event.showsNewFrame)
                {
                    ++summary.shownFrames;
                    shownAt.push_back(event.ns);
                }
                break;
            default:
                break;
        }
    }
    // Saturating: a dropped arrival can leave more results than arrivals.
    summary.swapsUnfinished = summary.swapAttempts > summary.swapsAssembled ? summary.swapAttempts - summary.swapsAssembled : 0;
    summary.complete        = summary.complete && summary.swapsUnfinished == 0;
    const auto perSecond    = [&](std::uint64_t count)
    {
        return summary.windowNs ? double(count) * 1e9 / double(summary.windowNs) : 0.0;
    };
    summary.swapAttemptRate = perSecond(summary.swapAttempts);
    summary.publicationRate = perSecond(summary.publications);
    summary.shownFrameRate  = perSecond(summary.shownFrames);
    std::vector<std::uint64_t> intervals;
    for (std::size_t i = 1; i < shownAt.size(); ++i)
        intervals.push_back(shownAt[i] >= shownAt[i - 1] ? shownAt[i] - shownAt[i - 1] : 0);
    summary.intervalCount = intervals.size();
    if (!intervals.empty())
    {
        std::sort(intervals.begin(), intervals.end());
        // Nearest rank: the smallest value with at least that percent of the intervals at or below it.
        const auto rank = [&](std::uint64_t percent)
        {
            return intervals[(percent * intervals.size() + 99) / 100 - 1];
        };
        summary.p50Ns = rank(50);
        summary.p95Ns = rank(95);
        summary.p99Ns = rank(99);
    }
    return summary;
}

void NativeCadenceTrace::Write(std::ostream& out) const
{
    std::lock_guard lock(mutex_);
    for (const auto& event : events_)
    {
        out << "[[event]]\nseq = " << event.sequence << "\nboundary = '" << CadenceBoundaryName(event.boundary) << "'\nns = " << event.ns << '\n';
        if (event.boundary == CadenceBoundary::Present)
            WriteId(out, "begin_ns", event.beginNs);
        WriteId(out, "swap_sequence", event.swapSequence);
        WriteId(out, "frame_id", event.frameId);
        WriteId(out, "lease_token", event.leaseToken);
        WriteId(out, "surface_generation", event.surfaceGeneration);
        if (event.boundary == CadenceBoundary::Publish)
            out << "gamma_generation = " << event.aux << '\n';
        if (event.boundary == CadenceBoundary::Present)
        {
            out << "native_code = " << event.aux << '\n';
            WriteBool(out, "repeat", event.repeat);
            WriteBool(out, "new_frame", event.newFrame);
            WriteBool(out, "shows_new_frame", event.showsNewFrame);
        }
        out << "outcome = '" << event.outcome << "'\n";
    }
    const auto s = summarizeLocked();
    out << "[summary]\n# Rates cover the whole trace window, session open to close, including loading and shutdown.\nwindow_ns = " << s.windowNs << "\nevents = " << s.events << "\ncapacity = " << capacity_ << "\ndropped = " << s.dropped << '\n';
    out << "swap_attempts = " << s.swapAttempts << "\nswap_attempt_rate_hz = " << Rate(s.swapAttemptRate) << "\nswaps_assembled = " << s.swapsAssembled << "\nswaps_unfinished = " << s.swapsUnfinished << "\nswap_assembly_failed = " << s.swapFailed << '\n';
    out << "publications = " << s.publications << "\npublication_rate_hz = " << Rate(s.publicationRate) << '\n';
    out << "distinct_shown_frames = " << s.shownFrames << "\ndistinct_shown_frame_rate_hz = " << Rate(s.shownFrameRate) << '\n';
    out << "present_results = " << s.presentResults << "\npresent_polls_without_result = " << s.idlePolls << '\n';
    out << "shown_interval_count = " << s.intervalCount << '\n';
    if (s.intervalCount)
        out << "shown_interval_p50_ns = " << s.p50Ns << "\nshown_interval_p95_ns = " << s.p95Ns << "\nshown_interval_p99_ns = " << s.p99Ns << '\n';
    else
        out << "shown_interval_p50_ns = 'none'\nshown_interval_p95_ns = 'none'\nshown_interval_p99_ns = 'none'\n";
    WriteBool(out, "complete", s.complete);
    out << "verdict = '" << (s.complete ? "complete" : "incomplete: events were dropped or a swap arrival has no assembled result, so counts are lower bounds and shown-frame intervals may span unrecorded frames") << "'\n";
}

std::uint64_t NativeCadenceNowNs() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Epoch()).count());
}

namespace detail
{

void Append(const CadenceEvent& event)
{
    if (auto* live = processLive.load(std::memory_order_acquire))
        live->trace.Record(event);
}

void AppendPresent(std::uint64_t beginNs, const PresentResult& result)
{
    auto* live = processLive.load(std::memory_order_acquire);
    if (!live)
        return;
    if (!CadencePresentAttempted(result))
    {
        live->trace.CountIdlePoll();
        return;
    }
    live->trace.Record(MakeCadencePresentEvent(beginNs, NativeCadenceNowNs(), result));
}

} // namespace detail

bool OpenNativeCadenceTrace(const std::filesystem::path& path, std::string& error)
{
    std::lock_guard lock(processMutex);
    if (processTrace)
    {
        error = "native_cadence_trace is already open";
        return false;
    }
    if (std::filesystem::exists(path))
    {
        error = "native_cadence_trace must name a new file";
        return false;
    }
    // Claim the name now so a bad path fails at startup instead of at close.
    std::ofstream probe(path);
    if (!probe)
    {
        error = "could not open native_cadence_trace";
        return false;
    }
    probe.close();
    Epoch();
    processTrace       = new ProcessTrace;
    processTrace->path = path;
    CadenceEvent open;
    open.boundary = CadenceBoundary::SessionOpen;
    open.ns       = NativeCadenceNowNs();
    open.outcome  = "open";
    processTrace->trace.Record(open);
    processLive.store(processTrace, std::memory_order_release);
    detail::cadenceEnabled.store(true, std::memory_order_relaxed);
    return true;
}

bool CloseNativeCadenceTrace(std::string& error)
{
    std::lock_guard lock(processMutex);
    if (!processTrace || processTrace->written)
        return true;
    detail::cadenceEnabled.store(false, std::memory_order_relaxed);
    processLive.store(nullptr, std::memory_order_release);
    processTrace->trace.Close(NativeCadenceNowNs());
    processTrace->written = true;
    std::ofstream out(processTrace->path, std::ios::trunc);
    if (out)
        processTrace->trace.Write(out);
    out.flush();
    if (!out)
    {
        error = "could not write native_cadence_trace";
        return false;
    }
    return true;
}

} // namespace rerevved::gpu

#endif // REREVVED_DEVELOPER
