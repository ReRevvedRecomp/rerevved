// CPU-only model of the native presentation contract: output lease states,
// shared drain deadlines, surface and resize state, letterboxing, color
// quantization and the telemetry counting rules.
#include "gpu/present/native_output_lease.h"
#include "gpu/present/native_presentation.h"
#include "gpu/present/offer_gate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rerevved::gpu;
using namespace std::chrono_literals;

namespace
{

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "native_presentation_tests: " << message << '\n';
    std::exit(1);
}

std::shared_ptr<NativeOutputLease> publish(std::uint64_t token = 1)
{
    return std::make_shared<NativeOutputLease>(token);
}

// Valid paths into every state.
std::shared_ptr<NativeOutputLease> leaseIn(OutputLeaseState state)
{
    auto lease = publish();
    switch (state)
    {
        case OutputLeaseState::Published:
            break;
        case OutputLeaseState::Claimed:
            UNSCOPED_INFO("claim for setup");
            REQUIRE(lease->Claim());
            break;
        case OutputLeaseState::CopySubmitted:
            UNSCOPED_INFO("submit for setup");
            REQUIRE((lease->Claim() && lease->MarkCopySubmitted(7)));
            break;
        case OutputLeaseState::Retired:
            UNSCOPED_INFO("retire for setup");
            REQUIRE((lease->Claim() && lease->MarkCopySubmitted(7) && lease->Retire()));
            break;
        case OutputLeaseState::Cancelled:
            UNSCOPED_INFO("cancel for setup");
            REQUIRE(lease->Cancel());
            break;
        case OutputLeaseState::Failed:
            UNSCOPED_INFO("fail for setup");
            REQUIRE((lease->Claim() && lease->Fail(OutputLeaseFailure::Timeout)));
            break;
    }
    UNSCOPED_INFO("setup reached the requested state");
    REQUIRE(lease->State() == state);
    return lease;
}

void leaseHappyPath()
{
    auto lease = publish(42);
    UNSCOPED_INFO("a new lease is published");
    REQUIRE((lease->Token() == 42 && lease->State() == OutputLeaseState::Published));
    UNSCOPED_INFO("a published lease has no copy submission or failure");
    REQUIRE((lease->CopySubmissionId() == 0 && lease->Failure() == OutputLeaseFailure::None));
    UNSCOPED_INFO("a published lease is not retired");
    REQUIRE(!lease->AwaitRetirement(0ms));
    UNSCOPED_INFO("claim");
    REQUIRE((lease->Claim() && lease->State() == OutputLeaseState::Claimed));
    UNSCOPED_INFO("a claimed lease is not retired");
    REQUIRE(!lease->AwaitRetirement(0ms));
    UNSCOPED_INFO("copy submitted records the submission id");
    REQUIRE((lease->MarkCopySubmitted(9) && lease->State() == OutputLeaseState::CopySubmitted && lease->CopySubmissionId() == 9));
    UNSCOPED_INFO("a submitted copy is not retired");
    REQUIRE(!lease->AwaitRetirement(0ms));
    UNSCOPED_INFO("retire after the copy fence");
    REQUIRE((lease->Retire() && lease->State() == OutputLeaseState::Retired));
    UNSCOPED_INFO("a retired lease releases the producer");
    REQUIRE(lease->AwaitRetirement(0ms));
}

void cancelBeforeClaim()
{
    auto lease = publish();
    UNSCOPED_INFO("an unclaimed offer cancels");
    REQUIRE((lease->Cancel() && lease->State() == OutputLeaseState::Cancelled));
    UNSCOPED_INFO("a cancelled lease releases the producer");
    REQUIRE(lease->AwaitRetirement(0ms));
    UNSCOPED_INFO("a cancelled lease cannot be claimed");
    REQUIRE(!lease->Claim());
    UNSCOPED_INFO("a cancelled lease cannot submit a copy");
    REQUIRE(!lease->MarkCopySubmitted(3));
    UNSCOPED_INFO("a cancelled lease cannot retire");
    REQUIRE(!lease->Retire());
    UNSCOPED_INFO("a cancelled lease cannot fail");
    REQUIRE(!lease->Fail(OutputLeaseFailure::Timeout));
    UNSCOPED_INFO("rejected transitions leave the state");
    REQUIRE(lease->State() == OutputLeaseState::Cancelled);
}

void cancelAfterClaimBeforeSubmit()
{
    auto lease = leaseIn(OutputLeaseState::Claimed);
    UNSCOPED_INFO("a claimed lease cannot cancel or release its slot");
    REQUIRE((!lease->Cancel() && !lease->AwaitReuse(0ms)));
    auto submitted = leaseIn(OutputLeaseState::CopySubmitted);
    UNSCOPED_INFO("a submitted copy never cancels");
    REQUIRE((!submitted->Cancel() && submitted->State() == OutputLeaseState::CopySubmitted));
}

void claimThenDeviceLost()
{
    auto claimed = leaseIn(OutputLeaseState::Claimed);
    UNSCOPED_INFO("device loss terminates a claimed lease");
    REQUIRE(claimed->Terminate(OutputLeaseFailure::DeviceLost));
    UNSCOPED_INFO("a claimed lease fails with the device loss");
    REQUIRE((claimed->State() == OutputLeaseState::Failed && claimed->Failure() == OutputLeaseFailure::DeviceLost));
    UNSCOPED_INFO("a device-lost lease never releases the producer");
    REQUIRE(!claimed->AwaitRetirement(0ms));
    UNSCOPED_INFO("a failed lease accepts no transition");
    REQUIRE((!claimed->Retire() && !claimed->Cancel() && !claimed->Claim()));

    auto submitted = leaseIn(OutputLeaseState::CopySubmitted);
    UNSCOPED_INFO("a submitted copy fails on device loss");
    REQUIRE((submitted->Terminate(OutputLeaseFailure::DeviceLost) && submitted->State() == OutputLeaseState::Failed));

    auto unclaimed = publish();
    UNSCOPED_INFO("an unclaimed offer was never used and cancels");
    REQUIRE((unclaimed->Terminate(OutputLeaseFailure::DeviceLost) && unclaimed->State() == OutputLeaseState::Cancelled));
    UNSCOPED_INFO("a cancelled offer releases the producer");
    REQUIRE(unclaimed->AwaitRetirement(0ms));

    auto noReason = leaseIn(OutputLeaseState::Claimed);
    UNSCOPED_INFO("termination always records a reason");
    REQUIRE((noReason->Terminate(OutputLeaseFailure::None) && noReason->Failure() == OutputLeaseFailure::HostClosed));

    auto retired = leaseIn(OutputLeaseState::Retired);
    UNSCOPED_INFO("a retired lease survives device loss");
    REQUIRE((!retired->Terminate(OutputLeaseFailure::DeviceLost) && retired->State() == OutputLeaseState::Retired));
}

void doubleRetireRejected()
{
    auto lease = leaseIn(OutputLeaseState::CopySubmitted);
    UNSCOPED_INFO("first retire");
    REQUIRE(lease->Retire());
    UNSCOPED_INFO("second retire is rejected");
    REQUIRE(!lease->Retire());
    UNSCOPED_INFO("a double retire leaves the state");
    REQUIRE(lease->State() == OutputLeaseState::Retired);
    auto early = leaseIn(OutputLeaseState::Claimed);
    UNSCOPED_INFO("retire before a copy was submitted is rejected");
    REQUIRE(!early->Retire());
}

void sentinelsAndReasons()
{
    auto claimed = leaseIn(OutputLeaseState::Claimed);
    UNSCOPED_INFO("a zero copy submission id is rejected");
    REQUIRE(!claimed->MarkCopySubmitted(0));
    UNSCOPED_INFO("the device-removal sentinel is never a copy submission id");
    REQUIRE(!claimed->MarkCopySubmitted(UINT64_MAX));
    UNSCOPED_INFO("rejected submission ids leave the state");
    REQUIRE(claimed->State() == OutputLeaseState::Claimed);
    UNSCOPED_INFO("a failure needs a reason");
    REQUIRE(!claimed->Fail(OutputLeaseFailure::None));
    UNSCOPED_INFO("an unclaimed offer cannot fail");
    REQUIRE(!publish()->Fail(OutputLeaseFailure::Timeout));
}

struct Expectation
{
    OutputLeaseState state;
    bool             claim, cancel, submit, retire, fail, terminate;
};

void transitionMatrix()
{
    // Rows: starting state. Columns: legality of each operation.
    const std::array<Expectation, 6> table = { {
        { OutputLeaseState::Published, true, true, false, false, false, true },
        { OutputLeaseState::Claimed, false, false, true, false, true, true },
        { OutputLeaseState::CopySubmitted, false, false, false, true, true, true },
        { OutputLeaseState::Retired, false, false, false, false, false, false },
        { OutputLeaseState::Cancelled, false, false, false, false, false, false },
        { OutputLeaseState::Failed, false, false, false, false, false, false },
    } };
    for (const auto& row : table)
    {
        const std::string name = OutputLeaseStateName(row.state);
        UNSCOPED_INFO(name + ": claim");
        REQUIRE(leaseIn(row.state)->Claim() == row.claim);
        UNSCOPED_INFO(name + ": cancel");
        REQUIRE(leaseIn(row.state)->Cancel() == row.cancel);
        UNSCOPED_INFO(name + ": submit");
        REQUIRE(leaseIn(row.state)->MarkCopySubmitted(5) == row.submit);
        UNSCOPED_INFO(name + ": retire");
        REQUIRE(leaseIn(row.state)->Retire() == row.retire);
        UNSCOPED_INFO(name + ": fail");
        REQUIRE(leaseIn(row.state)->Fail(OutputLeaseFailure::Timeout) == row.fail);
        UNSCOPED_INFO(name + ": terminate");
        REQUIRE(leaseIn(row.state)->Terminate(OutputLeaseFailure::DeviceLost) == row.terminate);
        const bool terminal = IsTerminalOutputLeaseState(row.state);
        auto       lease    = leaseIn(row.state);
        UNSCOPED_INFO(name + ": only retirement or cancellation releases the producer");
        REQUIRE(lease->AwaitRetirement(0ms) == (row.state == OutputLeaseState::Retired || row.state == OutputLeaseState::Cancelled));
        UNSCOPED_INFO(name + ": terminal classification");
        REQUIRE(terminal == (row.state == OutputLeaseState::Retired || row.state == OutputLeaseState::Cancelled || row.state == OutputLeaseState::Failed));
    }
}

void waitsAreBounded()
{
    auto lease = leaseIn(OutputLeaseState::Claimed);
    auto start = std::chrono::steady_clock::now();
    UNSCOPED_INFO("a claimed lease times out");
    REQUIRE(!lease->AwaitRetirement(60ms));
    UNSCOPED_INFO("the wait honors its timeout");
    REQUIRE(std::chrono::steady_clock::now() - start >= 55ms);

    // An expired shared deadline ends a long wait at once.
    lease->SetDeadline(std::chrono::steady_clock::now() - 1ms);
    start = std::chrono::steady_clock::now();
    UNSCOPED_INFO("an expired deadline ends the wait");
    REQUIRE(!lease->AwaitRetirement(5000ms));
    UNSCOPED_INFO("the deadline caps the producer wait");
    REQUIRE(std::chrono::steady_clock::now() - start < 1000ms);

    // A deadline installed during a wait ends it.
    auto              waiting = leaseIn(OutputLeaseState::Claimed);
    std::atomic<bool> result{ true };
    start = std::chrono::steady_clock::now();
    std::thread waiter([&]
                       {
                           result = waiting->AwaitRetirement(10000ms);
                       });
    std::this_thread::sleep_for(50ms);
    waiting->SetDeadline(std::chrono::steady_clock::now() + 20ms);
    waiter.join();
    UNSCOPED_INFO("a late deadline ends a pending wait");
    REQUIRE((!result.load() && std::chrono::steady_clock::now() - start < 3000ms));

    // The earliest deadline wins.
    auto capped = leaseIn(OutputLeaseState::Claimed);
    capped->SetDeadline(std::chrono::steady_clock::now() + 10000ms);
    capped->SetDeadline(std::chrono::steady_clock::now() - 1ms);
    capped->SetDeadline(std::chrono::steady_clock::now() + 10000ms);
    start = std::chrono::steady_clock::now();
    UNSCOPED_INFO("a later deadline never extends an earlier one");
    REQUIRE((!capped->AwaitRetirement(5000ms) && std::chrono::steady_clock::now() - start < 1000ms));

    // Retirement from another thread releases a blocked producer.
    auto              pending = leaseIn(OutputLeaseState::CopySubmitted);
    std::atomic<bool> released{ false };
    std::thread       producer([&]
                               {
                             released = pending->AwaitRetirement(5000ms);
                               });
    std::this_thread::sleep_for(30ms);
    UNSCOPED_INFO("retire while the producer waits");
    REQUIRE(pending->Retire());
    producer.join();
    UNSCOPED_INFO("retirement wakes the producer");
    REQUIRE(released.load());
}

void producerConsumerHandoff()
{
    // A consumer drives claim, submit and retire while the producer waits for
    // each lease; cancelled leases release the producer and failed ones do not.
    constexpr int                                  count = 300;
    std::mutex                                     mutex;
    std::deque<std::shared_ptr<NativeOutputLease>> queue;
    std::atomic<int>                               produced{ 0 };
    std::vector<int>                               expectedRelease(count, 0);
    std::vector<int>                               observedRelease(count, 0);
    std::thread                                    consumer([&]
                                                            {
                             int handled = 0;
                             while (handled < count)
                             {
                                 std::shared_ptr<NativeOutputLease> lease;
                                 {
                                     std::lock_guard lock(mutex);
                                     if (!queue.empty())
                                     {
                                         lease = queue.front();
                                         queue.pop_front();
                                     }
                                 }
                                 if (!lease)
                                 {
                                     std::this_thread::yield();
                                     continue;
                                 }
                                 const auto index = static_cast<int>(lease->Token()) - 1;
                                 switch (index % 3)
                                 {
                                     case 0:
                                         lease->Claim();
                                         lease->MarkCopySubmitted(static_cast<std::uint64_t>(index) + 1);
                                         lease->Retire();
                                         break;
                                     case 1:
                                         lease->Cancel();
                                         break;
                                     default:
                                         lease->Claim();
                                         lease->MarkCopySubmitted(static_cast<std::uint64_t>(index) + 1);
                                         lease->Fail(OutputLeaseFailure::Timeout);
                                         break;
                                 }
                                 ++handled;
                             }
                                                            });
    for (int index = 0; index < count; ++index)
    {
        auto lease = std::make_shared<NativeOutputLease>(static_cast<std::uint64_t>(index) + 1);
        {
            std::lock_guard lock(mutex);
            queue.push_back(lease);
        }
        ++produced;
        observedRelease[index] = lease->AwaitRetirement(5000ms) ? 1 : 0;
        expectedRelease[index] = index % 3 == 2 ? 0 : 1;
    }
    consumer.join();
    UNSCOPED_INFO("producer release matches retirement, cancellation and failure");
    REQUIRE(observedRelease == expectedRelease);
}

void deadlineAccounting()
{
    using Clock         = OperationDeadline::Clock;
    const auto t0       = Clock::time_point{} + 1000s;
    const auto deadline = OperationDeadline::After(100ms, t0);
    UNSCOPED_INFO("the expiry is the start plus the budget");
    REQUIRE(deadline.Expiry() == t0 + 100ms);
    UNSCOPED_INFO("remaining time shrinks with the clock");
    REQUIRE((deadline.Remaining(t0) == 100ms && deadline.Remaining(t0 + 30ms) == 70ms));
    UNSCOPED_INFO("remaining time never goes negative");
    REQUIRE((deadline.Remaining(t0 + 100ms) == 0ms && deadline.Remaining(t0 + 5s) == 0ms));
    UNSCOPED_INFO("the deadline expires at its expiry");
    REQUIRE((!deadline.Expired(t0 + 99ms) && deadline.Expired(t0 + 100ms)));

    // One deadline across producer, copy and host: each phase draws from the same remainder.
    auto shared = OperationDeadline::After(100ms, t0);
    auto now    = t0;
    auto wait   = shared.Allot(1s, now);
    UNSCOPED_INFO("the first wait may use the whole budget");
    REQUIRE(wait == 100ms);
    now += 40ms;
    shared.Charge(DeadlinePhase::Producer, 40ms);
    wait = shared.Allot(80ms, now);
    UNSCOPED_INFO("the copy wait is capped by the remainder");
    REQUIRE(wait == 60ms);
    now += 60ms;
    shared.Charge(DeadlinePhase::Copy, 60ms);
    wait = shared.Allot(1s, now);
    UNSCOPED_INFO("the host phase finds the budget spent");
    REQUIRE((wait == 0ms && shared.Expired(now)));
    shared.Charge(DeadlinePhase::Host, 0ms);
    UNSCOPED_INFO("spend is attributed to its phase");
    REQUIRE((shared.Spent(DeadlinePhase::Producer) == 40ms && shared.Spent(DeadlinePhase::Copy) == 60ms && shared.Spent(DeadlinePhase::Host) == 0ms));
    UNSCOPED_INFO("attributed spend never exceeds the shared budget");
    REQUIRE((shared.SpentTotal() == 100ms && shared.SpentTotal() <= 100ms));

    // A cap below the remainder wins; charging cannot extend the expiry.
    auto capped = OperationDeadline::After(500ms, t0);
    UNSCOPED_INFO("a per-wait cap below the remainder wins");
    REQUIRE(capped.Allot(20ms, t0) == 20ms);
    capped.Charge(DeadlinePhase::Host, -5ms);
    UNSCOPED_INFO("negative charges and charges never move the expiry");
    REQUIRE((capped.Spent(DeadlinePhase::Host) == 0ms && capped.Expiry() == t0 + 500ms));

    UNSCOPED_INFO("a negative budget is already expired");
    REQUIRE(OperationDeadline::After(-3s, t0).Expired(t0));
    UNSCOPED_INFO("an expiry in the past allots nothing");
    REQUIRE(OperationDeadline::At(t0 - 1s).Allot(1s, t0) == 0ms);
}

void surfaceStateModel()
{
    SurfaceModel surface;
    UNSCOPED_INFO("a new surface has nothing to present or resize");
    REQUIRE((!surface.CanPresent() && !surface.Suspended() && !surface.PendingResize()));
    surface.RequestExtent({ 1280, 720 });
    UNSCOPED_INFO("the first valid extent is pending");
    REQUIRE(surface.PendingResize() == std::optional<SurfaceExtent>(SurfaceExtent{ 1280, 720 }));
    surface.MarkApplied({ 1280, 720 }, { 1280, 720 });
    UNSCOPED_INFO("an applied extent clears the pending resize");
    REQUIRE((surface.CanPresent() && !surface.PendingResize() && surface.Generation() == 1));
    UNSCOPED_INFO("an applied resize asks for exactly one repaint");
    REQUIRE((surface.TakeRepaint() && !surface.TakeRepaint()));

    // A resize storm coalesces to the newest extent.
    for (std::uint32_t step = 1; step <= 100; ++step)
    {
        surface.RequestExtent({ 200 + step, 100 + step });
    }
    UNSCOPED_INFO("a storm resolves to the latest extent");
    REQUIRE(surface.PendingResize() == std::optional<SurfaceExtent>(SurfaceExtent{ 300, 200 }));
    UNSCOPED_INFO("every request is counted");
    REQUIRE(surface.ExtentRequests() == 101);
    surface.MarkApplied({ 300, 200 }, { 300, 200 });
    UNSCOPED_INFO("one resize serves the whole storm");
    REQUIRE((!surface.PendingResize() && surface.Generation() == 2));
    UNSCOPED_INFO("the storm resize asks for a repaint");
    REQUIRE(surface.TakeRepaint());

    // A zero extent suspends presentation and never becomes a resize target.
    surface.RequestExtent({ 0, 0 });
    UNSCOPED_INFO("a zero extent suspends and does not resize");
    REQUIRE((surface.Suspended() && !surface.CanPresent() && !surface.PendingResize()));
    surface.RequestExtent({ 640, 0 });
    surface.RequestExtent({ 0, 480 });
    UNSCOPED_INFO("a zero width or height is a zero extent");
    REQUIRE((surface.Suspended() && surface.ZeroExtentRequests() == 3 && !surface.PendingResize()));
    UNSCOPED_INFO("zero requests never replace a valid extent");
    REQUIRE((surface.LatestValid() == SurfaceExtent{ 300, 200 } && surface.Applied() == SurfaceExtent{ 300, 200 }));
    surface.RequestExtent({ 300, 200 });
    UNSCOPED_INFO("restoring the applied extent needs no resize");
    REQUIRE((!surface.Suspended() && surface.CanPresent() && !surface.PendingResize()));
    UNSCOPED_INFO("leaving a zero extent asks for one repaint");
    REQUIRE((surface.TakeRepaint() && !surface.TakeRepaint()));

    // A different extent after a zero extent resizes to that extent, never to zero.
    surface.RequestExtent({ 0, 0 });
    surface.RequestExtent({ 1024, 768 });
    UNSCOPED_INFO("restore after zero resizes to the new extent");
    REQUIRE((!surface.Suspended() && surface.PendingResize() == std::optional<SurfaceExtent>(SurfaceExtent{ 1024, 768 })));
    surface.MarkApplied({ 1024, 768 }, { 1024, 768 });

    // Minimize suspends; extents requested meanwhile apply after restore.
    surface.SetMinimized(true);
    UNSCOPED_INFO("minimize suspends presentation");
    REQUIRE((surface.Suspended() && !surface.CanPresent()));
    surface.RequestExtent({ 800, 600 });
    UNSCOPED_INFO("a resize while minimized stays pending");
    REQUIRE((surface.Suspended() && surface.PendingResize() == std::optional<SurfaceExtent>(SurfaceExtent{ 800, 600 })));
    surface.SetMinimized(false);
    UNSCOPED_INFO("restore resumes with a repaint");
    REQUIRE((!surface.Suspended() && surface.TakeRepaint()));
    surface.SetMinimized(false);
    UNSCOPED_INFO("restoring twice repaints once");
    REQUIRE(!surface.TakeRepaint());

    // The swap chain may reach a different extent than requested.
    surface.MarkApplied({ 790, 590 }, { 800, 600 });
    UNSCOPED_INFO("the reached extent becomes the request, avoiding a resize loop");
    REQUIRE((surface.Applied() == SurfaceExtent{ 790, 590 } && !surface.PendingResize()));
    surface.RequestExtent({ 900, 700 });
    surface.MarkApplied({ 880, 690 }, { 800, 600 });
    UNSCOPED_INFO("a newer request survives an older resize");
    REQUIRE(surface.PendingResize() == std::optional<SurfaceExtent>(SurfaceExtent{ 900, 700 }));
}

void letterboxMath()
{
    const SurfaceExtent source{ 1280, 720 };
    UNSCOPED_INFO("an identical extent fills the target");
    REQUIRE(ComputeLetterbox(source, { 1280, 720 }) == LetterboxRect{ 0, 0, 1280, 720 });
    UNSCOPED_INFO("a same-aspect target fills");
    REQUIRE(ComputeLetterbox(source, { 1920, 1080 }) == LetterboxRect{ 0, 0, 1920, 1080 });
    UNSCOPED_INFO("a taller target letterboxes vertically");
    REQUIRE(ComputeLetterbox(source, { 1280, 1000 }) == LetterboxRect{ 0, 140, 1280, 720 });
    UNSCOPED_INFO("a 4:3 target letterboxes");
    REQUIRE(ComputeLetterbox(source, { 800, 600 }) == LetterboxRect{ 0, 75, 800, 450 });
    UNSCOPED_INFO("a wider target pillarboxes");
    REQUIRE(ComputeLetterbox(source, { 2000, 500 }) == LetterboxRect{ 555, 0, 889, 500 });
    UNSCOPED_INFO("a one pixel target keeps one pixel");
    REQUIRE(ComputeLetterbox(source, { 1, 1 }) == LetterboxRect{ 0, 0, 1, 1 });
    UNSCOPED_INFO("a zero target is empty");
    REQUIRE((ComputeLetterbox(source, { 0, 100 }).Empty() && ComputeLetterbox(source, { 100, 0 }).Empty()));
    UNSCOPED_INFO("a zero source is empty");
    REQUIRE((ComputeLetterbox({ 0, 720 }, { 100, 100 }).Empty() && ComputeLetterbox({ 1280, 0 }, { 100, 100 }).Empty()));
    for (std::uint32_t width = 1; width <= 96; ++width)
    {
        for (std::uint32_t height = 1; height <= 96; ++height)
        {
            const auto rect = ComputeLetterbox(source, { width, height });
            UNSCOPED_INFO("the box stays inside the target");
            REQUIRE((!rect.Empty() && rect.x + rect.width <= width && rect.y + rect.height <= height));
            UNSCOPED_INFO("the box touches both sides of one axis");
            REQUIRE((rect.width == width || rect.height == height));
            UNSCOPED_INFO("the box is centered");
            REQUIRE((rect.x == (width - rect.width) / 2 && rect.y == (height - rect.height) / 2));
        }
    }
}

void colorMath()
{
    UNSCOPED_INFO("red occupies the low ten bits and alpha is opaque");
    REQUIRE(PackRgb10A2(1023, 0, 0) == 0xC00003FFU);
    UNSCOPED_INFO("green and blue positions");
    REQUIRE((PackRgb10A2(0, 1023, 0) == 0xC00FFC00U && PackRgb10A2(0, 0, 1023) == 0xFFF00000U));
    UNSCOPED_INFO("out of range fields are masked");
    REQUIRE(PackRgb10A2(1024, 1024, 1024, 4) == 0);
    UNSCOPED_INFO("gamma entry layout matches the producer pass");
    REQUIRE((PackGammaEntry(1023, 0, 0) == 0x3FF00000U && PackGammaEntry(0, 1023, 0) == 0x000FFC00U && PackGammaEntry(0, 0, 1023) == 0x000003FFU));
    UNSCOPED_INFO("quantization keeps the endpoints");
    REQUIRE((QuantizeUnorm10To8(0) == 0 && QuantizeUnorm10To8(1023) == 255));
    std::array<bool, 256> reached{};
    std::uint8_t          previous = 0;
    for (std::uint32_t value = 0; value < 1024; ++value)
    {
        const auto expected = static_cast<std::uint8_t>(std::lround(static_cast<long double>(value) * 255.0L / 1023.0L));
        const auto actual   = QuantizeUnorm10To8(value);
        UNSCOPED_INFO("quantization rounds to nearest at " + std::to_string(value));
        REQUIRE(actual == expected);
        UNSCOPED_INFO("quantization is monotonic");
        REQUIRE(actual >= previous);
        UNSCOPED_INFO("no input falls on a rounding tie");
        REQUIRE((510U * value + 1023U) % 2046U != 0U);
        previous        = actual;
        reached[actual] = true;
    }
    UNSCOPED_INFO("every 8-bit value is produced");
    REQUIRE(std::all_of(reached.begin(), reached.end(), [](bool seen)
                        {
                            return seen;
                        }));
}

void offerGatePolicy()
{
    OfferGate  gate;
    const auto offer = [&]
    {
        std::lock_guard lock(gate.mutex);
        return gate.offerLocked({});
    };
    auto first  = offer();
    auto second = offer();
    auto third  = offer();
    UNSCOPED_INFO("capacity evicts the oldest offer");
    REQUIRE(first->State() == OutputLeaseState::Cancelled);
    UNSCOPED_INFO("claim-newest drops older offers");
    REQUIRE((gate.claimNewest() == third && second->State() == OutputLeaseState::Cancelled));
    UNSCOPED_INFO("eviction and claim drops count once");
    REQUIRE((!gate.claimNewest() && gate.ledger.Counters().dropped == 2));
    auto queued = offer();
    gate.setSuspended(true);
    auto suspended = offer();
    UNSCOPED_INFO("suspend cancels queued and arriving offers");
    REQUIRE((queued->State() == OutputLeaseState::Cancelled && suspended->State() == OutputLeaseState::Cancelled));
    UNSCOPED_INFO("suspended offers never reach the consumer");
    REQUIRE((gate.ledger.Counters().droppedWhileSuspended == 2 && !gate.claimNewest()));
    gate.setSuspended(false);
    auto reused = offer();
    UNSCOPED_INFO("producer reclaims a still-published slot");
    REQUIRE(reused->AwaitReuse(0ms));
    auto newest = offer();
    UNSCOPED_INFO("producer cancellation counts as a drop, not stale");
    REQUIRE((gate.claimNewest() == newest && gate.ledger.Counters().dropped == 3));
    UNSCOPED_INFO("producer cannot reclaim a claimed slot");
    REQUIRE((!newest->AwaitReuse(0ms) && newest->State() == OutputLeaseState::Claimed));
    UNSCOPED_INFO("producer cannot reclaim a submitted slot");
    REQUIRE((newest->MarkCopySubmitted(1) && !newest->AwaitReuse(0ms)));
    UNSCOPED_INFO("copy retirement releases a slot");
    REQUIRE((newest->Retire() && newest->AwaitReuse(0ms)));
}

void closeCapsOutstandingLeases()
{
    OfferGate  gate;
    const auto offer = [&]
    {
        std::lock_guard lock(gate.mutex);
        return gate.offerLocked({});
    };
    auto claimed = offer();
    UNSCOPED_INFO("claim before close");
    REQUIRE(gate.claimNewest() == claimed);
    auto submitted = offer();
    UNSCOPED_INFO("submit before close");
    REQUIRE((gate.claimNewest() == submitted && submitted->MarkCopySubmitted(2)));
    auto              queued = offer();
    std::atomic<bool> result{ true };
    std::thread       producer([&]
                               {
                             result = claimed->AwaitRetirement(5s);
                               });
    auto              deadline = OperationDeadline::After(30ms);
    const auto        start    = OperationDeadline::Clock::now();
    gate.close(deadline);
    producer.join();
    UNSCOPED_INFO("one close deadline bounds claimed and submitted producer waits");
    REQUIRE((!result && !submitted->AwaitRetirement(5s)));
    UNSCOPED_INFO("close does not spend five seconds per lease");
    REQUIRE(OperationDeadline::Clock::now() - start < 1s);
    UNSCOPED_INFO("slot polling observes the same expiry");
    REQUIRE((claimed->DeadlineExpired() && submitted->DeadlineExpired()));
    UNSCOPED_INFO("close cancels queued offers and rejects late offers");
    REQUIRE((queued->State() == OutputLeaseState::Cancelled && !offer()));
    auto later = OperationDeadline::After(5s);
    gate.close(later);
    UNSCOPED_INFO("repeated close cannot extend the drain");
    REQUIRE(claimed->DeadlineExpired());
    {
        std::lock_guard lock(gate.mutex);
        gate.terminateLocked(OutputLeaseFailure::HostClosed);
    }
    UNSCOPED_INFO("termination reaches leases removed from the queue");
    REQUIRE((claimed->State() == OutputLeaseState::Failed && submitted->State() == OutputLeaseState::Failed));
}

void frameRateCountsNewFrames()
{
    PresentResult shown;
    shown.outcome  = PresentOutcome::Presented;
    shown.frameId  = 7;
    shown.newFrame = true;
    UNSCOPED_INFO("a first presentation of a new frame counts toward the frame rate");
    REQUIRE(ShowsNewFrame(shown));
    auto repaint     = shown;
    repaint.newFrame = false;
    repaint.repeat   = true;
    UNSCOPED_INFO("an overlay repaint of a shown frame does not count");
    REQUIRE(!ShowsNewFrame(repaint));
    auto idle    = shown;
    idle.frameId = 0;
    UNSCOPED_INFO("the idle canvas does not count");
    REQUIRE(!ShowsNewFrame(idle));
    auto occluded    = shown;
    occluded.outcome = PresentOutcome::Occluded;
    UNSCOPED_INFO("an occluded present does not count");
    REQUIRE(!ShowsNewFrame(occluded));
    // The ledger decides repeat: a frame first presented by a repaint after
    // its own present was occluded is not a repeat, and counts once.
    PresentationLedger ledger;
    UNSCOPED_INFO("the occluded first present is no repeat");
    REQUIRE(!ledger.OnPresent(7, HostStatus::Occluded));
    auto late     = shown;
    late.newFrame = false;
    late.repeat   = ledger.OnPresent(7, HostStatus::Ok);
    UNSCOPED_INFO("a repaint that first shows an occluded frame counts");
    REQUIRE(ShowsNewFrame(late));
    auto again   = late;
    again.repeat = ledger.OnPresent(7, HostStatus::Ok);
    UNSCOPED_INFO("a later repaint of that frame does not count");
    REQUIRE(!ShowsNewFrame(again));
}

void telemetryRules()
{
    PresentationLedger ledger;
    ledger.OnOffered(1, 1, 1);
    ledger.OnOffered(2, 1, 1);
    ledger.OnOffered(3, 1, 2);
    UNSCOPED_INFO("offers record frame, surface and gamma identity");
    REQUIRE((ledger.Counters().offered == 3 && ledger.Counters().surfaceGeneration == 1 && ledger.Counters().gammaGeneration == 2));
    ledger.OnDropped(false);
    ledger.OnClaimed();
    ledger.OnCopySubmitted();
    ledger.OnCopyRetired(3);
    ledger.OnHostSubmitted();
    UNSCOPED_INFO("a first successful present is not a repeat");
    REQUIRE(!ledger.OnPresent(3, HostStatus::Ok));
    UNSCOPED_INFO("a distinct presentation is counted");
    REQUIRE((ledger.Counters().presentedFrames == 1 && ledger.Counters().lastPresentedFrameId == 3));
    UNSCOPED_INFO("presenting the same frame again repeats");
    REQUIRE(ledger.OnPresent(3, HostStatus::Ok));
    UNSCOPED_INFO("a repeat is not a distinct presentation");
    REQUIRE((ledger.Counters().presentedFrames == 1 && ledger.Counters().repeats == 1));
    UNSCOPED_INFO("occlusion is not a presentation");
    REQUIRE((!ledger.OnPresent(3, HostStatus::Occluded) && ledger.Counters().occluded == 1 && ledger.Counters().presentedFrames == 1));

    ledger.OnOffered(4, 1, 2);
    ledger.OnCopyRetired(4);
    UNSCOPED_INFO("an occluded new frame is not presented");
    REQUIRE((!ledger.OnPresent(4, HostStatus::Occluded) && ledger.Counters().presentedFrames == 1));
    UNSCOPED_INFO("the first successful present of a frame counts even after occlusion");
    REQUIRE((!ledger.OnPresent(4, HostStatus::Ok) && ledger.Counters().presentedFrames == 2 && ledger.Counters().repeats == 1));
    ledger.OnPresent(4, HostStatus::OutOfDate);
    ledger.OnPresent(4, HostStatus::Failed);
    ledger.OnPresent(4, HostStatus::DeviceLost);
    UNSCOPED_INFO("out-of-date and failed presents are counted apart");
    REQUIRE((ledger.Counters().outOfDate == 1 && ledger.Counters().presentFailed == 2 && ledger.Counters().presentedFrames == 2));

    ledger.OnDropped(true);
    ledger.OnRejected();
    ledger.OnStale();
    ledger.OnCopyFailed();
    ledger.OnSuspendedPump();
    ledger.OnResize(true);
    ledger.OnResize(false);
    ledger.SetPresentInterval(0);
    const auto& counters = ledger.Counters();
    UNSCOPED_INFO("offer outcomes are counted apart");
    REQUIRE((counters.offered == 4 && counters.dropped == 1 && counters.droppedWhileSuspended == 1 && counters.rejected == 1 && counters.stale == 1));
    UNSCOPED_INFO("copy and host submissions are counted apart");
    REQUIRE((counters.claimed == 1 && counters.copySubmitted == 1 && counters.copyRetired == 2 && counters.copyFailed == 1 && counters.hostSubmissions == 1));
    UNSCOPED_INFO("suspension, resize drains and pacing are recorded");
    REQUIRE((counters.suspendedPumps == 1 && counters.resizes == 2 && counters.resizeDrains == 1 && counters.presentInterval == 0));
    UNSCOPED_INFO("frame identities track the newest frame");
    REQUIRE((counters.lastOfferedFrameId == 4 && counters.lastRetiredFrameId == 4 && counters.lastPresentedFrameId == 4));
    UNSCOPED_INFO("completed frames and presented frames are separate counts");
    REQUIRE(counters.offered > counters.presentedFrames);
}

} // namespace

TEST_CASE("native_presentation", "[ctest][release-smoke][native-presentation]")
{
    offerGatePolicy();
    closeCapsOutstandingLeases();
    leaseHappyPath();
    cancelBeforeClaim();
    cancelAfterClaimBeforeSubmit();
    claimThenDeviceLost();
    doubleRetireRejected();
    sentinelsAndReasons();
    transitionMatrix();
    waitsAreBounded();
    producerConsumerHandoff();
    deadlineAccounting();
    surfaceStateModel();
    letterboxMath();
    colorMath();
    telemetryRules();
    frameRateCountsNewFrames();
}
