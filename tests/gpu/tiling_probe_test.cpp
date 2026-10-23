// tiling_probe: native_tiling_quiesce_check counters through the hook scenarios:
// EndTiling snapshot, worker parking, 0x8269BD98 submits, list words/stores.
// No guest/kernel is involved; hook entries still count and return.

#include "gpu/guest_d3d/tiling_probe.h"

#include <rex/cvar.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DECLARE(bool, native_tiling_quiesce_check);

using namespace rerevved::gpu::guest_d3d;

namespace
{

// A window whose word at device offset `offset` is `value`.
void setWord(TilingWindow& window, std::uint32_t offset, std::uint32_t value)
{
    window[(offset - kTilingWindowBase) / 4] = value;
}

// A list run that entered and returned.
void runList(TilingProbe& probe)
{
    probe.NoteInterpreterEntry();
    probe.NoteInterpreterExit();
}

TilingWindow filled()
{
    TilingWindow window{};
    for (std::size_t i = 0; i < window.size(); ++i)
        window[i] = 0x1000U + static_cast<std::uint32_t>(i);
    return window;
}

TilingWorkerState idleState()
{
    return {};
}

void testExpectedOffsets()
{
    for (const std::uint32_t offset : { 0x2AC8U, 0x2AECU, 0x2B00U, 0x2B04U, 0x2C74U, 0x2C78U, 0x2CC4U, 0x2CC8U, 0x2D14U, 0x2D18U, 0x2D64U, 0x2D68U, 0x2DB4U, 0x2DB8U, 0x2E04U, 0x2E08U })
    {
        UNSCOPED_INFO("expected offset " + std::to_string(offset));
        REQUIRE(IsExpectedQuiesceOffset(offset));
    }
    // The neighbors of each expected word, including the worker's lock and the queue slots, are real state.
    for (const std::uint32_t offset : { 0x2AC0U, 0x2AC4U, 0x2ACCU, 0x2AE8U, 0x2AF0U, 0x2AFCU, 0x2B08U, 0x2C70U, 0x2C7CU, 0x2C80U, 0x2CC0U, 0x2D10U, 0x2D1CU, 0x2E0CU })
    {
        UNSCOPED_INFO("unexpected offset " + std::to_string(offset));
        REQUIRE(!IsExpectedQuiesceOffset(offset));
    }
}

void testCompare()
{
    const auto before = filled();
    auto       after  = before;
    auto       diff   = CompareQuiesceWindow(before, after);
    UNSCOPED_INFO("an identical window has no difference");
    REQUIRE((diff.words == 0 && !diff.waitCount && diff.logged == 0));

    // Every expected word may move.
    for (const std::uint32_t offset : { 0x2AC8U, 0x2B00U, 0x2B04U, 0x2C74U, 0x2C78U, 0x2CC4U, 0x2CC8U })
        setWord(after, offset, 0xDEAD0000U + offset);
    diff = CompareQuiesceWindow(before, after);
    UNSCOPED_INFO("the expected words may differ");
    REQUIRE((diff.words == 0 && diff.logged == 0 && !diff.waitCount));

    // The parked-thread count is reported apart and does not count as a delta.
    setWord(after, 0x2AEC, 7);
    diff = CompareQuiesceWindow(before, after);
    UNSCOPED_INFO("W+0x2C is flagged, not counted");
    REQUIRE((diff.words == 0 && diff.waitCount));

    // Each of the words just outside the expected ones counts, first and last of the window included.
    for (const std::uint32_t offset : { 0x2AC0U, 0x2AC4U, 0x2ACCU, 0x2AF0U, 0x2B08U, 0x2C70U, 0x2C7CU, 0x2CFCU })
    {
        auto changed = before;
        setWord(changed, offset, 0xBEEF);
        diff = CompareQuiesceWindow(before, changed);
        UNSCOPED_INFO("a word at " + std::to_string(offset) + " counts and is logged");
        REQUIRE((diff.words == 1 && diff.logged == 1 && diff.first[0].offset == offset && diff.first[0].before == before[(offset - kTilingWindowBase) / 4] && diff.first[0].after == 0xBEEF));
    }

    // More differences than the log keeps: all are counted, the first eight are kept in order.
    auto many = before;
    for (std::uint32_t offset = 0x2B10; offset < 0x2B10 + 12 * 4; offset += 4)
        setWord(many, offset, 0);
    diff = CompareQuiesceWindow(before, many);
    UNSCOPED_INFO("a long difference counts every word and logs the first eight");
    REQUIRE((diff.words == 12 && diff.logged == kQuiesceLogWords && diff.first[0].offset == 0x2B10 && diff.first[kQuiesceLogWords - 1].offset == 0x2B10 + 28));
}

void testWorkersIdle()
{
    UNSCOPED_INFO("an all-zero worker is idle");
    REQUIRE(TilingWorkersIdle(idleState()));
    auto state = idleState();
    state.lock = 1;
    UNSCOPED_INFO("a held lock is not idle");
    REQUIRE(!TilingWorkersIdle(state));
    state       = idleState();
    state.lists = 1;
    UNSCOPED_INFO("a list in progress is not idle");
    REQUIRE(!TilingWorkersIdle(state));
    state         = idleState();
    state.pending = 0x1000;
    UNSCOPED_INFO("a pending list is not idle");
    REQUIRE(!TilingWorkersIdle(state));
    for (std::size_t i = 0; i < kTilingQueueCount; ++i)
    {
        state             = idleState();
        state.pushed[i]   = 5;
        state.consumed[i] = 5;
        UNSCOPED_INFO("equal counts are idle, queue " + std::to_string(i));
        REQUIRE(TilingWorkersIdle(state));
        state.pushed[i] = 6;
        UNSCOPED_INFO("a queued list is not idle, queue " + std::to_string(i));
        REQUIRE(!TilingWorkersIdle(state));
    }
}

void testClassifyToken()
{
    struct Case
    {
        std::uint32_t word;
        TilingToken   kind;
    };

    const Case cases[] = {
        { 0x00000000, TilingToken::Record },
        { 0x7FFFFFFF, TilingToken::Record },
        { 0x80000000, TilingToken::T80 },
        { 0x80123456, TilingToken::T80 },
        { 0x81000010, TilingToken::T81 },
        { 0x82000000, TilingToken::T82 },
        { 0x83000000, TilingToken::T83 },
        { 0x84000000, TilingToken::T84 },
        { 0x85000000, TilingToken::T85 },
        { 0x86000000, TilingToken::T86 },
        { 0x87000000, TilingToken::T87 },
        { 0x88000000, TilingToken::T88 },
        { 0x89000000, TilingToken::T89 },
        { 0x8A000000, TilingToken::T8A },
        { 0x8B000000, TilingToken::T8B },
        { 0x8C000000, TilingToken::T8C },
        { 0x8C000001, TilingToken::Other },
        { 0x8D000000, TilingToken::T8D },
        { 0x8D000100, TilingToken::Other },
        { 0x8E000000, TilingToken::T8E },
        { 0x8E00FFFF, TilingToken::T8E },
        { 0x8F000000, TilingToken::Other },
        { 0x90000000, TilingToken::Other },
        { 0xBFFFFFFF, TilingToken::Other },
        { 0xC0000000, TilingToken::End },
        { 0xC0000004, TilingToken::Link },
        { 0xFFFFFFFF, TilingToken::Link },
    };
    for (const auto& c : cases)
    {
        UNSCOPED_INFO("token " + std::to_string(c.word));
        REQUIRE(ClassifyTilingToken(c.word) == c.kind);
    }
    UNSCOPED_INFO("token names");
    REQUIRE((std::string(TilingTokenName(TilingToken::T8A)) == "8a" && std::string(TilingTokenName(TilingToken::Other)) == "other"));
}

void testSubmitSite()
{
    UNSCOPED_INFO("optional return address");
    REQUIRE(ClassifyTilingSubmitSite(0x826AC368) == TilingSubmitSite::Optional);
    UNSCOPED_INFO("middle return address");
    REQUIRE(ClassifyTilingSubmitSite(0x826AC3B4) == TilingSubmitSite::Middle);
    UNSCOPED_INFO("final return address");
    REQUIRE(ClassifyTilingSubmitSite(0x826AC3E8) == TilingSubmitSite::Final);
    for (const std::uint32_t lr : { 0U, 0x826AC364U, 0x826AC3B0U, 0x826AC3E4U, 0x826AC36CU, 0x8269C4E8U })
    {
        UNSCOPED_INFO("other return address " + std::to_string(lr));
        REQUIRE(ClassifyTilingSubmitSite(lr) == TilingSubmitSite::Other);
    }
}

void testWriteClass()
{
    constexpr std::uint32_t device = 0x40010000, ring = 0x01000000, ringBytes = 0x20000;
    // The device block is the 0x5E00 bytes device creation allocates; a four-byte store must end inside it.
    UNSCOPED_INFO("the device allocation is 24064 bytes");
    REQUIRE(kTilingDeviceBytes == 0x5E00);
    UNSCOPED_INFO("G itself");
    REQUIRE(ClassifyTilingWrite(device, device, 0, ring, ringBytes) == TilingWriteClass::Device);
    UNSCOPED_INFO("past the fields the ring stubs read is still the device");
    REQUIRE(ClassifyTilingWrite(device + 0x5B10, device, 0, ring, ringBytes) == TilingWriteClass::Device);
    UNSCOPED_INFO("the last device word");
    REQUIRE(ClassifyTilingWrite(device + 0x5DFC, device, 0, ring, ringBytes) == TilingWriteClass::Device);
    UNSCOPED_INFO("a store straddling the device end");
    REQUIRE(ClassifyTilingWrite(device + 0x5DFD, device, 0, ring, ringBytes) == TilingWriteClass::Other);
    UNSCOPED_INFO("the device end");
    REQUIRE(ClassifyTilingWrite(device + 0x5E00, device, 0, ring, ringBytes) == TilingWriteClass::Other);
    UNSCOPED_INFO("below the device");
    REQUIRE(ClassifyTilingWrite(device - 4, device, 0, ring, ringBytes) == TilingWriteClass::Other);
    UNSCOPED_INFO("no device known");
    REQUIRE(ClassifyTilingWrite(device + 0x2B00, 0, 0, ring, ringBytes) == TilingWriteClass::Other);
    UNSCOPED_INFO("no device known, low address");
    REQUIRE(ClassifyTilingWrite(0x2B00, 0, 0, ring, ringBytes) == TilingWriteClass::Other);
    // The ring is judged by the physical alias.
    UNSCOPED_INFO("ring start");
    REQUIRE(ClassifyTilingWrite(0xA1000000, device, ring, ring, ringBytes) == TilingWriteClass::Ring);
    UNSCOPED_INFO("last ring word");
    REQUIRE(ClassifyTilingWrite(0xA1000000, device, ring + ringBytes - 4, ring, ringBytes) == TilingWriteClass::Ring);
    UNSCOPED_INFO("straddling the ring end");
    REQUIRE(ClassifyTilingWrite(0xA1000000, device, ring + ringBytes - 3, ring, ringBytes) == TilingWriteClass::Other);
    UNSCOPED_INFO("the ring end");
    REQUIRE(ClassifyTilingWrite(0xA1000000, device, ring + ringBytes, ring, ringBytes) == TilingWriteClass::Other);
    UNSCOPED_INFO("below the ring");
    REQUIRE(ClassifyTilingWrite(0xA1000000, device, ring - 4, ring, ringBytes) == TilingWriteClass::Other);
    UNSCOPED_INFO("no ring registered");
    REQUIRE(ClassifyTilingWrite(0xA1000000, device, ring, 0, 0) == TilingWriteClass::Other);
    UNSCOPED_INFO("a ring at the top of the space is not wrapped");
    REQUIRE(ClassifyTilingWrite(0xA1000000, device, 0xFFFFFFFC, 0xFFFFFFF0, 0x40) == TilingWriteClass::Ring);
    UNSCOPED_INFO("a device at the top of the space does not wrap");
    REQUIRE(ClassifyTilingWrite(0x00000010, 0xFFFFFFF0, 0, ring, ringBytes) == TilingWriteClass::Other);
    UNSCOPED_INFO("write names");
    REQUIRE((std::string(TilingWriteSiteName(TilingWriteSite::B60C)) == "b60c" && std::string(TilingWriteClassName(TilingWriteClass::Ring)) == "ring"));
}

void testQuiesce()
{
    TilingProbe probe;
    const auto  start = filled();
    auto        later = start;
    setWord(later, 0x2B00, 0);
    setWord(later, 0x2AC8, 99);

    // Idle with nothing pending is counted and ignored.
    UNSCOPED_INFO("no bracket pending");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::NoBracket);
    // A bracket that ended in a drop snapshots nothing.
    UNSCOPED_INFO("a dropped bracket replaces nothing");
    REQUIRE(!probe.NoteBracketEnd(true, &start, probe.NoteBracketStart()));
    UNSCOPED_INFO("a dropped bracket leaves nothing pending");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::NoBracket);
    // An unreadable window snapshots nothing.
    UNSCOPED_INFO("an unreadable window replaces nothing");
    REQUIRE(!probe.NoteBracketEnd(false, nullptr, probe.NoteBracketStart()));
    UNSCOPED_INFO("an unreadable window leaves nothing pending");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::NoBracket);

    UNSCOPED_INFO("the first snapshot replaces nothing");
    REQUIRE(!probe.NoteBracketEnd(false, &start, probe.NoteBracketStart()));
    // The worker parks before it interpreted any list since the snapshot: not yet.
    UNSCOPED_INFO("idle before any list is early");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Early);
    runList(probe);
    // Another queue still holds work.
    auto busy      = idleState();
    busy.pushed[3] = 2;
    UNSCOPED_INFO("a queued list is busy");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), busy, &later, false).kind == IdleVerdict::Kind::Busy);
    // A compare needs the current window.
    UNSCOPED_INFO("no window, no verdict");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), nullptr, false).kind == IdleVerdict::Kind::NoBracket);
    auto verdict = probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, true);
    UNSCOPED_INFO("only expected words moved");
    REQUIRE((verdict.kind == IdleVerdict::Kind::Ok && verdict.diff.words == 0));
    // The bracket is consumed.
    UNSCOPED_INFO("a compared bracket is gone");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::NoBracket);

    // A bracket whose worker leaves a mark.
    probe.NoteBracketEnd(false, &start, probe.NoteBracketStart());
    runList(probe);
    auto marked = later;
    setWord(marked, 0x2AF0, 123);
    setWord(marked, 0x2AEC, 3);
    verdict = probe.NoteWorkerIdle(probe.Sample(), idleState(), &marked, false);
    UNSCOPED_INFO("a mark outside the expected words is a delta");
    REQUIRE((verdict.kind == IdleVerdict::Kind::Delta && verdict.diff.words == 1 && verdict.diff.logged == 1 && verdict.diff.waitCount));

    // A bracket that ends before the worker reported for the last one replaces it.
    probe.NoteBracketEnd(false, &start, probe.NoteBracketStart());
    UNSCOPED_INFO("the second snapshot replaces the first");
    REQUIRE(probe.NoteBracketEnd(false, &start, probe.NoteBracketStart()));
    // A dropped bracket in between leaves the pending one alone.
    UNSCOPED_INFO("a drop keeps the snapshot");
    REQUIRE(!probe.NoteBracketEnd(true, &start, probe.NoteBracketStart()));
    runList(probe);
    UNSCOPED_INFO("the snapshot a drop left is compared");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Ok);

    const auto counts = probe.Take();
    UNSCOPED_INFO("bracket counts");
    REQUIRE((counts.bracketEnd == 7 && counts.bracketDropped == 2 && counts.bracketUnreadable == 1));
    UNSCOPED_INFO("quiesce counts");
    REQUIRE((counts.quiesceSnapshots == 4 && counts.quiesceOk == 2 && counts.quiesceDelta == 1 && counts.quiesceDeltaWords == 1));
    UNSCOPED_INFO("quiesce side counts");
    REQUIRE((counts.quiesceUnobserved == 1 && counts.quiesceBusy == 1 && counts.quiesceEarly == 1 && counts.quiescePendingNonzero == 1 && counts.quiesceWaitCount == 1));
    UNSCOPED_INFO("worker counts");
    REQUIRE((counts.workerIdle == 10 && counts.workerLists == 3));
    UNSCOPED_INFO("Take resets the window");
    REQUIRE(probe.Take().quiesceOk == 0);

    // The pending snapshot outlives a Take.
    probe.NoteBracketEnd(false, &start, probe.NoteBracketStart());
    probe.Take();
    runList(probe);
    UNSCOPED_INFO("the snapshot survives the window");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Ok);
}

void testStaleIdle()
{
    TilingProbe probe;
    const auto  start = filled();
    auto        later = start;
    setWord(later, 0x2B00, 0);

    // A sample taken before a newer snapshot says nothing about it.
    probe.NoteBracketEnd(false, &start, probe.NoteBracketStart());
    runList(probe);
    auto sampled = probe.Sample();
    probe.NoteBracketEnd(false, &start, probe.NoteBracketStart());
    runList(probe);
    UNSCOPED_INFO("a sample from before the bracket is stale");
    REQUIRE(probe.NoteWorkerIdle(sampled, idleState(), &later, false).kind == IdleVerdict::Kind::Stale);
    // A bracket alone, with no list entered since the sample, makes it stale.
    TilingProbe bracketOnly;
    bracketOnly.NoteBracketEnd(false, &start, bracketOnly.NoteBracketStart());
    runList(bracketOnly);
    const auto bracketSample = bracketOnly.Sample();
    bracketOnly.NoteBracketEnd(false, &start, bracketOnly.NoteBracketStart());
    UNSCOPED_INFO("a bracket after the sample makes it stale");
    REQUIRE(bracketOnly.NoteWorkerIdle(bracketSample, idleState(), &later, false).kind == IdleVerdict::Kind::Stale);
    UNSCOPED_INFO("the bracket-only stale count");
    REQUIRE(bracketOnly.Take().quiesceStale == 1);
    // A list that started after the sample was read may be missing from it.
    sampled = probe.Sample();
    runList(probe);
    UNSCOPED_INFO("a sample from before a list is stale");
    REQUIRE(probe.NoteWorkerIdle(sampled, idleState(), &later, false).kind == IdleVerdict::Kind::Stale);
    // A sample from before any bracket is stale against the first.
    TilingProbe fresh;
    sampled = fresh.Sample();
    fresh.NoteBracketEnd(false, &start, fresh.NoteBracketStart());
    runList(fresh);
    UNSCOPED_INFO("a sample from before the first bracket is stale");
    REQUIRE(fresh.NoteWorkerIdle(sampled, idleState(), &later, false).kind == IdleVerdict::Kind::Stale);
    UNSCOPED_INFO("the snapshot stays pending for a current sample");
    REQUIRE(fresh.NoteWorkerIdle(fresh.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Ok);
    // Nothing pending: staleness is not reported.
    UNSCOPED_INFO("a stale sample with nothing pending is ignored");
    REQUIRE(fresh.NoteWorkerIdle(sampled, idleState(), &later, false).kind == IdleVerdict::Kind::NoBracket);
    // A stale sample leaves the snapshot, so the current one compares it.
    UNSCOPED_INFO("a stale sample does not consume the snapshot");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Ok);
    const auto counts = probe.Take();
    UNSCOPED_INFO("stale counts: " + std::to_string(counts.quiesceStale));
    REQUIRE((counts.quiesceStale == 2 && counts.quiesceOk == 1 && counts.quiesceSnapshots == 2));
    UNSCOPED_INFO("the first-bracket stale count");
    REQUIRE(fresh.Take().quiesceStale == 1);
}

void testInFlight()
{
    const auto start = filled();
    auto       later = start;
    setWord(later, 0x2B00, 0);

    // A run that entered before the sample and is still going: the zero worker fields read
    // after it may be the moment before it took the lock and consumed queued work.
    TilingProbe running;
    running.NoteBracketEnd(false, &start, running.NoteBracketStart());
    running.NoteInterpreterEntry();
    UNSCOPED_INFO("the entry is in flight");
    REQUIRE(running.Sample().inFlight == 1);
    UNSCOPED_INFO("a run in flight at the sample is stale");
    REQUIRE(running.NoteWorkerIdle(running.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Stale);
    // It returns after the sample but before the verdict: the entry count is unchanged, yet the sample saw it running.
    running.NoteBracketEnd(false, &start, running.NoteBracketStart());
    running.NoteInterpreterEntry();
    const auto sampled = running.Sample();
    UNSCOPED_INFO("two runs in flight");
    REQUIRE(sampled.inFlight == 2);
    running.NoteInterpreterExit();
    running.NoteInterpreterExit();
    UNSCOPED_INFO("a run that returned after the sample is stale");
    REQUIRE(running.NoteWorkerIdle(sampled, idleState(), &later, false).kind == IdleVerdict::Kind::Stale);
    // With every run returned before the sample, the next sample is clean and compares.
    UNSCOPED_INFO("all runs returned");
    REQUIRE(running.Sample().inFlight == 0);
    UNSCOPED_INFO("no run in flight compares the snapshot");
    REQUIRE(running.NoteWorkerIdle(running.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Ok);
    const auto counts = running.Take();
    UNSCOPED_INFO("in-flight counts: " + std::to_string(counts.quiesceStale));
    REQUIRE((counts.quiesceStale == 2 && counts.quiesceOk == 1 && counts.errors == 0));

    // Another worker sharing W enters between the field reads and the verdict.
    TilingProbe shared;
    shared.NoteBracketEnd(false, &start, shared.NoteBracketStart());
    shared.NoteInterpreterEntry();
    shared.NoteInterpreterExit();
    const auto beforeReads = shared.Sample();
    shared.NoteInterpreterEntry();
    UNSCOPED_INFO("an entry between the reads is stale");
    REQUIRE(shared.NoteWorkerIdle(beforeReads, idleState(), &later, false).kind == IdleVerdict::Kind::Stale);
    shared.NoteInterpreterExit();
    UNSCOPED_INFO("the sample after it returned compares");
    REQUIRE(shared.NoteWorkerIdle(shared.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Ok);

    // A return without an entry is an error, not an underflow.
    TilingProbe unbalanced;
    unbalanced.NoteInterpreterExit();
    UNSCOPED_INFO("an unmatched return is an error");
    REQUIRE((unbalanced.Sample().inFlight == 0 && unbalanced.Take().errors == 1));
}

void testOverlap()
{
    TilingProbe probe;
    const auto  start = filled();
    auto        later = start;
    setWord(later, 0x2B00, 0);
    // Replay starts inside EndTiling: the list entry comes before the snapshot.
    auto started = probe.NoteBracketStart();
    runList(probe);
    probe.NoteBracketEnd(false, &start, started);
    UNSCOPED_INFO("a list started inside EndTiling counts for the next idle");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Ok);
    // Words the worker moves after the snapshot are still a delta, and the overlap is counted.
    started = probe.NoteBracketStart();
    runList(probe);
    probe.NoteBracketEnd(false, &start, started);
    auto marked = later;
    setWord(marked, 0x2AF0, 123);
    const auto verdict = probe.NoteWorkerIdle(probe.Sample(), idleState(), &marked, false);
    UNSCOPED_INFO("a change after an overlapped snapshot is a delta");
    REQUIRE((verdict.kind == IdleVerdict::Kind::Delta && verdict.diff.words == 1));
    // No list started since the bracket began: still early, and not an overlap.
    probe.NoteBracketEnd(false, &start, probe.NoteBracketStart());
    UNSCOPED_INFO("no list since the bracket began is early");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Early);
    runList(probe);
    UNSCOPED_INFO("a list after the snapshot ends the early state");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Ok);
    const auto counts = probe.Take();
    UNSCOPED_INFO("overlap counts: " + std::to_string(counts.quiesceOverlap));
    REQUIRE((counts.quiesceOverlap == 2 && counts.quiesceOk == 2 && counts.quiesceDelta == 1 && counts.quiesceEarly == 1));
    // A bracket that began before the entry of a list from an earlier bracket does not get that entry.
    runList(probe);
    started = probe.NoteBracketStart();
    probe.NoteBracketEnd(false, &start, started);
    UNSCOPED_INFO("an earlier list is not this bracket's");
    REQUIRE(probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false).kind == IdleVerdict::Kind::Early);
}

void testDeltaLogLimit()
{
    TilingProbe probe;
    const auto  start = filled();
    auto        later = start;
    setWord(later, 0x2B20, 1);
    std::size_t logged = 0;
    for (std::size_t i = 0; i < kTilingLogLimit + 4; ++i)
    {
        probe.NoteBracketEnd(false, &start, probe.NoteBracketStart());
        runList(probe);
        const auto verdict = probe.NoteWorkerIdle(probe.Sample(), idleState(), &later, false);
        UNSCOPED_INFO("every delta is counted");
        REQUIRE((verdict.kind == IdleVerdict::Kind::Delta && verdict.diff.words == 1));
        logged += verdict.diff.logged ? 1 : 0;
    }
    UNSCOPED_INFO("only the first deltas are logged");
    REQUIRE(logged == kTilingLogLimit);
    UNSCOPED_INFO("all deltas are counted");
    REQUIRE(probe.Take().quiesceDelta == kTilingLogLimit + 4);
}

void testSubmit()
{
    TilingProbe probe;
    probe.NoteGuard(true);
    probe.NoteGuard(false);
    probe.NoteGuard(false);
    probe.NoteSubmit(TilingSubmitSite::Middle, 1, 0, 1, TilingArm::Direct);
    probe.NoteSubmit(TilingSubmitSite::Final, 0, 1, 1, TilingArm::Deferred);
    probe.NoteSubmit(TilingSubmitSite::Final, 0, 0, 0, TilingArm::Direct);
    // The worker lowered the counter while the call ran.
    probe.NoteSubmit(TilingSubmitSite::Middle, 1, 2, 2, TilingArm::Deferred);
    probe.NoteSubmit(TilingSubmitSite::Other, 3, 0xFFFFFFFF, 2, TilingArm::None);
    probe.NoteSubmit(TilingSubmitSite::Optional, 1, 0, 1, TilingArm::Deferred);
    const auto counts = probe.Take();
    UNSCOPED_INFO("guard counts");
    REQUIRE((counts.guardNonzero == 1 && counts.guardZero == 2));
    const auto& mid = counts.submit[static_cast<std::size_t>(TilingSubmitSite::Middle)];
    UNSCOPED_INFO("middle submit counts");
    REQUIRE((mid.calls == 2 && mid.r7One == 2 && mid.r7Zero == 0 && mid.direct == 1 && mid.deferred == 1 && mid.beforeNonzero == 1 && mid.afterNonzero == 2 && mid.addsR7 == 1 && mid.addsOther == 1));
    const auto& final = counts.submit[static_cast<std::size_t>(TilingSubmitSite::Final)];
    UNSCOPED_INFO("final submit counts");
    REQUIRE((final.calls == 2 && final.r7Zero == 2 && final.deferred == 1 && final.direct == 1 && final.beforeNonzero == 1 && final.afterNonzero == 1 && final.addsR7 == 2 && final.addsOther == 0));
    const auto& other = counts.submit[static_cast<std::size_t>(TilingSubmitSite::Other)];
    UNSCOPED_INFO("other submit counts, with a wrapping before");
    REQUIRE((other.calls == 1 && other.r7Other == 1 && other.noArm == 1 && other.addsR7 == 1));
    const auto& optional = counts.submit[static_cast<std::size_t>(TilingSubmitSite::Optional)];
    UNSCOPED_INFO("optional submit counts");
    REQUIRE((optional.calls == 1 && optional.deferred == 1));
}

void testTokens()
{
    TilingProbe probe;
    // The interpreter counts what it reads itself.
    for (const std::uint32_t word : { 0x00040000U, 0x00040000U, 0x8C000000U, 0x8D000000U, 0xC0000000U, 0xC0001234U })
    {
        UNSCOPED_INFO("interpreter words need no log");
        REQUIRE(!probe.NoteToken(word, false));
    }
    // A control word at the interpreter is the handler's.
    for (const std::uint32_t word : { 0x80000000U, 0x81000000U, 0x8E000000U, 0x8F000000U })
    {
        UNSCOPED_INFO("control words are not counted at the interpreter");
        REQUIRE(!probe.NoteToken(word, false));
    }
    // The handler counts the control words and leaves the interpreter's.
    for (const std::uint32_t word : { 0x80000000U, 0x81000010U, 0x81000010U, 0x8B000000U, 0x8E000000U, 0x8C000000U, 0x8D000000U, 0xC0000000U, 0x00010000U })
    {
        UNSCOPED_INFO("handler tokens need no log");
        REQUIRE(!probe.NoteToken(word, true));
    }
    // Unknown control words log once per distinct value, up to the limit.
    UNSCOPED_INFO("the first unknown control word is logged");
    REQUIRE(probe.NoteToken(0x8F000000U, true));
    UNSCOPED_INFO("the same word is not logged again");
    REQUIRE(!probe.NoteToken(0x8F000000U, true));
    for (std::uint32_t i = 1; i < kTilingLogLimit; ++i)
    {
        UNSCOPED_INFO("distinct unknown words are logged up to the limit");
        REQUIRE(probe.NoteToken(0x90000000U + i, true));
    }
    UNSCOPED_INFO("no log past the limit");
    REQUIRE(!probe.NoteToken(0xA0000000U, true));
    const auto counts = probe.Take();
    const auto at     = [&](TilingToken kind)
    {
        return counts.token[static_cast<std::size_t>(kind)];
    };
    UNSCOPED_INFO("interpreter token counts");
    REQUIRE((at(TilingToken::Record) == 2 && at(TilingToken::T8C) == 1 && at(TilingToken::T8D) == 1 && at(TilingToken::End) == 1 && at(TilingToken::Link) == 1));
    UNSCOPED_INFO("handler token counts");
    REQUIRE((at(TilingToken::T80) == 1 && at(TilingToken::T81) == 2 && at(TilingToken::T8B) == 1 && at(TilingToken::T8E) == 1));
    UNSCOPED_INFO("unknown control words count at the handler: " + std::to_string(at(TilingToken::Other)));
    REQUIRE(at(TilingToken::Other) == 2 + (kTilingLogLimit - 1) + 1);
    UNSCOPED_INFO("tokens never seen stay zero");
    REQUIRE((at(TilingToken::T82) == 0 && at(TilingToken::T88) == 0));
}

void testHandlerLinks()
{
    // The handler follows a link itself; the same count as the interpreter's, from the other site.
    TilingProbe probe;
    UNSCOPED_INFO("a link needs no log");
    REQUIRE(!probe.NoteToken(0xC0001234U, false));
    UNSCOPED_INFO("a second link needs no log");
    REQUIRE(!probe.NoteToken(0xC0001238U, false));
    UNSCOPED_INFO("an unknown control word is not a link");
    REQUIRE(!probe.NoteToken(0x8F000000U, false));
    UNSCOPED_INFO("links count where they are followed");
    REQUIRE(probe.Take().token[static_cast<std::size_t>(TilingToken::Link)] == 2);
}

void testCallbacks()
{
    TilingProbe probe;
    UNSCOPED_INFO("the first install is logged");
    REQUIRE(probe.NoteInstalled(0x826AB740, 0x7000F000));
    UNSCOPED_INFO("the same install is not logged again");
    REQUIRE(!probe.NoteInstalled(0x826AB740, 0x7000F000));
    UNSCOPED_INFO("another context is logged");
    REQUIRE(probe.NoteInstalled(0x826AB740, 0x7000F100));
    UNSCOPED_INFO("the first callback is logged");
    REQUIRE(probe.NoteCallback(0x826AB740, 0x7000F000));
    UNSCOPED_INFO("the same callback is not logged again");
    REQUIRE(!probe.NoteCallback(0x826AB740, 0x7000F000));
    for (std::uint32_t i = 1; i < kTilingLogLimit; ++i)
    {
        UNSCOPED_INFO("distinct callbacks are logged up to the limit");
        REQUIRE(probe.NoteCallback(0x82000000U + i, 0));
    }
    UNSCOPED_INFO("no log past the limit");
    REQUIRE(!probe.NoteCallback(0x82FFFFFF, 0));
    // The set is capped, but every call is counted.
    for (std::uint32_t i = 0; i < kTilingDistinctCap + 10; ++i)
        probe.NoteCallback(0x83000000U + i, 1);
    const auto counts = probe.Take();
    UNSCOPED_INFO("distinct installs");
    REQUIRE(counts.installDistinct == 2);
    UNSCOPED_INFO("every call counts");
    REQUIRE(counts.callbackCalls == 1 + 1 + (kTilingLogLimit - 1) + 1 + (kTilingDistinctCap + 10));
    UNSCOPED_INFO("the distinct set is capped");
    REQUIRE(counts.callbackDistinct == kTilingDistinctCap);
}

void testWrites()
{
    TilingProbe probe;
    UNSCOPED_INFO("a device store is not logged");
    REQUIRE(!probe.NoteWrite(TilingWriteSite::B120, TilingWriteClass::Device, 0x40012B00));
    UNSCOPED_INFO("a ring store is not logged");
    REQUIRE(!probe.NoteWrite(TilingWriteSite::B4C8, TilingWriteClass::Ring, 0xA1000010));
    UNSCOPED_INFO("an other store is logged");
    REQUIRE(probe.NoteWrite(TilingWriteSite::B60C, TilingWriteClass::Other, 0x70001000));
    UNSCOPED_INFO("the same target is not logged again, whatever the site");
    REQUIRE(!probe.NoteWrite(TilingWriteSite::B6FC, TilingWriteClass::Other, 0x70001000));
    for (std::uint32_t i = 1; i < kTilingLogLimit; ++i)
    {
        UNSCOPED_INFO("distinct targets are logged up to the limit");
        REQUIRE(probe.NoteWrite(TilingWriteSite::B60C, TilingWriteClass::Other, 0x70001000U + i * 0x100));
    }
    UNSCOPED_INFO("no log past the limit");
    REQUIRE(!probe.NoteWrite(TilingWriteSite::B60C, TilingWriteClass::Other, 0x7FFFFF00));
    const auto counts = probe.Take();
    const auto at     = [&](TilingWriteSite site, TilingWriteClass kind)
    {
        return counts.write[static_cast<std::size_t>(site)][static_cast<std::size_t>(kind)];
    };
    UNSCOPED_INFO("device and ring counts");
    REQUIRE((at(TilingWriteSite::B120, TilingWriteClass::Device) == 1 && at(TilingWriteSite::B4C8, TilingWriteClass::Ring) == 1));
    UNSCOPED_INFO("other counts");
    REQUIRE((at(TilingWriteSite::B60C, TilingWriteClass::Other) == kTilingLogLimit + 1 && at(TilingWriteSite::B6FC, TilingWriteClass::Other) == 1));
    UNSCOPED_INFO("the rest stay zero");
    REQUIRE((at(TilingWriteSite::B120, TilingWriteClass::Other) == 0 && at(TilingWriteSite::B4C8, TilingWriteClass::Device) == 0));
}

void testRingSinksWatchdog()
{
    TilingProbe   probe;
    std::uint32_t physical = 1, bytes = 1;
    probe.RingExtent(physical, bytes);
    UNSCOPED_INFO("no ring before registration");
    REQUIRE((physical == 0 && bytes == 0));
    probe.NoteRing(0x01000000, 16);
    probe.RingExtent(physical, bytes);
    UNSCOPED_INFO("ring bytes are 1 << (log2 + 3)");
    REQUIRE((physical == 0x01000000 && bytes == (1U << 19)));
    probe.NoteRing(0x02000000, 40);
    probe.RingExtent(physical, bytes);
    UNSCOPED_INFO("an absurd size registers no extent");
    REQUIRE((physical == 0x02000000 && bytes == 0));
    probe.NoteRing(0x03000000, 12);
    probe.RingExtent(physical, bytes);
    UNSCOPED_INFO("the latest ring wins");
    REQUIRE((physical == 0x03000000 && bytes == (1U << 15)));

    UNSCOPED_INFO("null sinks need no log");
    REQUIRE(!probe.NotePublisher(0, 0));
    UNSCOPED_INFO("a sink is logged");
    REQUIRE(probe.NotePublisher(0x70000100, 0));
    UNSCOPED_INFO("the same sink is not logged again");
    REQUIRE(!probe.NotePublisher(0x70000100, 0));
    UNSCOPED_INFO("a callback is logged");
    REQUIRE(probe.NotePublisher(0, 0x82000000));
    UNSCOPED_INFO("the pair is a new value");
    REQUIRE(probe.NotePublisher(0x70000100, 0x82000000));

    UNSCOPED_INFO("entry and the global gate are not logged");
    REQUIRE((!probe.NoteWatchdog(0) && !probe.NoteWatchdog(1)));
    for (std::size_t i = 0; i < kTilingLogLimit; ++i)
    {
        UNSCOPED_INFO("the first resets and zeros are logged");
        REQUIRE(probe.NoteWatchdog(i % 2 ? 2 : 3));
    }
    UNSCOPED_INFO("no log past the limit");
    REQUIRE(!probe.NoteWatchdog(2));

    const auto counts = probe.Take();
    UNSCOPED_INFO("ring registrations");
    REQUIRE(counts.ringRegistered == 3);
    UNSCOPED_INFO("sink counts");
    REQUIRE((counts.publisher == 5 && counts.sink541C == 3 && counts.sink5B0C == 2));
    UNSCOPED_INFO("watchdog counts");
    REQUIRE((counts.watchdogEnter == 1 && counts.watchdogGlobal == 1 && counts.watchdogReset == kTilingLogLimit / 2 + 1 && counts.watchdogZero == kTilingLogLimit / 2));
}

void testErrors()
{
    TilingProbe probe;
    probe.NoteError();
    probe.NoteError();
    UNSCOPED_INFO("errors are taken with the window");
    REQUIRE((probe.Take().errors == 2 && probe.Take().errors == 0));
}

// The fields the timing line carries, in order. The runtime document lists the same names.
constexpr const char* kExpectedFormat =
    " tiling_bracket_end=0 tiling_bracket_dropped=0 tiling_bracket_unreadable=0"
    " tiling_quiesce_snapshots=0 tiling_quiesce_ok=0 tiling_quiesce_delta=0 tiling_quiesce_delta_words=0 tiling_quiesce_wait_count=0"
    " tiling_quiesce_unobserved=0 tiling_quiesce_busy=0 tiling_quiesce_early=0 tiling_quiesce_stale=0 tiling_quiesce_overlap=0 tiling_quiesce_pending_nonzero=0"
    " tiling_worker_idle=0 tiling_worker_lists=0 tiling_submit_guard_nonzero=0 tiling_submit_guard_zero=0"
    " tiling_submit_opt_calls=0 tiling_submit_opt_r7_0=0 tiling_submit_opt_r7_1=0 tiling_submit_opt_r7_other=0"
    " tiling_submit_opt_deferred=0 tiling_submit_opt_direct=0 tiling_submit_opt_noarm=0"
    " tiling_submit_opt_before_nonzero=0 tiling_submit_opt_after_nonzero=0 tiling_submit_opt_adds_r7=0 tiling_submit_opt_adds_other=0"
    " tiling_submit_mid_calls=0 tiling_submit_mid_r7_0=0 tiling_submit_mid_r7_1=0 tiling_submit_mid_r7_other=0"
    " tiling_submit_mid_deferred=0 tiling_submit_mid_direct=0 tiling_submit_mid_noarm=0"
    " tiling_submit_mid_before_nonzero=0 tiling_submit_mid_after_nonzero=0 tiling_submit_mid_adds_r7=0 tiling_submit_mid_adds_other=0"
    " tiling_submit_final_calls=0 tiling_submit_final_r7_0=0 tiling_submit_final_r7_1=0 tiling_submit_final_r7_other=0"
    " tiling_submit_final_deferred=0 tiling_submit_final_direct=0 tiling_submit_final_noarm=0"
    " tiling_submit_final_before_nonzero=0 tiling_submit_final_after_nonzero=0 tiling_submit_final_adds_r7=0 tiling_submit_final_adds_other=0"
    " tiling_submit_other_calls=0 tiling_submit_other_r7_0=0 tiling_submit_other_r7_1=0 tiling_submit_other_r7_other=0"
    " tiling_submit_other_deferred=0 tiling_submit_other_direct=0 tiling_submit_other_noarm=0"
    " tiling_submit_other_before_nonzero=0 tiling_submit_other_after_nonzero=0 tiling_submit_other_adds_r7=0 tiling_submit_other_adds_other=0"
    " tiling_token_record=0 tiling_token_80=0 tiling_token_81=0 tiling_token_82=0 tiling_token_83=0 tiling_token_84=0 tiling_token_85=0"
    " tiling_token_86=0 tiling_token_87=0 tiling_token_88=0 tiling_token_89=0 tiling_token_8a=0 tiling_token_8b=0 tiling_token_8e=0"
    " tiling_token_8c=0 tiling_token_8d=0 tiling_token_end=0 tiling_token_link=0 tiling_token_other=0"
    " tiling_callback_calls=0 tiling_callback_distinct=0 tiling_token_8c_installs=0"
    " tiling_write_b120_device=0 tiling_write_b120_ring=0 tiling_write_b120_other=0"
    " tiling_write_b4c8_device=0 tiling_write_b4c8_ring=0 tiling_write_b4c8_other=0"
    " tiling_write_b60c_device=0 tiling_write_b60c_ring=0 tiling_write_b60c_other=0"
    " tiling_write_b6fc_device=0 tiling_write_b6fc_ring=0 tiling_write_b6fc_other=0"
    " tiling_ring_registered=0 tiling_sink_publisher=0 tiling_sink_541c=0 tiling_sink_5b0c=0"
    " tiling_watchdog_enter=0 tiling_watchdog_global=0 tiling_watchdog_reset=0 tiling_watchdog_zero=0 tiling_probe_error=0";

void testFormat()
{
    UNSCOPED_INFO("the format of an empty window: " + FormatTilingProbeCounts(TilingProbeCounts{}));
    REQUIRE(FormatTilingProbeCounts(TilingProbeCounts{}) == kExpectedFormat);
    TilingProbeCounts counts;
    counts.quiesceOk                                                                                                = 3;
    counts.submit[static_cast<std::size_t>(TilingSubmitSite::Final)].r7One                                          = 4;
    counts.token[static_cast<std::size_t>(TilingToken::T8C)]                                                        = 5;
    counts.write[static_cast<std::size_t>(TilingWriteSite::B6FC)][static_cast<std::size_t>(TilingWriteClass::Ring)] = 6;
    counts.watchdogZero                                                                                             = 7;
    const auto text                                                                                                 = FormatTilingProbeCounts(counts);
    for (const char* field : { " tiling_quiesce_ok=3 ", " tiling_submit_final_r7_1=4 ", " tiling_token_8c=5 ", " tiling_write_b6fc_ring=6 ", " tiling_watchdog_zero=7 " })
    {
        UNSCOPED_INFO(std::string("formatted field") + field);
        REQUIRE(text.find(field) != std::string::npos);
    }
}

void testHookEntries()
{
    // Off by default: every entry returns at once.
    UNSCOPED_INFO("the probe is off by default");
    REQUIRE(!TilingProbeRequested());
    PublishedTilingProbe().Take();
    {
        std::uint64_t            result = 0;
        const TilingBracketScope bracket(0x40010000, result);
        const TilingSubmitScope  submit(0x40010000, 0x826AC3E8, 0);
        NoteTilingSubmitArm(TilingArm::Direct);
        ObserveTilingGuard(1);
        ObserveTilingInterpreter();
        ObserveTilingInterpreterExit();
        ObserveTilingListWord(0x70000000);
        ObserveTilingHandlerToken(0x80000000);
        ObserveTilingHandlerLink(0xC0001234);
        ObserveTilingCallback(0x826AB740, 0x70000000);
        ObserveTilingWrite(TilingWriteSite::B120, 0x70000008);
        ObserveTilingWorkerIdle(0x40012AC0);
        ObserveTilingPublisher(0x40010000);
        ObserveTilingWatchdog(2, 0x40010000);
        ObserveTilingRing(0x01000000, 16);
    }
    UNSCOPED_INFO("the entries count nothing while the probe is off");
    REQUIRE(FormatTilingProbeCounts(PublishedTilingProbe().Take()) == kExpectedFormat);

    // On, with no kernel: each entry counts what it can and an error for what it cannot read, and returns.
    REXCVAR_SET(native_tiling_quiesce_check, true);
    UNSCOPED_INFO("the cvar is read once");
    REQUIRE(!TilingProbeRequested());
    UNSCOPED_INFO("the cvar turns the probe on");
    REQUIRE((RefreshTilingProbeRequested() && TilingProbeRequested()));
    ObserveTilingInterpreter();
    ObserveTilingInterpreterExit();
    ObserveTilingGuard(0);
    ObserveTilingRing(0x01000000, 16);
    ObserveTilingWatchdog(0, 0x40010000);
    ObserveTilingWatchdog(1, 0x40010000);
    ObserveTilingHandlerToken(0x81000000);
    ObserveTilingHandlerLink(0xC0001234);
    ObserveTilingHandlerLink(0x8F000000);
    ObserveTilingListWord(0x70000000);
    ObserveTilingCallback(0x826AB740, 0x70000000);
    ObserveTilingWorkerIdle(0x40012AC0);
    ObserveTilingPublisher(0x40010000);
    ObserveTilingWrite(TilingWriteSite::B4C8, 0x70000000);
    {
        std::uint64_t            result = 0;
        const TilingBracketScope bracket(0x40010000, result);
        const TilingSubmitScope  submit(0x40010000, 0x826AC368, 1);
    }
    const auto counts = PublishedTilingProbe().Take();
    UNSCOPED_INFO("entries that read nothing still count: " + FormatTilingProbeCounts(counts));
    REQUIRE((counts.workerLists == 1 && counts.guardZero == 1 && counts.ringRegistered == 1 && counts.watchdogEnter == 1 && counts.watchdogGlobal == 1 &&
             counts.token[static_cast<std::size_t>(TilingToken::T81)] == 1 &&
             counts.token[static_cast<std::size_t>(TilingToken::Link)] == 1));
    // The write classifies against an unreadable device and counts as other; the ring is registered but small.
    UNSCOPED_INFO("an unreadable device leaves the store in other");
    REQUIRE(counts.write[static_cast<std::size_t>(TilingWriteSite::B4C8)][static_cast<std::size_t>(TilingWriteClass::Other)] == 1);
    // Without a kernel the reads of the list word, callback, idle, publisher and submit fail and are errors; a bracket counts as unreadable.
    UNSCOPED_INFO("unreadable guest memory counts errors: " + std::to_string(counts.errors));
    REQUIRE((counts.bracketUnreadable == 1 && counts.errors == 5));
    REXCVAR_SET(native_tiling_quiesce_check, false);
    UNSCOPED_INFO("the cvar turns the probe off again");
    REQUIRE((TilingProbeRequested() && !RefreshTilingProbeRequested() && !TilingProbeRequested()));
}

} // namespace

TEST_CASE("tiling_probe", "[ctest][tiling-probe][timeout-60]")
{
    testExpectedOffsets();
    testCompare();
    testWorkersIdle();
    testClassifyToken();
    testSubmitSite();
    testWriteClass();
    testQuiesce();
    testStaleIdle();
    testOverlap();
    testInFlight();
    testHandlerLinks();
    testDeltaLogLimit();
    testSubmit();
    testTokens();
    testCallbacks();
    testWrites();
    testRingSinksWatchdog();
    testErrors();
    testFormat();
    testHookEntries();
}
