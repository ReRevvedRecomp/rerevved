// CPU-only model of the native cadence trace: what counts as a swap attempt, a
// publication and a distinct shown frame, bounded storage, and percentiles.
#include "gpu/present/native_cadence_trace.h"
#include "process_id.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

using namespace rerevved::gpu;

namespace
{

constexpr std::uint64_t kMs = 1000000;

PresentResult Result(PresentOutcome outcome, std::uint64_t frameId, bool repeat = false, bool newFrame = false)
{
    PresentResult result;
    result.outcome  = outcome;
    result.frameId  = frameId;
    result.repeat   = repeat;
    result.newFrame = newFrame;
    return result;
}

void Present(NativeCadenceTrace& trace, std::uint64_t atMs, const PresentResult& result)
{
    trace.Record(MakeCadencePresentEvent((atMs - 1) * kMs, atMs * kMs, result));
}

void Open(NativeCadenceTrace& trace)
{
    CadenceEvent open;
    open.boundary = CadenceBoundary::SessionOpen;
    open.outcome  = "open";
    trace.Record(open);
}

// An arrival at swap entry: the sequence does not exist yet.
void SwapArrived(NativeCadenceTrace& trace, std::uint64_t atMs)
{
    CadenceEvent event;
    event.boundary = CadenceBoundary::SwapAttempt;
    event.ns       = atMs * kMs;
    event.outcome  = "arrived";
    trace.Record(event);
}

void SwapAssembled(NativeCadenceTrace& trace, std::uint64_t sequence, std::uint64_t atMs, const char* status)
{
    CadenceEvent event;
    event.boundary     = CadenceBoundary::SwapAssembled;
    event.ns           = atMs * kMs;
    event.swapSequence = sequence;
    event.outcome      = status;
    trace.Record(event);
}

void Publish(NativeCadenceTrace& trace, std::uint64_t lease, std::uint64_t atMs)
{
    CadenceEvent event;
    event.boundary   = CadenceBoundary::Publish;
    event.ns         = atMs * kMs;
    event.leaseToken = lease;
    event.outcome    = "published";
    trace.Record(event);
}

std::string Text(const NativeCadenceTrace& trace)
{
    std::ostringstream out;
    trace.Write(out);
    return out.str();
}

bool Has(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

// The [[event]] table with this sequence number.
std::string EventBlock(const std::string& text, std::uint64_t seq)
{
    const auto start = text.find("seq = " + std::to_string(seq) + "\n");
    UNSCOPED_INFO("event " + std::to_string(seq) + " is written");
    REQUIRE(start != std::string::npos);
    const auto end = text.find("[[event]]", start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

void TestShownFrameRules()
{
    NativeCadenceTrace trace;
    Open(trace);
    Present(trace, 100, Result(PresentOutcome::Presented, 5, false, true));
    Present(trace, 116, Result(PresentOutcome::Presented, 5, true));
    // The idle canvas presents frame zero with overlays; it is not a title frame.
    Present(trace, 132, Result(PresentOutcome::Presented, 0));
    Present(trace, 148, Result(PresentOutcome::Occluded, 6, false, true));
    // The repaint after occlusion is the first successful presentation of frame 6.
    Present(trace, 164, Result(PresentOutcome::Presented, 6));
    Present(trace, 180, Result(PresentOutcome::Presented, 6, true));
    Present(trace, 196, Result(PresentOutcome::OutOfDate, 6));
    trace.Close(1000 * kMs);
    const auto s = trace.Summarize();
    UNSCOPED_INFO("every attempted present is stored");
    REQUIRE(s.presentResults == 7);
    UNSCOPED_INFO("repeat, idle frame zero, occluded and out-of-date presents are not distinct shown frames");
    REQUIRE(s.shownFrames == 2);
    UNSCOPED_INFO("two shown frames make one interval");
    REQUIRE(s.intervalCount == 1);
    UNSCOPED_INFO("the first repaint after occlusion counts once, 64 ms after frame 5");
    REQUIRE((s.p50Ns == 64 * kMs && s.p95Ns == 64 * kMs && s.p99Ns == 64 * kMs));
    UNSCOPED_INFO("the window runs from open to close");
    REQUIRE(s.windowNs == 1000 * kMs);
    UNSCOPED_INFO("two shown frames in one second");
    REQUIRE(s.shownFrameRate == 2.0);
    UNSCOPED_INFO("presents are neither swaps nor publications");
    REQUIRE((s.swapAttempts == 0 && s.publications == 0));
    UNSCOPED_INFO("nothing was dropped");
    REQUIRE((s.complete && s.dropped == 0));

    UNSCOPED_INFO("a no-frame poll is not a present attempt");
    REQUIRE(!CadencePresentAttempted(Result(PresentOutcome::NoFrame, 0)));
    UNSCOPED_INFO("a suspended poll is not a present attempt");
    REQUIRE(!CadencePresentAttempted(Result(PresentOutcome::Suspended, 0)));
    UNSCOPED_INFO("a failed present is an attempt");
    REQUIRE(CadencePresentAttempted(Result(PresentOutcome::Failed, 0)));

    const auto text   = Text(trace);
    const auto repeat = EventBlock(text, 2);
    UNSCOPED_INFO("the repeat is recorded as a repeat of frame 5");
    REQUIRE((Has(repeat, "repeat = true") && Has(repeat, "shows_new_frame = false") && Has(repeat, "frame_id = 5")));
    UNSCOPED_INFO("the repaint shows frame 6 once");
    REQUIRE((Has(EventBlock(text, 5), "shows_new_frame = true") && Has(EventBlock(text, 5), "outcome = 'presented'")));
    UNSCOPED_INFO("the occluded present keeps its classification");
    REQUIRE(Has(EventBlock(text, 4), "outcome = 'occluded'"));
}

void TestFailedAssembly()
{
    NativeCadenceTrace trace;
    Open(trace);
    SwapArrived(trace, 10);
    SwapAssembled(trace, 1, 11, "failed");
    SwapArrived(trace, 20);
    SwapAssembled(trace, 2, 21, "complete");
    trace.Close(100 * kMs);
    const auto s = trace.Summarize();
    UNSCOPED_INFO("a failed assembly still counts as a swap attempt");
    REQUIRE(s.swapAttempts == 2);
    UNSCOPED_INFO("arrival and result pairs leave nothing unfinished");
    REQUIRE((s.swapsAssembled == 2 && s.swapsUnfinished == 0 && s.complete));
    UNSCOPED_INFO("the file reports no unfinished swap");
    REQUIRE((Has(Text(trace), "swaps_unfinished = 0\n") && Has(Text(trace), "verdict = 'complete'")));
    UNSCOPED_INFO("one assembly failed");
    REQUIRE(s.swapFailed == 1);
    UNSCOPED_INFO("no assembly result is a publication or a shown frame");
    REQUIRE((s.publications == 0 && s.shownFrames == 0));
    UNSCOPED_INFO("two attempts in a 100 ms window");
    REQUIRE(s.swapAttemptRate == 20.0);
}

void TestUnfinishedSwap()
{
    NativeCadenceTrace trace;
    Open(trace);
    SwapArrived(trace, 10);
    SwapAssembled(trace, 1, 11, "complete");
    // Stalled in the draw drain when the trace closes: arrived, never assembled.
    SwapArrived(trace, 20);
    trace.Close(100 * kMs);
    const auto s = trace.Summarize();
    UNSCOPED_INFO("a swap with no result at close is an attempt and unfinished");
    REQUIRE((s.swapAttempts == 2 && s.swapsAssembled == 1 && s.swapsUnfinished == 1));
    UNSCOPED_INFO("an unfinished swap makes the trace incomplete without any drop");
    REQUIRE((s.dropped == 0 && !s.complete));
    const auto text = Text(trace);
    UNSCOPED_INFO("the file names the unfinished swap and the incomplete verdict");
    REQUIRE((Has(text, "swap_attempts = 2\n") && Has(text, "swaps_assembled = 1\n") && Has(text, "swaps_unfinished = 1\n") && Has(text, "complete = false\n") && Has(text, "verdict = 'incomplete")));
    const auto arrival = EventBlock(text, 3);
    UNSCOPED_INFO("the arrival keeps its sequence unknown");
    REQUIRE((Has(arrival, "boundary = 'swap_attempt'") && Has(arrival, "swap_sequence = 'unknown'") && Has(arrival, "outcome = 'arrived'")));
}

void TestUnknownLinks()
{
    NativeCadenceTrace trace;
    Open(trace);
    // Swap sequence 7, lease token 7 and frame ID 7 are numerically equal and still unrelated.
    SwapArrived(trace, 9);
    SwapAssembled(trace, 7, 10, "complete");
    Publish(trace, 7, 12);
    Present(trace, 14, Result(PresentOutcome::Presented, 7, false, true));
    trace.Close(20 * kMs);
    const auto text    = Text(trace);
    const auto attempt = EventBlock(text, 2);
    UNSCOPED_INFO("a swap names only its sequence");
    REQUIRE((Has(attempt, "swap_sequence = 7") && Has(attempt, "frame_id = 'unknown'") && Has(attempt, "lease_token = 'unknown'") && Has(attempt, "surface_generation = 'unknown'")));
    const auto publish = EventBlock(text, 3);
    UNSCOPED_INFO("a publication names only its lease token");
    REQUIRE((Has(publish, "lease_token = 7") && Has(publish, "swap_sequence = 'unknown'") && Has(publish, "frame_id = 'unknown'")));
    const auto present = EventBlock(text, 4);
    UNSCOPED_INFO("a present names only its frame and an absent generation stays unknown");
    REQUIRE((Has(present, "frame_id = 7") && Has(present, "swap_sequence = 'unknown'") && Has(present, "lease_token = 'unknown'") && Has(present, "surface_generation = 'unknown'")));

    PresentResult generation     = Result(PresentOutcome::Presented, 8, false, true);
    generation.surfaceGeneration = 3;
    const auto event             = MakeCadencePresentEvent(0, kMs, generation);
    UNSCOPED_INFO("a known surface generation is carried");
    REQUIRE(event.surfaceGeneration == 3);

    // A swap with no frame and a frame with no swap are both legal.
    const auto s = trace.Summarize();
    UNSCOPED_INFO("the three counts stay independent");
    REQUIRE((s.swapAttempts == 1 && s.publications == 1 && s.shownFrames == 1));
}

void TestOverflow()
{
    NativeCadenceTrace trace(4);
    Open(trace);
    for (std::uint64_t sequence = 1; sequence <= 10; ++sequence)
        SwapArrived(trace, sequence);
    trace.Close(50 * kMs);
    const auto s = trace.Summarize();
    UNSCOPED_INFO("ten attempts into three free slots drop seven");
    REQUIRE(s.dropped == 7);
    UNSCOPED_INFO("only stored attempts are counted");
    REQUIRE(s.swapAttempts == 3);
    UNSCOPED_INFO("open, three attempts and the close event outside the bound");
    REQUIRE(s.events == 5);
    UNSCOPED_INFO("a drop makes the trace incomplete");
    REQUIRE(!s.complete);
    const auto text = Text(trace);
    UNSCOPED_INFO("the summary names the drop and the incomplete verdict");
    REQUIRE((Has(text, "dropped = 7\n") && Has(text, "complete = false\n") && Has(text, "verdict = 'incomplete")));
    UNSCOPED_INFO("dropped events still consume sequence numbers, so the gap is visible");
    REQUIRE(Has(EventBlock(text, 11), "boundary = 'session_close'"));

    NativeCadenceTrace whole(4);
    Open(whole);
    whole.Close(kMs);
    UNSCOPED_INFO("an undropped trace says complete");
    REQUIRE(Has(Text(whole), "verdict = 'complete'"));
    whole.Record(CadenceEvent{});
    UNSCOPED_INFO("events after close are refused");
    REQUIRE(whole.Summarize().events == 2);
}

void TestPercentiles()
{
    // Unsorted intervals of 10..100 ms: nearest rank 5 of 10, then 10, then 10.
    {
        NativeCadenceTrace trace;
        Open(trace);
        std::uint64_t at = 1;
        Present(trace, at, Result(PresentOutcome::Presented, 1, false, true));
        std::uint64_t frame = 1;
        for (const std::uint64_t gap : { 30, 10, 50, 20, 100, 40, 90, 60, 80, 70 })
        {
            at += gap;
            Present(trace, at, Result(PresentOutcome::Presented, ++frame, false, true));
        }
        trace.Close((at + 1) * kMs);
        const auto s = trace.Summarize();
        UNSCOPED_INFO("eleven shown frames make ten intervals");
        REQUIRE((s.shownFrames == 11 && s.intervalCount == 10));
        UNSCOPED_INFO("p50 of 10..100 ms is the 5th value, 50 ms");
        REQUIRE(s.p50Ns == 50 * kMs);
        UNSCOPED_INFO("p95 of ten intervals is the 10th value, 100 ms");
        REQUIRE(s.p95Ns == 100 * kMs);
        UNSCOPED_INFO("p99 of ten intervals is the 10th value, 100 ms");
        REQUIRE(s.p99Ns == 100 * kMs);
        UNSCOPED_INFO("the file carries the percentiles");
        REQUIRE((Has(Text(trace), "shown_interval_p50_ns = 50000000\n") && Has(Text(trace), "shown_interval_p95_ns = 100000000\n")));
    }
    // Intervals 1..100 ms reversed: ranks 50, 95 and 99.
    {
        NativeCadenceTrace trace(1000);
        Open(trace);
        std::uint64_t at = 1;
        Present(trace, at, Result(PresentOutcome::Presented, 1, false, true));
        for (std::uint64_t gap = 100; gap >= 1; --gap)
        {
            at += gap;
            Present(trace, at, Result(PresentOutcome::Presented, 200 - gap, false, true));
        }
        trace.Close((at + 1) * kMs);
        const auto s = trace.Summarize();
        UNSCOPED_INFO("a hundred intervals");
        REQUIRE(s.intervalCount == 100);
        UNSCOPED_INFO("p50, p95 and p99 of 1..100 ms are 50, 95 and 99 ms");
        REQUIRE((s.p50Ns == 50 * kMs && s.p95Ns == 95 * kMs && s.p99Ns == 99 * kMs));
    }
    {
        NativeCadenceTrace trace;
        Open(trace);
        Present(trace, 1, Result(PresentOutcome::Presented, 1, false, true));
        trace.Close(2 * kMs);
        UNSCOPED_INFO("one shown frame has no interval");
        REQUIRE((trace.Summarize().intervalCount == 0 && Has(Text(trace), "shown_interval_p50_ns = 'none'")));
    }
}

void TestSeparateRates()
{
    NativeCadenceTrace trace;
    Open(trace);
    for (std::uint64_t i = 1; i <= 20; ++i)
        SwapArrived(trace, i);
    for (std::uint64_t i = 1; i <= 10; ++i)
        Publish(trace, 100 + i, i);
    for (std::uint64_t i = 1; i <= 5; ++i)
        Present(trace, 10 * i, Result(PresentOutcome::Presented, 100 + i, false, true));
    // Several present attempts for one frame are legal and not new frames.
    Present(trace, 60, Result(PresentOutcome::Presented, 105, true));
    Present(trace, 61, Result(PresentOutcome::Presented, 105, true));
    trace.Close(2000 * kMs);
    const auto s = trace.Summarize();
    UNSCOPED_INFO("20 attempts, 10 publications and 5 shown frames over 2 s are 10, 5 and 2.5 per second");
    REQUIRE((s.swapAttemptRate == 10.0 && s.publicationRate == 5.0 && s.shownFrameRate == 2.5));
    UNSCOPED_INFO("repeat presents are results, not frames");
    REQUIRE(s.presentResults == 7);
    const auto text = Text(trace);
    UNSCOPED_INFO("the file reports the three rates separately");
    REQUIRE((Has(text, "swap_attempt_rate_hz = 10.000\n") && Has(text, "publication_rate_hz = 5.000\n") && Has(text, "distinct_shown_frame_rate_hz = 2.500\n")));
}

void TestProcessTrace()
{
    UNSCOPED_INFO("the hooks start disabled");
    REQUIRE(!NativeCadenceEnabled());
    cadence::SwapArrived();
    cadence::Present(0, Result(PresentOutcome::Presented, 9, false, true));
    UNSCOPED_INFO("a disabled clock read is free");
    REQUIRE(cadence::Now() == 0);

    const auto path = std::filesystem::temp_directory_path() / ("rerevved_native_cadence_trace_test_" + std::to_string(rerevved::tests::ProcessId()) + ".toml");
    std::filesystem::remove(path);
    std::string error;
    UNSCOPED_INFO("a new file opens the trace");
    REQUIRE((OpenNativeCadenceTrace(path, error) && NativeCadenceEnabled()));
    UNSCOPED_INFO("a second open is refused");
    REQUIRE((!OpenNativeCadenceTrace(path, error) && !error.empty()));

    cadence::SwapArrived();
    cadence::RecordSwapAssembled(1, "failed");
    cadence::Publish(9, 3);
    cadence::Present(cadence::Now(), Result(PresentOutcome::Presented, 9, false, true));
    cadence::Present(cadence::Now(), Result(PresentOutcome::NoFrame, 0));
    cadence::Present(cadence::Now(), Result(PresentOutcome::Suspended, 0));
    UNSCOPED_INFO("close writes the file");
    REQUIRE(CloseNativeCadenceTrace(error));
    UNSCOPED_INFO("close disables the hooks");
    REQUIRE(!NativeCadenceEnabled());
    cadence::SwapArrived();
    UNSCOPED_INFO("a second close is a no-op");
    REQUIRE(CloseNativeCadenceTrace(error));

    std::ifstream      file(path);
    std::ostringstream text;
    text << file.rdbuf();
    file.close();
    const auto written = text.str();
    UNSCOPED_INFO("the file holds the live counts and nothing after close");
    REQUIRE((Has(written, "swap_attempts = 1\n") && Has(written, "swap_assembly_failed = 1\n") && Has(written, "publications = 1\n") && Has(written, "distinct_shown_frames = 1\n")));
    UNSCOPED_INFO("polls that presented nothing are counted, not stored");
    REQUIRE((Has(written, "present_polls_without_result = 2\n") && Has(written, "present_results = 1\n")));
    UNSCOPED_INFO("the file is complete and carries the gamma generation");
    REQUIRE((Has(written, "complete = true\n") && Has(written, "gamma_generation = 3\n")));
    UNSCOPED_INFO("a closed trace is not reopened in the same process");
    REQUIRE(!OpenNativeCadenceTrace(path, error));
    std::filesystem::remove(path);
}

} // namespace

TEST_CASE("native_cadence_trace", "[ctest][native-cadence-trace]")
{
    TestShownFrameRules();
    TestFailedAssembly();
    TestUnfinishedSwap();
    TestUnknownLinks();
    TestOverflow();
    TestPercentiles();
    TestSeparateRates();
    TestProcessTrace();
}
