#include "gpu/completion/completion_ledger.h"
#include "gpu/d3d12/native_completion_source.h"

#include <atomic>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <format>
#include <future>
#include <latch>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace rerevved::gpu;
using namespace rerevved::gpu::guest_d3d;
using namespace std::chrono_literals;

namespace
{

constexpr std::uint32_t kPanelCaller = 0x82303E40;
constexpr std::uint32_t kBlock       = 0x1FC9C000;
constexpr std::uint32_t kWriteback   = kBlock + completion::kReadPointerOffset;

// A minimal valid draw on the assembled target; vertexConstants[0] tags it.
NativeDrawReplayRecipe makeDraw(std::uint8_t tag)
{
    NativeDrawReplayRecipe draw;
    draw.schemaVersion    = 2;
    draw.width            = 1280;
    draw.height           = 720;
    draw.sampleCount      = 4;
    draw.sampleMask       = 9;
    draw.vertexShaderHash = 1;
    draw.pixelShaderHash  = 2;
    draw.vertexShaderDxil.resize(4);
    draw.pixelShaderDxil.resize(4);
    draw.vertexAttributeCount = 2;
    draw.vertexStrideBytes    = 32;
    draw.vertexData.resize(3 * 32);
    draw.indices    = { 0, 1, 2 };
    draw.indexCount = 3;
    draw.vertexConstants.resize(256 * 16);
    draw.pixelConstants.resize(224 * 16);
    draw.sharedConstants.resize(336);
    draw.viewport           = { 0, 0, 1280, 720, 0, 1 };
    draw.scissor            = { 0, 0, 1280, 720 };
    draw.clearColor         = { 0, 0, 0, 0 };
    draw.vertexConstants[0] = tag;
    return draw;
}

NativeDrawSubmission submission(std::uint64_t sequence, std::uint32_t tag)
{
    NativeDrawSubmission value;
    value.hook      = NativeDrawHook::IndexedUp;
    value.caller    = kPanelCaller;
    value.stack     = 0x7000;
    value.arguments = { 0x1000, 4, 0, 3, 3, 0x3000, 1, tag };
    value.stateRead = true;
    value.sequence  = sequence;
    return value;
}

bool panelBuilder(const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string&)
{
    draw = makeDraw(static_cast<std::uint8_t>(value.arguments[7]));
    return true;
}

void submit(NativeFrameAssembler& assembler, const NativeDrawSubmission& value)
{
    assembler.Began(value);
    assembler.Recorded(value, assembler.Admit(value));
}

NativeFrameSwap makeSwap(std::uint64_t sequence, std::uint64_t numbered)
{
    NativeFrameSwap swap;
    swap.sequence            = sequence;
    swap.numberedThrough     = numbered;
    swap.frontbufferPhysical = 0x1F000000;
    swap.fetch               = { 2, 0x1F000000 | 6, 1279 | (719U << 13), 0xD10, 0, 0 };
    swap.textureFormat       = 6;
    swap.width               = 1280;
    swap.height              = 720;
    swap.gammaAssumed        = true;
    return swap;
}

// A modeled untiled scanout of the presented target to makeSwap's
// frontbuffer in the title's 1280x720 view with its k01 control; each
// presenting swap needs one in its own epoch.
void scanOut(NativeFrameAssembler& assembler)
{
    GuestTarget main;
    main.native = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    GuestResolveCopy copy;
    copy.copyControl = 0x00100040;
    copy.destBase    = 0x1F000000;
    copy.destPitch   = 0x02D00500;
    copy.destInfo    = 0x300;
    copy.rectWidth   = 1280;
    copy.rectHeight  = 720;
    std::string error;
    bool        partial = false;
    UNSCOPED_INFO("the presented target is scanned out: " + error);
    REQUIRE(assembler.Resolve(main, copy, true, partial, error) != 0);
}

std::vector<std::uint8_t> passTags(const NativeRenderFramePass& pass)
{
    std::vector<std::uint8_t> tags;
    for (std::size_t i = 0; i < pass.DrawCount(); ++i)
        tags.push_back(pass.Draw(i).vertexConstants[0]);
    return tags;
}

// The renderer stream as the source sees it: each queued request settles only
// when the test says so, and the fence completes only when the test moves it.
struct FakeProducer final : INativeFrameProducer
{
    struct Queued
    {
        NativeRenderFrame                                     frame;
        bool                                                  prefix = false;
        std::shared_ptr<std::promise<NativeDrawReplayResult>> promise;
    };

    std::mutex                 mutex;
    std::vector<Queued>        queued;
    std::atomic<std::uint64_t> fence{ 0 };
    std::string                refuse;
    // Holds QueueFrame until released, as admission does while output slots are held.
    std::latch*           entered = nullptr;
    std::latch*           release = nullptr;
    std::function<void()> pollFence;

    bool queue(NativeRenderFrame frame, bool prefix, std::shared_future<NativeDrawReplayResult>& future, std::string& error)
    {
        if (entered)
            entered->count_down();
        if (release)
            release->wait();
        std::lock_guard lock(mutex);
        if (!refuse.empty())
        {
            error = refuse;
            return false;
        }
        auto promise = std::make_shared<std::promise<NativeDrawReplayResult>>();
        future       = promise->get_future().share();
        queued.push_back({ std::move(frame), prefix, std::move(promise) });
        return true;
    }

    bool QueueFrame(NativeRenderFrame frame, std::shared_future<NativeDrawReplayResult>& completion, std::string& error) override
    {
        return queue(std::move(frame), false, completion, error);
    }

    bool QueuePasses(NativeRenderFrame passes, std::shared_future<NativeDrawReplayResult>& recording, std::string& error) override
    {
        return queue(std::move(passes), true, recording, error);
    }

    std::uint64_t CompletedFence() const override
    {
        if (pollFence)
            pollFence();
        return fence.load();
    }

    std::size_t Count()
    {
        std::lock_guard lock(mutex);
        return queued.size();
    }

    // Prefixes settle at recording with their covering value; terminals at completion.
    void Settle(std::size_t index, bool success = true, std::uint64_t value = 0, std::string error = {}, std::uint64_t token = 0, std::vector<std::uint8_t> pixels = {})
    {
        NativeDrawReplayResult result;
        result.success              = success;
        result.completionFenceValue = value;
        result.error                = std::move(error);
        result.presenterToken       = token;
        result.resolvedOutput       = std::move(pixels);
        std::lock_guard lock(mutex);
        queued.at(index).promise->set_value(std::move(result));
    }
};

struct Failures
{
    std::mutex               mutex;
    std::vector<std::string> details;

    std::function<void(const std::string&)> Callback()
    {
        return [this](const std::string& detail)
        {
            std::lock_guard lock(mutex);
            details.push_back(detail);
        };
    }

    std::vector<std::string> Details()
    {
        std::lock_guard lock(mutex);
        return details;
    }
};

NativeFrameAssemblerConfig panelConfig(INativeCompletionSource* source)
{
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = source;
    return config;
}

struct Rig
{
    std::shared_ptr<FakeProducer> producer = std::make_shared<FakeProducer>();
    Failures                      failures;
    D3D12CompletionSource         source{ D3D12CompletionSourceConfig{ producer, failures.Callback() } };
    NativeFrameAssembler          assembler{ panelConfig(&source) };
};

// Values increase strictly; an empty span completes at its predecessor.
void testMonotonicAndEmpty()
{
    Rig        rig;
    const auto empty = rig.assembler.Swap(makeSwap(1, 0));
    UNSCOPED_INFO("an empty epoch with nothing pending completes at once and queues nothing");
    REQUIRE((empty.status == NativeFrameStatus::Empty && empty.hostFence == 1 && rig.source.Completed() == 1 && rig.producer->Count() == 0));
    submit(rig.assembler, submission(1, 1));
    scanOut(rig.assembler);
    const auto frame = rig.assembler.Swap(makeSwap(2, 1));
    const auto after = rig.assembler.Swap(makeSwap(3, 1));
    UNSCOPED_INFO("values increase across span kinds");
    REQUIRE((frame.hostFence == 2 && after.status == NativeFrameStatus::Empty && after.hostFence == 3));
    UNSCOPED_INFO("an empty span waits for the frame before it");
    REQUIRE(rig.source.Completed() == 1);
    rig.producer->Settle(0);
    UNSCOPED_INFO("the empty span completes with its predecessor");
    REQUIRE((rig.source.Completed() == 3 && rig.source.WaitFor(3, 0ms)));
    UNSCOPED_INFO("nothing failed");
    REQUIRE((rig.failures.Details().empty() && !rig.assembler.Failure()));
}

// Only final spans carry output; completion follows prefix then terminal frame.
void testSegmentThenFinal()
{
    Rig rig;
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
        submit(rig.assembler, submission(sequence, static_cast<std::uint32_t>(sequence)));
    const auto flush = rig.assembler.FlushThrough(2);
    UNSCOPED_INFO("the flush queues one prefix: " + flush.error);
    REQUIRE((flush.fence == 1 && rig.producer->Count() == 1));
    scanOut(rig.assembler);
    const auto result = rig.assembler.Swap(makeSwap(1, 3));
    UNSCOPED_INFO("the final span closes the epoch and carries the gamma assumption");
    REQUIRE((result.status == NativeFrameStatus::Provisional && result.hostFence == 2 && result.assumptions.gammaAssumed));
    UNSCOPED_INFO("segment then final");
    REQUIRE(rig.producer->Count() == 2);
    {
        std::lock_guard lock(rig.producer->mutex);
        const auto&     prefix   = rig.producer->queued[0];
        const auto&     terminal = rig.producer->queued[1];
        UNSCOPED_INFO("the segment is a prefix packet without output");
        REQUIRE((prefix.prefix && prefix.frame.passes.size() == 1 && passTags(prefix.frame.passes[0]) == std::vector<std::uint8_t>{ 1, 2 } &&
                 !prefix.frame.returnGpuOutput && !prefix.frame.outputTextureId));
        UNSCOPED_INFO("the terminal carries only its own pass and continues the recorded prefix's target");
        REQUIRE((!terminal.prefix && terminal.frame.passes.size() == 1 && terminal.frame.continuedPrefixPasses == 1 &&
                 passTags(terminal.frame.passes[0]) == std::vector<std::uint8_t>{ 3 } && terminal.frame.passes[0].preservePreviousAttachments &&
                 terminal.frame.passes[0].resolves.size() == 1));
        UNSCOPED_INFO("only the final span publishes");
        REQUIRE((terminal.frame.returnGpuOutput && terminal.frame.outputTextureId));
        auto        joined = terminal.frame;
        auto        passes = prefix.frame.passes;
        std::string error;
        UNSCOPED_INFO("the tail joined to the recorded prefix validates: " + error);
        REQUIRE((JoinNativeRenderFrameTail(joined, std::move(passes), prefix.frame.textures, prefix.frame.typedGuestWords, error) &&
                 ValidateNativeRenderFrame(joined, error)));
    }
    UNSCOPED_INFO("nothing completes before the prefix is recorded");
    REQUIRE(rig.source.Completed() == 0);
    rig.producer->Settle(0, true, 5);
    rig.producer->fence = 4;
    UNSCOPED_INFO("a recorded prefix waits for its fence value");
    REQUIRE(rig.source.Completed() == 0);
    rig.producer->fence = 5;
    UNSCOPED_INFO("the prefix completes at its fence value");
    REQUIRE(rig.source.Completed() == 1);
    rig.producer->fence = 9;
    UNSCOPED_INFO("the terminal frame completes only when its completion settles");
    REQUIRE(rig.source.Completed() == 1);
    rig.producer->Settle(1);
    UNSCOPED_INFO("the final span completes");
    REQUIRE(rig.source.Completed() == 2);

    // A segment cut does not wait for the previous terminal, and completion
    // stays in submission order. The scanouts took marks 4 and 6, so the
    // draws take marks 5, 7 and 8.
    submit(rig.assembler, submission(5, 4));
    scanOut(rig.assembler);
    rig.assembler.Swap(makeSwap(2, 4));
    submit(rig.assembler, submission(6, 5));
    submit(rig.assembler, submission(7, 6));
    const auto cut = rig.assembler.FlushThrough(7);
    UNSCOPED_INFO("the segment queues while the previous terminal is pending: " + cut.error);
    REQUIRE((cut.fence == 4 && rig.producer->Count() == 4 && rig.producer->queued[3].prefix &&
             passTags(rig.producer->queued[3].frame.passes[0]) == std::vector<std::uint8_t>{ 5 }));
    rig.producer->Settle(3, true, 12);
    rig.producer->fence = 12;
    UNSCOPED_INFO("a reached segment waits for the terminal before it");
    REQUIRE(rig.source.Completed() == 2);
    rig.producer->Settle(2);
    UNSCOPED_INFO("the terminal and then the segment complete");
    REQUIRE(rig.source.Completed() == 4);
    UNSCOPED_INFO("nothing failed");
    REQUIRE((rig.failures.Details().empty() && !rig.assembler.Failure()));
}

void testSegmentAfterPendingTerminal()
{
    Rig rig;
    submit(rig.assembler, submission(1, 1));
    scanOut(rig.assembler);
    rig.assembler.Swap(makeSwap(1, 1));
    // The scanout took mark 2.
    submit(rig.assembler, submission(3, 2));
    const auto cut = rig.assembler.FlushThrough(3);
    UNSCOPED_INFO("a pending terminal does not hold the segment: " + cut.error);
    REQUIRE((cut.fence == 2 && rig.producer->Count() == 2 && rig.producer->queued[1].prefix));
    scanOut(rig.assembler);
    const auto swap = rig.assembler.Swap(makeSwap(2, 2));
    UNSCOPED_INFO("the final swap continues the segment without waiting for the earlier terminal");
    REQUIRE((swap.hostFence == 3 && swap.status == NativeFrameStatus::Provisional && rig.producer->Count() == 3 &&
             !rig.producer->queued[2].prefix && rig.producer->queued[2].frame.passes.size() == 1 &&
             rig.producer->queued[2].frame.continuedPrefixPasses == 1 && passTags(rig.producer->queued[1].frame.passes[0]) == std::vector<std::uint8_t>{ 2 } &&
             rig.producer->queued[2].frame.passes[0].preservePreviousAttachments));
}

NativeCompletionSpan segmentSpan(std::uint64_t epoch, NativeRenderFramePass::Target target, bool preserve)
{
    NativeCompletionSpan span;
    span.epoch   = epoch;
    span.segment = true;
    NativeRenderFramePass pass;
    pass.target                      = target;
    pass.preservePreviousAttachments = preserve;
    span.frame.emplace();
    span.frame->passes.push_back(std::move(pass));
    return span;
}

// Preserve continues only the previous segment's exact target.
void testPreserveTarget()
{
    auto                                producer = std::make_shared<FakeProducer>();
    D3D12CompletionSource               source({ producer });
    const NativeRenderFramePass::Target full{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, half{ 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    std::string                         error;
    UNSCOPED_INFO("a first segment cannot preserve");
    REQUIRE((!source.Submit(segmentSpan(1, full, true), error) && error == "preserve_target_mismatch" && !producer->Count()));
    UNSCOPED_INFO("a fresh segment queues");
    REQUIRE(source.Submit(segmentSpan(1, full, false), error) == 1);
    UNSCOPED_INFO("a preserve onto another target is rejected before the renderer sees it");
    REQUIRE((!source.Submit(segmentSpan(1, half, true), error) && error == "preserve_target_mismatch" && producer->Count() == 1));
    UNSCOPED_INFO("the same target continues");
    REQUIRE((source.Submit(segmentSpan(1, full, true), error) == 2 && producer->Count() == 2));
    UNSCOPED_INFO("a new target starts fresh");
    REQUIRE(source.Submit(segmentSpan(1, half, false), error) == 3);
}

// census-0009 checks typed views and terminal ownership.
void testSegmentTypedViews()
{
    auto                                producer = std::make_shared<FakeProducer>();
    D3D12CompletionSource               source({ producer });
    const NativeRenderFramePass::Target full{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    std::string                         error;
    auto                                typed = segmentSpan(1, full, false);
    typed.frame->typedGuestWords              = true;
    UNSCOPED_INFO("a typed segment queues: " + error);
    REQUIRE(source.Submit(std::move(typed), error) == 1);
    UNSCOPED_INFO("an untyped segment continues it: " + error);
    REQUIRE(source.Submit(segmentSpan(1, full, true), error) == 2);
    auto terminal    = segmentSpan(1, full, true);
    terminal.segment = false;
    UNSCOPED_INFO("the tail continues both segments, and the typed segment carries its own flag: " + error);
    REQUIRE((source.Submit(std::move(terminal), error) == 3 && producer->Count() == 3 && !producer->queued[2].prefix &&
             producer->queued[2].frame.passes.size() == 1 && producer->queued[2].frame.continuedPrefixPasses == 2 &&
             producer->queued[0].frame.typedGuestWords && !producer->queued[2].frame.typedGuestWords));
    UNSCOPED_INFO("the next epoch's segment queues: " + error);
    REQUIRE(source.Submit(segmentSpan(2, full, false), error) == 4);
    auto plain    = segmentSpan(2, full, true);
    plain.segment = false;
    UNSCOPED_INFO("the flag does not outlive its epoch: " + error);
    REQUIRE((source.Submit(std::move(plain), error) == 5 && producer->Count() == 5 && !producer->queued[4].frame.typedGuestWords));
}

// Refused segments add no passes; later refusal preserves count and end target.
void testPreserveAfterRefusedSegment()
{
    auto                                producer = std::make_shared<FakeProducer>();
    D3D12CompletionSource               source({ producer });
    const NativeRenderFramePass::Target full{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, half{ 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    std::string                         error;
    producer->refuse = "fixture_refused";
    UNSCOPED_INFO("the renderer refuses the segment");
    REQUIRE((!source.Submit(segmentSpan(1, full, false), error) && error == "fixture_refused" && !producer->Count()));
    producer->refuse.clear();
    UNSCOPED_INFO("a preserve after the refused segment is rejected: " + error);
    REQUIRE((!source.Submit(segmentSpan(1, full, true), error) && error == "preserve_target_mismatch" && !producer->Count()));
    auto terminal    = segmentSpan(1, full, true);
    terminal.segment = false;
    UNSCOPED_INFO("a preserving terminal after it is rejected too: " + error);
    REQUIRE((!source.Submit(std::move(terminal), error) && error == "preserve_target_mismatch" && !producer->Count()));

    auto two                                = segmentSpan(1, full, false);
    two.frame->passes.emplace_back().target = half;
    UNSCOPED_INFO("a two-pass segment queues: " + error);
    REQUIRE((source.Submit(std::move(two), error) == 1 && producer->Count() == 1));
    producer->refuse = "fixture_refused";
    UNSCOPED_INFO("the renderer refuses a later segment");
    REQUIRE((!source.Submit(segmentSpan(1, full, false), error) && error == "fixture_refused" && producer->Count() == 1));
    producer->refuse.clear();
    UNSCOPED_INFO("the refused segment's target is not the end target: " + error);
    REQUIRE((!source.Submit(segmentSpan(1, full, true), error) && error == "preserve_target_mismatch" && producer->Count() == 1));
    auto tail    = segmentSpan(1, half, true);
    tail.segment = false;
    UNSCOPED_INFO("the tail continues the recorded segment's end target and both its passes: " + error);
    REQUIRE((source.Submit(std::move(tail), error) == 2 && producer->Count() == 2 && producer->queued[1].frame.continuedPrefixPasses == 2));
}

// Recorded segments still hold their epoch open until a final span joins them.
void testSegmentsNeedFinalSpan()
{
    auto                                producer = std::make_shared<FakeProducer>();
    D3D12CompletionSource               source({ producer });
    const NativeRenderFramePass::Target full{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    std::string                         error;
    UNSCOPED_INFO("the segment queues: " + error);
    REQUIRE(source.Submit(segmentSpan(1, full, false), error) == 1);
    UNSCOPED_INFO("an epoch change after a segment is refused: " + error);
    REQUIRE((!source.Submit(segmentSpan(2, full, false), error) && error == "segments_without_final_span" && producer->Count() == 1));
    NativeCompletionSpan empty;
    empty.epoch = 1;
    UNSCOPED_INFO("a final span without a frame cannot drop the segment: " + error);
    REQUIRE((!source.Submit(std::move(empty), error) && error == "final_span_drops_segments" && producer->Count() == 1));
}

// Close refuses later work; the held span remains incomplete.
void testClose()
{
    Rig rig;
    submit(rig.assembler, submission(1, 1));
    UNSCOPED_INFO("a segment is in flight");
    REQUIRE(rig.assembler.FlushThrough(1).fence == 1);
    rig.source.Close();
    rig.producer->Settle(0, true, 3);
    rig.producer->fence = 3;
    UNSCOPED_INFO("after close nothing completes, even finished work");
    REQUIRE(rig.source.Completed() == 0);
    submit(rig.assembler, submission(2, 2));
    scanOut(rig.assembler);
    const auto stopped = rig.assembler.Swap(makeSwap(1, 2));
    UNSCOPED_INFO("the swap after close stops without a failure or new work");
    REQUIRE((stopped.status == NativeFrameStatus::Stopped && stopped.detail == kNativeCompletionClosing && !rig.assembler.Failure() &&
             rig.assembler.Stopped() && rig.producer->Count() == 1));
    submit(rig.assembler, submission(4, 3));
    const auto held = rig.assembler.FlushThrough(4);
    UNSCOPED_INFO("a flush after close keeps its span held");
    REQUIRE((!held.fence && held.retry && held.error == kNativeCompletionClosing));
    UNSCOPED_INFO("every later swap stays stopped");
    REQUIRE((rig.assembler.Swap(makeSwap(2, 3)).status == NativeFrameStatus::Stopped && !rig.assembler.Failure()));
    std::string error;
    UNSCOPED_INFO("Submit names the close");
    REQUIRE((!rig.source.Submit({}, error) && error == kNativeCompletionClosing));
    UNSCOPED_INFO("closing is not a failure");
    REQUIRE(rig.failures.Details().empty());

    // Close while Submit waits inside the producer: the stopped stream's error
    // becomes the close.
    Rig        waiting;
    std::latch entered(1), release(1);
    waiting.producer->entered = &entered;
    waiting.producer->release = &release;
    waiting.producer->refuse  = "native D3D12 draw stream is unavailable";
    submit(waiting.assembler, submission(1, 1));
    scanOut(waiting.assembler);
    NativeAssembledFrame result;
    std::thread          swapper([&]
                                 {
                            result = waiting.assembler.Swap(makeSwap(1, 1));
                                 });
    entered.wait();
    waiting.source.Close();
    release.count_down();
    swapper.join();
    UNSCOPED_INFO("a submission cut by the close stops: " + result.detail);
    REQUIRE((result.status == NativeFrameStatus::Stopped && !waiting.assembler.Failure()));
}

void testCloseDuringPoll()
{
    Rig rig;
    submit(rig.assembler, submission(1, 1));
    scanOut(rig.assembler);
    rig.assembler.Swap(makeSwap(1, 1));
    rig.producer->Settle(0);
    std::latch entered(1), release(1), closing(1);
    rig.producer->pollFence = [&]
    {
        entered.count_down();
        release.wait();
    };
    auto poll = std::async(std::launch::async, [&]
                           {
                               return rig.source.Completed();
                           });
    entered.wait();
    auto close = std::async(std::launch::async, [&]
                            {
                                closing.count_down();
                                rig.source.Close();
                            });
    closing.wait();
    const bool serialized = close.wait_for(20ms) == std::future_status::timeout;
    release.count_down();
    close.get();
    const auto completed = rig.source.Completed();
    UNSCOPED_INFO("Close waits for an in-flight poll and freezes its final observed value");
    REQUIRE((serialized && poll.get() == completed && completed == 1));
    rig.producer->pollFence = []
    {
        UNSCOPED_INFO("a closed source never polls the producer");
        REQUIRE(false);
    };
    UNSCOPED_INFO("completion remains frozen after Close returns");
    REQUIRE(rig.source.Completed() == completed);
}

void testBoundedOutputReadback()
{
    auto                         producer = std::make_shared<FakeProducer>();
    std::vector<std::uint64_t>   observed;
    std::function<void()>        checkUnlocked;
    D3D12CompletionSource        source({ producer, {}, [&](const NativeDrawReplayResult& result)
                                          {
                                       checkUnlocked();
                                       observed.push_back(result.presenterToken);
                                       UNSCOPED_INFO("published gamma bytes reach the diagnostic");
                                       REQUIRE(result.resolvedOutput == std::vector<std::uint8_t>{ 0, 0, 0, 0xC0 });
                                       if (result.presenterToken == 1)
                                           throw std::runtime_error("diagnostic failure");
                                          } });
    NativeFrameAssembler         assembler(panelConfig(&source));
    completion::CompletionLedger ledger({ NativeDrawMode::Skip, nullptr, &assembler });
    ledger.Resolve(0x8250AFEC, 0x1E3B0000);
    ledger.FinishResolve(1, true, false);
    ledger.Kick(0x16);
    ledger.Swap(0x1F6F8000);
    checkUnlocked = [&]
    {
        UNSCOPED_INFO("diagnostics can reenter the ledger after polling");
        REQUIRE(ledger.Snapshot().hostCompleted == 3);
        source.Close();
    };
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
    {
        scanOut(assembler);
        submit(assembler, submission(sequence, 1));
        assembler.Swap(makeSwap(sequence, sequence));
        const auto& frame = producer->queued.back().frame;
        UNSCOPED_INFO("only the first two outputs request readback");
        REQUIRE((frame.returnGpuOutput && frame.returnResolvedOutput == (sequence <= 2)));
        producer->Settle(sequence - 1, true, sequence, {}, sequence, { 0, 0, 0, 0xC0 });
        const auto debt = ledger.Snapshot();
        UNSCOPED_INFO("debt snapshots poll live completion even when an unowned resolve has no host target");
        REQUIRE((debt.hostCompleted == sequence && debt.hostTarget == 0 && debt.unowned.starts_with("frontbuffer_resolve_mismatch")));
        UNSCOPED_INFO("polling under the ledger lock only collects readbacks");
        REQUIRE(observed.empty());
    }
    UNSCOPED_INFO("readback outputs complete before diagnostics run");
    REQUIRE(source.Completed() == 3);
    source.ObserveCompletedOutputs();
    source.ObserveCompletedOutputs();
    UNSCOPED_INFO("only two published outputs are inspected, once each");
    REQUIRE(observed == std::vector<std::uint64_t>{ 1, 2 });
    UNSCOPED_INFO("a diagnostic exception does not fail completed work or suppress the next output");
    REQUIRE(!source.Failure());
}

void testDrawVisibility()
{
    auto       draw  = makeDraw(1);
    const auto store = [](std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
    {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned c = 0; c < 4; ++c)
            bytes[offset + c] = static_cast<std::uint8_t>(bits >> (8 * c));
    };
    store(draw.vertexData, 0, -10);
    store(draw.vertexData, 32, 20);
    store(draw.vertexData, 68, 30);
    store(draw.pixelConstants, 12, 0.5F);
    store(draw.pixelConstants, 7 * 16, 1);
    const auto detail = DescribeNativeDrawVisibility(submission(1, 1), draw);
    UNSCOPED_INFO("visibility reports decoded bounds, viewport, primitives and pixel constants including alpha");
    REQUIRE((detail.find("indexed_up from 0x82303e40 seq=1 viewport=[0,0,1280,720,0,1]") != std::string::npos &&
             detail.find("raw_position_min=[-10,0,0,0] raw_position_max=[20,30,0,0] primitives=1") != std::string::npos &&
             detail.find("ps_constants=[c0=[0,0,0,0.5] c7=[1,0,0,0]] ps_omitted=zero") != std::string::npos));

    auto config = panelConfig(nullptr);
    config.mode = NativeDrawMode::Skip;
    std::vector<std::pair<unsigned, std::uint64_t>> observed;
    config.observeDraw = [&](unsigned ordinal, const NativeDrawSubmission& value, const NativeDrawReplayRecipe& recipe)
    {
        UNSCOPED_INFO("diagnostic sees the admitted recipe before it is submitted");
        REQUIRE(recipe.indexCount == 3);
        observed.emplace_back(ordinal, value.sequence);
    };
    NativeFrameAssembler assembler(std::move(config));
    for (std::uint64_t sequence = 1; sequence <= 6; ++sequence)
    {
        if (sequence == 1 || sequence == 4)
            scanOut(assembler);
        const auto value = submission(sequence, 1);
        assembler.Began(value);
        assembler.Recorded(value, sequence == 2 ? std::optional{ NativeDrawReason::NoNativeRoute } : assembler.Admit(value));
        if (sequence == 3 || sequence == 6)
            assembler.Swap(makeSwap(sequence / 3, sequence));
    }
    UNSCOPED_INFO("draw diagnostics count only admissions and stop at four across swap epochs");
    REQUIRE((observed == std::vector<std::pair<unsigned, std::uint64_t>>{ { 1, 1 }, { 2, 3 }, { 3, 4 }, { 4, 5 } }));
}

// A removed device or a failed frame stops Completed() and reports once.
void testDeviceFailure()
{
    Rig rig;
    submit(rig.assembler, submission(1, 1));
    scanOut(rig.assembler);
    rig.assembler.Swap(makeSwap(1, 1));
    rig.producer->fence = std::numeric_limits<std::uint64_t>::max();
    for (int poll = 0; poll < 3; ++poll)
    {
        UNSCOPED_INFO("a lost device never completes the frame");
        REQUIRE(rig.source.Completed() == 0);
    }
    rig.producer->Settle(0);
    UNSCOPED_INFO("completion stays stopped after the loss");
    REQUIRE((rig.source.Completed() == 0 && !rig.source.WaitFor(1, 10ms)));
    const auto details = rig.failures.Details();
    UNSCOPED_INFO("the loss is reported once");
    REQUIRE((details.size() == 1 && details[0] == "native_device_lost" && rig.source.Failure() == details[0]));
    std::string error;
    UNSCOPED_INFO("a failed source refuses new spans");
    REQUIRE((!rig.source.Submit({}, error) && error.starts_with("completion_failed")));

    Rig runtime;
    submit(runtime.assembler, submission(1, 1));
    scanOut(runtime.assembler);
    runtime.assembler.Swap(makeSwap(1, 1));
    runtime.producer->Settle(0, false, 0, "native terminal fence did not complete");
    runtime.source.Completed();
    runtime.source.Completed();
    const auto failed = runtime.failures.Details();
    UNSCOPED_INFO("a renderer runtime failure is reported once and never completes");
    REQUIRE((failed.size() == 1 && failed[0] == "native_frame_failed: native terminal fence did not complete" && runtime.source.Completed() == 0));
}

struct FakeSink final : completion::IGuestProgressSink
{
    std::mutex                                           mutex;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;

    bool WritePhysical(std::uint32_t address, std::uint32_t value) override
    {
        std::lock_guard lock(mutex);
        writes.emplace_back(address, value);
        return true;
    }

    std::optional<std::uint32_t> ReadPhysical(std::uint32_t) override
    {
        return 0;
    }

    completion::InterruptResult RaiseCallbackInterrupt(std::uint32_t) override
    {
        return completion::InterruptResult::Raised;
    }

    std::size_t Count()
    {
        std::lock_guard lock(mutex);
        return writes.size();
    }
};

// After close, a finished guest span writes nothing to guest memory.
void testShutdownWritesNothing()
{
    for (const bool close : { false, true })
    {
        Rig                          rig;
        FakeSink                     sink;
        std::vector<std::string>     failures;
        completion::CompletionLedger ledger(completion::LedgerConfig{ NativeDrawMode::Strict, &sink, &rig.assembler, [&](const completion::LedgerFailure& failure)
                                                                      {
                                                                          failures.push_back(failure.detail);
                                                                      } });
        ledger.BeginRingEpoch();
        ledger.RegisterWriteback(kWriteback);
        // The app's fan-out: assembler first, then the ledger.
        const auto draw = submission(1, 1);
        rig.assembler.Began(draw);
        ledger.BeginDraw(draw.hook, draw.caller);
        const auto verdict = rig.assembler.Admit(draw);
        rig.assembler.Recorded(draw, verdict);
        ledger.FinishDraw(draw.sequence, draw.hook, draw.caller, verdict);
        ledger.Fence(kBlock, 7, 0x40);
        ledger.Kick(0x20);
        UNSCOPED_INFO("the span waits for its segment's host fence");
        REQUIRE((!ledger.Pump() && rig.producer->Count() == 1 && sink.Count() == 0));
        if (close)
        {
            rig.source.Close();
            ledger.Close();
        }
        rig.producer->Settle(0, true, 2);
        rig.producer->fence = 2;
        ledger.Pump();
        ledger.Pump();
        if (close)
        {
            UNSCOPED_INFO("nothing is written after the source and ledger closed");
            REQUIRE((sink.Count() == 0 && rig.source.Completed() == 0));
        }
        else
        {
            UNSCOPED_INFO("without the close the finished segment releases B+4, B+0 and RPTR");
            REQUIRE(sink.Count() == 3);
        }
        UNSCOPED_INFO("no failure either way");
        REQUIRE((failures.empty() && rig.failures.Details().empty()));
    }
}

GuestResolveCopy rawCopy()
{
    GuestResolveCopy copy;
    copy.copyControl = 0x100000;
    copy.destBase    = 0x1E3B0000;
    copy.destPitch   = 0x00200020;
    copy.destInfo    = 0x01000300;
    copy.destFetch   = { 0x80400002, 0xFE3AF006, 31 | (31U << 13), 0xC14, 0, 0x200 };
    copy.rectWidth = copy.rectHeight = 32;
    return copy;
}

// A retain-only producer frame whose raw readback becomes the image at mark.
std::uint64_t submitRaw(D3D12CompletionSource& source, std::uint64_t epoch, std::uint64_t mark, const GuestResolveCopy& copy)
{
    NativeCompletionSpan span;
    span.epoch        = epoch;
    span.resolveMark  = mark;
    span.guestResolve = copy;
    span.frame.emplace();
    span.frame->retainOnly = span.frame->returnRawOutput = true;
    span.frame->outputTextureId                          = 1;
    span.frame->passes.emplace_back();
    std::string error;
    const auto  value = source.Submit(std::move(span), error);
    UNSCOPED_INFO("raw producer submission: " + error);
    REQUIRE(value != 0);
    return value;
}

void settleRaw(FakeProducer& producer, std::size_t index, std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(4096, 255))
{
    NativeDrawReplayResult result;
    result.success     = true;
    result.rawRowPitch = 128;
    result.rawOutput   = std::move(bytes);
    std::lock_guard lock(producer.mutex);
    producer.queued.at(index).promise->set_value(std::move(result));
}

// Fails a queued request as the renderer does: after `submitted`'s batch, or
// with no submitted fence for a refused admission.
void failQueued(FakeProducer& producer, std::size_t index, std::string error, std::uint64_t submitted)
{
    NativeDrawReplayResult result;
    result.error               = std::move(error);
    result.submittedFenceValue = submitted;
    std::lock_guard lock(producer.mutex);
    producer.queued.at(index).promise->set_value(std::move(result));
}

// Census refusal outside prefix completes; post-submit failure ends its epoch.
void testCensusRecovery()
{
    auto                  producer = std::make_shared<FakeProducer>();
    Failures              failures;
    D3D12CompletionSource source{ D3D12CompletionSourceConfig{ producer, failures.Callback(), {}, true } };
    auto                  config = panelConfig(&source);
    config.mode                  = NativeDrawMode::Census;
    NativeFrameAssembler assembler(config);

    // A frame refused at admission submitted nothing and completes at once.
    submit(assembler, submission(1, 1));
    scanOut(assembler);
    UNSCOPED_INFO("the first frame is queued");
    REQUIRE(assembler.Swap(makeSwap(1, 1)).hostFence == 1);
    failQueued(*producer, 0, "fixture_admission_refused", 0);
    UNSCOPED_INFO("a refused frame completes without failing the source");
    REQUIRE((source.Completed() == 1 && failures.Details().empty() && !source.Failure()));
    UNSCOPED_INFO("the failure is queued once for the census");
    REQUIRE((source.TakeSkippedFailure() == "native_frame_failed: fixture_admission_refused" && !source.TakeSkippedFailure()));

    // Each refusal is skipped; the second draw takes mark 3 after the first scanout.
    submit(assembler, submission(2, 2));
    scanOut(assembler);
    UNSCOPED_INFO("the second frame is queued");
    REQUIRE(assembler.Swap(makeSwap(2, 2)).hostFence == 2);
    failQueued(*producer, 1, "fixture_capacity_refused", 0);
    UNSCOPED_INFO("a second refused frame completes at once");
    REQUIRE((source.Completed() == 2 && source.TakeSkippedFailure() == "native_frame_failed: fixture_capacity_refused" && !source.Failure()));

    // Draws 3..5 (marks 5..7), a segment through mark 6, then a swap
    // without its scanout.
    for (std::uint64_t sequence = 3; sequence <= 5; ++sequence)
        submit(assembler, submission(sequence, static_cast<std::uint32_t>(sequence)));
    const auto flush = assembler.FlushThrough(6);
    UNSCOPED_INFO("the segment is queued: " + flush.error);
    REQUIRE((flush.fence == 3 && producer->Count() == 3));
    const auto failed = assembler.Swap(makeSwap(3, 5));
    UNSCOPED_INFO("the failed epoch ends its segment with a terminal frame: " + failed.detail);
    REQUIRE((failed.status == NativeFrameStatus::Failed && failed.detail.starts_with("swap_scanout_unmodeled") && producer->Count() == 4));
    {
        std::lock_guard lock(producer->mutex);
        const auto&     segment = producer->queued[2];
        const auto&     closing = producer->queued[3];
        auto            joined  = closing.frame;
        auto            passes  = segment.frame.passes;
        std::string     error;
        UNSCOPED_INFO("the closing frame continues the segment's target and joins it: " + error);
        REQUIRE((!closing.prefix && closing.frame.passes.size() == 1 && closing.frame.passes[0].preservePreviousAttachments &&
                 closing.frame.continuedPrefixPasses == 1 &&
                 JoinNativeRenderFrameTail(joined, std::move(passes), segment.frame.textures, segment.frame.typedGuestWords, error) &&
                 ValidateNativeRenderFrame(joined, error)));
    }
    producer->Settle(2, true, 7);
    producer->fence = 7;
    producer->Settle(3);
    UNSCOPED_INFO("the segment and its closing frame complete");
    REQUIRE(source.Completed() == 4);

    submit(assembler, submission(7, 7));
    scanOut(assembler);
    const auto later = assembler.Swap(makeSwap(4, 6));
    UNSCOPED_INFO("the next epoch presents: " + later.detail);
    REQUIRE((later.status == NativeFrameStatus::Provisional && later.hostFence == 5 && !assembler.Failure()));
    UNSCOPED_INFO("the source never failed");
    REQUIRE(failures.Details().empty());
}

// Prefix failures stop stream; Submit then refuses the terminal.
void testCensusChainFailure()
{
    {
        auto                  producer = std::make_shared<FakeProducer>();
        Failures              failures;
        D3D12CompletionSource source{ D3D12CompletionSourceConfig{ producer, failures.Callback(), {}, true } };
        auto                  config = panelConfig(&source);
        config.mode                  = NativeDrawMode::Census;
        NativeFrameAssembler assembler(config);
        submit(assembler, submission(1, 1));
        scanOut(assembler);
        UNSCOPED_INFO("the frame is queued");
        REQUIRE(assembler.Swap(makeSwap(1, 1)).hostFence == 1);
        failQueued(*producer, 0, "fixture_pso_failed", 7);
        producer->fence  = 7;
        const auto fatal = std::string("native_frame_failed: fixture_pso_failed");
        UNSCOPED_INFO("a frame that failed after submitting a batch fails the census source");
        REQUIRE((source.Completed() == 0 && source.Failure() == fatal && failures.Details() == std::vector<std::string>{ fatal } && !source.TakeSkippedFailure()));
    }
    for (const bool prefixFails : { true, false })
    {
        auto                  producer = std::make_shared<FakeProducer>();
        Failures              failures;
        D3D12CompletionSource source{ D3D12CompletionSourceConfig{ producer, failures.Callback(), {}, true } };
        auto                  config = panelConfig(&source);
        config.mode                  = NativeDrawMode::Census;
        NativeFrameAssembler assembler(config);
        submit(assembler, submission(1, 1));
        const auto flush = assembler.FlushThrough(1);
        UNSCOPED_INFO("the segment is queued: " + flush.error);
        REQUIRE((flush.fence == 1 && producer->Count() == 1 && producer->queued[0].prefix));
        std::string expected;
        if (prefixFails)
        {
            expected = "native_prefix_failed: fixture_admission_refused";
            failQueued(*producer, 0, "fixture_admission_refused", 0);
            UNSCOPED_INFO("a failed prefix fails the census source");
            REQUIRE((source.Completed() == 0 && source.Failure() == expected));
            scanOut(assembler);
            const auto closing = assembler.Swap(makeSwap(1, 1));
            UNSCOPED_INFO("Submit refuses the epoch's terminal: " + closing.detail);
            REQUIRE((closing.status == NativeFrameStatus::Failed && closing.detail.find("completion_failed: " + expected) != std::string::npos &&
                     producer->Count() == 1));
        }
        else
        {
            expected = "native_frame_failed: fixture_admission_refused";
            scanOut(assembler);
            UNSCOPED_INFO("the terminal closes the prefix");
            REQUIRE((assembler.Swap(makeSwap(1, 1)).hostFence == 2 && producer->Count() == 2 && !producer->queued[1].prefix));
            producer->Settle(0, true, 3);
            producer->fence = 3;
            failQueued(*producer, 1, "fixture_admission_refused", 0);
            UNSCOPED_INFO("a failed frame that closes a prefix fails the census source");
            REQUIRE((source.Completed() == 1 && source.Failure() == expected));
        }
        UNSCOPED_INFO("the failure is reported once and not skipped");
        REQUIRE((failures.Details() == std::vector<std::string>{ expected } && !source.TakeSkippedFailure()));
    }
}

void testRawResolveCompletion()
{
    auto                  producer = std::make_shared<FakeProducer>();
    D3D12CompletionSource source({ producer });
    const auto            copy = rawCopy();
    UNSCOPED_INFO("the raw producer span is the first value");
    REQUIRE(submitRaw(source, 1, 2, copy) == 1);
    const auto early = source.TakeGuestImage(2);
    UNSCOPED_INFO("image cannot arrive before terminal completion: " + early.error);
    REQUIRE((source.Completed() == 0 && !early.image && !early.closing && early.error.starts_with("guest_image_incomplete")));
    settleRaw(*producer, 0);
    UNSCOPED_INFO("raw producer completes");
    REQUIRE(source.Completed() == 1);
    const auto image = source.TakeGuestImage(2);
    UNSCOPED_INFO("completion encodes raw texels, independent of empty gamma output: " + image.error);
    REQUIRE((image.image && image.error.empty() && image.image->physical == copy.destBase && image.image->bytes == std::vector<std::uint8_t>(4096, 255)));
    const auto again = source.TakeGuestImage(2);
    UNSCOPED_INFO("guest image transfers once: " + again.error);
    REQUIRE((!again.image && again.error.starts_with("guest_image_missing")));
}

// Take after source close reports closing and leaves the span in the ledger.
void testClosingTake()
{
    auto                  producer = std::make_shared<FakeProducer>();
    D3D12CompletionSource source({ producer });
    submitRaw(source, 1, 2, rawCopy());
    settleRaw(*producer, 0);
    UNSCOPED_INFO("the readback completed before the close");
    REQUIRE(source.Completed() == 1);
    source.Close();
    const auto take = source.TakeGuestImage(2);
    UNSCOPED_INFO("a take after Close reports closing, not a readback failure: " + take.error);
    REQUIRE((take.closing && !take.image && take.error.empty()));
}

// Readback and encoder errors reach ledger resolution as named failures.
void testTakeErrors()
{
    auto                  producer = std::make_shared<FakeProducer>();
    D3D12CompletionSource source({ producer });
    auto                  unsupported = rawCopy();
    unsupported.copyControl |= 5U << 4;
    submitRaw(source, 1, 2, unsupported);
    submitRaw(source, 2, 4, rawCopy());
    settleRaw(*producer, 0);
    settleRaw(*producer, 1, {});
    UNSCOPED_INFO("both readbacks complete");
    REQUIRE(source.Completed() == 2);
    const auto encode = source.TakeGuestImage(2);
    UNSCOPED_INFO("the encoder's rejection is propagated: " + encode.error);
    REQUIRE((!encode.image && !encode.closing && encode.error.starts_with("guest_resolve_encode_failed: ") &&
             encode.error.find("resolve_sample_select_unsupported") != std::string::npos));
    const auto missing = source.TakeGuestImage(4);
    UNSCOPED_INFO("an empty raw readback is named: " + missing.error);
    REQUIRE((!missing.image && missing.error == "guest_resolve_readback_missing"));
}

// Untaken images remain available after swaps and guest waits.
void testUntakenImagesKept()
{
    auto                  producer = std::make_shared<FakeProducer>();
    D3D12CompletionSource source({ producer });
    constexpr auto        count = D3D12CompletionSource::kGuestImageAlarm + 4;
    for (std::uint64_t index = 0; index < count; ++index)
        submitRaw(source, index + 1, 10 * (index + 1), rawCopy());
    for (std::size_t index = 0; index < count; ++index)
        settleRaw(*producer, index);
    UNSCOPED_INFO("every readback completes and the alarm is not a failure");
    REQUIRE((source.Completed() == count && !source.Failure()));
    for (std::uint64_t index = 1; index < count; ++index)
    {
        const auto take = source.TakeGuestImage(10 * index);
        UNSCOPED_INFO(std::format("untaken image {} is still taken: {}", 10 * index, take.error));
        REQUIRE((take.image && take.error.empty()));
    }
    const auto last = source.TakeGuestImage(10 * count);
    UNSCOPED_INFO("the newest image is taken: " + last.error);
    REQUIRE(last.image.has_value());

    // A take still discards every older image: the ledger takes in op-mark order.
    submitRaw(source, count + 1, 1000, rawCopy());
    submitRaw(source, count + 2, 1010, rawCopy());
    settleRaw(*producer, count);
    settleRaw(*producer, count + 1);
    UNSCOPED_INFO("the later image is taken first");
    REQUIRE((source.Completed() == count + 2 && source.TakeGuestImage(1010).image.has_value()));
    const auto older = source.TakeGuestImage(1000);
    UNSCOPED_INFO("the take discarded every older image: " + older.error);
    REQUIRE((!older.image && older.error.starts_with("guest_image_discarded")));
}

// DiscardThrough drops images for dropped spans, including in-flight reads.
void testDiscardThrough()
{
    auto                  producer = std::make_shared<FakeProducer>();
    D3D12CompletionSource source({ producer });
    for (std::uint64_t mark : { 2, 4, 6, 8 })
        submitRaw(source, mark, mark, rawCopy());
    for (std::size_t index = 0; index < 3; ++index)
        settleRaw(*producer, index);
    UNSCOPED_INFO("three readbacks complete; mark 8 is in flight");
    REQUIRE(source.Completed() == 3);
    source.DiscardGuestImagesThrough(4);
    for (std::uint64_t mark : { 2, 4 })
    {
        const auto take = source.TakeGuestImage(mark);
        UNSCOPED_INFO("a discarded image fails as discarded: " + take.error);
        REQUIRE((!take.image && take.error.starts_with("guest_image_discarded")));
    }
    const auto kept = source.TakeGuestImage(6);
    UNSCOPED_INFO("an image above the discard point is kept: " + kept.error);
    REQUIRE((kept.image && kept.error.empty()));
    source.DiscardGuestImagesThrough(8);
    settleRaw(*producer, 3);
    UNSCOPED_INFO("the discarded readback still completes its value");
    REQUIRE(source.Completed() == 4);
    const auto late = source.TakeGuestImage(8);
    UNSCOPED_INFO("a readback discarded in flight is not kept: " + late.error);
    REQUIRE((!late.image && late.error.starts_with("guest_image_discarded")));
}

// Peek waits for in-flight readback and takes neither it nor older images.
void testPeekKeepsImages()
{
    auto                  producer = std::make_shared<FakeProducer>();
    D3D12CompletionSource source({ producer });
    submitRaw(source, 1, 2, rawCopy());
    submitRaw(source, 2, 4, rawCopy());
    const auto early = source.PeekGuestImage(4, std::chrono::milliseconds(0));
    UNSCOPED_INFO("an unfinished readback is not peeked: " + early.error);
    REQUIRE((!early.image && early.error == "guest_image_incomplete: op mark 4"));
    settleRaw(*producer, 0);
    settleRaw(*producer, 1, std::vector<std::uint8_t>(4096, 7));
    UNSCOPED_INFO("both readbacks complete");
    REQUIRE(source.Completed() == 2);
    const auto peek = source.PeekGuestImage(4, std::chrono::milliseconds(0));
    UNSCOPED_INFO("a finished readback is peeked: " + peek.error);
    REQUIRE((peek.image && peek.image->bytes == std::vector<std::uint8_t>(4096, 7)));
    UNSCOPED_INFO("the ledger still takes the older and the peeked image");
    REQUIRE((source.TakeGuestImage(2).image && source.TakeGuestImage(4).image));
    const auto taken = source.PeekGuestImage(4, std::chrono::milliseconds(0));
    UNSCOPED_INFO("a taken image is no longer peeked: " + taken.error);
    REQUIRE((!taken.image && taken.error == "guest_image_missing: op mark 4"));
    source.Close();
    UNSCOPED_INFO("a peek after Close reports closing");
    REQUIRE(source.PeekGuestImage(4, std::chrono::milliseconds(0)).closing);
}

// Peek, repeat peek, and take reuse one encoded image.
void testPeekEncodesOnce()
{
    auto                        producer = std::make_shared<FakeProducer>();
    D3D12CompletionSourceConfig config{ producer };
    config.timeEncodes = true;
    D3D12CompletionSource source(config);
    const auto            encodes = [&]
    {
        return source.GuestImageEncodes().first;
    };
    submitRaw(source, 1, 2, rawCopy());
    submitRaw(source, 2, 4, rawCopy());
    submitRaw(source, 3, 6, rawCopy());
    submitRaw(source, 4, 8, rawCopy());
    settleRaw(*producer, 0, std::vector<std::uint8_t>(4096, 3));
    UNSCOPED_INFO("the first readback completes");
    REQUIRE(source.Completed() == 1);
    const auto first = source.PeekGuestImage(2, std::chrono::milliseconds(0));
    const auto again = source.PeekGuestImage(2, std::chrono::milliseconds(0));
    UNSCOPED_INFO("a repeated peek reuses the encoding");
    REQUIRE((first.image && again.image && again.image->bytes == first.image->bytes && encodes() == 1));
    {
        // A second resolve under a held mark would leave the cached peek stale.
        NativeCompletionSpan reused;
        reused.epoch        = 5;
        reused.resolveMark  = 2;
        reused.guestResolve = rawCopy();
        reused.frame.emplace();
        reused.frame->retainOnly = reused.frame->returnRawOutput = true;
        reused.frame->outputTextureId                            = 1;
        reused.frame->passes.emplace_back();
        std::string error;
        UNSCOPED_INFO("a reused resolve mark is refused: " + error);
        REQUIRE((source.Submit(std::move(reused), error) == 0 && error == "resolve_mark_reused" && producer->queued.size() == 4));
    }
    const auto taken = source.TakeGuestImage(2);
    UNSCOPED_INFO("the take reuses the peeked encoding: " + taken.error);
    REQUIRE((taken.image && taken.image->bytes == first.image->bytes && taken.image->physical == first.image->physical && encodes() == 1));

    // Settled but not yet polled into the kept images.
    settleRaw(*producer, 1, std::vector<std::uint8_t>(4096, 9));
    const auto unpolled = source.PeekGuestImage(4, std::chrono::milliseconds(0));
    UNSCOPED_INFO("an in-flight image is peeked once: " + unpolled.error);
    REQUIRE((unpolled.image && encodes() == 2));
    UNSCOPED_INFO("the second readback completes");
    REQUIRE(source.Completed() == 2);
    const auto later = source.TakeGuestImage(4);
    UNSCOPED_INFO("the take after polling reuses it: " + later.error);
    REQUIRE((later.image && later.image->bytes == unpolled.image->bytes && encodes() == 2));

    // A discard drops the peeked encoding; an unpeeked take still encodes.
    settleRaw(*producer, 2);
    settleRaw(*producer, 3);
    UNSCOPED_INFO("the remaining readbacks complete");
    REQUIRE(source.Completed() == 4);
    UNSCOPED_INFO("the third image is peeked");
    REQUIRE((source.PeekGuestImage(6, std::chrono::milliseconds(0)).image && encodes() == 3));
    source.DiscardGuestImagesThrough(6);
    const auto discarded = source.TakeGuestImage(6);
    UNSCOPED_INFO("a discarded peek is not taken: " + discarded.error);
    REQUIRE((!discarded.image && discarded.error.starts_with("guest_image_discarded") && encodes() == 3));
    UNSCOPED_INFO("an unpeeked take encodes");
    REQUIRE((source.TakeGuestImage(8).image && encodes() == 4));
}

// Held image outlives source; handle encoding counts and peek returns encoded.
void testHoldGuestImage()
{
    auto                        producer = std::make_shared<FakeProducer>();
    D3D12CompletionSourceConfig config{ producer };
    config.timeEncodes = true;
    std::shared_ptr<const rerevved::gpu::guest_d3d::IHeldGuestImage> held2, held4;
    {
        D3D12CompletionSource source(config);
        const auto            encodes = [&]
        {
            return source.GuestImageEncodes().first;
        };
        submitRaw(source, 1, 2, rawCopy());
        submitRaw(source, 2, 4, rawCopy());
        submitRaw(source, 3, 6, rawCopy());
        settleRaw(*producer, 0, std::vector<std::uint8_t>(4096, 3));
        settleRaw(*producer, 1, std::vector<std::uint8_t>(4096, 7));
        settleRaw(*producer, 2, std::vector<std::uint8_t>(4096, 9));
        UNSCOPED_INFO("the readbacks complete");
        REQUIRE(source.Completed() == 3);
        UNSCOPED_INFO("the third image is peeked");
        REQUIRE((source.PeekGuestImage(6, std::chrono::milliseconds(0)).image && encodes() == 1));
        const auto first = source.HoldGuestImage(2, held2);
        UNSCOPED_INFO("a hold hands the readback over unencoded: " + first.error);
        REQUIRE((held2 && !first.image && first.error.empty() && !first.held && encodes() == 1 && held2->Bytes() == 4096));
        const auto second = source.HoldGuestImage(4, held4);
        UNSCOPED_INFO("a later hold takes its own readback: " + second.error);
        REQUIRE((held4 && !second.image && encodes() == 1));
        const auto gone = source.TakeGuestImage(2);
        UNSCOPED_INFO("a held image is gone from the source: " + gone.error);
        REQUIRE((!gone.image && gone.error.starts_with("guest_image_discarded")));
        std::shared_ptr<const rerevved::gpu::guest_d3d::IHeldGuestImage> peeked;
        const auto                                                       encoded = source.HoldGuestImage(6, peeked);
        UNSCOPED_INFO("a peeked image comes back encoded, not held: " + encoded.error);
        REQUIRE((!peeked && encoded.image && encoded.image->bytes == std::vector<std::uint8_t>(4096, 9) && encodes() == 1));
        const auto first2 = held2->Encode();
        UNSCOPED_INFO("the handle encodes the readback and counts it: " + first2.error);
        REQUIRE((first2.image && first2.image->bytes == std::vector<std::uint8_t>(4096, 3) && first2.image->physical == rawCopy().destBase && encodes() == 2));
        submitRaw(source, 4, 8, rawCopy());
        settleRaw(*producer, 3);
        source.Close();
        std::shared_ptr<const rerevved::gpu::guest_d3d::IHeldGuestImage> closed;
        UNSCOPED_INFO("a hold after Close reports closing");
        REQUIRE((source.HoldGuestImage(8, closed).closing && !closed));
    }
    const auto late = held4->Encode();
    UNSCOPED_INFO("a handle encodes after its source is gone: " + late.error);
    REQUIRE((late.image && late.image->bytes == std::vector<std::uint8_t>(4096, 7)));
}

// A recorded prefix's readback of one resolve destination.
struct FakeReadback final : NativeRawReadback
{
    std::vector<std::uint8_t> texels;

    explicit FakeReadback(std::uint8_t value)
    : texels(4096, value)
    {
    }

    bool Read(std::vector<std::uint8_t>& bytes, std::uint32_t& rowPitch, std::string&) const override
    {
        bytes    = texels;
        rowPitch = 128;
        return true;
    }

    std::uint64_t Bytes() const noexcept override
    {
        return texels.size();
    }
};

// A graph segment of one off-screen pass reading back each named resolve.
NativeCompletionSpan graphSpan(std::uint64_t epoch, std::vector<NativeSpanResolve> resolves)
{
    NativeCompletionSpan span;
    span.epoch   = epoch;
    span.segment = true;
    span.frame.emplace();
    span.frame->identityAttachments = true;
    NativeRenderFramePass pass;
    pass.target = { 32, 32, 1, NativeDrawReplayTargetFormat::Rgba8, 0x300 };
    for (const auto& named : resolves)
    {
        span.frame->textures.push_back({ named.textureId, 32, 32, NativeDrawReplayTextureFormat::Rgba8 });
        NativeRenderFrameResolve resolve;
        resolve.textureId = named.textureId;
        resolve.width = resolve.height = 32;
        resolve.readback               = true;
        pass.resolves.push_back(resolve);
    }
    span.frame->passes.push_back(std::move(pass));
    span.resolves = std::move(resolves);
    return span;
}

// Graph resolves become images only after their segment fence completes.
void testGraphResolves()
{
    auto                        producer = std::make_shared<FakeProducer>();
    D3D12CompletionSourceConfig config{ producer };
    config.timeEncodes = true;
    D3D12CompletionSource source(config);
    const auto            copy = rawCopy();
    std::string           error;
    {
        auto unread                                  = graphSpan(1, { { 2, 20, copy } });
        unread.frame->passes[0].resolves[0].readback = false;
        UNSCOPED_INFO("a graph resolve needs its readback: " + error);
        REQUIRE((!source.Submit(std::move(unread), error) && error == "graph_resolve_without_readback"));
        auto terminal    = graphSpan(1, { { 2, 20, copy } });
        terminal.segment = false;
        UNSCOPED_INFO("graph resolves ride segments only: " + error);
        REQUIRE((!source.Submit(std::move(terminal), error) && error == "graph_resolves_outside_segment"));
        UNSCOPED_INFO("a span names each mark once: " + error);
        REQUIRE((!source.Submit(graphSpan(1, { { 2, 20, copy }, { 2, 21, copy } }), error) && error == "resolve_mark_reused"));
    }
    UNSCOPED_INFO("the segment queues as prefix passes: " + error);
    REQUIRE((source.Submit(graphSpan(1, { { 2, 20, copy }, { 4, 21, copy } }), error) == 1 && producer->Count() == 1 && producer->queued[0].prefix));
    UNSCOPED_INFO("a pending graph mark is not reused: " + error);
    REQUIRE((!source.Submit(graphSpan(1, { { 4, 22, copy } }), error) && error == "resolve_mark_reused"));
    const auto early = source.TakeGuestImage(2);
    UNSCOPED_INFO("no image before the segment completes: " + early.error);
    REQUIRE((!early.image && early.error == "guest_image_incomplete: op mark 2"));
    UNSCOPED_INFO("no peek before recording");
    REQUIRE(source.PeekGuestImage(4, 0ms).error == "guest_image_incomplete: op mark 4");
    {
        NativeDrawReplayResult result;
        result.success              = true;
        result.completionFenceValue = 5;
        result.rawReadbacks         = { { 20, std::make_shared<FakeReadback>(255) }, { 21, std::make_shared<FakeReadback>(7) } };
        std::lock_guard lock(producer->mutex);
        producer->queued[0].promise->set_value(std::move(result));
    }
    producer->fence = 4;
    UNSCOPED_INFO("the recorded segment waits for its fence");
    REQUIRE(source.Completed() == 0);
    UNSCOPED_INFO("a peek before the fence reports incomplete");
    REQUIRE(source.PeekGuestImage(4, 0ms).error == "guest_image_incomplete: op mark 4");
    int polls           = 0;
    producer->pollFence = [&]
    {
        if (++polls == 3)
            producer->fence = 5;
    };
    const auto peek     = source.PeekGuestImage(4, 2s);
    producer->pollFence = {};
    UNSCOPED_INFO("the peek waits for the segment fence and reads its readback: " + peek.error);
    REQUIRE((peek.image && peek.image->bytes == std::vector<std::uint8_t>(4096, 7) && polls >= 3));
    UNSCOPED_INFO("timing counts each peek of a pending graph resolve, failed ones included");
    REQUIRE((source.GraphPeekWaits().first == 3 && source.GraphPeekWaits().second > 0));
    UNSCOPED_INFO("the segment completes at its fence");
    REQUIRE(source.Completed() == 1);
    const auto first = source.TakeGuestImage(2);
    UNSCOPED_INFO("the first resolve encodes its readback: " + first.error);
    REQUIRE((first.image && first.image->physical == copy.destBase && first.image->bytes == std::vector<std::uint8_t>(4096, 255)));
    const auto second = source.TakeGuestImage(4);
    UNSCOPED_INFO("the take reuses the peek: " + second.error);
    REQUIRE((second.image && second.image->bytes == peek.image->bytes && source.GuestImageEncodes().first == 2));

    // The swap's tail declares only its own textures; the segment declared
    // its destinations to the renderer, whose join adds them.
    NativeCompletionSpan final;
    final.epoch = 1;
    final.frame.emplace();
    final.frame->identityAttachments = true;
    final.frame->textures.push_back({ 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 });
    final.frame->passes.emplace_back();
    final.frame->passes.back().presented = true;
    final.frame->passes.back().target    = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    UNSCOPED_INFO("the final span queues the graph: " + error);
    REQUIRE((source.Submit(std::move(final), error) == 2 && producer->Count() == 2));
    std::lock_guard lock(producer->mutex);
    const auto&     graph = producer->queued[1].frame;
    UNSCOPED_INFO("the tail continues the segment pass and declares only its own texture");
    REQUIRE((graph.passes.size() == 1 && graph.continuedPrefixPasses == 1 && graph.textures.size() == 1 && graph.textures[0].id == 1 &&
             producer->queued[0].frame.textures.size() == 2));
}

} // namespace

TEST_CASE("native_completion_source", "[ctest][native-completion-source][timeout-60]")
{
    testRawResolveCompletion();
    testGraphResolves();
    testPeekKeepsImages();
    testPeekEncodesOnce();
    testHoldGuestImage();
    testClosingTake();
    testTakeErrors();
    testUntakenImagesKept();
    testDiscardThrough();
    testMonotonicAndEmpty();
    testSegmentThenFinal();
    testSegmentAfterPendingTerminal();
    testPreserveTarget();
    testSegmentTypedViews();
    testPreserveAfterRefusedSegment();
    testSegmentsNeedFinalSpan();
    testClose();
    testCloseDuringPoll();
    testBoundedOutputReadback();
    testDrawVisibility();
    testDeviceFailure();
    testCensusRecovery();
    testCensusChainFailure();
    testShutdownWritesNothing();
}
