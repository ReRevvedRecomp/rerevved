#include "gpu/completion/completion_ledger.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <latch>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace rerevved::gpu::completion;
using rerevved::gpu::guest_d3d::NativeDrawHook;
using rerevved::gpu::guest_d3d::NativeDrawMode;
using rerevved::gpu::guest_d3d::NativeDrawReason;

namespace
{

constexpr std::uint32_t kBlock     = 0x1FC9C000; // physical B
constexpr std::uint32_t kWriteback = kBlock + kReadPointerOffset;
constexpr std::uint32_t kScratch   = 0x1FC9B000; // physical S2
constexpr std::uint32_t kFlip      = kScratch + kScratchFlipOffset;
constexpr std::uint32_t kCallback  = 0x826A3FB8;
constexpr std::uint32_t kCaller    = 0x82303E40;

using Write = std::pair<std::uint32_t, std::uint32_t>;

// Guest memory as words; every ledger store is logged in order.
struct FakeSink final : IGuestProgressSink
{
    std::mutex                             mutex;
    std::map<std::uint32_t, std::uint32_t> words;
    std::vector<Write>                     writes;
    std::vector<std::uint32_t>             interrupts;
    std::function<void(FakeSink&)>         handler; // the guest's source-1 handler
    std::function<void()>                  raising; // runs before the handler, unlocked
    std::function<void()>                  copying;
    std::optional<InterruptResult>         forcedResult; // the interrupt is not delivered; the handler does not run
    std::optional<std::uint32_t>           failImageAt;  // WriteGuestImage fails at this address
    std::vector<std::vector<std::uint8_t>> imageBytes;   // the bytes of every image write

    bool WriteGuestImage(std::uint32_t address, std::span<const std::uint8_t> bytes) override
    {
        if (copying)
            copying();
        if (failImageAt == address)
            return false;
        imageBytes.emplace_back(bytes.begin(), bytes.end());
        return WritePhysical(address, static_cast<std::uint32_t>(bytes.size()));
    }

    bool WritePhysical(std::uint32_t address, std::uint32_t value) override
    {
        std::lock_guard lock(mutex);
        words[address] = value;
        writes.emplace_back(address, value);
        return true;
    }

    std::optional<std::uint32_t> ReadPhysical(std::uint32_t address) override
    {
        std::lock_guard lock(mutex);
        const auto      found = words.find(address);
        return found == words.end() ? 0 : found->second;
    }

    InterruptResult RaiseCallbackInterrupt(std::uint32_t cpu) override
    {
        if (raising)
            raising();
        std::lock_guard lock(mutex);
        interrupts.push_back(cpu);
        if (forcedResult)
            return *forcedResult;
        if (handler)
            handler(*this);
        return InterruptResult::Raised;
    }

    std::vector<Write> Writes()
    {
        std::lock_guard lock(mutex);
        return writes;
    }

    void Set(std::uint32_t address, std::uint32_t value)
    {
        std::lock_guard lock(mutex);
        words[address] = value;
    }
};

// The guest handler 0x8269B450 clears its CPU's bit in S2+0.
void clearCpuBit(FakeSink& sink)
{
    sink.words[kScratch + kScratchMaskOffset] &= ~(1U << kCallbackCpu);
}

struct FakeDraws final : IDrawCompletion
{
    std::optional<rerevved::gpu::guest_d3d::GuestResolveImage> image;
    // Images by op mark; a take of a listed mark ignores `image`.
    std::map<std::uint64_t, rerevved::gpu::guest_d3d::GuestResolveImage> images;
    std::string                                                          imageError;
    bool                                                                 closing = false;
    std::vector<std::uint64_t>                                           taken, discarded, written;
    // Marks the source holds unencoded, and older held images a take writes first.
    std::vector<std::uint64_t>                                                                                  holds;
    std::map<std::uint64_t, std::vector<std::pair<std::uint64_t, rerevved::gpu::guest_d3d::GuestResolveImage>>> before;

    GuestImageTake TakeGuestImage(std::uint64_t opMark) override
    {
        taken.push_back(opMark);
        if (closing)
            return { std::nullopt, {}, true };
        if (std::ranges::find(holds, opMark) != holds.end())
            return { std::nullopt, {}, false, true };
        GuestImageTake take;
        if (const auto found = images.find(opMark); found != images.end())
            take = { found->second, {}, false };
        else
            take = { std::exchange(image, std::nullopt), imageError, false };
        if (const auto found = before.find(opMark); found != before.end())
            take.before = found->second;
        return take;
    }

    void DiscardGuestImagesThrough(std::uint64_t opMark) override
    {
        discarded.push_back(opMark);
    }

    void GuestImageWritten(std::uint64_t opMark) override
    {
        written.push_back(opMark);
    }

    std::vector<std::uint64_t> skipped;
    std::uint64_t              submitted = 0;

    void GuestImageSkipped(std::uint64_t opMark) override
    {
        skipped.push_back(opMark);
    }

    std::uint64_t LastSubmitted() const override
    {
        return submitted;
    }

    std::vector<std::uint64_t> flushed;
    std::uint64_t              next = 0, completed = 0;
    bool                       refuse  = false;
    int                        retries = 0; // flushes answered "not yet" first

    DrawFlush FlushThrough(std::uint64_t sequence) override
    {
        if (refuse)
            return { 0, false, "fixture_flush_refused" };
        if (retries > 0)
        {
            --retries;
            return { 0, true, "fixture_not_recorded" };
        }
        flushed.push_back(sequence);
        return { ++next, false, {} };
    }

    std::uint64_t Completed() const override
    {
        return completed;
    }
};

struct Harness
{
    FakeSink                   sink;
    FakeDraws                  draws;
    std::vector<LedgerFailure> failures;
    std::vector<LedgerFailure> censused; // census reports, one per new form
    CompletionLedger           ledger;

    explicit Harness(NativeDrawMode mode = NativeDrawMode::Strict, bool withDraws = true)
    : ledger(LedgerConfig{ mode, &sink, withDraws ? &draws : nullptr, [this](const LedgerFailure& failure)
                           {
                               failures.push_back(failure);
                           },
                           [this](const LedgerFailure& failure)
                           {
                               censused.push_back(failure);
                           } })
    {
        ledger.BeginRingEpoch();
        ledger.RegisterWriteback(kWriteback);
    }
};

// A persistent thread standing in for the tile-replay worker, so its id stays
// distinct from the recording thread. Run blocks until the task finished.
class Worker
{
public:
    Worker()
    : thread([this]
             {
                 loop();
             })
    {
    }

    ~Worker()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        thread.join();
    }

    void Run(std::function<void()> work)
    {
        std::unique_lock lock(mutex);
        task = std::move(work);
        changed.notify_all();
        changed.wait(lock, [&]
                     {
                         return !task;
                     });
    }

    // The 0x826AB890 submit and the 0x8269B900 kick that follows it.
    void Submit(CompletionLedger& ledger, std::uint32_t physical, std::uint32_t dwords, std::uint32_t writePointer)
    {
        Run([&]
            {
                ledger.ReplaySubmit(physical, dwords, 0x826AB890);
                ledger.Kick(writePointer);
            });
    }

private:
    void loop()
    {
        std::unique_lock lock(mutex);
        for (;;)
        {
            changed.wait(lock, [&]
                         {
                             return stopping || task;
                         });
            if (!task)
                return;
            task();
            task = nullptr;
            changed.notify_all();
        }
    }

    std::mutex              mutex;
    std::condition_variable changed;
    std::function<void()>   task;
    bool                    stopping = false;
    std::thread             thread;
};

constexpr std::uint32_t kTile0      = 0x00110000; // tiled segments
constexpr std::uint32_t kTile1      = 0x00120000;
constexpr std::uint32_t kEndFence   = 0x00130000; // EndTiling's fence side buffer
constexpr std::uint32_t kNextFrame  = 0x00140000; // a main segment deferred after it
constexpr std::uint32_t kWorkerKick = 0x826AC020;
constexpr std::uint32_t kSideBuffer = 0x00150000; // the 0x826AC32C worker-kick callback buffer
constexpr std::uint32_t kCoher      = 0x00160000; // EndTiling's COHER buffer

// The values written to RPTR, in order.
std::vector<std::uint32_t> readPointers(FakeSink& sink)
{
    std::vector<std::uint32_t> values;
    for (const auto& [address, value] : sink.Writes())
        if (address == kWriteback)
            values.push_back(value);
    return values;
}

// EndTiling's 0x826AC32C callback for the newest list, its buffer submitted
// directly by 0x8269BD98 while the worker is idle: the worker may replay that list.
void startList(CompletionLedger& ledger, std::uint32_t buffer, std::uint32_t writePointer)
{
    ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000007, std::nullopt, buffer);
    ledger.DirectSubmit(buffer, 0x20);
    ledger.Kick(writePointer);
}

std::string failureDetail(const std::vector<LedgerFailure>& failures)
{
    return failures.empty() ? std::string("none") : failures.front().detail;
}

void testHeldHostFence()
{
    Harness h;
    h.sink.handler = clearCpuBit;
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    h.ledger.FinishDraw(7, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    h.ledger.InvalidateState(0x8269BCA8);
    h.ledger.Fence(kBlock, 0x25, 0x00C0FFE1);
    h.ledger.Callback(kScratch, kCallback, 0x1234, 0, std::nullopt);
    h.ledger.Kick(0x1F);
    for (int i = 0; i < 3; ++i)
        h.ledger.Pump();
    UNSCOPED_INFO("a held host fence allows no guest write and no source-1 interrupt");
    REQUIRE((h.sink.Writes().empty() && h.sink.interrupts.empty()));
    UNSCOPED_INFO("the ledger flushes through the span's last admitted draw once");
    REQUIRE(h.draws.flushed == std::vector<std::uint64_t>{ 7 });

    h.draws.completed = 1;
    UNSCOPED_INFO("the span releases once its host fence completes");
    REQUIRE(h.ledger.Pump());
    const std::vector<Write> expected{
        { kBlock + kCursorOffset, 0x00C0FFE1 },
        { kBlock + kFenceOffset, 0x25 },
        { kScratch + kScratchCallbackOffset, kCallback },
        { kScratch + kScratchParameterOffset, 0x1234 },
        { kScratch + kScratchMaskOffset, kCallbackMask },
        { kScratch + kScratchCallbackOffset, kRetiredCallback },
        { kWriteback, 0x1F },
    };
    UNSCOPED_INFO("release writes B+4, B+0, the callback slot, retires it, and RPTR last with the raw WPTR");
    REQUIRE(h.sink.Writes() == expected);
    UNSCOPED_INFO("one source-1 interrupt on CPU 2");
    REQUIRE(h.sink.interrupts == std::vector<std::uint32_t>{ kCallbackCpu });
    const auto snapshot = h.ledger.Snapshot();
    UNSCOPED_INFO("the snapshot reports the released fence and callback");
    REQUIRE((snapshot.spansReleased == 1 && snapshot.lastReleasedFence == 0x25 && snapshot.callbacksReleased == 1 && !snapshot.pending));
    UNSCOPED_INFO("an admitted span is not a failure");
    REQUIRE(h.failures.empty());
}

void testNoDrawSpanReleasesAtKick()
{
    Harness h;
    h.ledger.Fence(kBlock, 3, 0x40);
    for (int i = 0; i < 3; ++i)
    {
        UNSCOPED_INFO("an open span never releases");
        REQUIRE(!h.ledger.Pump());
    }
    UNSCOPED_INFO("nothing is written before the kick");
    REQUIRE(h.sink.Writes().empty());
    UNSCOPED_INFO("the debt view names the missing kick");
    REQUIRE(h.ledger.Snapshot().oldestRecord == "awaiting CP_RB_WPTR kick");
    h.ledger.Kick(0x16);
    UNSCOPED_INFO("the kick closes a no-draw span, which releases at once");
    REQUIRE(h.ledger.Pump());
    UNSCOPED_INFO("a no-draw span releases its fence and RPTR");
    REQUIRE(h.sink.Writes() == std::vector<Write>{ { kBlock + kCursorOffset, 0x40 }, { kBlock, 3 }, { kWriteback, 0x16 } });
    h.ledger.Kick(0x1A);
    UNSCOPED_INFO("a kick with no records still owes RPTR");
    REQUIRE((h.ledger.Pump() && h.sink.Writes().back() == Write{ kWriteback, 0x1A }));
    UNSCOPED_INFO("no host work is requested without admitted draws");
    REQUIRE(h.draws.flushed.empty());
}

void testUnownedSpan()
{
    for (const auto mode : { NativeDrawMode::Strict, NativeDrawMode::Skip })
    {
        Harness h(mode);
        h.ledger.Fence(kBlock, 3, 0x40);
        h.ledger.Unmodeled(0x826A9CA8);
        h.ledger.Kick(0x10);
        h.ledger.Fence(kBlock, 5, 0x80);
        h.ledger.Kick(0x20);
        for (int i = 0; i < 3; ++i)
            h.ledger.Pump();
        UNSCOPED_INFO("an unowned span writes nothing, not even its earlier fence");
        REQUIRE(h.sink.Writes().empty());
        const auto snapshot = h.ledger.Snapshot();
        // A failed strict session ignores the later kick, so only skip mode closes the span.
        const bool closed = mode == NativeDrawMode::Skip ? snapshot.oldestClosed && snapshot.writePointer == 0x10 : !snapshot.oldestClosed;
        UNSCOPED_INFO("the debt view names the span, its WPTR and the unmodeled emitter: " + snapshot.unowned);
        REQUIRE((snapshot.pending && closed &&
                 snapshot.unowned == "unmodeled_emitter 0x826a9ca8" && snapshot.unmodeledEmitter == 0x826A9CA8 &&
                 snapshot.oldestRecord == "unmodeled at 0x826a9ca8"));
        if (mode == NativeDrawMode::Strict)
        {
            UNSCOPED_INFO("strict mode fails the session once at the unmodeled record");
            REQUIRE((h.failures.size() == 1 && h.failures[0].kind == ProgressKind::Unmodeled && h.failures[0].address == 0x826A9CA8 &&
                     h.ledger.Failure()));
        }
        else
        {
            UNSCOPED_INFO("skip mode only holds the span");
            REQUIRE((h.failures.empty() && !h.ledger.Failure()));
        }
    }
}

void testStrictDrawFailure()
{
    Harness h;
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.Kick(0x10);
    UNSCOPED_INFO("boot spans before the first draw release");
    REQUIRE(h.ledger.Pump());
    h.ledger.BeginDraw(NativeDrawHook::NonindexedUp, 0x82E3987C);
    h.ledger.FinishDraw(1, NativeDrawHook::NonindexedUp, 0x82E3987C, NativeDrawReason::NoNativeRoute);
    UNSCOPED_INFO("the first unsupported draw fails at once, naming hook and caller: " + (h.failures.empty() ? std::string() : h.failures[0].detail));
    REQUIRE((h.failures.size() == 1 && h.failures[0].detail == "draw nonindexed_up from 0x82e3987c: no_native_route" &&
             h.failures[0].kind == ProgressKind::Draw && h.failures[0].address == 0x82E3987C));
    h.ledger.Fence(kBlock, 5, 0x80);
    h.ledger.Kick(0x20);
    const auto before = h.sink.Writes().size();
    UNSCOPED_INFO("a failed session releases nothing more");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().size() == before));

    Harness refused;
    refused.ledger.BeginDraw(NativeDrawHook::IndexedUp, 0x82303E90);
    refused.ledger.FinishDraw(1, NativeDrawHook::IndexedUp, 0x82303E90, NativeDrawReason::NoNativeRoute, "gfx_blend_control 0x2201: 0x1");
    UNSCOPED_INFO("the failure names the family refusal: " + (refused.failures.empty() ? std::string() : refused.failures[0].detail));
    REQUIRE((refused.failures.size() == 1 && refused.failures[0].detail == "draw indexed_up from 0x82303e90: no_native_route; gfx_blend_control 0x2201: 0x1"));
}

void testSkipMode()
{
    Harness h(NativeDrawMode::Skip);
    h.ledger.BeginDraw(NativeDrawHook::NonindexedUp, kCaller);
    h.ledger.FinishDraw(1, NativeDrawHook::NonindexedUp, kCaller, NativeDrawReason::NoNativeRoute);
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.Kick(0x10);
    UNSCOPED_INFO("a raster-only skip releases");
    REQUIRE(h.ledger.Pump());
    UNSCOPED_INFO("the swap epoch that skipped a draw is labeled partial");
    REQUIRE(h.ledger.Swap(0x1F000000));
    UNSCOPED_INFO("a later epoch without skips is not partial");
    REQUIRE(!h.ledger.Swap(0x1F000000));
    auto snapshot = h.ledger.Snapshot();
    UNSCOPED_INFO("the partial span and its no_native_route skip are counted");
    REQUIRE((snapshot.partialSpans == 1 && snapshot.partialSwaps == 1 && snapshot.skipped[0] == 1 && h.failures.empty()));

    h.ledger.Resolve(0x82511000, 0x1E000000);
    h.ledger.FinishResolve(0, false, false, "resolve_form_unsupported");
    h.ledger.Fence(kBlock, 5, 0x80);
    h.ledger.Kick(0x20);
    const auto before = h.sink.Writes().size();
    UNSCOPED_INFO("a skipped resolve to guest memory blocks its span");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().size() == before));
    snapshot = h.ledger.Snapshot();
    UNSCOPED_INFO("skip mode names the blocking resolve without failing");
    REQUIRE((snapshot.unowned == "resolve_form_unsupported" && h.failures.empty()));

    for (const auto reason : { NativeDrawReason::AutomaticIndex, NativeDrawReason::NestingMismatch, NativeDrawReason::StateUnreadable, NativeDrawReason::DispatchFailure })
    {
        Harness other(NativeDrawMode::Skip);
        other.ledger.BeginDraw(NativeDrawHook::AutomaticIndex, kCaller);
        other.ledger.FinishDraw(1, NativeDrawHook::AutomaticIndex, kCaller, reason);
        other.ledger.Kick(0x10);
        UNSCOPED_INFO(std::string("skip mode may not release a ") + rerevved::gpu::guest_d3d::NativeDrawReasonName(reason) + " skip");
        REQUIRE((!other.ledger.Pump() && other.sink.Writes().empty() &&
                 other.ledger.Snapshot().unowned.ends_with("_not_skippable")));
    }
}

void testFrontbufferException()
{
    for (auto mode : { NativeDrawMode::Strict, NativeDrawMode::Skip })
    {
        Harness h(mode);
        h.ledger.Resolve(kFrontbufferResolveCall, 0x1F000000);
        h.ledger.FinishResolve(1, true, false);
        h.ledger.Kick(0x10);
        UNSCOPED_INFO("frontbuffer ownership waits for VdSwap");
        REQUIRE(!h.ledger.Pump());
        h.ledger.Swap(0x1F000000);
        UNSCOPED_INFO("frontbuffer ownership waits for swap host output");
        REQUIRE(!h.ledger.Pump());
        h.draws.completed = 1;
        UNSCOPED_INFO("frontbuffer output owns the resolve in both modes without guest image bytes");
        REQUIRE((h.ledger.Pump() && h.sink.Writes() == std::vector<Write>{ { kWriteback, 0x10 } }));
    }
    Harness mismatch(NativeDrawMode::Skip);
    mismatch.ledger.Resolve(kFrontbufferResolveCall, 0x1F000000);
    mismatch.ledger.FinishResolve(1, true, false);
    mismatch.ledger.Kick(0x10);
    mismatch.ledger.Swap(0x1F400000);
    UNSCOPED_INFO("another scanout buffer cannot own a resolve");
    REQUIRE((!mismatch.ledger.Pump() && mismatch.ledger.Snapshot().unowned.starts_with("frontbuffer_resolve_mismatch")));
}

void testGuestImageOrder()
{
    constexpr std::uint32_t destination = 0x1E3B0000;
    Harness                 h;
    h.draws.image = rerevved::gpu::guest_d3d::GuestResolveImage{ destination, { 255, 255, 255, 255 } };
    h.ledger.Clear(1);
    h.ledger.Resolve(kFrontbufferResolveCall, destination);
    h.ledger.FinishResolve(2, false, false);
    h.ledger.Fence(kBlock, 7, 0x80);
    h.ledger.Kick(0x20);
    UNSCOPED_INFO("image waits for its host fence");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().empty()));
    h.draws.completed = 1;
    UNSCOPED_INFO("completed image releases");
    REQUIRE(h.ledger.Pump());
    UNSCOPED_INFO("image precedes B+0 and RPTR is last");
    REQUIRE((h.draws.flushed == std::vector<std::uint64_t>{ 2 } && h.sink.Writes() == std::vector<Write>{
                                                                                          { destination, 4 }, { kBlock + 4, 0x80 }, { kBlock, 7 }, { kWriteback, 0x20 } }));
    UNSCOPED_INFO("the take names the Resolve record's op mark, not the clear's, and the written report follows the write");
    REQUIRE((h.draws.taken == std::vector<std::uint64_t>{ 2 } && h.draws.written == h.draws.taken));

    // Two spans take their images in op-mark order, each by its Resolve
    // record's mark even when a later clear is the span's last admitted work.
    constexpr std::uint32_t other = 0x1E400000;
    Harness                 two;
    two.draws.images = { { 2, { destination, { 1, 2 } } }, { 5, { other, { 3, 4, 5 } } } };
    two.ledger.Resolve(kFrontbufferResolveCall, destination);
    two.ledger.FinishResolve(2, false, false);
    two.ledger.Clear(3);
    two.ledger.Kick(0x10);
    two.ledger.Resolve(kFrontbufferResolveCall, other);
    two.ledger.FinishResolve(5, false, false);
    two.ledger.Kick(0x20);
    two.draws.completed = 2;
    UNSCOPED_INFO("both spans take their own marks in order");
    REQUIRE((two.ledger.Pump() && two.draws.taken == std::vector<std::uint64_t>{ 2, 5 } && two.failures.empty()));
    UNSCOPED_INFO("each image lands before its span's RPTR");
    REQUIRE(two.sink.Writes() == std::vector<Write>{ { destination, 2 }, { kWriteback, 0x10 }, { other, 3 }, { kWriteback, 0x20 } });
    for (bool epoch : { false, true })
    {
        Harness closed;
        closed.draws.image = rerevved::gpu::guest_d3d::GuestResolveImage{ destination, { 255 } };
        closed.ledger.Resolve(kFrontbufferResolveCall, destination);
        closed.ledger.FinishResolve(1, false, false);
        closed.ledger.Kick(8);
        if (epoch)
            closed.ledger.CloseEpochWrites();
        else
            closed.ledger.Close();
        closed.draws.completed = 1;
        UNSCOPED_INFO("no image after close or epoch close");
        REQUIRE((!closed.ledger.Pump() && closed.sink.Writes().empty()));
    }
    Harness missing;
    missing.ledger.Resolve(kFrontbufferResolveCall, destination);
    missing.ledger.FinishResolve(1, false, false);
    missing.ledger.Kick(8);
    missing.draws.completed = 1;
    UNSCOPED_INFO("missing image fails strict before guest progress and is never reported written");
    REQUIRE((!missing.ledger.Pump() && missing.ledger.Failure() == "resolve_readback_failed" && missing.sink.Writes().empty() &&
             missing.draws.written.empty()));
    Harness detailed;
    detailed.draws.imageError = "readback_lost";
    detailed.ledger.Resolve(kFrontbufferResolveCall, destination);
    detailed.ledger.FinishResolve(1, false, false);
    detailed.ledger.Kick(8);
    detailed.draws.completed = 1;
    UNSCOPED_INFO("the readback failure carries the source's reason");
    REQUIRE((!detailed.ledger.Pump() && detailed.ledger.Failure() == "resolve_readback_failed: readback_lost"));
    // Shutdown closes the source before the ledger; that is not a readback failure.
    Harness closing;
    closing.draws.closing = true;
    closing.ledger.Resolve(kFrontbufferResolveCall, destination);
    closing.ledger.FinishResolve(1, false, false);
    closing.ledger.Kick(8);
    closing.draws.completed = 1;
    UNSCOPED_INFO("a closing source holds the resolve span without failing");
    REQUIRE((!closing.ledger.Pump() && !closing.ledger.Pump() && closing.failures.empty() && closing.ledger.Snapshot().unowned.empty() &&
             closing.sink.Writes().empty()));
    closing.ledger.Close();
    UNSCOPED_INFO("ledger close after a closing source stays quiet");
    REQUIRE((!closing.ledger.Pump() && closing.failures.empty()));
    Harness partial(NativeDrawMode::Skip);
    partial.ledger.Resolve(kFrontbufferResolveCall, destination);
    partial.ledger.FinishResolve(1, false, true);
    partial.ledger.Kick(8);
    partial.draws.completed = 1;
    UNSCOPED_INFO("skip cannot release partial source bytes");
    REQUIRE((!partial.ledger.Pump() && partial.ledger.Snapshot().unowned == "resolve_source_partial" && partial.sink.Writes().empty()));
}

// Takes land older held images first; the sink stops all writes at close.
void testHeldGuestImages()
{
    using rerevved::gpu::guest_d3d::GuestResolveImage;
    constexpr std::uint32_t destination = 0x1E3B0000, other = 0x1E400000;
    Harness                 held;
    held.draws.holds = { 2 };
    held.ledger.Resolve(kFrontbufferResolveCall, destination);
    held.ledger.FinishResolve(2, false, false);
    held.ledger.Fence(kBlock, 7, 0x80);
    held.ledger.Kick(0x20);
    held.draws.completed = 1;
    UNSCOPED_INFO("a held image releases its span");
    REQUIRE((held.ledger.Pump() && held.failures.empty()));
    UNSCOPED_INFO("a held image writes nothing and is not reported written");
    REQUIRE((held.sink.Writes() == std::vector<Write>{ { kBlock + 4, 0x80 }, { kBlock, 7 }, { kWriteback, 0x20 } } && held.draws.written.empty() &&
             held.draws.taken == std::vector<std::uint64_t>{ 2 }));

    Harness ordered;
    ordered.draws.images = { { 5, { other, { 3, 4, 5 } } } };
    ordered.draws.before = { { 5, { { 2, GuestResolveImage{ destination, { 1, 2 } } }, { 3, GuestResolveImage{ destination + 0x100, { 9 } } } } } };
    ordered.ledger.Resolve(kFrontbufferResolveCall, other);
    ordered.ledger.FinishResolve(5, false, false);
    ordered.ledger.Kick(0x20);
    ordered.draws.completed = 1;
    UNSCOPED_INFO("a take with older held images releases");
    REQUIRE((ordered.ledger.Pump() && ordered.failures.empty()));
    UNSCOPED_INFO("older held images land first, in op-mark order, each reported written");
    REQUIRE((ordered.sink.Writes() == std::vector<Write>{ { destination, 2 }, { destination + 0x100, 1 }, { other, 3 }, { kWriteback, 0x20 } } &&
             ordered.draws.written == std::vector<std::uint64_t>{ 2, 3, 5 }));

    Harness lost;
    lost.draws.images     = { { 5, { other, { 3 } } } };
    lost.draws.before     = { { 5, { { 2, GuestResolveImage{ destination, { 1 } } } } } };
    lost.sink.failImageAt = destination;
    lost.ledger.Resolve(kFrontbufferResolveCall, other);
    lost.ledger.FinishResolve(5, false, false);
    lost.ledger.Kick(0x20);
    lost.draws.completed = 1;
    UNSCOPED_INFO("an older held image that cannot land fails the record before its own image");
    REQUIRE((!lost.ledger.Pump() && lost.ledger.Failure() == "resolve_readback_failed: held_image_unwritten: op mark 2" && lost.sink.Writes().empty() &&
             lost.draws.written.empty()));

    Harness gate;
    bool    ran = false;
    UNSCOPED_INFO("the gate writes an image through the sink and refuses an empty one");
    REQUIRE((gate.ledger.WithImageWrites([&](const GuestImageWriter& write)
                                         {
                                             ran = write(GuestResolveImage{ destination, { 7, 7, 7 } }) && !write(GuestResolveImage{ destination, {} });
                                         }) &&
             ran && gate.sink.Writes() == std::vector<Write>{ { destination, 3 } }));
    gate.ledger.Close();
    ran = false;
    UNSCOPED_INFO("a closed ledger runs no held write");
    REQUIRE((!gate.ledger.WithImageWrites([&](const GuestImageWriter&)
                                          {
                                              ran = true;
                                          }) &&
             !ran));
}

// An image with runs writes each run at physical + offset and nothing else.
void testGuestImageRuns()
{
    constexpr std::uint32_t   destination = 0x1E3B0000;
    std::vector<std::uint8_t> bytes(0x3000);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(i ^ ((i >> 8) * 17));
    using rerevved::gpu::guest_d3d::GuestResolveImage;
    using rerevved::gpu::guest_d3d::GuestResolveRun;
    const auto submit = [&](Harness& h, const GuestResolveImage& image)
    {
        h.draws.image = image;
        h.ledger.Clear(1);
        h.ledger.Resolve(kFrontbufferResolveCall, image.physical);
        h.ledger.FinishResolve(2, false, false);
        h.ledger.Fence(kBlock, 7, 0x80);
        h.ledger.Kick(0x20);
        h.draws.completed = 1;
    };
    const std::vector<GuestResolveRun> runs{ { 0, 0x10 }, { 0x1000, 0x20 }, { 0x2FF0, 0x10 } };

    Harness h;
    submit(h, { destination, bytes, runs });
    UNSCOPED_INFO("an image with runs releases");
    REQUIRE((h.ledger.Pump() && h.failures.empty()));
    UNSCOPED_INFO("each run lands at physical + offset before B+0 and RPTR");
    REQUIRE(h.sink.Writes() == std::vector<Write>{ { destination, 0x10 }, { destination + 0x1000, 0x20 }, { destination + 0x2FF0, 0x10 }, { kBlock + 4, 0x80 }, { kBlock, 7 }, { kWriteback, 0x20 } });
    UNSCOPED_INFO("each run carries its own bytes");
    REQUIRE((h.sink.imageBytes.size() == 3 && std::equal(h.sink.imageBytes[1].begin(), h.sink.imageBytes[1].end(), bytes.begin() + 0x1000) &&
             std::equal(h.sink.imageBytes[2].begin(), h.sink.imageBytes[2].end(), bytes.begin() + 0x2FF0)));

    // A failing second run fails the record; the first run stays written and
    // nothing after it, including RPTR, is.
    Harness failing;
    failing.sink.failImageAt = destination + 0x1000;
    submit(failing, { destination, bytes, runs });
    UNSCOPED_INFO("a failing run fails the record after the earlier runs");
    REQUIRE((!failing.ledger.Pump() && failing.ledger.Failure() == "resolve_readback_failed" &&
             failing.sink.Writes() == std::vector<Write>{ { destination, 0x10 } }));

    // A run outside the image fails before any write.
    for (const auto& bad : { GuestResolveRun{ 0x2FF8, 0x10 }, GuestResolveRun{ 0x3000, 4 }, GuestResolveRun{ 0x3004, 4 }, GuestResolveRun{ 8, 0 } })
    {
        Harness malformed;
        submit(malformed, { destination, bytes, { { 0, 0x10 }, bad } });
        UNSCOPED_INFO("a malformed run list writes nothing");
        REQUIRE((!malformed.ledger.Pump() && malformed.ledger.Failure() == "resolve_readback_failed" && malformed.sink.Writes().empty()));
    }
    // A run whose physical address wraps past 4 GiB is malformed too.
    Harness wrapping;
    submit(wrapping, { 0xFFFFFF00, bytes, { { 0, 0x10 }, { 0x100, 0x10 } } });
    UNSCOPED_INFO("a run that wraps the physical address writes nothing");
    REQUIRE((!wrapping.ledger.Pump() && wrapping.ledger.Failure() == "resolve_readback_failed" && wrapping.sink.Writes().empty()));
}

// Close/CloseEpochWrites wait for copy; gated copy-side close waits.
void testCloseDuringImage()
{
    for (bool epoch : { false, true })
    {
        Harness h;
        h.draws.image     = rerevved::gpu::guest_d3d::GuestResolveImage{ 0x1E3B0000, { 255 } };
        h.draws.completed = 1;
        h.ledger.Resolve(kFrontbufferResolveCall, 0x1E3B0000);
        h.ledger.FinishResolve(1, false, false);
        h.ledger.Fence(kBlock, 7, 8);
        h.ledger.Kick(8);
        std::latch               entered(1), closing(1);
        std::mutex               gate;
        std::condition_variable  changed;
        bool                     closed = false, closedInsideCopy = false;
        std::atomic<std::size_t> writesAtClose{ 0 };
        h.sink.copying = [&]
        {
            entered.count_down();
            closing.wait();
            std::unique_lock lock(gate);
            closedInsideCopy = changed.wait_for(lock, std::chrono::milliseconds(500), [&]
                                                {
                                                    return closed;
                                                });
        };
        std::thread pump([&]
                         {
                             h.ledger.Pump();
                         });
        entered.wait();
        std::thread closer([&]
                           {
                               closing.count_down();
                               if (epoch)
                                   h.ledger.CloseEpochWrites();
                               else
                                   h.ledger.Close();
                               writesAtClose = h.sink.Writes().size();
                               {
                                   std::lock_guard lock(gate);
                                   closed = true;
                               }
                               changed.notify_all();
                           });
        closer.join();
        pump.join();
        UNSCOPED_INFO("teardown cannot return while a bulk copy is in flight");
        REQUIRE(!closedInsideCopy);
        const auto writes = h.sink.Writes();
        UNSCOPED_INFO("the in-flight image finishes before teardown returns and nothing is written after it");
        REQUIRE((!writes.empty() && writes.front().first == 0x1E3B0000 && writesAtClose == writes.size()));
        h.ledger.Pump();
        UNSCOPED_INFO("nothing writes after completed teardown");
        REQUIRE(writes == h.sink.Writes());
    }
}

// A missed copy hook fails at the next record instead of holding forever.
void testResolveCopyGuard()
{
    Harness strict;
    strict.ledger.Resolve(kFrontbufferResolveCall, 0x1E3B0000);
    UNSCOPED_INFO("the debt view names the awaited copy hook");
    REQUIRE(strict.ledger.Snapshot().oldestRecord == "resolve at 0x8250afec awaiting its copy words");
    strict.ledger.Kick(0x10);
    UNSCOPED_INFO("strict fails at the kick that follows a resolve without its copy words");
    REQUIRE((strict.failures.size() == 1 && strict.failures[0].detail == "resolve_copy_not_captured" &&
             strict.failures[0].kind == ProgressKind::Resolve && strict.failures[0].address == kFrontbufferResolveCall));
    strict.ledger.FinishResolve(1, false, false);
    UNSCOPED_INFO("a late copy cannot revive the abandoned resolve");
    REQUIRE((strict.failures.size() == 1 && !strict.ledger.Pump()));

    Harness skip(NativeDrawMode::Skip);
    skip.ledger.Resolve(kFrontbufferResolveCall, 0x1E3B0000);
    skip.ledger.Resolve(kFrontbufferResolveCall, 0x1F000000);
    skip.ledger.FinishResolve(1, true, false);
    skip.ledger.Kick(0x10);
    skip.ledger.Swap(0x1F000000);
    skip.draws.completed = 1;
    const auto snapshot  = skip.ledger.Snapshot();
    UNSCOPED_INFO("skip names the abandoned resolve as the span's blocker: " + snapshot.unowned);
    REQUIRE((!skip.ledger.Pump() && skip.sink.Writes().empty() && snapshot.unowned == "resolve_copy_not_captured" &&
             snapshot.oldestRecord == "resolve at 0x8250afec" && skip.failures.empty()));
}

// Clears write only EDRAM; skip mode releases an unsupported one as partial.
void testUnsupportedClear()
{
    Harness skip(NativeDrawMode::Skip);
    skip.ledger.Clear(0, "clear_state_unreadable");
    skip.ledger.Fence(kBlock, 3, 0x40);
    skip.ledger.Kick(0x10);
    UNSCOPED_INFO("skip mode releases a span with an unsupported clear");
    REQUIRE((skip.ledger.Pump() && skip.failures.empty()));
    UNSCOPED_INFO("the span and its swap epoch are partial");
    REQUIRE((skip.ledger.Snapshot().partialSpans == 1 && skip.ledger.Swap(0x1F000000)));

    Harness strict;
    strict.ledger.Clear(0, "clear_state_unreadable");
    strict.ledger.Kick(0x10);
    UNSCOPED_INFO("strict fails at an unsupported clear");
    REQUIRE((!strict.ledger.Pump() && strict.failures.size() == 1 && strict.failures[0].detail == "clear_state_unreadable" &&
             strict.failures[0].kind == ProgressKind::Clear));
}

// Strict mode stops; census reports each refusal once and releases fences.
void testCensusMode()
{
    constexpr std::uint32_t destination = 0x1E3B0000;
    const std::string       unmodeled   = "resolve_source_unmodeled: other_source (fixture)";
    const std::string       encode      = "guest_resolve_encode_failed: fixture";

    Harness strict;
    strict.ledger.Resolve(kFrontbufferResolveCall, destination);
    strict.ledger.FinishResolve(0, false, false, unmodeled);
    strict.ledger.Fence(kBlock, 3, 0x40);
    strict.ledger.Kick(0x10);
    UNSCOPED_INFO("strict mode fails at an unmodeled resolve source and releases nothing");
    REQUIRE((!strict.ledger.Pump() && strict.sink.Writes().empty() && strict.failures.size() == 1 && strict.failures[0].detail == unmodeled &&
             strict.ledger.Failure() == unmodeled && strict.censused.empty() && strict.ledger.Census().empty()));

    Harness readback;
    readback.draws.imageError = encode;
    readback.ledger.Resolve(kFrontbufferResolveCall, destination);
    readback.ledger.FinishResolve(1, false, false);
    readback.ledger.Fence(kBlock, 3, 0x40);
    readback.ledger.Kick(0x10);
    readback.draws.completed = 100;
    for (int i = 0; i < 3; ++i)
        readback.ledger.Pump();
    UNSCOPED_INFO("strict mode fails at a failed resolve readback before the span's fence");
    REQUIRE((readback.sink.Writes().empty() && readback.failures.size() == 1 && readback.failures[0].detail == "resolve_readback_failed: " + encode));

    Harness h(NativeDrawMode::Census);
    h.draws.completed  = 100;
    const auto pumpAll = [&]
    {
        for (int i = 0; i < 4; ++i)
            h.ledger.Pump();
    };
    // An unmodeled resolve source, then an unsupported clear.
    h.ledger.Resolve(kFrontbufferResolveCall, destination);
    h.ledger.FinishResolve(0, false, false, unmodeled);
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.Kick(0x10);
    pumpAll();
    h.ledger.Clear(0, "clear_form_unsupported: color rect");
    h.ledger.Fence(kBlock, 5, 0x80);
    h.ledger.Kick(0x20);
    pumpAll();
    // A failed readback of an admitted resolve.
    h.draws.imageError = encode;
    h.ledger.Resolve(kFrontbufferResolveCall, destination);
    h.ledger.FinishResolve(1, false, false);
    h.ledger.Fence(kBlock, 7, 0xC0);
    h.ledger.Kick(0x30);
    pumpAll();
    // The same resolve form again, a refused draw and a swap-time failure.
    h.ledger.Resolve(kFrontbufferResolveCall, destination);
    h.ledger.FinishResolve(0, false, false, unmodeled);
    h.ledger.BeginDraw(NativeDrawHook::NonindexedUp, kCaller);
    h.ledger.FinishDraw(2, NativeDrawHook::NonindexedUp, kCaller, NativeDrawReason::NoNativeRoute);
    h.ledger.Fence(kBlock, 9, 0x100);
    h.ledger.Kick(0x40);
    h.ledger.Fail("native frame at VdSwap: fixture", ProgressKind::Swap, 0x1F000000);
    h.ledger.Fence(kBlock, 11, 0x140);
    h.ledger.Kick(0x50);
    pumpAll();

    UNSCOPED_INFO("census mode never fails the session for these forms");
    REQUIRE((h.failures.empty() && !h.ledger.Failure()));
    UNSCOPED_INFO("every span releases its fence and RPTR past the skipped operations");
    REQUIRE((readPointers(h.sink) == std::vector<std::uint32_t>{ 0x10, 0x20, 0x30, 0x40, 0x50 } && h.sink.words[kBlock] == 11 &&
             h.sink.words[kBlock + kCursorOffset] == 0x140 && h.ledger.Snapshot().spansReleased == 5));
    UNSCOPED_INFO("a skipped resolve writes no guest image");
    REQUIRE(h.sink.imageBytes.empty());
    std::vector<std::string> reported;
    for (const auto& form : h.censused)
        reported.push_back(form.detail);
    const std::vector<std::string> expected{ unmodeled,
                                             "clear_form_unsupported: color rect",
                                             "resolve_readback_failed: " + encode,
                                             "draw nonindexed_up from 0x82303e40: no_native_route",
                                             "native frame at VdSwap: fixture" };
    UNSCOPED_INFO("each distinct form is reported once, in first-seen order");
    REQUIRE(reported == expected);
    const auto forms = h.ledger.Census();
    UNSCOPED_INFO("the census counts each form with its kind and address");
    REQUIRE((forms.size() == expected.size() && forms[0].count == 2 && forms[0].failure.kind == ProgressKind::Resolve &&
             forms[0].failure.address == kFrontbufferResolveCall && forms[1].count == 1 && forms[1].failure.kind == ProgressKind::Clear &&
             forms[4].failure.kind == ProgressKind::Swap));

    // Per-operation counters do not make a new form.
    Harness marks(NativeDrawMode::Census);
    marks.draws.completed = 100;
    for (std::uint32_t mark = 1; mark <= 2; ++mark)
    {
        marks.draws.imageError = std::format("guest_image_missing: op mark {}", mark);
        marks.ledger.Resolve(kFrontbufferResolveCall, destination);
        marks.ledger.FinishResolve(mark, false, false);
        marks.ledger.Kick(0x10 * mark);
        for (int i = 0; i < 4; ++i)
            marks.ledger.Pump();
    }
    const auto markForms = marks.ledger.Census();
    UNSCOPED_INFO("readbacks that differ only by op mark share one form");
    REQUIRE((marks.censused.size() == 1 && marks.censused[0].detail == "resolve_readback_failed: guest_image_missing: op mark 1" &&
             markForms.size() == 1 && markForms[0].count == 2 && readPointers(marks.sink) == std::vector<std::uint32_t>{ 0x10, 0x20 }));
    UNSCOPED_INFO("each skipped admitted resolve retires its pending image");
    REQUIRE((marks.draws.skipped == std::vector<std::uint64_t>{ 1, 2 } && marks.draws.written.empty()));

    // A failed flush still waits for the work submitted before it.
    Harness flushed(NativeDrawMode::Census);
    flushed.draws.refuse    = true;
    flushed.draws.submitted = 5;
    flushed.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    flushed.ledger.FinishDraw(1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    flushed.ledger.Fence(kBlock, 3, 0x40);
    flushed.ledger.Kick(0x10);
    flushed.draws.completed = 4;
    for (int i = 0; i < 4; ++i)
        flushed.ledger.Pump();
    UNSCOPED_INFO("the span waits for the submitted fence after a failed flush");
    REQUIRE((flushed.sink.Writes().empty() && flushed.censused.size() == 1 && flushed.censused[0].detail == "draw_flush_failed: fixture_flush_refused"));
    flushed.draws.completed = 5;
    UNSCOPED_INFO("the span releases once the earlier submission completes");
    REQUIRE((flushed.ledger.Pump() && readPointers(flushed.sink) == std::vector<std::uint32_t>{ 0x10 } && flushed.failures.empty()));

    // A fatal failure, such as a stopped draw stream or device loss, fails
    // the session in census mode too: only the span closed before it releases.
    Harness fatal(NativeDrawMode::Census);
    fatal.ledger.Fail("native_frame_failed: fixture_refused", ProgressKind::Draw, 0);
    fatal.ledger.Fence(kBlock, 3, 0x40);
    fatal.ledger.Kick(0x10);
    fatal.ledger.Fence(kBlock, 5, 0x80);
    fatal.ledger.Fail("native_device_lost", ProgressKind::Draw, 0, true);
    fatal.ledger.Kick(0x20);
    UNSCOPED_INFO("census counts a recoverable failure and fails the session at a fatal one");
    REQUIRE((fatal.censused.size() == 1 && fatal.censused[0].detail == "native_frame_failed: fixture_refused" && fatal.failures.size() == 1 &&
             fatal.failures[0].detail == "native_device_lost" && fatal.ledger.Failure() == "native_device_lost"));
    for (int i = 0; i < 3; ++i)
        fatal.ledger.Pump();
    UNSCOPED_INFO("the span closed before the fatal failure releases, and no later one");
    REQUIRE(readPointers(fatal.sink) == std::vector<std::uint32_t>{ 0x10 });
}

void testGpuWait()
{
    Harness h;
    h.sink.handler = clearCpuBit; // the flip callback does not clear S2+4 here
    h.ledger.ScratchWrite(kFlip, 1, 0x826A419C);
    h.ledger.GpuWait(kFlip, 1, 0x826A419C);
    h.ledger.Callback(kScratch, kCallback, 0x100, 0, std::nullopt);
    h.ledger.GpuWait(kFlip, 0, 0x826A42BC);
    h.ledger.Fence(kBlock, 9, 0x44);
    h.ledger.Kick(0x30);
    UNSCOPED_INFO("the span holds at the flip wait");
    REQUIRE(!h.ledger.Pump());
    auto writes = h.sink.Writes();
    UNSCOPED_INFO("the scratch write satisfies the first wait and the callback runs");
    REQUIRE((!writes.empty() && writes.front() == Write{ kFlip, 1 } && h.sink.interrupts.size() == 1));
    UNSCOPED_INFO("records after the unmet wait are not applied");
    REQUIRE(std::none_of(writes.begin(), writes.end(), [](const Write& write)
                         {
                             return write.first == kBlock || write.first == kWriteback;
                         }));
    UNSCOPED_INFO("the debt view names the wait");
    REQUIRE(h.ledger.Snapshot().oldestRecord == "gpu_wait at 0x826a42bc");
    const auto held = writes.size();
    UNSCOPED_INFO("polling does not repeat effects");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().size() == held));
    h.sink.Set(kFlip, 0); // the flip callback or vblank clears the pending flip
    UNSCOPED_INFO("the guest word change releases the span");
    REQUIRE(h.ledger.Pump());
    writes = h.sink.Writes();
    UNSCOPED_INFO("the fence and RPTR follow the satisfied wait");
    REQUIRE((writes.size() == held + 3 && writes[held] == Write{ kBlock + kCursorOffset, 0x44 } && writes.back() == Write{ kWriteback, 0x30 }));
    UNSCOPED_INFO("the callback is not raised again");
    REQUIRE(h.sink.interrupts.size() == 1);
}

void testSourceOneSequencing()
{
    Harness h; // the handler leaves S2+0 set until the test clears it
    h.ledger.Callback(kScratch, kCallback, 0x77, 0, std::nullopt);
    h.ledger.Kick(0x8);
    UNSCOPED_INFO("the span waits for S2+0 to clear");
    REQUIRE(!h.ledger.Pump());
    UNSCOPED_INFO("callback, parameter, then mask are written before the interrupt; 0x0BADF00D waits");
    REQUIRE(h.sink.Writes() == std::vector<Write>{ { kScratch + kScratchCallbackOffset, kCallback }, { kScratch + kScratchParameterOffset, 0x77 }, { kScratch + kScratchMaskOffset, 4 } });
    UNSCOPED_INFO("mask 4 raises source 1 on CPU 2");
    REQUIRE(h.sink.interrupts == std::vector<std::uint32_t>{ kCallbackCpu });
    h.sink.Set(kScratch + kScratchMaskOffset, 0);
    UNSCOPED_INFO("a cleared mask completes the callback");
    REQUIRE(h.ledger.Pump());
    const auto writes = h.sink.Writes();
    UNSCOPED_INFO("the slot is retired with 0x0BADF00D before RPTR");
    REQUIRE((writes.size() == 5 && writes[3] == Write{ kScratch + kScratchCallbackOffset, kRetiredCallback } && writes[4] == Write{ kWriteback, 0x8 }));
    UNSCOPED_INFO("one interrupt per callback");
    REQUIRE(h.sink.interrupts.size() == 1);

    Harness flagFour;
    flagFour.ledger.Callback(kScratch, kCallback, 0x77, 4, std::nullopt);
    flagFour.ledger.Kick(0x8);
    UNSCOPED_INFO("flag 4 alone only waits for S2+0 and retires the slot");
    REQUIRE((flagFour.ledger.Pump() && flagFour.sink.interrupts.empty() &&
             flagFour.sink.Writes() == std::vector<Write>{ { kScratch + kScratchCallbackOffset, kRetiredCallback }, { kWriteback, 0x8 } }));

    Harness masked(NativeDrawMode::Skip);
    masked.ledger.Callback(kScratch, kCallback, 0x77, 0x01000006, std::nullopt);
    masked.ledger.Kick(0x8);
    UNSCOPED_INFO("a callback for another CPU has no model");
    REQUIRE((!masked.ledger.Pump() && masked.sink.Writes().empty() && masked.ledger.Snapshot().unowned == "callback_mask_0x1"));
}

// EndTiling accepts only Xenos's single-pass bin mask 0x80000000.
void testPredicatedCallback()
{
    for (const std::uint32_t mask : { 0x0U, 0x3U, 0xCU, 0xFU, 0xFFFFFFFFU })
    {
        Harness predicated(NativeDrawMode::Skip);
        predicated.ledger.Callback(kScratch, kCallback, 0x77, 0, mask);
        predicated.ledger.Kick(0x8);
        const auto expected = std::format("callback_predicated {:#x}", mask);
        UNSCOPED_INFO("a predicated callback with mask other than 0x80000000 has no model: " + expected);
        REQUIRE((!predicated.ledger.Pump() && predicated.sink.Writes().empty() && predicated.ledger.Snapshot().unowned == expected));
    }

    // The first-pass callback waits for the 0x826AC32C callback's buffer to be
    // submitted, here with flags 4 alone so only the first one interrupts.
    Harness once(NativeDrawMode::Strict);
    once.ledger.Callback(kScratch, kCallback, 0x77, 0x04000006, 0x80000000);
    once.ledger.Kick(0x8);
    UNSCOPED_INFO("a kick of another buffer does not release the first-pass callback");
    REQUIRE((once.ledger.Pump() && once.sink.Writes() == std::vector<Write>{ { kWriteback, 0x8 } } && once.sink.interrupts.empty()));
    once.ledger.Callback(kScratch, kCallback, 0x78, 4, std::nullopt, kSideBuffer);
    once.ledger.DirectSubmit(kSideBuffer, 0x20);
    once.ledger.Kick(0x10);
    UNSCOPED_INFO("the first-pass callback waits for S2+0 to clear");
    REQUIRE(!once.ledger.Pump());
    UNSCOPED_INFO("the first-pass callback writes its slot and raises source 1 exactly once; 0x0BADF00D waits");
    REQUIRE((once.sink.Writes() == std::vector<Write>{ { kWriteback, 0x8 }, { kScratch + kScratchCallbackOffset, kCallback }, { kScratch + kScratchParameterOffset, 0x77 }, { kScratch + kScratchMaskOffset, 4 } } &&
             once.sink.interrupts == std::vector<std::uint32_t>{ kCallbackCpu } && once.ledger.Snapshot().callbacksReleased == 1 && once.failures.empty()));
    once.sink.Set(kScratch + kScratchMaskOffset, 0);
    UNSCOPED_INFO("a cleared mask retires the slot and releases RPTR without a second interrupt");
    REQUIRE((once.ledger.Pump() && once.sink.interrupts.size() == 1 && once.ledger.Snapshot().callbacksReleased == 1 &&
             once.sink.Writes().size() == 7 && once.sink.Writes()[4] == Write{ kScratch + kScratchCallbackOffset, kRetiredCallback } &&
             once.sink.Writes()[6] == Write{ kWriteback, 0x10 }));

    // The 0x826AC32C form follows with the same function and parameter, unpredicated.
    Harness pair(NativeDrawMode::Strict);
    pair.sink.handler = clearCpuBit;
    pair.ledger.Callback(kScratch, kCallback, 0x88, 0x04000006, 0x80000000);
    pair.ledger.Callback(kScratch, kCallback, 0x99, 0x04000007, std::nullopt, kSideBuffer);
    pair.ledger.DirectSubmit(kSideBuffer, 0x20);
    pair.ledger.Kick(0x10);
    UNSCOPED_INFO("a first-pass callback and the unpredicated one after it raise two interrupts");
    REQUIRE((pair.ledger.Pump() && pair.sink.interrupts == std::vector<std::uint32_t>{ kCallbackCpu, kCallbackCpu } &&
             pair.ledger.Snapshot().callbacksReleased == 2 && pair.failures.empty()));
    const std::vector<Write> expected{
        { kScratch + kScratchCallbackOffset, kCallback },
        { kScratch + kScratchParameterOffset, 0x88 },
        { kScratch + kScratchMaskOffset, 4 },
        { kScratch + kScratchCallbackOffset, kRetiredCallback },
        { kScratch + kScratchCallbackOffset, kCallback },
        { kScratch + kScratchParameterOffset, 0x99 },
        { kScratch + kScratchMaskOffset, 4 },
        { kScratch + kScratchCallbackOffset, kRetiredCallback },
        { kWriteback, 0x10 },
    };
    UNSCOPED_INFO("the two callbacks release in record order before RPTR");
    REQUIRE(pair.sink.Writes() == expected);
}

void testFailures()
{
    // A session failure reported through Fail, as the fence hook reports CPU fence mode.
    Harness injected(NativeDrawMode::Skip);
    injected.ledger.Fence(kBlock, 3, 0x40);
    injected.ledger.Kick(0x10);
    injected.ledger.Fence(kBlock, 5, 0x80);
    injected.ledger.Fail("cpu_fence_mode: G+0x2ABD bit 0x2", ProgressKind::Fence, 0x8269BCA8);
    injected.ledger.Kick(0x20);
    UNSCOPED_INFO("a failure even in skip mode lets only the span closed before it release");
    REQUIRE((injected.failures.size() == 1 && injected.ledger.Pump() &&
             injected.sink.Writes() == std::vector<Write>{ { kBlock + kCursorOffset, 0x40 }, { kBlock, 3 }, { kWriteback, 0x10 } }));
    UNSCOPED_INFO("the failing span and later work never release");
    REQUIRE((!injected.ledger.Pump() && injected.sink.Writes().size() == 3));

    Harness identity(NativeDrawMode::Skip);
    identity.ledger.Fence(kBlock + 0x1000, 3, 0x40);
    UNSCOPED_INFO("a fence block whose B+0x3C is not the writeback fails the session");
    REQUIRE((identity.failures.size() == 1 && identity.failures[0].detail.starts_with("identity_mismatch")));

    Harness late(NativeDrawMode::Skip);
    late.ledger.BeginRingEpoch();
    late.ledger.Fence(kBlock, 3, 0x40);
    late.ledger.RegisterWriteback(kWriteback + 0x40);
    UNSCOPED_INFO("a writeback registered after the fence is checked against it");
    REQUIRE((late.failures.size() == 1 && late.failures[0].detail.starts_with("identity_mismatch")));

    Harness threads(NativeDrawMode::Skip);
    threads.ledger.Fence(kBlock, 3, 0x40);
    std::thread other([&]
                      {
                          threads.ledger.Fence(kBlock, 5, 0x80);
                      });
    other.join();
    UNSCOPED_INFO("a second thread recording into one span fails the session");
    REQUIRE((threads.failures.size() == 1 && threads.failures[0].detail == "second_submit_thread"));
}

void testRingEpochReset()
{
    Harness h;
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.Kick(0x10);
    UNSCOPED_INFO("first epoch span releases");
    REQUIRE(h.ledger.Pump());
    h.ledger.GpuWait(kFlip, 1, 0x826A419C);
    h.ledger.Fence(kBlock, 5, 0x80);
    h.ledger.Kick(0x20);
    UNSCOPED_INFO("a span waits on its guest word");
    REQUIRE(!h.ledger.Pump());
    const auto before = h.sink.Writes().size();
    UNSCOPED_INFO("the first epoch's fence block is known");
    REQUIRE(h.ledger.Snapshot().fenceBlock == kBlock);
    // 0x8269C8E0 drained and is about to free B and S2.
    h.ledger.CloseEpochWrites();
    UNSCOPED_INFO("closing the epoch forgets the old fence block");
    REQUIRE(h.ledger.Snapshot().fenceBlock == 0);
    h.sink.Set(kFlip, 1);
    UNSCOPED_INFO("no write for an earlier span starts after the epoch closes");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().size() == before));
    // A kick before the new writeback registration must not reach the freed block.
    h.ledger.Kick(0x30);
    UNSCOPED_INFO("a span between epoch close and re-registration writes no RPTR");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().size() == before && h.ledger.AwaitingWritebackRegistration() &&
             h.ledger.Snapshot().oldestRecord == "awaiting writeback registration"));
    h.ledger.RequestFence(9, kBlock);
    h.ledger.BeginRingEpoch();
    UNSCOPED_INFO("a new ring epoch shows no fence block until it reveals one");
    REQUIRE(h.ledger.Snapshot().fenceBlock == 0);
    constexpr std::uint32_t kNewBlock = 0x1FCA4000;
    h.ledger.RegisterWriteback(kNewBlock + kReadPointerOffset);
    h.ledger.Fence(kNewBlock, 7, 0x10);
    h.ledger.Kick(0x4);
    UNSCOPED_INFO("the new epoch releases against its own block");
    REQUIRE(h.ledger.Pump());
    const auto writes = h.sink.Writes();
    UNSCOPED_INFO("RPTR goes to the new writeback");
    REQUIRE((writes.size() == before + 3 && writes.back() == Write{ kNewBlock + kReadPointerOffset, 0x4 }));
    const auto snapshot = h.ledger.Snapshot();
    UNSCOPED_INFO("the new epoch's fence record reveals its block");
    REQUIRE(snapshot.fenceBlock == kNewBlock);
    UNSCOPED_INFO("dropped spans are counted, not failed");
    REQUIRE((snapshot.ringEpoch == 2 && snapshot.spansDropped == 2 && h.failures.empty()));
    UNSCOPED_INFO("dropping spans without resolves discards no image");
    REQUIRE(h.draws.discarded.empty());
}

// Dropping a span discards its queued resolve images.
void testDropDiscardsImages()
{
    Harness h;
    h.ledger.Resolve(kFrontbufferResolveCall, 0x1E3B0000);
    h.ledger.FinishResolve(2, false, false);
    h.ledger.GpuWait(kFlip, 1, 0x826A419C);
    h.ledger.Kick(0x10);
    h.ledger.Resolve(kFrontbufferResolveCall, 0x1E400000);
    h.ledger.FinishResolve(5, false, false);
    h.ledger.Kick(0x20);
    // A resolve still awaiting its copy words has no op mark yet.
    h.ledger.Resolve(kFrontbufferResolveCall, 0x1E500000);
    h.ledger.CloseEpochWrites();
    UNSCOPED_INFO("closing the epoch discards through the newest dropped resolve");
    REQUIRE(h.draws.discarded == std::vector<std::uint64_t>{ 5 });
    h.ledger.BeginRingEpoch();
    UNSCOPED_INFO("an epoch with no dropped resolve discards nothing");
    REQUIRE(h.draws.discarded.size() == 1);
    h.ledger.RegisterWriteback(kWriteback);
    h.ledger.Resolve(kFrontbufferResolveCall, 0x1E3B0000);
    h.ledger.FinishResolve(8, false, false);
    h.ledger.Kick(0x30);
    h.ledger.BeginRingEpoch();
    UNSCOPED_INFO("a new ring epoch discards the spans it drops, without taking or failing");
    REQUIRE((h.draws.discarded == std::vector<std::uint64_t>{ 5, 8 } && h.draws.taken.empty() && h.failures.empty()));
}

void testMidEpochFlush()
{
    Harness h;
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    h.ledger.FinishDraw(1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    h.ledger.FinishDraw(2, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    h.ledger.Fence(kBlock, 11, 0x50);
    h.ledger.Kick(0x40);
    // The guest now waits on fence 11 before its VdSwap.
    h.ledger.Pump();
    UNSCOPED_INFO("the ledger asks for completion through the span's draws without a swap");
    REQUIRE(h.draws.flushed == std::vector<std::uint64_t>{ 2 });
    h.draws.completed = 1;
    UNSCOPED_INFO("the mid-epoch fence releases before any swap");
    REQUIRE((h.ledger.Pump() && h.sink.Writes()[1] == Write{ kBlock, 11 }));

    Harness later;
    later.draws.retries = 1;
    later.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    later.ledger.FinishDraw(1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    later.ledger.Kick(0x8);
    UNSCOPED_INFO("a flush that is not possible yet is retried, not failed");
    REQUIRE((!later.ledger.Pump() && later.draws.flushed.empty() && later.ledger.Snapshot().unowned.empty() &&
             later.ledger.Snapshot().oldestRecord == "awaiting draw flush: fixture_not_recorded" && later.failures.empty()));
    later.draws.completed = 1;
    UNSCOPED_INFO("the next poll flushes and releases");
    REQUIRE((later.ledger.Pump() && later.draws.flushed == std::vector<std::uint64_t>{ 1 }));

    Harness refused;
    refused.draws.refuse = true;
    refused.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    refused.ledger.FinishDraw(1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    refused.ledger.Kick(0x8);
    UNSCOPED_INFO("a refused flush leaves the span unowned");
    REQUIRE((!refused.ledger.Pump() && refused.sink.Writes().empty() &&
             refused.ledger.Snapshot().unowned == "draw_flush_failed: fixture_flush_refused" && refused.failures.size() == 1));

    Harness none(NativeDrawMode::Strict, false);
    none.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    none.ledger.FinishDraw(1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    none.ledger.Kick(0x8);
    UNSCOPED_INFO("admitted draws need a host completion");
    REQUIRE((!none.ledger.Pump() && none.ledger.Snapshot().unowned == "no_draw_completion"));
}

// Strict failure still releases earlier fully owned spans after guest waits.
void testFailureBoundary()
{
    Harness h;
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.Kick(0x10);
    h.ledger.GpuWait(kFlip, 1, 0x826A419C);
    h.ledger.Fence(kBlock, 5, 0x80);
    h.ledger.Kick(0x20);
    h.ledger.Fence(kBlock, 7, 0xC0);
    h.ledger.Unmodeled(0x826A9CA8);
    h.ledger.Kick(0x30);
    UNSCOPED_INFO("the unmodeled record fails the strict session");
    REQUIRE(h.failures.size() == 1);
    UNSCOPED_INFO("the first span closed before the failure releases");
    REQUIRE((h.ledger.Pump() && h.sink.Writes() == std::vector<Write>{ { kBlock + kCursorOffset, 0x40 }, { kBlock, 3 }, { kWriteback, 0x10 } }));
    h.sink.Set(kFlip, 1);
    UNSCOPED_INFO("the second, fully owned, releases once its wait is met");
    REQUIRE((h.ledger.Pump() && h.sink.Writes().back() == Write{ kWriteback, 0x20 }));
    const auto released = h.sink.Writes().size();
    UNSCOPED_INFO("the failing span never releases");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().size() == released));
    UNSCOPED_INFO("the debt view names the failing record");
    REQUIRE(h.ledger.Snapshot().unowned == "unmodeled_emitter 0x826a9ca8");
}

// COHER_STATUS_HOST orders prior host work/effects and writes nothing.
void testCoherBarrier()
{
    Harness h;
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    h.ledger.FinishDraw(4, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.Barrier(0x8269BC54);
    h.ledger.Fence(kBlock, 5, 0x80);
    h.ledger.Kick(0x10);
    UNSCOPED_INFO("nothing after the barrier, or before it, applies while the draw is pending on the host");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().empty()));
    h.draws.completed = 1;
    UNSCOPED_INFO("the span releases once its draws host-complete");
    REQUIRE(h.ledger.Pump());
    UNSCOPED_INFO("the barrier writes nothing and keeps record order");
    REQUIRE(h.sink.Writes() == std::vector<Write>{ { kBlock + kCursorOffset, 0x40 }, { kBlock, 3 }, { kBlock + kCursorOffset, 0x80 }, { kBlock, 5 }, { kWriteback, 0x10 } });
    UNSCOPED_INFO("a barrier is modeled, not a failure");
    REQUIRE(h.failures.empty());

    Harness noDraws;
    noDraws.ledger.Barrier(0x8269BB94);
    noDraws.ledger.Kick(0x8);
    UNSCOPED_INFO("a flush with only a barrier releases at its kick in strict mode");
    REQUIRE((noDraws.ledger.Pump() && noDraws.sink.Writes() == std::vector<Write>{ { kWriteback, 0x8 } } && noDraws.failures.empty()));

    Harness waiter;
    UNSCOPED_INFO("no fence block is known before a fence or fence wait");
    REQUIRE(waiter.ledger.Snapshot().fenceBlock == 0);
    waiter.ledger.RequestFence(5, kBlock);
    const auto snapshot = waiter.ledger.Snapshot();
    UNSCOPED_INFO("a fence wait names the block the guest polls before any fence record");
    REQUIRE((snapshot.fenceRequested && snapshot.requestedFence == 5 && snapshot.fenceBlock == kBlock));
}

// After VdSwap, 0x826A4458 waits for draw fence, then releases fence/RPTR.
void testDisplayWait()
{
    Harness h;
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    h.ledger.FinishDraw(4, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    h.ledger.Swap(0x1F6F8000);
    h.ledger.Barrier(0x826A44E0);
    h.ledger.Barrier(0x826A45C8);
    h.ledger.Fence(kBlock, 7, 0x40);
    h.ledger.Kick(0x10);
    UNSCOPED_INFO("the display waits release nothing before the draw host-completes");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().empty()));
    h.draws.completed = 1;
    UNSCOPED_INFO("the display waits write nothing and the later fence and RPTR release");
    REQUIRE((h.ledger.Pump() && h.sink.Writes() == std::vector<Write>{ { kBlock + kCursorOffset, 0x40 }, { kBlock, 7 }, { kWriteback, 0x10 } }));
    const auto snapshot = h.ledger.Snapshot();
    UNSCOPED_INFO("display waits do not fail the strict session");
    REQUIRE((h.failures.empty() && !h.ledger.Failure() && snapshot.unmodeledEmitter == 0 && !snapshot.pending));
}

void testDrawVerdictPending()
{
    Harness h;
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    // The draw's own flush kicked before its Original returned.
    h.ledger.Kick(0x10);
    UNSCOPED_INFO("a span holding a draw without its verdict waits");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().empty()));
    UNSCOPED_INFO("the debt view names the draw");
    REQUIRE(h.ledger.Snapshot().oldestRecord == "draw indexed_up from 0x82303e40 awaiting its verdict");
    h.ledger.FinishDraw(1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    h.draws.completed = 1;
    UNSCOPED_INFO("the verdict lands in the span where the draw began");
    REQUIRE((h.ledger.Pump() && h.draws.flushed == std::vector<std::uint64_t>{ 1 }));
}

// Detached draws retain spans until decided; abandoned draws fail.
void testDetachedDraw()
{
    Harness h;
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    const auto draw = h.ledger.DetachDraw();
    UNSCOPED_INFO("the open draw detaches once");
    REQUIRE((draw && !h.ledger.DetachDraw()));
    h.ledger.Kick(0x10);
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    h.ledger.FinishDraw(2, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    UNSCOPED_INFO("a span holding a detached draw waits, and a later draw does not abandon it");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().empty() && h.failures.empty()));
    std::thread decider([&]
                        {
                            h.ledger.FinishDraw(*draw, 1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
                        });
    decider.join();
    h.draws.completed = 2;
    UNSCOPED_INFO("the detached verdict lands in the span where the draw began");
    REQUIRE((h.ledger.Pump() && h.draws.flushed.size() == 1 && h.draws.flushed[0] == 1 && h.failures.empty()));
    h.ledger.FinishDraw(*draw, 1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    UNSCOPED_INFO("a settled detached draw is not settled again");
    REQUIRE(h.failures.empty());

    Harness abandoned;
    abandoned.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    const auto lost = abandoned.ledger.DetachDraw();
    UNSCOPED_INFO("the draw detaches");
    REQUIRE(lost.has_value());
    abandoned.ledger.AbandonDraw(*lost);
    UNSCOPED_INFO("an abandoned detached draw fails its span: " + (abandoned.failures.empty() ? std::string() : abandoned.failures[0].detail));
    REQUIRE((abandoned.failures.size() == 1 && abandoned.failures[0].detail.find("draw_abandoned") != std::string::npos));

    // A later span's strict failure leaves the earlier span releasable once
    // its detached draw reports.
    Harness failed;
    failed.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    const auto early = failed.ledger.DetachDraw();
    failed.ledger.Kick(0x10);
    failed.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    failed.ledger.FinishDraw(2, NativeDrawHook::IndexedUp, kCaller, NativeDrawReason::NoNativeRoute);
    UNSCOPED_INFO("the later draw fails the session");
    REQUIRE((early && failed.failures.size() == 1));
    failed.ledger.FinishDraw(*early, 1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    failed.draws.completed = 1;
    UNSCOPED_INFO("a detached draw before the failing span still settles and its span releases");
    REQUIRE((failed.ledger.Pump() && failed.draws.flushed.size() == 1 && failed.draws.flushed[0] == 1 && failed.failures.size() == 1));
}

void testCloseDuringInterrupt()
{
    Harness                 h;
    std::mutex              gate;
    std::condition_variable changed;
    bool                    entered = false, closed = false;
    h.sink.raising = [&]
    {
        std::unique_lock lock(gate);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&]
                     {
                         return closed;
                     });
    };
    h.sink.handler = clearCpuBit;
    h.ledger.Callback(kScratch, kCallback, 0x77, 0, std::nullopt);
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.Kick(0x8);
    std::thread pump([&]
                     {
                         h.ledger.Pump();
                     });
    {
        std::unique_lock lock(gate);
        changed.wait(lock, [&]
                     {
                         return entered;
                     });
    }
    const auto before = h.sink.Writes().size();
    h.ledger.Close();
    {
        std::lock_guard lock(gate);
        closed = true;
    }
    changed.notify_all();
    pump.join();
    UNSCOPED_INFO("no write starts after Close, even for a span mid-callback");
    REQUIRE((before == 3 && h.sink.Writes().size() == before));
    UNSCOPED_INFO("a closed ledger never releases");
    REQUIRE(!h.ledger.Pump());
}

// Undelivered source-1 interrupts keep spans pending; delivery restores order.
void testUndeliveredInterrupt()
{
    for (const auto result : { InterruptResult::NoHandler, InterruptResult::Stopped })
        for (const std::uint32_t flags : { 0U, 2U })
        {
            Harness h;
            h.sink.handler      = clearCpuBit;
            h.sink.forcedResult = result;
            h.ledger.Fence(kBlock, 3, 0x40);
            h.ledger.Kick(0x10);
            h.ledger.Callback(kScratch, kCallback, 0x1234, flags, std::nullopt);
            h.ledger.Fence(kBlock, 5, 0x80);
            h.ledger.Kick(0x20);
            h.ledger.Fence(kBlock, 7, 0xC0);
            h.ledger.Kick(0x30);
            UNSCOPED_INFO("the earlier fully owned span releases while the callback span is pending");
            REQUIRE(h.ledger.Pump());
            for (int i = 0; i < 3; ++i)
            {
                UNSCOPED_INFO("an undelivered interrupt releases nothing");
                REQUIRE(!h.ledger.Pump());
            }
            const std::vector<Write> held{
                { kBlock + kCursorOffset, 0x40 },
                { kBlock, 3 },
                { kWriteback, 0x10 },
                { kScratch + kScratchCallbackOffset, kCallback },
                { kScratch + kScratchParameterOffset, 0x1234 },
                { kScratch + kScratchMaskOffset, kCallbackMask },
            };
            UNSCOPED_INFO("only the earlier span and the callback slot stores are written: no retirement, fence or RPTR");
            REQUIRE(h.sink.Writes() == held);
            UNSCOPED_INFO("each pump retries the interrupt once");
            REQUIRE(h.sink.interrupts.size() == 4);
            const auto snapshot = h.ledger.Snapshot();
            UNSCOPED_INFO("an undelivered interrupt is pending debt, not a release and not a failure");
            REQUIRE((snapshot.spansReleased == 1 && snapshot.callbacksReleased == 0 && snapshot.pending && h.failures.empty()));

            h.sink.forcedResult.reset();
            UNSCOPED_INFO("the delivered interrupt releases the held and later spans");
            REQUIRE(h.ledger.Pump());
            auto expected = held;
            if (flags == 0)
                expected.push_back({ kScratch + kScratchCallbackOffset, kRetiredCallback });
            expected.insert(expected.end(),
                            {
                                { kBlock + kCursorOffset, 0x80 },
                                { kBlock, 5 },
                                { kWriteback, 0x20 },
                                { kBlock + kCursorOffset, 0xC0 },
                                { kBlock, 7 },
                                { kWriteback, 0x30 },
                            });
            UNSCOPED_INFO("slot stores stay single; the retirement, fence and RPTR follow the delivery in order");
            REQUIRE((h.sink.Writes() == expected && h.sink.interrupts.size() == 5 && h.ledger.Snapshot().callbacksReleased == 1 && h.failures.empty()));
        }
}

// Closing the ledger prevents every owed guest write, even after completion.
void testCloseWithWorkInFlight()
{
    constexpr std::uint32_t destination = 0x1E3B0000;
    Harness                 h;
    h.sink.handler = clearCpuBit;
    h.draws.image  = rerevved::gpu::guest_d3d::GuestResolveImage{ destination, { 255 } };
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.Kick(0x10);
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    h.ledger.FinishDraw(7, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    h.ledger.Resolve(kFrontbufferResolveCall, destination);
    h.ledger.FinishResolve(8, false, false);
    h.ledger.Callback(kScratch, kCallback, 0x1234, 0, std::nullopt);
    h.ledger.Fence(kBlock, 5, 0x80);
    h.ledger.Kick(0x20);
    UNSCOPED_INFO("the earlier span releases");
    REQUIRE(h.ledger.Pump());
    for (int i = 0; i < 3; ++i)
    {
        UNSCOPED_INFO("the span waits for its host work");
        REQUIRE(!h.ledger.Pump());
    }
    const std::vector<Write> earlier{ { kBlock + kCursorOffset, 0x40 }, { kBlock, 3 }, { kWriteback, 0x10 } };
    UNSCOPED_INFO("the open host work holds every effect of its span");
    REQUIRE((h.sink.Writes() == earlier && h.draws.flushed == std::vector<std::uint64_t>{ 8 }));

    h.ledger.Close();
    h.draws.completed = 100;
    UNSCOPED_INFO("a closed ledger releases nothing once the host completes");
    REQUIRE(!h.ledger.Pump());
    UNSCOPED_INFO("no callback, image, fence or RPTR is written after the close, and the close is no failure");
    REQUIRE((h.sink.Writes() == earlier && h.sink.interrupts.empty() && h.sink.imageBytes.empty() && h.draws.taken.empty() && h.draws.written.empty() &&
             h.failures.empty()));
}

// Missing worker submits drop callbacks, fence and later RPTR writes.
void testMissingWorkerSubmit()
{
    Harness h;
    Worker  worker;
    h.sink.handler = clearCpuBit;
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.Kick(0x10);
    h.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000006, kFirstPassPredicate);
    h.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000007, std::nullopt, kSideBuffer);
    h.ledger.Fence(kBlock, 9, 0x80, kEndFence);
    h.ledger.Defer(kCoher, 0x10);
    h.ledger.Defer(kSideBuffer, 0x20);
    h.ledger.Defer(kEndFence, 0x8);
    h.ledger.Fence(kBlock, 11, 0xC0);
    h.ledger.Kick(0x30);
    UNSCOPED_INFO("the span before the bracket releases");
    REQUIRE(h.ledger.Pump());
    for (int i = 0; i < 3; ++i)
    {
        UNSCOPED_INFO("the bracket holds without its worker submits");
        REQUIRE(!h.ledger.Pump());
    }
    const std::vector<Write> earlier{ { kBlock + kCursorOffset, 0x40 }, { kBlock, 3 }, { kWriteback, 0x10 } };
    const auto               snapshot = h.ledger.Snapshot();
    UNSCOPED_INFO("no callback, fence or RPTR is fabricated, and the later kick's fence and RPTR wait behind the bracket");
    REQUIRE((h.sink.Writes() == earlier && h.sink.interrupts.empty() && snapshot.callbacksReleased == 0 && snapshot.spansReleased == 1 &&
             snapshot.deferredSpans == 3 && h.failures.empty()));

    h.ledger.Close();
    UNSCOPED_INFO("closing drops the bracket without writing");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes() == earlier));
    worker.Submit(h.ledger, kCoher, 0x10, 0x10);
    worker.Submit(h.ledger, kSideBuffer, 0x20, 0x18);
    worker.Submit(h.ledger, kEndFence, 0x8, 0x20);
    UNSCOPED_INFO("a worker submit after the close writes nothing");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes() == earlier && h.sink.interrupts.empty()));
}

// 0x826A9BA0: B+0x40..0x5F counters rise; other addresses fail.
void testGpuTimestamp()
{
    Harness h;
    h.ledger.GpuTimestamp(kBlock + kTimestampOffset + 4, kBlock);
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.GpuTimestamp(kBlock + kTimestampOffset + 8, kBlock);
    UNSCOPED_INFO("a timestamp waits for its kick");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().empty()));
    h.ledger.Kick(0x10);
    UNSCOPED_INFO("a timestamp span releases at its kick");
    REQUIRE((h.ledger.Pump() && h.failures.empty()));
    UNSCOPED_INFO("timestamps write increasing nonzero values in record order before RPTR");
    REQUIRE(h.sink.Writes() == std::vector<Write>{ { kBlock + 0x44, 1 }, { kBlock + kCursorOffset, 0x40 }, { kBlock, 3 }, { kBlock + 0x48, 2 }, { kWriteback, 0x10 } });

    for (const std::uint32_t address : { kBlock + 0x3C, kBlock + kTimestampEnd, kBlock + 0x42 })
    {
        Harness outside(NativeDrawMode::Skip);
        outside.ledger.GpuTimestamp(address, kBlock);
        outside.ledger.Kick(0x10);
        const auto expected = std::format("timestamp_address_outside_block {:#010x}", address);
        UNSCOPED_INFO("a timestamp outside B+0x40..0x5F fails the session even in skip mode: " + expected);
        REQUIRE((outside.failures.size() == 1 && outside.failures[0].detail == expected && outside.failures[0].address == 0x826A9BDC &&
                 !outside.ledger.Pump() && outside.sink.Writes().empty()));
    }
}

// G+0x2B00 defers a kick until worker submit; later spans queue behind it.
void testDeferredKick()
{
    Harness h;
    Worker  worker;
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.Kick(0x10);
    h.ledger.Fence(kBlock, 5, 0x80);
    h.ledger.Defer(kNextFrame, 0x20);
    h.ledger.Fence(kBlock, 7, 0xC0);
    h.ledger.Defer(kNextFrame + 0x1000, 0x30);
    UNSCOPED_INFO("the directly kicked span releases, the deferred one holds");
    REQUIRE((h.ledger.Pump() && !h.ledger.Pump()));
    UNSCOPED_INFO("no deferred fence is written before its submit");
    REQUIRE(h.sink.Writes() == std::vector<Write>{ { kBlock + kCursorOffset, 0x40 }, { kBlock, 3 }, { kWriteback, 0x10 } });
    const auto held = h.ledger.Snapshot();
    UNSCOPED_INFO("the debt view names the deferred segment");
    REQUIRE((held.oldestRecord == "awaiting worker submit 0x00140000" && !held.oldestClosed && held.deferredSpans == 2));

    worker.Submit(h.ledger, kNextFrame, 0x20, 0x40);
    UNSCOPED_INFO("the worker submit releases the deferred fence with the worker's WPTR");
    REQUIRE((h.ledger.Pump() && h.sink.Writes().size() == 6 && h.sink.Writes()[4] == Write{ kBlock, 5 } &&
             h.sink.Writes()[5] == Write{ kWriteback, 0x40 }));
    worker.Submit(h.ledger, kNextFrame + 0x1000, 0x30, 0x50);
    UNSCOPED_INFO("the next deferred span follows in order");
    REQUIRE((h.ledger.Pump() && h.sink.Writes().back() == Write{ kWriteback, 0x50 } && h.failures.empty()));
}

// Worker kicks keep main spans open; tiled replays let RPTR pass host-only IBs.
void testReplayKicks()
{
    Harness h;
    Worker  worker;
    h.sink.handler = clearCpuBit;
    h.ledger.BeginTiledList();
    h.ledger.TiledSegment(kTile0, 0x10);
    startList(h.ledger, kSideBuffer, 0x10);
    while (h.ledger.Pump())
    {
    }
    const auto started = h.sink.Writes();
    h.ledger.Fence(kBlock, 3, 0x40);
    worker.Submit(h.ledger, kTile0, 0x10, 0x20);
    UNSCOPED_INFO("a replay kick writes only RPTR and leaves the open span's fence unwritten");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().size() == started.size() + 1 && h.sink.Writes().back() == Write{ kWriteback, 0x20 }));
    const auto snapshot = h.ledger.Snapshot();
    UNSCOPED_INFO("the main span stays open");
    REQUIRE((snapshot.replaySubmits == 1 && snapshot.oldestRecord == "awaiting CP_RB_WPTR kick"));
    h.ledger.Kick(0x30);
    auto expected = started;
    expected.insert(expected.end(), { { kWriteback, 0x20 }, { kBlock + kCursorOffset, 0x40 }, { kBlock, 3 }, { kWriteback, 0x30 } });
    UNSCOPED_INFO("the main kick releases the fence");
    REQUIRE((h.ledger.Pump() && h.sink.Writes() == expected));

    // The active list stays replayable however many lists BeginTiling starts after it.
    h.ledger.BeginTiledList();
    h.ledger.BeginTiledList();
    worker.Submit(h.ledger, kTile0, 0x10, 0x34);
    UNSCOPED_INFO("a segment of the active list is a replay behind two newer lists");
    REQUIRE((h.failures.empty() && h.ledger.Snapshot().replaySubmits == 2 && h.ledger.Snapshot().maxTiledLists == 3));

    // RPTR never passes a replay kicked after a span that is still held.
    Harness held;
    Worker  heldWorker;
    held.sink.handler = clearCpuBit;
    held.ledger.BeginTiledList();
    held.ledger.TiledSegment(kTile0, 0x10);
    held.ledger.GpuWait(kFlip, 1, 0x826A419C);
    startList(held.ledger, kSideBuffer, 0x10);
    held.ledger.Defer(kEndFence, 0x8);
    heldWorker.Submit(held.ledger, kTile0, 0x10, 0x20);
    UNSCOPED_INFO("a replay after a held span writes no RPTR");
    REQUIRE((!held.ledger.Pump() && held.sink.Writes().empty()));
    held.sink.Set(kFlip, 1);
    while (held.ledger.Pump())
    {
    }
    UNSCOPED_INFO("RPTR reaches the replay after the earlier span released");
    REQUIRE(readPointers(held.sink) == std::vector<std::uint32_t>{ 0x10, 0x20 });
    heldWorker.Submit(held.ledger, kEndFence, 0x8, 0x28);
    UNSCOPED_INFO("the deferred span follows");
    REQUIRE((held.ledger.Pump() && readPointers(held.sink).back() == 0x28 && held.failures.empty()));
}

// Each worker-model failure fails the session in either draw mode.
void testReplayFailures()
{
    const auto failed = [](Harness& h, const std::string& detail)
    {
        return h.failures.size() == 1 && h.failures[0].detail == detail;
    };

    Harness unknown(NativeDrawMode::Skip);
    Worker  worker;
    worker.Submit(unknown.ledger, kTile1, 0x10, 0x20);
    UNSCOPED_INFO("an unknown worker submit fails");
    REQUIRE(failed(unknown, "replay_submit_unmatched 0x00120000 dwords 16, none deferred"));

    Harness order(NativeDrawMode::Skip);
    order.ledger.Defer(kEndFence, 0x8);
    order.ledger.Defer(kNextFrame, 0x20);
    worker.Submit(order.ledger, kNextFrame, 0x20, 0x20);
    UNSCOPED_INFO("a deferred segment submitted out of order fails and holds");
    REQUIRE((failed(order, "replay_submit_unmatched 0x00140000 dwords 32, oldest deferred 0x00130000 dwords 8") && !order.ledger.Pump()));

    Harness size(NativeDrawMode::Skip);
    size.ledger.Defer(kEndFence, 0x8);
    worker.Submit(size.ledger, kEndFence, 0x9, 0x20);
    UNSCOPED_INFO("a deferred size mismatch fails");
    REQUIRE(failed(size, "replay_submit_unmatched 0x00130000 dwords 9, oldest deferred 0x00130000 dwords 8"));

    Harness unattributed(NativeDrawMode::Skip);
    worker.Run([&]
               {
                   unattributed.ledger.MarkReplayThread();
                   unattributed.ledger.Kick(0x20);
               });
    UNSCOPED_INFO("a worker kick without a submit fails");
    REQUIRE(failed(unattributed, "worker_kick_unattributed 0x00000020"));

    Harness unkicked(NativeDrawMode::Skip);
    worker.Run([&]
               {
                   unkicked.ledger.ReplaySubmit(kTile0, 0x10, 0x826AB890);
                   unkicked.ledger.ReplaySubmit(kTile1, 0x10, 0x826AB890);
               });
    UNSCOPED_INFO("a second submit before a kick fails");
    REQUIRE(failed(unkicked, "replay_submit_unkicked 0x00110000"));

    for (const auto kind : { ProgressKind::Fence, ProgressKind::ScratchWrite, ProgressKind::GpuWait, ProgressKind::Unmodeled, ProgressKind::Callback })
    {
        Harness tiled(NativeDrawMode::Skip);
        tiled.ledger.TiledProgress(kind, 0x8269BCA8);
        UNSCOPED_INFO("progress while tiling fails");
        REQUIRE(failed(tiled, std::format("tiled_progress_{} at 0x8269bca8", ProgressKindName(kind))));
    }

    Harness fence(NativeDrawMode::Skip);
    worker.Run([&]
               {
                   fence.ledger.MarkReplayThread();
                   fence.ledger.Fence(kBlock, 3, 0x40);
               });
    UNSCOPED_INFO("a fence on the worker thread fails");
    REQUIRE(failed(fence, "progress_on_replay_thread fence at 0x8269bca8"));
    for (const auto& [name, record] : std::vector<std::pair<std::string, std::function<void(CompletionLedger&)>>>{
             { "deferred_kick at 0x8269bde8", [](CompletionLedger& ledger)
               {
                   ledger.Defer(kEndFence, 0x8);
               } },
             { "tiled_segment at 0x8269c4e8", [](CompletionLedger& ledger)
               {
                   ledger.TiledSegment(kTile0, 0x10);
               } },
             { "unmodeled at 0x826ab268", [](CompletionLedger& ledger)
               {
                   ledger.TiledProgress(ProgressKind::Unmodeled, 0x826AB268);
               } },
         })
    {
        Harness on(NativeDrawMode::Skip);
        worker.Run([&]
                   {
                       on.ledger.MarkReplayThread();
                       record(on.ledger);
                   });
        UNSCOPED_INFO("a worker-thread record fails: " + name);
        REQUIRE(failed(on, "progress_on_replay_thread " + name));
    }

    Harness notSubmitted(NativeDrawMode::Skip);
    notSubmitted.ledger.Fence(kBlock, 9, 0x40, kEndFence);
    notSubmitted.ledger.DirectSubmit(kSideBuffer, 0x20);
    notSubmitted.ledger.Kick(0x10);
    notSubmitted.ledger.Fence(kBlock, 10, 0x80);
    UNSCOPED_INFO("a record before the EndTiling fence buffer is submitted fails");
    REQUIRE(failed(notSubmitted, "end_tiling_buffer_not_submitted 0x00130000"));

    Harness unattributedMain(NativeDrawMode::Skip);
    unattributedMain.ledger.Fence(kBlock, 9, 0x40, kEndFence);
    unattributedMain.ledger.Kick(0x10);
    UNSCOPED_INFO("a main kick without its submit fails while a side buffer is held");
    REQUIRE(failed(unattributedMain, "main_kick_unattributed 0x00000010"));

    Harness firstPass(NativeDrawMode::Skip);
    firstPass.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000006, kFirstPassPredicate);
    firstPass.ledger.Swap(0x1F000000);
    UNSCOPED_INFO("a first-pass callback still held at VdSwap fails");
    REQUIRE(failed(firstPass, "first_pass_callback_not_submitted"));

    Harness directUnkicked(NativeDrawMode::Skip);
    directUnkicked.ledger.DirectSubmit(kCoher, 0x10);
    directUnkicked.ledger.DirectSubmit(kSideBuffer, 0x20);
    UNSCOPED_INFO("a second direct submit before a kick fails");
    REQUIRE(failed(directUnkicked, "direct_submit_unkicked 0x00160000"));

    // A Silent direct submit has no kick: its host-only seal takes the pending record, so a
    // later direct submit is no unkicked one, and seals the work before it.
    Harness silent(NativeDrawMode::Skip);
    silent.ledger.Fence(kBlock, 9, 0x40, kEndFence);
    silent.ledger.DirectSubmit(kEndFence, 0x20);
    silent.ledger.SilentSubmit();
    silent.ledger.DirectSubmit(kCoher, 0x10);
    silent.ledger.Kick(0x10);
    silent.ledger.Fence(kBlock, 10, 0x80);
    UNSCOPED_INFO("a sealed Silent submit leaves no unkicked record and no held buffer: " + failureDetail(silent.failures));
    REQUIRE(silent.failures.empty());
    silent.ledger.SilentSubmit();
    UNSCOPED_INFO("a seal with nothing pending is a no-op");
    REQUIRE(silent.failures.empty());

    // The draws before a Silent submit retire once host completion reaches them, with no
    // later Kick: the held end-fence record is carried by the sealed span, RPTR is not written.
    Harness sealed(NativeDrawMode::Skip);
    sealed.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    sealed.ledger.FinishDraw(7, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    sealed.ledger.Fence(kBlock, 9, 0x40, kEndFence);
    sealed.ledger.DirectSubmit(kEndFence, 0x20);
    sealed.ledger.SilentSubmit();
    for (int i = 0; i < 3; ++i)
        sealed.ledger.Pump();
    UNSCOPED_INFO("a sealed span flushes its draws and releases nothing while host completion is below its target");
    REQUIRE((sealed.sink.Writes().empty() && sealed.draws.flushed == std::vector<std::uint64_t>{ 7 }));
    UNSCOPED_INFO("the sealed span is the closed oldest span");
    REQUIRE((sealed.ledger.Snapshot().pending && sealed.ledger.Snapshot().oldestClosed));
    sealed.draws.completed = 1;
    UNSCOPED_INFO("the sealed span retires when host completion reaches it, with no Kick");
    REQUIRE(sealed.ledger.Pump());
    UNSCOPED_INFO("the sealed span writes its fence words and no RPTR");
    REQUIRE(sealed.sink.Writes() == std::vector<Write>{ { kBlock + kCursorOffset, 0x40 }, { kBlock + kFenceOffset, 9 } });
    UNSCOPED_INFO("the sealed span released and nothing else waits: " + failureDetail(sealed.failures));
    REQUIRE((!sealed.ledger.Snapshot().pending && sealed.ledger.Snapshot().spansReleased == 1 && sealed.failures.empty()));
    sealed.ledger.DirectSubmit(kCoher, 0x10);
    sealed.ledger.Kick(0x10);
    UNSCOPED_INFO("a direct submit and kick after the seal are ordinary");
    REQUIRE(sealed.failures.empty());

    // A buffer the worker replayed in the active list was reused for a deferred
    // segment; submitting it ahead of the older deferral is not a replay.
    Harness reused(NativeDrawMode::Skip);
    reused.ledger.BeginTiledList();
    reused.ledger.TiledSegment(kTile0, 0x10);
    startList(reused.ledger, kSideBuffer, 0x10);
    while (reused.ledger.Pump())
    {
    }
    reused.ledger.BeginTiledList();
    reused.ledger.Defer(kEndFence, 0x8);
    reused.ledger.Defer(kTile0, 0x10);
    const auto reusedWrites = reused.sink.Writes();
    worker.Submit(reused.ledger, kTile0, 0x10, 0x20);
    UNSCOPED_INFO("a deferred segment that is also a tiled segment of the active list is never taken for a replay");
    REQUIRE((failed(reused, "replay_submit_unmatched 0x00110000 dwords 16, oldest deferred 0x00130000 dwords 8") && reused.ledger.Snapshot().replaySubmits == 0 &&
             !reused.ledger.Pump() && reused.sink.Writes() == reusedWrites));

    // The oldest deferral reuses an active list's tiled address with another
    // size; a submit with the tiled size is neither that deferral nor a replay.
    Harness resized(NativeDrawMode::Skip);
    resized.ledger.BeginTiledList();
    resized.ledger.TiledSegment(kTile0, 0x10);
    startList(resized.ledger, kSideBuffer, 0x10);
    while (resized.ledger.Pump())
    {
    }
    resized.ledger.Defer(kTile0, 0x20);
    const auto resizedWrites = resized.sink.Writes();
    worker.Submit(resized.ledger, kTile0, 0x10, 0x20);
    UNSCOPED_INFO("a submit naming the oldest deferral's address with a tiled size is never a replay");
    REQUIRE((failed(resized, "replay_submit_unmatched 0x00110000 dwords 16, oldest deferred 0x00110000 dwords 32") &&
             resized.ledger.Snapshot().replaySubmits == 0 && !resized.ledger.Pump() && resized.sink.Writes() == resizedWrites));

    // The worker replays a list only after its callback buffer was submitted.
    Harness unstarted(NativeDrawMode::Skip);
    unstarted.ledger.BeginTiledList();
    unstarted.ledger.TiledSegment(kTile0, 0x10);
    worker.Submit(unstarted.ledger, kTile0, 0x10, 0x20);
    UNSCOPED_INFO("a list whose callback buffer was not submitted is not replayed");
    REQUIRE((failed(unstarted, "replay_submit_inactive_list 0x00110000 dwords 16, list 1 active none") && unstarted.ledger.Snapshot().replaySubmits == 0));

    // A newer list waits for its own callback buffer while an older one is active.
    Harness later(NativeDrawMode::Skip);
    later.ledger.BeginTiledList();
    later.ledger.TiledSegment(kTile0, 0x10);
    startList(later.ledger, kSideBuffer, 0x10);
    later.ledger.BeginTiledList();
    later.ledger.TiledSegment(kTile1, 0x20);
    worker.Submit(later.ledger, kTile1, 0x20, 0x20);
    UNSCOPED_INFO("a newer list is not replayed before it starts");
    REQUIRE(failed(later, "replay_submit_inactive_list 0x00120000 dwords 32, list 2 active 1"));

    // Starting a list retires every older one.
    Harness retired(NativeDrawMode::Skip);
    retired.ledger.BeginTiledList();
    retired.ledger.TiledSegment(kTile0, 0x10);
    startList(retired.ledger, kSideBuffer, 0x10);
    retired.ledger.BeginTiledList();
    retired.ledger.TiledSegment(kTile1, 0x20);
    startList(retired.ledger, kSideBuffer + 0x1000, 0x18);
    worker.Submit(retired.ledger, kTile1, 0x20, 0x20);
    worker.Submit(retired.ledger, kTile0, 0x10, 0x24);
    UNSCOPED_INFO("a segment of a list retired by a later start is unmatched");
    REQUIRE((failed(retired, "replay_submit_unmatched 0x00110000 dwords 16, none deferred") && retired.ledger.Snapshot().replaySubmits == 1 &&
             retired.ledger.Snapshot().tiledListsStarted == 2));

    Harness unlisted(NativeDrawMode::Skip);
    unlisted.ledger.TiledSegment(kTile0, 0x10);
    UNSCOPED_INFO("a tiled segment before BeginTiling fails");
    REQUIRE((failed(unlisted, "tiled_segment_without_list 0x00110000") && unlisted.failures[0].address == 0x8269C4E8));
}

// EndTiling kicks wait for worker submission; fences own buffers.
void testEndTilingSubmissions()
{
    // Pumped between every main kick: nothing raises before the callback buffer's kick.
    Harness direct;
    direct.sink.handler = clearCpuBit;
    direct.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000006, kFirstPassPredicate);
    direct.ledger.Barrier(0x8269BC54);
    direct.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000007, std::nullopt, kSideBuffer);
    direct.ledger.Fence(kBlock, 9, 0x40, kEndFence);
    direct.ledger.DirectSubmit(kCoher, 0x10);
    direct.ledger.Kick(0x10);
    UNSCOPED_INFO("the COHER kick raises neither worker-kick callback");
    REQUIRE((direct.ledger.Pump() && direct.sink.Writes() == std::vector<Write>{ { kWriteback, 0x10 } } && direct.sink.interrupts.empty()));
    direct.ledger.DirectSubmit(kSideBuffer, 0x20);
    direct.ledger.Kick(0x18);
    UNSCOPED_INFO("the callback buffer's kick releases both callbacks in record order");
    REQUIRE((direct.ledger.Pump() && direct.sink.interrupts.size() == 2 && direct.sink.Writes().back() == Write{ kWriteback, 0x18 } &&
             direct.ledger.Snapshot().callbacksReleased == 2));
    // The worker already finished, so 0x8269BD98 submits the fence buffer directly.
    const auto before = direct.sink.Writes().size();
    direct.ledger.DirectSubmit(kEndFence, 0x8);
    direct.ledger.Kick(0x20);
    UNSCOPED_INFO("a directly submitted fence buffer releases its fence");
    REQUIRE((direct.ledger.Pump() && direct.sink.Writes().size() == before + 3 && direct.sink.Writes()[before + 1] == Write{ kBlock, 9 } &&
             direct.sink.Writes().back() == Write{ kWriteback, 0x20 } && direct.failures.empty()));

    // A worker still running from the previous frame: all three buffers are deferred.
    Harness deferred;
    Worker  worker;
    deferred.sink.handler = clearCpuBit;
    deferred.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000006, kFirstPassPredicate);
    deferred.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000007, std::nullopt, kSideBuffer);
    deferred.ledger.Fence(kBlock, 9, 0x40, kEndFence);
    deferred.ledger.Defer(kCoher, 0x10);
    deferred.ledger.Defer(kSideBuffer, 0x20);
    deferred.ledger.Defer(kEndFence, 0x8);
    UNSCOPED_INFO("deferred side buffers hold their records");
    REQUIRE((!deferred.ledger.Pump() && deferred.sink.Writes().empty()));
    worker.Submit(deferred.ledger, kCoher, 0x10, 0x10);
    worker.Submit(deferred.ledger, kSideBuffer, 0x20, 0x18);
    while (deferred.ledger.Pump())
    {
    }
    UNSCOPED_INFO("the callbacks release with the deferred callback buffer; the fence waits for its own");
    REQUIRE((deferred.sink.interrupts.size() == 2 && deferred.sink.Writes().back() == Write{ kWriteback, 0x18 } &&
             deferred.ledger.Snapshot().oldestRecord == "awaiting worker submit 0x00130000"));
    worker.Submit(deferred.ledger, kEndFence, 0x8, 0x20);
    UNSCOPED_INFO("the deferred fence buffer follows");
    REQUIRE((deferred.ledger.Pump() && deferred.sink.Writes().back() == Write{ kWriteback, 0x20 } && deferred.failures.empty()));
}

// Replay marks end with their thread, including when its ID is reused.
void testReplayMarkLifetime()
{
    Harness h;
    {
        Worker worker;
        worker.Run([&]
                   {
                       h.ledger.MarkReplayThread();
                   });
    }
    for (std::uint32_t i = 0; i < 64 && h.failures.empty(); ++i)
    {
        std::thread recorder([&]
                             {
                                 h.ledger.Fence(kBlock, i, 0x40);
                                 h.ledger.Kick(0x10 + i);
                             });
        recorder.join();
    }
    UNSCOPED_INFO("threads after an exited worker record progress");
    REQUIRE(h.failures.empty());
}

// Worker replay submits deferred EndTiling buffers after replaying tiles.
void testTiledFrame()
{
    Harness h;
    Worker  worker;
    h.sink.handler    = clearCpuBit;
    h.draws.completed = 100;

    h.ledger.BeginTiledList();
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    h.ledger.FinishDraw(1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    h.ledger.TiledSegment(kTile0, 0x10);
    h.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000006, kFirstPassPredicate);
    h.ledger.TiledSegment(kTile1, 0x20);
    // 0x826AC218 records the COHER barrier, the worker-kick callback and the
    // fence into side buffers, then kicks COHER and the callback directly.
    h.ledger.Barrier(0x8269BC54);
    h.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000007, std::nullopt, kSideBuffer);
    h.ledger.Fence(kBlock, 9, 0x40, kEndFence);
    // Each kick is pumped before the next, as the completion thread may.
    h.ledger.DirectSubmit(kCoher, 0x10);
    h.ledger.Kick(0x10);
    while (h.ledger.Pump())
    {
    }
    UNSCOPED_INFO("no worker-kick callback raises at the COHER kick");
    REQUIRE((h.sink.Writes() == std::vector<Write>{ { kWriteback, 0x10 } } && h.sink.interrupts.empty()));
    h.ledger.DirectSubmit(kSideBuffer, 0x20);
    h.ledger.Kick(0x18);
    while (h.ledger.Pump())
    {
    }
    h.ledger.Defer(kEndFence, 0x8);
    // The next frame's first segment is deferred behind it.
    h.ledger.Fence(kBlock, 10, 0x80);
    h.ledger.Defer(kNextFrame, 0x20);

    while (h.ledger.Pump())
    {
    }
    const std::vector<Write> started{
        { kWriteback, 0x10 },
        { kScratch + kScratchCallbackOffset, kWorkerKick },
        { kScratch + kScratchParameterOffset, 0x5000 },
        { kScratch + kScratchMaskOffset, 4 },
        { kScratch + kScratchCallbackOffset, kRetiredCallback },
        { kScratch + kScratchCallbackOffset, kWorkerKick },
        { kScratch + kScratchParameterOffset, 0x5000 },
        { kScratch + kScratchMaskOffset, 4 },
        { kScratch + kScratchCallbackOffset, kRetiredCallback },
        { kWriteback, 0x18 },
    };
    UNSCOPED_INFO("both worker-kick callbacks raise before any worker submit; the EndTiling fence waits for it");
    REQUIRE((h.sink.Writes() == started && h.sink.interrupts.size() == 2 && h.ledger.Snapshot().oldestRecord == "awaiting worker submit 0x00130000"));

    // Tile 0, then the resumed entry replays tile 1 and submits the deferred segments.
    worker.Run([&]
               {
                   h.ledger.MarkReplayThread();
               });
    std::uint32_t wptr = 0x20;
    for (int tile = 0; tile < 2; ++tile)
        for (const auto [segment, dwords] : { std::pair{ kTile0, 0x10U }, std::pair{ kTile1, 0x20U } })
        {
            worker.Submit(h.ledger, segment, dwords, wptr);
            wptr += 3;
        }
    h.ledger.Pump();
    UNSCOPED_INFO("replays write only RPTR, through the last replay");
    REQUIRE((h.sink.Writes().size() == started.size() + 1 && h.sink.Writes().back() == Write{ kWriteback, 0x29 }));
    worker.Submit(h.ledger, kEndFence, 0x8, 0x30);
    worker.Submit(h.ledger, kNextFrame, 0x20, 0x33);
    while (h.ledger.Pump())
    {
    }
    auto expected = started;
    expected.insert(expected.end(),
                    {
                        { kWriteback, 0x29 },
                        { kBlock + kCursorOffset, 0x40 },
                        { kBlock, 9 },
                        { kWriteback, 0x30 },
                        { kBlock + kCursorOffset, 0x80 },
                        { kBlock, 10 },
                        { kWriteback, 0x33 },
                    });
    const auto snapshot = h.ledger.Snapshot();
    UNSCOPED_INFO("the deferred fences release after their worker submits, in order, with RPTR increasing");
    REQUIRE((h.sink.Writes() == expected && snapshot.replaySubmits == 4 && snapshot.deferredSpans == 2 && snapshot.callbacksReleased == 2 &&
             h.failures.empty()));
}

// MRT prepass backlog preserves two tiled lists across worker replay.
void testTiledListBacklog()
{
    constexpr std::uint32_t kMrtSegment = 0x1EBB00C0, kMrtDwords = 4063;
    constexpr std::uint32_t kMrtFence = 0x1EBFD360; // EndTiling's 10-dword fence buffer
    constexpr std::uint32_t kMrtSide = 0x00170000, kMrtCoher = 0x00180000;
    constexpr std::uint32_t kMainSegment = 0x00190000, kMainSide = 0x001A0000, kMainCoher = 0x001B0000, kMainFence = 0x001C0000;

    Harness h;
    Worker  worker;
    h.sink.handler       = clearCpuBit;
    const auto endTiling = [&](std::uint32_t side, std::uint32_t fenceBuffer, std::uint32_t fence)
    {
        h.ledger.Barrier(0x8269BC54);
        h.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000007, std::nullopt, side);
        h.ledger.Fence(kBlock, fence, fence << 4, fenceBuffer);
    };

    // The MRT list ends while the worker is idle: COHER and the callback buffer are kicked directly.
    h.ledger.BeginTiledList();
    h.ledger.TiledSegment(kMrtSegment, kMrtDwords);
    h.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000006, kFirstPassPredicate);
    endTiling(kMrtSide, kMrtFence, 9);
    h.ledger.DirectSubmit(kMrtCoher, 0x10);
    h.ledger.Kick(0x10);
    h.ledger.DirectSubmit(kMrtSide, 0x20);
    h.ledger.Kick(0x18);
    h.ledger.Defer(kMrtFence, 10);
    // The main list ends while the worker is busy: all three buffers are deferred.
    h.ledger.BeginTiledList();
    h.ledger.TiledSegment(kMainSegment, 0x20);
    h.ledger.Callback(kScratch, kWorkerKick, 0x5000, 0x04000006, kFirstPassPredicate);
    endTiling(kMainSide, kMainFence, 10);
    h.ledger.Defer(kMainCoher, 0x10);
    h.ledger.Defer(kMainSide, 0x20);
    h.ledger.Defer(kMainFence, 10);
    // The next frame's list begins before the worker replays the MRT list.
    h.ledger.BeginTiledList();
    h.ledger.TiledSegment(0x001D0000, 0x30);
    while (h.ledger.Pump())
    {
    }
    UNSCOPED_INFO("the MRT callbacks start the worker; the MRT fence waits for it");
    REQUIRE((h.sink.interrupts.size() == 2 && h.ledger.Snapshot().oldestRecord == "awaiting worker submit 0x1ebfd360"));

    worker.Run([&]
               {
                   h.ledger.MarkReplayThread();
               });
    std::uint32_t wptr = 0x20;
    for (int tile = 0; tile < 3; ++tile, wptr += 3)
        worker.Submit(h.ledger, kMrtSegment, kMrtDwords, wptr);
    UNSCOPED_INFO("the worker replays the MRT list behind two newer lists: " + failureDetail(h.failures));
    REQUIRE(h.failures.empty());
    for (const auto [buffer, dwords] : { std::pair{ kMrtFence, 10U }, std::pair{ kMainCoher, 0x10U }, std::pair{ kMainSide, 0x20U } })
    {
        worker.Submit(h.ledger, buffer, dwords, wptr);
        wptr += 3;
    }
    while (h.ledger.Pump())
    {
    }
    UNSCOPED_INFO("the main list's callback buffer, chained after the MRT fence, starts it: " + failureDetail(h.failures));
    REQUIRE((h.sink.interrupts.size() == 4 && h.ledger.Snapshot().oldestRecord == "awaiting worker submit 0x001c0000" && h.failures.empty()));
    for (int tile = 0; tile < 2; ++tile, wptr += 3)
        worker.Submit(h.ledger, kMainSegment, 0x20, wptr);
    worker.Submit(h.ledger, kMainFence, 10, wptr);
    while (h.ledger.Pump())
    {
    }
    const auto snapshot = h.ledger.Snapshot();
    UNSCOPED_INFO("both lists replay and every fence releases in order: " + failureDetail(h.failures));
    REQUIRE((h.failures.empty() && snapshot.replaySubmits == 5 && snapshot.deferredSpans == 4 && snapshot.tiledListsStarted == 2 &&
             snapshot.maxTiledLists == 3 && snapshot.lastReleasedFence == 10 && readPointers(h.sink).back() == wptr));
}

} // namespace

// Suspended swap re-presents after 30 ms; bracket is device-owned.
void testDeviceOwnerSwap()
{
    constexpr std::uint32_t kFront = 0x1F000000;
    Harness                 h;
    Worker                  worker;
    worker.Run([&]
               {
                   h.ledger.MarkReplayThread();
               });
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    h.ledger.FinishDraw(1, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    h.ledger.Swap(kFront);
    // BlockUntilIdle 0x8269C810 fences and kicks before the flag store.
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.Kick(0x10);
    h.ledger.Suspend();
    h.ledger.RequestFence(3, kBlock);
    // The main frame's draw is still on the host while the worker swaps, so
    // nothing the brackets recorded may reach the guest before it completes.
    for (std::uint32_t swap = 0; swap < 3; ++swap)
        worker.Run([&]
                   {
                       h.ledger.BeginDeviceOwnerSwap();
                       h.ledger.ScratchWrite(kFlip, 1, 0x826A419C);
                       h.ledger.GpuWait(kFlip, 1, 0x826A419C);
                       h.ledger.GpuTimestamp(kBlock + kTimestampOffset, kBlock);
                       h.ledger.Swap(kFront);
                       h.ledger.Fence(kBlock, 4 + swap, 0x80 + 0x40 * swap);
                       h.ledger.DirectSubmit(kNextFrame, 0x10);
                       h.ledger.Kick(0x20 + 0x10 * swap);
                       h.ledger.RequestFence(4 + swap, kBlock);
                       UNSCOPED_INFO("the bracket's records wait for the pending host fence");
                       REQUIRE((!h.ledger.Pump() && h.sink.Writes().empty()));
                       h.ledger.EndDeviceOwnerSwap();
                   });
    h.ledger.Resume();
    h.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
    h.ledger.FinishDraw(2, NativeDrawHook::IndexedUp, kCaller, std::nullopt);
    h.ledger.Fence(kBlock, 7, 0x140);
    h.ledger.Kick(0x50);
    UNSCOPED_INFO("after resume everything still waits for the main frame's host fence");
    REQUIRE((!h.ledger.Pump() && h.sink.Writes().empty() && h.draws.flushed == std::vector<std::uint64_t>{ 1 }));

    h.draws.completed = 1;
    while (h.ledger.Pump())
    {
    }
    std::vector<Write> expected{ { kBlock + kCursorOffset, 0x40 }, { kBlock, 3 }, { kWriteback, 0x10 } };
    for (std::uint32_t swap = 0; swap < 3; ++swap)
        expected.insert(expected.end(),
                        { { kFlip, 1 }, { kBlock + kTimestampOffset, swap + 1 }, { kBlock + kCursorOffset, 0x80 + 0x40 * swap }, { kBlock, 4 + swap }, { kWriteback, 0x20 + 0x10 * swap } });
    UNSCOPED_INFO("the completed fence releases the main span, then each bracket's scratch, timestamp, fence and RPTR in order");
    REQUIRE(h.sink.Writes() == expected);

    h.draws.completed = 2;
    while (h.ledger.Pump())
    {
    }
    const auto snapshot = h.ledger.Snapshot();
    UNSCOPED_INFO("three deferred swaps between suspend and resume pass: " + snapshot.failure.value_or("none"));
    REQUIRE((h.failures.empty() && snapshot.deviceSuspends == 1 && snapshot.deferredSwaps == 3 && snapshot.swaps == 4));
    UNSCOPED_INFO("the main span after resume releases last");
    REQUIRE((readPointers(h.sink) == std::vector<std::uint32_t>{ 0x10, 0x20, 0x30, 0x40, 0x50 } && snapshot.lastReleasedFence == 7 && !snapshot.pending));
}

// Each departure from the suspend and deferred-swap form fails with its code.
void testDeviceOwnerSwapFailures()
{
    const auto failed = [](Harness& h, const std::string& detail)
    {
        return h.failures.size() == 1 && h.failures[0].detail == detail;
    };
    Worker worker;
    // The worker's bracket body from the flip request through its kick.
    const auto deferredSwap = [](CompletionLedger& ledger)
    {
        ledger.ScratchWrite(kFlip, 1, 0x826A419C);
        ledger.Swap(0x1F000000);
        ledger.Kick(0x20);
    };

    Harness unsuspended(NativeDrawMode::Skip);
    worker.Run([&]
               {
                   unsuspended.ledger.BeginDeviceOwnerSwap();
               });
    UNSCOPED_INFO("a deferred swap without a suspend fails");
    REQUIRE(failed(unsuspended, "deferred_swap_unsuspended"));

    Harness mainRecord(NativeDrawMode::Skip);
    mainRecord.ledger.Suspend();
    mainRecord.ledger.RequestFence(3, kBlock);
    mainRecord.ledger.Fence(kBlock, 3, 0x40);
    UNSCOPED_INFO("a main record while suspended fails; its fence wait does not");
    REQUIRE(failed(mainRecord, "device_suspended_record fence at 0x8269bca8"));

    // Only the bracket's own thread is the device thread while it is open.
    Harness duringBracket(NativeDrawMode::Skip);
    duringBracket.ledger.Suspend();
    worker.Run([&]
               {
                   duringBracket.ledger.BeginDeviceOwnerSwap();
               });
    duringBracket.ledger.Fence(kBlock, 3, 0x40);
    UNSCOPED_INFO("a main record during the worker's bracket fails");
    REQUIRE(failed(duringBracket, "device_suspended_record fence at 0x8269bca8"));

    Harness workerDraw(NativeDrawMode::Skip);
    workerDraw.ledger.Suspend();
    worker.Run([&]
               {
                   workerDraw.ledger.MarkReplayThread();
                   workerDraw.ledger.BeginDraw(NativeDrawHook::IndexedUp, kCaller);
               });
    UNSCOPED_INFO("a worker draw outside the bracket fails");
    REQUIRE(failed(workerDraw, "progress_on_replay_thread draw at 0x82303e40"));

    Harness unkicked(NativeDrawMode::Skip);
    unkicked.ledger.Suspend();
    worker.Run([&]
               {
                   unkicked.ledger.BeginDeviceOwnerSwap();
                   unkicked.ledger.ScratchWrite(kFlip, 1, 0x826A419C);
                   unkicked.ledger.Swap(0x1F000000);
                   unkicked.ledger.EndDeviceOwnerSwap();
               });
    UNSCOPED_INFO("a deferred swap that never kicks fails");
    REQUIRE(failed(unkicked, "deferred_swap_unkicked: scratch_write at 0x826a419c"));

    Harness unswapped(NativeDrawMode::Skip);
    unswapped.ledger.Suspend();
    worker.Run([&]
               {
                   unswapped.ledger.BeginDeviceOwnerSwap();
                   unswapped.ledger.ScratchWrite(kFlip, 1, 0x826A419C);
                   unswapped.ledger.Kick(0x20);
                   unswapped.ledger.EndDeviceOwnerSwap();
               });
    UNSCOPED_INFO("a deferred swap without its VdSwap fails");
    REQUIRE(failed(unswapped, "deferred_swap_unswapped"));

    Harness spanOpen(NativeDrawMode::Skip);
    spanOpen.ledger.Fence(kBlock, 3, 0x40);
    spanOpen.ledger.Suspend();
    worker.Run([&]
               {
                   spanOpen.ledger.BeginDeviceOwnerSwap();
               });
    UNSCOPED_INFO("a deferred swap over an unkicked span fails");
    REQUIRE(failed(spanOpen, "deferred_swap_span_open: fence at 0x8269bca8"));

    Harness held(NativeDrawMode::Skip);
    held.ledger.Fence(kBlock, 9, 0x40, kEndFence);
    held.ledger.Suspend();
    worker.Run([&]
               {
                   held.ledger.BeginDeviceOwnerSwap();
               });
    UNSCOPED_INFO("a deferred swap over a held EndTiling buffer fails");
    REQUIRE(failed(held, "deferred_swap_held"));

    Harness epochOpen(NativeDrawMode::Skip);
    epochOpen.ledger.Clear(1);
    epochOpen.ledger.Kick(0x10);
    epochOpen.ledger.Suspend();
    worker.Run([&]
               {
                   epochOpen.ledger.BeginDeviceOwnerSwap();
               });
    UNSCOPED_INFO("a deferred swap would close the main thread's unswapped frame work");
    REQUIRE(failed(epochOpen, "deferred_swap_epoch_open"));

    Harness resumed(NativeDrawMode::Skip);
    resumed.ledger.Suspend();
    worker.Run([&]
               {
                   resumed.ledger.BeginDeviceOwnerSwap();
               });
    resumed.ledger.Resume();
    UNSCOPED_INFO("a resume inside the bracket fails");
    REQUIRE(failed(resumed, "resume_during_deferred_swap"));

    Harness unsuspendedResume(NativeDrawMode::Skip);
    unsuspendedResume.ledger.Resume();
    UNSCOPED_INFO("a resume without a suspend fails");
    REQUIRE(failed(unsuspendedResume, "resume_unsuspended"));

    Harness nestedSuspend(NativeDrawMode::Skip);
    nestedSuspend.ledger.Suspend();
    nestedSuspend.ledger.Suspend();
    UNSCOPED_INFO("a second suspend fails");
    REQUIRE(failed(nestedSuspend, "suspend_nested"));

    Harness nested(NativeDrawMode::Skip);
    nested.ledger.Suspend();
    worker.Run([&]
               {
                   nested.ledger.BeginDeviceOwnerSwap();
                   nested.ledger.BeginDeviceOwnerSwap();
               });
    UNSCOPED_INFO("a bracket inside a bracket fails");
    REQUIRE(failed(nested, "deferred_swap_nested"));

    Harness unopenedEnd(NativeDrawMode::Skip);
    unopenedEnd.ledger.Suspend();
    worker.Run([&]
               {
                   unopenedEnd.ledger.EndDeviceOwnerSwap();
               });
    UNSCOPED_INFO("a bracket end without its begin fails");
    REQUIRE(failed(unopenedEnd, "deferred_swap_unopened"));

    // The worker swap without its bracket is the replay-thread failure the
    // bracket replaces, and the worker is the replay thread again after one.
    Harness unbracketed(NativeDrawMode::Skip);
    unbracketed.ledger.Suspend();
    worker.Run([&]
               {
                   unbracketed.ledger.MarkReplayThread();
                   deferredSwap(unbracketed.ledger);
               });
    UNSCOPED_INFO("a worker swap without its bracket fails");
    REQUIRE(failed(unbracketed, "progress_on_replay_thread scratch_write at 0x826a419c"));
    Harness after(NativeDrawMode::Skip);
    after.ledger.Suspend();
    worker.Run([&]
               {
                   after.ledger.MarkReplayThread();
                   after.ledger.BeginDeviceOwnerSwap();
                   deferredSwap(after.ledger);
                   after.ledger.EndDeviceOwnerSwap();
                   after.ledger.Fence(kBlock, 3, 0x40);
               });
    UNSCOPED_INFO("a worker record after the bracket fails");
    REQUIRE(failed(after, "progress_on_replay_thread fence at 0x8269bca8"));
}

// Device release/acquire joins loading and main records and restores ordering.
void testDeviceHandoff()
{
    // Handoff follows 0x8268CF50 release/0x8268CF10 acquire.
    Harness h;
    Worker  loader;
    h.ledger.AcquireDevice();
    h.ledger.Fence(kBlock, 3, 0x40);
    h.ledger.ReleaseDevice();
    loader.Run([&]
               {
                   h.ledger.AcquireDevice();
                   h.ledger.Fence(kBlock, 4, 0x80);
                   h.ledger.Kick(0x10);
                   h.ledger.Fence(kBlock, 5, 0xC0);
                   h.ledger.ReleaseDevice();
               });
    h.ledger.AcquireDevice();
    h.ledger.Fence(kBlock, 6, 0x100);
    h.ledger.Kick(0x20);
    h.ledger.ReleaseDevice();
    UNSCOPED_INFO("each acquire takes over the open span the previous owner released");
    REQUIRE((h.failures.empty() && h.ledger.Snapshot().deviceHandoffs == 2));
    while (h.ledger.Pump())
        ;
    UNSCOPED_INFO("both threads' records release in program order");
    REQUIRE(h.sink.Writes() == std::vector<Write>{ { kBlock + kCursorOffset, 0x40 },
                                                   { kBlock, 3 },
                                                   { kBlock + kCursorOffset, 0x80 },
                                                   { kBlock, 4 },
                                                   { kWriteback, 0x10 },
                                                   { kBlock + kCursorOffset, 0xC0 },
                                                   { kBlock, 5 },
                                                   { kBlock + kCursorOffset, 0x100 },
                                                   { kBlock, 6 },
                                                   { kWriteback, 0x20 } });

    // The main thread kept the device: the loader's acquire is unordered.
    Harness stolen;
    Worker  thief;
    stolen.ledger.AcquireDevice();
    stolen.ledger.Fence(kBlock, 3, 0x40);
    thief.Run([&]
              {
                  stolen.ledger.AcquireDevice();
              });
    UNSCOPED_INFO("an acquire while another thread owns the device fails the session");
    REQUIRE((stolen.failures.size() == 1 && stolen.failures[0].detail == "device_acquired_while_owned"));

    // A record from a thread that never acquired the released device.
    Harness unacquired;
    Worker  stray;
    unacquired.ledger.AcquireDevice();
    unacquired.ledger.Fence(kBlock, 3, 0x40);
    unacquired.ledger.ReleaseDevice();
    stray.Run([&]
              {
                  unacquired.ledger.Fence(kBlock, 4, 0x80);
              });
    UNSCOPED_INFO("a record without an acquire still fails as a second thread");
    REQUIRE((unacquired.failures.size() == 1 && unacquired.failures[0].detail == "second_submit_thread"));

    // Only the owner's release frees the device.
    Harness foreign;
    Worker  other;
    foreign.ledger.AcquireDevice();
    other.Run([&]
              {
                  foreign.ledger.ReleaseDevice();
                  foreign.ledger.AcquireDevice();
              });
    UNSCOPED_INFO("another thread's release leaves the owner, so its acquire still fails");
    REQUIRE((foreign.failures.size() == 1 && foreign.failures[0].detail == "device_acquired_while_owned"));
}

TEST_CASE("completion_ledger", "[ctest][completion-ledger][timeout-60]")
{
    testGuestImageOrder();
    testHeldGuestImages();
    testGuestImageRuns();
    testCloseDuringImage();
    testResolveCopyGuard();
    testUnsupportedClear();
    testCensusMode();
    testHeldHostFence();
    testNoDrawSpanReleasesAtKick();
    testUnownedSpan();
    testStrictDrawFailure();
    testSkipMode();
    testFrontbufferException();
    testGpuWait();
    testSourceOneSequencing();
    testPredicatedCallback();
    testFailures();
    testRingEpochReset();
    testDropDiscardsImages();
    testMidEpochFlush();
    testFailureBoundary();
    testCoherBarrier();
    testDisplayWait();
    testDrawVerdictPending();
    testDetachedDraw();
    testCloseDuringInterrupt();
    testUndeliveredInterrupt();
    testCloseWithWorkInFlight();
    testMissingWorkerSubmit();
    testGpuTimestamp();
    testDeferredKick();
    testReplayKicks();
    testReplayFailures();
    testEndTilingSubmissions();
    testReplayMarkLifetime();
    testTiledFrame();
    testTiledListBacklog();
    testDeviceOwnerSwap();
    testDeviceOwnerSwapFailures();
    testDeviceHandoff();
}
