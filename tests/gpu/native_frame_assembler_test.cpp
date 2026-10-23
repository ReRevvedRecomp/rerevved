#include "gpu/completion/completion_ledger.h"
#include "gpu/d3d12/native_gameplay_draw.h"
#include "gpu/guest_d3d/gfx_draw_inputs.h"
#include "gpu/guest_d3d/guest_resolve_encoder.h"
#include "gpu/guest_d3d/label_draw_inputs.h"
#include "gpu/guest_d3d/native_frame_assembler.h"
#include "gpu/guest_d3d/scene_draw_coverage.h"
#include "manual_cases.hpp"

#include <rex/ppc/context.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <latch>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

using namespace rerevved::gpu;
using namespace rerevved::gpu::guest_d3d;
using namespace std::chrono_literals;

namespace
{

constexpr std::uint32_t kPanelCaller = 0x82303E40, kForeignCaller = 0x82400000;
// sub_825107A0 calls IndexedUpLowerer 0x826A3048 directly from here.
constexpr std::uint32_t kLowererCaller = 0x82510BFC;
constexpr std::uint32_t kGraphics = 0x1000, kStack = 0x7000;
constexpr std::uint32_t kDepthTag = 0xDE;
// A tag panelBuilder draws with every color channel masked.
constexpr std::uint32_t kNoColorTag = 0xC0;

// A refusal by name: the assembler appends the operation's form after " (".
bool refusedAs(const std::string& error, const std::string& expected)
{
    return error == expected || error.starts_with(expected + " (");
}

std::size_t slot(NativeDrawReason reason)
{
    return static_cast<std::size_t>(reason);
}

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

NativeDrawSubmission submission(NativeDrawHook hook, std::uint32_t caller, std::uint64_t sequence, std::uint32_t tag)
{
    NativeDrawSubmission value;
    value.hook          = hook;
    value.caller        = caller;
    value.stack         = kStack;
    value.arguments     = { kGraphics, 4, 0, 3, 3, 0x3000, 1, tag };
    value.stateRead     = true;
    value.sequence      = sequence;
    value.operationMark = sequence;
    return value;
}

// Admits the panel caller only, tagging the draw with its eighth argument.
bool panelBuilder(const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string& detail)
{
    if (value.hook != NativeDrawHook::IndexedUp || value.caller != kPanelCaller)
        return false;
    if (value.arguments[7] == 0xBAD)
    {
        detail = "fixture_recipe_rejected";
        return false;
    }
    draw = makeDraw(static_cast<std::uint8_t>(value.arguments[7]));
    // A tag the depth tests use for a depth-tested draw.
    draw.depth.enabled = value.arguments[7] == kDepthTag;
    if (value.arguments[7] == kNoColorTag)
        draw.blend.writeMask = 0;
    return true;
}

// Drives the dispatcher seam for one root submission as the dispatcher does;
// a direct 0x826A3048 root is committed at once (0x826A2FF0).
std::optional<NativeDrawReason> submit(NativeFrameAssembler& assembler, const NativeDrawSubmission& value)
{
    assembler.Began(value);
    if (NativeDrawDefersToCommit(value.hook, value.caller))
        assembler.Committed(value);
    const auto verdict = assembler.Admit(value);
    assembler.Recorded(value, verdict);
    return verdict;
}

NativeFrameSwap makeSwap(std::uint64_t sequence, std::uint64_t numbered = 0)
{
    NativeFrameSwap swap;
    swap.sequence            = sequence;
    swap.numberedThrough     = numbered;
    swap.frontbufferVirtual  = 0x9F000000;
    swap.frontbufferPhysical = 0x1F000000;
    swap.fetch               = { 2, 0x1F000000 | 6, 1279 | (719U << 13), 0xD10, 0, 0 };
    swap.textureFormat       = 6;
    swap.width               = 1280;
    swap.height              = 720;
    // A non-identity table, the same at every swap as the app's table is.
    for (std::uint32_t i = 0; i < swap.gammaTable.size(); ++i)
        swap.gammaTable[i] = i * 0x00100401U + 1;
    return swap;
}

// RB_COPY_DEST_INFO and RB_COPY_DEST_PITCH of the title's scanout resolves
// (copy=...,02D00500,01000300 in the native resolve log): k_8_8_8_8, no
// endian swap, red/blue swapped, into a 1280x720 frontbuffer view.
constexpr std::uint32_t kScanoutDestInfo = 0x01000300, kScanoutDestPitch = 0x02D00500;

// An untiled scanout of the whole frame to makeSwap's frontbuffer with the
// title's k01 control, unswapped by default so the swap fetch swizzle stands.
GuestResolveCopy scanoutCopy(std::uint32_t destInfo = 0x300)
{
    GuestResolveCopy copy;
    copy.copyControl = 0x00100040;
    copy.destBase    = 0x1F000000;
    copy.destPitch   = kScanoutDestPitch;
    copy.destInfo    = destInfo;
    copy.rectWidth   = 1280;
    copy.rectHeight  = 720;
    return copy;
}

// A presenting swap needs a modeled scanout of its frontbuffer: this scans
// out the 1280x720x4 presented target at `base`.
void scanOut(NativeFrameAssembler& assembler, std::uint32_t base = 0)
{
    GuestTarget main;
    main.native = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, base };
    std::string error;
    bool        partial = false;
    UNSCOPED_INFO("the presented target is scanned out: " + error);
    REQUIRE((assembler.Resolve(main, scanoutCopy(), true, partial, error) && error.empty()));
}

struct FakeCompletion final : INativeCompletionSource
{
    std::vector<NativeCompletionSpan> spans;
    std::uint64_t                     next = 0, completed = 0, terminal = 0;
    bool                              refuse = false, repeat = false, throws = false, closing = false;
    // As the D3D12 source: no segment while the last terminal frame is unfinished.
    bool                       gateSegments = false;
    completion::GuestImageTake image;
    std::vector<std::uint64_t> discarded;

    std::uint64_t Submit(NativeCompletionSpan span, std::string& error) override
    {
        if (throws)
            throw 42;
        if (refuse || closing)
        {
            error = closing ? "closing" : "fixture_device_lost";
            return 0;
        }
        if (span.segment && !ReadyForSegment())
        {
            error = "segment_not_ready";
            return 0;
        }
        const bool frame = span.frame && !span.segment;
        spans.push_back(std::move(span));
        const auto value = repeat ? 7 : ++next;
        if (frame)
            terminal = value;
        return value;
    }

    bool ReadyForSegment() const override
    {
        return !gateSegments || completed >= terminal;
    }

    // Runs inside a take, as the source's encode would.
    std::function<void()> onTake;

    completion::GuestImageTake TakeGuestImage(std::uint64_t) override
    {
        if (onTake)
            onTake();
        return image;
    }

    // Readbacks a hold hands over unencoded, by op mark; other marks take.
    std::map<std::uint64_t, std::shared_ptr<const IHeldGuestImage>> holds;

    completion::GuestImageTake HoldGuestImage(std::uint64_t mark, std::shared_ptr<const IHeldGuestImage>& held) override
    {
        const auto found = holds.find(mark);
        if (found == holds.end())
            return INativeCompletionSource::HoldGuestImage(mark, held);
        held = found->second;
        holds.erase(found);
        return {};
    }

    // Images a read may peek, by op mark, and the marks peeked.
    std::map<std::uint64_t, completion::GuestImageTake> peeks;
    std::vector<std::uint64_t>                          peeked;
    std::atomic<unsigned>                               peekCount{ 0 };

    completion::GuestImageTake PeekGuestImage(std::uint64_t mark, std::chrono::milliseconds) override
    {
        peeked.push_back(mark);
        ++peekCount;
        const auto found = peeks.find(mark);
        return found == peeks.end() ? completion::GuestImageTake{ std::nullopt, "fixture_peek_missing", false } : found->second;
    }

    void DiscardGuestImagesThrough(std::uint64_t mark) override
    {
        discarded.push_back(mark);
    }

    std::uint64_t Completed() const override
    {
        return completed;
    }

    unsigned waits = 0;

    bool WaitFor(std::uint64_t value, std::chrono::milliseconds) override
    {
        ++waits;
        return completed >= value;
    }
};

std::vector<std::uint8_t> drawTags(const NativeCompletionSpan& span)
{
    UNSCOPED_INFO("span carries a frame");
    REQUIRE(span.frame.has_value());
    std::vector<std::uint8_t> tags;
    for (const auto& pass : span.frame->passes)
        for (std::size_t i = 0; i < pass.DrawCount(); ++i)
            tags.push_back(pass.Draw(i).vertexConstants[0]);
    return tags;
}

void checkFrameShape(const NativeRenderFrame& frame, const NativeFrameSwap& swap)
{
    std::string error;
    const bool  valid = ValidateNativeRenderFrame(frame, error);
    UNSCOPED_INFO("assembled frame validates: " + error);
    REQUIRE(valid);
    UNSCOPED_INFO("one pass resolves one output texture");
    REQUIRE((frame.passes.size() == 1 && frame.textures.size() == 1 && frame.outputTextureId == frame.textures[0].id));
    const auto& pass = frame.passes[0];
    UNSCOPED_INFO("a fresh cleared target and one frontbuffer resolve");
    REQUIRE((!pass.preservePreviousAttachments && pass.clears.empty() && pass.resolves.size() == 1));
    const auto& resolve = pass.resolves[0];
    UNSCOPED_INFO("the resolve covers the swap extent from host samples 0 and 3");
    REQUIRE((resolve.textureId == frame.outputTextureId && resolve.width == 1280 && resolve.height == 720 &&
             resolve.samples == std::array<std::uint32_t, 2>{ 0, 3 } && !resolve.depth));
    UNSCOPED_INFO("swizzle comes from fetch dword 3");
    REQUIRE((frame.outputSwizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 3 }));
    UNSCOPED_INFO("the swap's gamma table is copied once into the frame");
    REQUIRE(frame.gammaTable == swap.gammaTable);
}

void testOneEpochOneFrame()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    UNSCOPED_INFO("first panel draw admitted");
    REQUIRE(!submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 11)));
    UNSCOPED_INFO("second panel draw admitted");
    REQUIRE(!submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 2, 12)));
    scanOut(assembler);
    const auto swap   = makeSwap(1, 2);
    const auto result = assembler.Swap(swap);
    // Nothing hooks the guest clear or frontbuffer resolve yet, so Complete is unreachable.
    UNSCOPED_INFO("an epoch of admitted draws is provisional and names its assumptions");
    REQUIRE((result.status == NativeFrameStatus::Provisional && result.epoch == 1 && result.swapSequence == 1 &&
             result.assumptions.clearAssumed && result.assumptions.impliedResolve));
    UNSCOPED_INFO("coverage counts both draws");
    REQUIRE((result.coverage.attempted == 2 && result.coverage.admitted == 2));
    UNSCOPED_INFO("the frame went to the completion source with its sequence range, through the scanout");
    REQUIRE((result.firstSequence == 1 && result.lastSequence == 3 && result.hostFence == 1 && !result.frame));
    UNSCOPED_INFO("batches are not frames: exactly one span per epoch");
    REQUIRE(completion.spans.size() == 1);
    const auto& span = completion.spans[0];
    UNSCOPED_INFO("span carries epoch, sequences, status, coverage and assumptions");
    REQUIRE((span.epoch == 1 && span.firstSequence == 1 && span.lastSequence == 3 && span.status == NativeFrameStatus::Provisional &&
             span.coverage.admitted == 2 && span.assumptions.clearAssumed && span.assumptions.impliedResolve));
    checkFrameShape(*span.frame, swap);
    UNSCOPED_INFO("draws keep dispatcher order");
    REQUIRE(drawTags(span) == std::vector<std::uint8_t>{ 11, 12 });
    UNSCOPED_INFO("live frames publish a GPU output");
    REQUIRE((span.frame->returnGpuOutput && !span.frame->returnResolvedOutput && span.frame->outputPath.empty()));
    UNSCOPED_INFO("a provisional epoch leaves the session healthy");
    REQUIRE(!assembler.Failure());
}

void testEpochOrdering()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    std::uint64_t        sequence = 0;
    for (std::uint8_t epoch = 1; epoch <= 3; ++epoch)
    {
        // Recording order can differ from numbering order across guest threads.
        const auto first = ++sequence, second = ++sequence;
        submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, second, epoch * 10 + 2));
        submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, first, epoch * 10 + 1));
        scanOut(assembler);
        const auto scanned = ++sequence;
        const auto result  = assembler.Swap(makeSwap(epoch * 2, 2 * epoch));
        UNSCOPED_INFO("each swap closes one epoch with an increasing fence");
        REQUIRE((result.status == NativeFrameStatus::Provisional && result.epoch == epoch && result.hostFence == epoch));
        UNSCOPED_INFO("epoch sequence range");
        REQUIRE((result.firstSequence == first && result.lastSequence == scanned));
    }
    UNSCOPED_INFO("three epochs produce three spans");
    REQUIRE(completion.spans.size() == 3);
    for (std::uint8_t epoch = 1; epoch <= 3; ++epoch)
    {
        const auto& span = completion.spans[epoch - 1];
        UNSCOPED_INFO("spans arrive in epoch order");
        REQUIRE(span.epoch == epoch);
        UNSCOPED_INFO("each frame holds only its epoch's draws, sorted by sequence");
        REQUIRE(drawTags(span) == std::vector<std::uint8_t>{ std::uint8_t(epoch * 10 + 1), std::uint8_t(epoch * 10 + 2) });
    }
    const auto stale = assembler.Swap(makeSwap(6, 6));
    UNSCOPED_INFO("a repeated swap record fails instead of publishing");
    REQUIRE((stale.status == NativeFrameStatus::Failed && stale.detail == "swap_sequence_not_increasing" &&
             completion.spans.size() == 3));
}

// One draw thread records every pre-swap draw before swap closes.
void testDrawThreads()
{
    for (const auto mode : { NativeDrawMode::Strict, NativeDrawMode::Skip })
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.mode       = mode;
        config.build      = panelBuilder;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1));
        std::thread other([&]
                          {
                              submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 2, 2));
                          });
        other.join();
        const auto result = assembler.Swap(makeSwap(1, 2));
        UNSCOPED_INFO("a second drawing thread fails the epoch and names both threads: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.find("second_draw_thread") != std::string::npos &&
                 result.detail.find(" and ") != std::string::npos && completion.spans.empty()));
    }

    // A sequencer records draws for the guest thread that drew them.
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.build      = panelBuilder;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1));
        auto deferred   = submission(NativeDrawHook::IndexedUp, kPanelCaller, 2, 2);
        deferred.thread = std::this_thread::get_id();
        std::thread sequencer([&]
                              {
                                  submit(assembler, deferred);
                              });
        sequencer.join();
        const auto result = assembler.Swap(makeSwap(1, 2));
        UNSCOPED_INFO("a draw recorded for its guest thread keeps the epoch: " + result.detail);
        REQUIRE(result.detail.find("second_draw_thread") == std::string::npos);
    }

    // A device acquire hands the open epoch to the loading screen's thread,
    // which draws, scans out and swaps it; the main thread takes it back.
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.build      = panelBuilder;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1));
        NativeAssembledFrame loaded;
        std::thread          loader([&]
                                    {
                               assembler.DeviceAcquired();
                               submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 2, 2));
                               scanOut(assembler);
                               loaded = assembler.Swap(makeSwap(1, 2));
                                    });
        loader.join();
        UNSCOPED_INFO("the acquiring thread completes the epoch with both threads' draws: " + loaded.detail);
        REQUIRE((loaded.status == NativeFrameStatus::Provisional && completion.spans.size() == 1 &&
                 drawTags(completion.spans[0]) == std::vector<std::uint8_t>{ 1, 2 } && !assembler.Failure()));
        assembler.DeviceAcquired();
        submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 3, 3));
        scanOut(assembler);
        const auto back = assembler.Swap(makeSwap(2, 3));
        UNSCOPED_INFO("the main thread draws again after acquiring: " + back.detail);
        REQUIRE((back.status == NativeFrameStatus::Provisional && completion.spans.size() == 2 && !assembler.Failure()));
    }

    // Thread A numbered 3 but records only after B's swap closed through 3.
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    std::latch           admitted(1), swapped(1);
    std::thread          late([&]
                              {
                         const auto value = submission(NativeDrawHook::IndexedUp, kPanelCaller, 3, 3);
                         assembler.Began(value);
                         const auto verdict = assembler.Admit(value);
                         admitted.count_down();
                         swapped.wait();
                         assembler.Recorded(value, verdict);
                              });
    admitted.wait();
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1));
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 2, 2));
    const auto closed = assembler.Swap(makeSwap(1, 3));
    swapped.count_down();
    late.join();
    UNSCOPED_INFO("an unrecorded numbered draw fails the epoch it belongs to: " + closed.detail);
    REQUIRE((closed.status == NativeFrameStatus::Failed && closed.detail.find("draw_numbered_not_recorded") != std::string::npos &&
             completion.spans.empty()));

    // Backstop: a record at or below a closed sequence still fails its epoch.
    NativeFrameAssembler backstop(config);
    submit(backstop, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1));
    scanOut(backstop);
    UNSCOPED_INFO("first epoch closes cleanly");
    REQUIRE(backstop.Swap(makeSwap(1, 1)).status == NativeFrameStatus::Provisional);
    submit(backstop, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1));
    const auto late2 = backstop.Swap(makeSwap(2, 1));
    UNSCOPED_INFO("a late record is caught after its swap: " + late2.detail);
    REQUIRE((late2.status == NativeFrameStatus::Failed && late2.detail.find("draw_crossed_swap") != std::string::npos));
}

void testStrictRejection()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1));
    UNSCOPED_INFO("a non-solid draw has no native route");
    REQUIRE(submit(assembler, submission(NativeDrawHook::NonindexedUp, kForeignCaller, 2, 2)) == NativeDrawReason::NoNativeRoute);
    const auto result = assembler.Swap(makeSwap(1));
    UNSCOPED_INFO("a strict rejection submits nothing");
    REQUIRE((result.status == NativeFrameStatus::Failed && !result.frame && !result.hostFence && completion.spans.empty()));
    UNSCOPED_INFO("the rejection is counted under its slice 5 reason");
    REQUIRE((result.coverage.rejected[slot(NativeDrawReason::NoNativeRoute)] == 1 && result.coverage.admitted == 1));
    UNSCOPED_INFO("the failure names hook, caller and reason: " + result.detail);
    REQUIRE(result.detail.find("nonindexed_up from 0x82400000: no_native_route") != std::string::npos);
    UNSCOPED_INFO("the session failure is visible");
    REQUIRE(assembler.Failure().has_value());
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 3, 3));
    const auto later = assembler.Swap(makeSwap(2));
    UNSCOPED_INFO("a failed strict session never presents again");
    REQUIRE((later.status == NativeFrameStatus::Failed && later.detail.starts_with("session_failed") && completion.spans.empty()));

    NativeFrameAssembler rejected(config);
    const auto           bad = submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 0xBAD);
    rejected.Began(bad);
    const auto  badVerdict = rejected.Admit(bad);
    std::string refusal    = "stale";
    rejected.Recorded(bad, badVerdict, &refusal);
    UNSCOPED_INFO("a candidate whose recipe fails is not admitted");
    REQUIRE(badVerdict == NativeDrawReason::NoNativeRoute);
    UNSCOPED_INFO("the record hands the family refusal to the rejection log: " + refusal);
    REQUIRE(refusal == "fixture_recipe_rejected");
    const auto foreign = submission(NativeDrawHook::NonindexedUp, kForeignCaller, 2, 2);
    rejected.Began(foreign);
    rejected.Recorded(foreign, rejected.Admit(foreign), &refusal);
    UNSCOPED_INFO("a draw outside every family hands over no refusal: " + refusal);
    REQUIRE(refusal.empty());
    const auto candidate = rejected.Swap(makeSwap(1));
    UNSCOPED_INFO("the recipe rejection detail reaches the failure");
    REQUIRE((candidate.status == NativeFrameStatus::Failed && candidate.detail.find("fixture_recipe_rejected") != std::string::npos));
}

// Census skips refused draws, drops only failed-epoch work, and presents next.
void testCensusRecovery()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.mode       = NativeDrawMode::Census;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 7));
    assembler.Recorded(submission(NativeDrawHook::AutomaticIndex, kForeignCaller, 2, 0), NativeDrawReason::AutomaticIndex);
    scanOut(assembler);
    const auto first = assembler.Swap(makeSwap(1, 2));
    UNSCOPED_INFO("census mode skips a non-raster refusal: " + first.detail);
    REQUIRE((first.status == NativeFrameStatus::Partial && first.coverage.skipped[slot(NativeDrawReason::AutomaticIndex)] == 1));

    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1));
    const auto failed = assembler.Swap(makeSwap(2, 3));
    UNSCOPED_INFO("a late record still fails its epoch: " + failed.detail);
    REQUIRE((failed.status == NativeFrameStatus::Failed && failed.detail.find("draw_crossed_swap") != std::string::npos));
    UNSCOPED_INFO("census mode does not fail the session");
    REQUIRE(!assembler.Failure());

    const auto spans = completion.spans.size();
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 4, 7));
    scanOut(assembler);
    const auto later = assembler.Swap(makeSwap(3, 4)); // four draws recorded
    UNSCOPED_INFO("the next epoch presents after a failed one: " + later.detail);
    REQUIRE((later.status == NativeFrameStatus::Provisional && later.hostFence && completion.spans.size() == spans + 1));
}

void testSkipPartial()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.mode       = NativeDrawMode::Skip;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 7));
    submit(assembler, submission(NativeDrawHook::NonindexedUp, kForeignCaller, 2, 0));
    scanOut(assembler);
    const auto result = assembler.Swap(makeSwap(1, 2));
    UNSCOPED_INFO("a raster skip labels the frame partial");
    REQUIRE((result.status == NativeFrameStatus::Partial && result.hostFence == 1));
    UNSCOPED_INFO("skips are counted per reason");
    REQUIRE((result.coverage.attempted == 2 && result.coverage.admitted == 1 &&
             result.coverage.skipped[slot(NativeDrawReason::NoNativeRoute)] == 1));
    UNSCOPED_INFO("the range covers the skipped draw and the scanout too");
    REQUIRE((result.firstSequence == 1 && result.lastSequence == 3));
    UNSCOPED_INFO("the first skip is named on the frame");
    REQUIRE(result.detail.find("no_native_route") != std::string::npos);
    UNSCOPED_INFO("one partial span");
    REQUIRE(completion.spans.size() == 1);
    const auto& span = completion.spans[0];
    UNSCOPED_INFO("the span is labeled partial with its skips, and only admitted work is rendered");
    REQUIRE((span.status == NativeFrameStatus::Partial && span.coverage.skipped[slot(NativeDrawReason::NoNativeRoute)] == 1 &&
             span.lastSequence == 3 && drawTags(span) == std::vector<std::uint8_t>{ 7 }));
    UNSCOPED_INFO("skip mode keeps the session alive");
    REQUIRE(!assembler.Failure());

    // Non-raster reasons carry guest effects; skip mode may not release them.
    for (const auto reason : { NativeDrawReason::AutomaticIndex, NativeDrawReason::NestingMismatch, NativeDrawReason::StateUnreadable, NativeDrawReason::DispatchFailure })
    {
        FakeCompletion source;
        config.completion = &source;
        NativeFrameAssembler strictReason(config);
        submit(strictReason, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 7));
        strictReason.Recorded(submission(NativeDrawHook::AutomaticIndex, kForeignCaller, 2, 0), reason);
        const auto failed = strictReason.Swap(makeSwap(1));
        UNSCOPED_INFO(std::string("skip mode fails the epoch for ") + NativeDrawReasonName(reason));
        REQUIRE((failed.status == NativeFrameStatus::Failed && failed.detail.find("_not_skippable") != std::string::npos &&
                 failed.coverage.skipped[slot(reason)] == 1 && source.spans.empty()));
    }

    // Only raster skips and no admitted draw: nothing to present.
    FakeCompletion skippedOnly;
    config.completion = &skippedOnly;
    NativeFrameAssembler foreign(config);
    submit(foreign, submission(NativeDrawHook::NonindexedUp, kForeignCaller, 1, 0));
    auto unsupported          = makeSwap(1, 1);
    unsupported.textureFormat = 54;
    const auto nothing        = foreign.Swap(unsupported);
    UNSCOPED_INFO("an epoch of only raster skips submits no frame and skips frontbuffer checks");
    REQUIRE((nothing.status == NativeFrameStatus::Partial && nothing.hostFence == 1 && skippedOnly.spans.size() == 1 &&
             !skippedOnly.spans[0].frame && skippedOnly.spans[0].lastSequence == 1 && !foreign.Failure()));

    NativeFrameAssembler noRoute(config);
    noRoute.Recorded(submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1), std::nullopt);
    UNSCOPED_INFO("an admission without a pre-Original draw is an assembly failure, not a skip");
    REQUIRE(noRoute.Swap(makeSwap(1)).status == NativeFrameStatus::Failed);
}

void testEmptyEpoch()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    const auto           result = assembler.Swap(makeSwap(1));
    UNSCOPED_INFO("an empty epoch is labeled and still completes through the source");
    REQUIRE((result.status == NativeFrameStatus::Empty && result.coverage.attempted == 0 && result.hostFence == 1 &&
             result.firstSequence == 0 && result.lastSequence == 0 && !result.assumptions.clearAssumed));
    UNSCOPED_INFO("the empty span carries no frame, so nothing new is presented");
    REQUIRE((completion.spans.size() == 1 && !completion.spans[0].frame && completion.spans[0].status == NativeFrameStatus::Empty &&
             completion.spans[0].firstSequence == 0 && completion.spans[0].lastSequence == 0));
    auto bad          = makeSwap(2);
    bad.textureFormat = 54;
    UNSCOPED_INFO("an empty epoch records its swap without frontbuffer checks");
    REQUIRE((assembler.Swap(bad).status == NativeFrameStatus::Empty && !assembler.Failure()));
}

// Suspended swap re-presents last frame; no-work epoch closes before next draw.
void testDeferredSwapEpoch()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 11));
    scanOut(assembler);
    UNSCOPED_INFO("the main frame before the suspend");
    REQUIRE(assembler.Swap(makeSwap(1, 1)).status == NativeFrameStatus::Provisional);
    NativeAssembledFrame deferred;
    std::thread          worker([&]
                                {
                           deferred = assembler.Swap(makeSwap(2, 1));
                                });
    worker.join();
    UNSCOPED_INFO("a worker swap with no work presents nothing, so the previous image stays");
    REQUIRE((deferred.status == NativeFrameStatus::Empty && completion.spans.size() == 2 && !completion.spans[1].frame && !assembler.Failure()));
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 2, 12));
    scanOut(assembler);
    const auto resumed = assembler.Swap(makeSwap(3, 2));
    UNSCOPED_INFO("the main thread draws the next epoch without second_draw_thread: " + resumed.detail);
    REQUIRE((resumed.status == NativeFrameStatus::Provisional && completion.spans.size() == 3 && drawTags(completion.spans[2]) == std::vector<std::uint8_t>{ 12 } &&
             !assembler.Failure()));
}

// No-work swap preserves frontbuffer, swizzle and gamma; work cannot replace.
void testDisplayedImage()
{
    enum class Change
    {
        None,
        WriteAfter,
        GuestWrite,
        Resolve,
        Unread,
        Frontbuffer,
        Swizzle,
        Gamma,
        Work
    };
    for (const auto& [change, expected] : { std::pair{ Change::None, "" }, std::pair{ Change::WriteAfter, "" }, std::pair{ Change::GuestWrite, "swap_display_overwritten: 0x1f000000 by a guest write at 0x1f001000" }, std::pair{ Change::Resolve, "swap_display_overwritten: 0x1f000000 by a resolve to 0x1f002000" }, std::pair{ Change::Unread, "swap_display_overwritten: 0x1f000000 by an unread resolve" }, std::pair{ Change::Frontbuffer, "swap_display_mismatch: 0x1f000000 is not swap 0x1f400000" }, std::pair{ Change::Swizzle, "swap_display_form_changed: 0x1f000000" }, std::pair{ Change::Gamma, "swap_display_form_changed: 0x1f000000" }, std::pair{ Change::Work, "swap_scanout_unmodeled: 0x1f000000" } })
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.build       = panelBuilder;
        config.completion  = &completion;
        config.watchWrites = [](std::uint32_t, std::uint32_t)
        {
        };
        NativeFrameAssembler assembler(config);
        submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 11));
        scanOut(assembler);
        UNSCOPED_INFO("the image is presented");
        REQUIRE(assembler.Swap(makeSwap(1, 1)).status == NativeFrameStatus::Provisional);
        auto again    = makeSwap(2, 1);
        auto copy     = scanoutCopy();
        copy.destBase = 0x1F002000;
        switch (change)
        {
            case Change::None:
                break;
            case Change::WriteAfter:
                assembler.GuestWrote(0x1F398000, 0x1000);
                break;
            case Change::GuestWrite:
                assembler.GuestWrote(0x1F001000, 4);
                break;
            case Change::Resolve:
                assembler.ResolveUnmodeled(copy);
                break;
            case Change::Unread:
                assembler.ResolveUnmodeled(std::nullopt);
                break;
            case Change::Frontbuffer:
                again.frontbufferPhysical = 0x1F400000;
                again.fetch[1]            = 0x1F400000 | 6;
                break;
            case Change::Swizzle:
                again.fetch[3] = 0x1414;
                break;
            case Change::Gamma:
                again.gammaTable[7] ^= 1;
                break;
            case Change::Work:
                submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 2, 12));
                again.numberedThrough = 2;
                break;
        }
        const auto result = assembler.Swap(again);
        const auto label  = std::format("change {}: {}", int(change), result.detail);
        if (*expected)
        {
            UNSCOPED_INFO("the shown image cannot be kept: " + label);
            REQUIRE((result.status == NativeFrameStatus::Failed && result.detail == expected && completion.spans.size() == 1));
        }
        else
        {
            UNSCOPED_INFO("the shown image stays: " + label);
            REQUIRE((result.status == NativeFrameStatus::Empty && completion.spans.size() == 2 && !completion.spans[1].frame && !assembler.Failure()));
        }
    }
}

// native_gamma_apply: gamma-only swap; else swap_display_form_changed.
void testGammaAdoption()
{
    const auto otherGamma = [](std::uint32_t seed)
    {
        std::array<std::uint32_t, 256> table{};
        for (std::uint32_t i = 0; i < table.size(); ++i)
            table[i] = ((i * 3 + seed) & 1023U) << 20 | ((i * 5 + seed) & 1023U) << 10 | ((i * 7 + seed) & 1023U);
        return table;
    };
    for (const bool adopt : { false, true })
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.build                 = panelBuilder;
        config.completion            = &completion;
        config.adoptGammaWithoutWork = adopt;
        config.watchWrites           = [](std::uint32_t, std::uint32_t)
        {
        };
        NativeFrameAssembler assembler(config);
        submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 11));
        scanOut(assembler);
        auto first           = makeSwap(1, 1);
        first.gammaTable     = otherGamma(1);
        first.gammaAssumed   = false;
        const auto presented = assembler.Swap(first);
        UNSCOPED_INFO("a swap with the guest's table carries it unassumed; the clear and resolve stay assumed");
        REQUIRE((presented.status == NativeFrameStatus::Provisional && !presented.assumptions.gammaAssumed && presented.assumptions.clearAssumed && presented.assumptions.impliedResolve &&
                 completion.spans.size() == 1 && completion.spans[0].frame && completion.spans[0].frame->gammaTable == first.gammaTable && !completion.spans[0].assumptions.gammaAssumed));

        auto changed         = makeSwap(2, 1);
        changed.gammaTable   = otherGamma(2);
        changed.gammaAssumed = false;
        const auto empty     = assembler.Swap(changed);
        if (!adopt)
        {
            UNSCOPED_INFO("without the flag a changed table fails: " + empty.detail);
            REQUIRE((empty.status == NativeFrameStatus::Failed && empty.detail == "swap_display_form_changed: 0x1f000000" && completion.spans.size() == 1));
            continue;
        }
        UNSCOPED_INFO("a changed table alone is adopted and the shown image stays: " + empty.detail);
        REQUIRE((empty.status == NativeFrameStatus::Empty && completion.spans.size() == 2 && !completion.spans[1].frame && !assembler.Failure()));
        // A repeated table, a return to an earlier one and a new one are all kept.
        auto same       = makeSwap(3, 1);
        same.gammaTable = changed.gammaTable;
        UNSCOPED_INFO("a repeated table is no change");
        REQUIRE((assembler.Swap(same).status == NativeFrameStatus::Empty && !assembler.Failure()));
        auto back       = makeSwap(4, 1);
        back.gammaTable = first.gammaTable;
        UNSCOPED_INFO("a table can be adopted back");
        REQUIRE((assembler.Swap(back).status == NativeFrameStatus::Empty && !assembler.Failure()));
        auto again       = makeSwap(5, 1);
        again.gammaTable = otherGamma(3);
        UNSCOPED_INFO("and again");
        REQUIRE((assembler.Swap(again).status == NativeFrameStatus::Empty && !assembler.Failure()));

        // Only the table: another swizzle still cannot keep the image, table change or not.
        auto swizzled       = makeSwap(6, 1);
        swizzled.gammaTable = otherGamma(4);
        swizzled.fetch[3]   = 0x1414;
        const auto refused  = assembler.Swap(swizzled);
        UNSCOPED_INFO("a changed swizzle still fails: " + refused.detail);
        REQUIRE((refused.status == NativeFrameStatus::Failed && refused.detail == "swap_display_form_changed: 0x1f000000"));
    }
    // The next presented frame carries the adopted table.
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.build                 = panelBuilder;
    config.completion            = &completion;
    config.adoptGammaWithoutWork = true;
    config.watchWrites           = [](std::uint32_t, std::uint32_t)
    {
    };
    NativeFrameAssembler assembler(config);
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 11));
    scanOut(assembler);
    auto first         = makeSwap(1, 1);
    first.gammaTable   = otherGamma(1);
    first.gammaAssumed = false;
    UNSCOPED_INFO("the image is presented");
    REQUIRE(assembler.Swap(first).status == NativeFrameStatus::Provisional);
    auto empty         = makeSwap(2, 1);
    empty.gammaTable   = otherGamma(2);
    empty.gammaAssumed = false;
    UNSCOPED_INFO("the table is adopted");
    REQUIRE(assembler.Swap(empty).status == NativeFrameStatus::Empty);
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 2, 12));
    scanOut(assembler);
    auto next         = makeSwap(3, 2);
    next.gammaTable   = otherGamma(2);
    next.gammaAssumed = false;
    const auto result = assembler.Swap(next);
    UNSCOPED_INFO("the next present uses the adopted table: " + result.detail);
    REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == 3 && completion.spans[2].frame && completion.spans[2].frame->gammaTable == otherGamma(2) &&
             !assembler.Failure()));
}

// Write notice widens ranges to pages; other writes stay armed.
void testWriteNotice()
{
    // SDK unwatch: written pages in 4 MB blocks; other buffers stay armed.
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 11));
    scanOut(assembler);
    UNSCOPED_INFO("the image is presented");
    REQUIRE(assembler.Swap(makeSwap(1, 1)).status == NativeFrameStatus::Provisional);
    const auto permitted = NativeFrameAssembler::WriteNotice(&assembler, 0x1F398000, 0x1000, false);
    UNSCOPED_INFO(std::format("a notice permits only its pages: {:#x}+{:#x}", permitted.first, permitted.second));
    REQUIRE(permitted == std::pair{ 0x1F398000U, 0x1000U });
    UNSCOPED_INFO("a write beside the frontbuffer keeps its image");
    REQUIRE(assembler.Swap(makeSwap(2, 1)).status == NativeFrameStatus::Empty);
    NativeFrameAssembler::WriteNotice(&assembler, 0x1F001000, 0x1000, false);
    const auto dropped = assembler.Swap(makeSwap(3, 1));
    UNSCOPED_INFO("a notice reports its write: " + dropped.detail);
    REQUIRE((dropped.status == NativeFrameStatus::Failed && dropped.detail == "swap_display_overwritten: 0x1f000000 by a guest write at 0x1f001000"));
}

void testSwapAndCompletionFailures()
{
    NativeFrameAssemblerConfig config;
    config.build    = panelBuilder;
    config.readback = true;
    for (const auto& [name, mutate] : std::initializer_list<std::pair<const char*, void (*)(NativeFrameSwap&)>>{ { "format", [](NativeFrameSwap& swap)
                                                                                                                   {
                                                                                                                       swap.textureFormat = 54;
                                                                                                                   } },
                                                                                                                 { "base", [](NativeFrameSwap& swap)
                                                                                                                   {
                                                                                                                       swap.frontbufferPhysical += 0x1000;
                                                                                                                   } },
                                                                                                                 { "extent", [](NativeFrameSwap& swap)
                                                                                                                   {
                                                                                                                       swap.width    = 1024;
                                                                                                                       swap.fetch[2] = 1023 | (719U << 13);
                                                                                                                   } },
                                                                                                                 { "swizzle", [](NativeFrameSwap& swap)
                                                                                                                   {
                                                                                                                       swap.fetch[3] = 7U << 1;
                                                                                                                   } } })
    {
        NativeFrameAssembler assembler(config);
        submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1));
        auto swap = makeSwap(1, 1);
        mutate(swap);
        UNSCOPED_INFO(std::string("unsupported swap ") + name + " fails visibly");
        REQUIRE(assembler.Swap(swap).status == NativeFrameStatus::Failed);
    }
    NativeFrameAssembler offline(config);
    submit(offline, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 5));
    scanOut(offline);
    auto result = offline.Swap(makeSwap(1, 1));
    UNSCOPED_INFO("without a completion source the caller receives the frame");
    REQUIRE((result.status == NativeFrameStatus::Provisional && result.frame && !result.hostFence));
    UNSCOPED_INFO("offline frames read back");
    REQUIRE((result.frame->returnResolvedOutput && !result.frame->returnGpuOutput));

    FakeCompletion completion;
    completion.refuse = true;
    config.completion = &completion;
    NativeFrameAssembler refused(config);
    const auto           lost = refused.Swap(makeSwap(1));
    UNSCOPED_INFO("a refused submission fails the session");
    REQUIRE((lost.status == NativeFrameStatus::Failed && lost.detail.find("fixture_device_lost") != std::string::npos &&
             refused.Failure()));

    FakeCompletion repeated;
    repeated.repeat   = true;
    config.completion = &repeated;
    NativeFrameAssembler stalled(config);
    UNSCOPED_INFO("the first fence is accepted");
    REQUIRE(stalled.Swap(makeSwap(1)).hostFence == 7);
    const auto stale = stalled.Swap(makeSwap(2));
    UNSCOPED_INFO("a non-increasing fence fails the session");
    REQUIRE((stale.status == NativeFrameStatus::Failed && stale.detail.find("fence_not_increasing") != std::string::npos));

    FakeCompletion throwing;
    throwing.throws   = true;
    config.completion = &throwing;
    NativeFrameAssembler thrown(config);
    const auto           escaped = thrown.Swap(makeSwap(1));
    UNSCOPED_INFO("a non-standard exception from Submit stays inside Swap");
    REQUIRE((escaped.status == NativeFrameStatus::Failed && escaped.detail.find("completion_submit_threw") != std::string::npos));
}

// The real dispatcher drives the seam: the builder runs before Original.
void testDispatcherSeam()
{
    std::vector<std::uint8_t>  memory(0x8000);
    constexpr std::uint32_t    kOriginalMark = 0x200;
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.mode       = NativeDrawMode::Skip;
    config.completion = &completion;
    config.build      = [&](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string& detail)
    {
        if (!panelBuilder(value, draw, detail))
            return false;
        draw.vertexConstants[1] = memory[kOriginalMark];
        return true;
    };
    unsigned builds = 0;
    config.build    = [&, build = config.build](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string& detail)
    {
        ++builds;
        // The direct lowerer caller stands in for a panel here.
        if (value.caller != kLowererCaller)
            return build(value, draw, detail);
        draw                    = makeDraw(0x5A);
        draw.vertexConstants[1] = memory[kOriginalMark];
        return true;
    };
    NativeFrameAssembler     assembler(config);
    NativeDrawDispatchConfig dispatch;
    dispatch.read = [](const std::uint8_t*, std::uint32_t, std::uint32_t& value)
    {
        value = 0x2000;
        return true;
    };
    assembler.Attach(dispatch);
    UNSCOPED_INFO("attach routes the dispatcher to the assembler");
    REQUIRE((dispatch.mode == NativeDrawMode::Skip && dispatch.began && dispatch.admit && dispatch.recorded && dispatch.committed));
    NativeDrawDispatcher dispatcher(dispatch);
    UNSCOPED_INFO("dispatcher installs");
    REQUIRE(InstallNativeDrawDispatcher(dispatcher));
    const auto root = [&](NativeDrawHook hook, std::uint32_t caller, std::optional<NativeDrawHook> child, std::uint32_t childReturn)
    {
        PPCContext ctx{};
        ctx.r1.u64            = kStack;
        ctx.r3.u64            = kGraphics;
        ctx.r10.u64           = 9;
        ctx.lr                = caller;
        memory[kOriginalMark] = 0;
        NativeDrawScope scope(hook, ctx, memory.data());
        // Original changes guest state that the pre-Original build must not see.
        memory[kOriginalMark] = 1;
        if (child)
        {
            PPCContext inner = ctx;
            inner.r1.u64 -= 0xB0;
            inner.lr = childReturn;
            NativeDrawScope nested(*child, inner, memory.data());
        }
    };
    root(NativeDrawHook::IndexedUp, kPanelCaller, NativeDrawHook::IndexedUpLowerer, kIndexedUpLowererReturn);
    root(NativeDrawHook::NonindexedUp, kForeignCaller, NativeDrawHook::NonindexedSink, kNonindexedSinkReturn);
    // A direct lowerer root is built at its commit 0x826A2FF0, after the
    // lowerer wrote its geometry, not at its own entry.
    {
        PPCContext ctx{};
        ctx.r1.u64            = kStack;
        ctx.r3.u64            = kGraphics;
        ctx.lr                = kLowererCaller;
        memory[kOriginalMark] = 0;
        builds                = 0;
        {
            NativeDrawScope scope(NativeDrawHook::IndexedUpLowerer, ctx, memory.data());
            memory[kOriginalMark] = 1;
            ctx.r3.u64            = 0;
        }
        UNSCOPED_INFO("a direct lowerer root is not built at its entry");
        REQUIRE(builds == 0);
        PPCContext commit{};
        commit.r1.u64 = kStack;
        commit.r3.u64 = kGraphics;
        NoteNativeDrawCommit(commit, memory.data());
        UNSCOPED_INFO("the commit builds the held lowerer root");
        REQUIRE(builds == 1);
    }
    UNSCOPED_INFO("dispatcher withdraws");
    REQUIRE(RemoveNativeDrawDispatcher(1s));
    const auto counters = dispatcher.CloseFrame();
    scanOut(assembler);
    auto       swap   = makeSwap(1, dispatcher.NumberedThrough());
    const auto result = assembler.Swap(swap);
    UNSCOPED_INFO("assembler coverage equals the dispatcher's frame counters");
    REQUIRE((swap.numberedThrough == 3 && result.status == NativeFrameStatus::Partial && result.coverage.attempted == counters.attempted &&
             result.coverage.admitted == counters.admitted && result.coverage.skipped == counters.skipped));
    UNSCOPED_INFO("an admitted panel and particle draw and one foreign skip");
    REQUIRE((result.coverage.admitted == 2 && result.coverage.skipped[slot(NativeDrawReason::NoNativeRoute)] == 1));
    UNSCOPED_INFO("the range carries the three dispatcher sequences and the scanout");
    REQUIRE((result.firstSequence == 1 && result.lastSequence == 4));
    const auto& pass = completion.spans.at(0).frame->passes.at(0);
    const auto& draw = pass.Draw(0);
    UNSCOPED_INFO("the draw was built from pre-Original state");
    REQUIRE((draw.vertexConstants[0] == 9 && draw.vertexConstants[1] == 0));
    UNSCOPED_INFO("the committed draw was built from post-lowerer state");
    REQUIRE((pass.DrawCount() == 2 && pass.Draw(1).vertexConstants[1] == 1));
}

// Teardown thread exit records nothing; other paths refuse.
void testHeldThreadExit()
{
    // src/app.cpp teardown records nothing after callback removal.
    for (const bool nextDraw : { true, false })
    {
        std::vector<std::uint8_t>  memory(0x8000);
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.mode       = NativeDrawMode::Strict;
        config.completion = &completion;
        config.build      = panelBuilder;
        NativeFrameAssembler frames(config);

        struct Sink final : completion::IGuestProgressSink
        {
            bool WritePhysical(std::uint32_t, std::uint32_t) override
            {
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
        } sink;

        completion::CompletionLedger ledger({ NativeDrawMode::Strict, &sink, nullptr, {} });
        ledger.BeginRingEpoch();
        ledger.RegisterWriteback(0x1000);
        NativeDrawDispatchConfig dispatch;
        dispatch.read = [](const std::uint8_t*, std::uint32_t, std::uint32_t& value)
        {
            value = 0x2000;
            return true;
        };
        frames.Attach(dispatch);
        dispatch.mode  = NativeDrawMode::Strict;
        dispatch.began = [&](const NativeDrawSubmission& submission)
        {
            frames.Began(submission);
            if (!NativeDrawDefersToCommit(submission.hook, submission.caller))
                ledger.BeginDraw(submission.hook, submission.caller);
        };
        dispatch.committed = [&](const NativeDrawSubmission& submission)
        {
            frames.Committed(submission);
            ledger.BeginDraw(submission.hook, submission.caller);
        };
        std::vector<std::optional<NativeDrawReason>> verdicts;
        dispatch.recorded = [&](const NativeDrawSubmission& submission, std::optional<NativeDrawReason> verdict)
        {
            std::string refusal;
            const auto  mark = frames.Recorded(submission, verdict, &refusal);
            ledger.FinishDraw(mark, submission.hook, submission.caller, verdict, refusal);
            verdicts.push_back(verdict);
        };
        std::vector<NativeDrawReason> failures;
        dispatch.failed = [&](const NativeDrawFailure& failure)
        {
            failures.push_back(failure.reason);
        };
        NativeDrawDispatcher dispatcher(dispatch);
        UNSCOPED_INFO("dispatcher installs");
        REQUIRE(InstallNativeDrawDispatcher(dispatcher));
        std::thread guest([&]
                          {
                              PPCContext ctx{};
                              ctx.r1.u64 = kStack;
                              ctx.r3.u64 = kGraphics;
                              ctx.lr     = kLowererCaller;
                              NativeDrawScope scope(NativeDrawHook::IndexedUpLowerer, ctx, memory.data());
                              ctx.r3.u64 = 0;
                          });
        guest.join();
        UNSCOPED_INFO("the exiting thread records nothing");
        REQUIRE((verdicts.empty() && failures.empty() && !ledger.Failure()));
        if (nextDraw)
        {
            PPCContext ctx{};
            ctx.r1.u64 = kStack;
            ctx.r3.u64 = kGraphics;
            ctx.lr     = kPanelCaller;
            NativeDrawScope scope(NativeDrawHook::IndexedUp, ctx, memory.data());
        }
        UNSCOPED_INFO("dispatcher withdraws");
        REQUIRE(RemoveNativeDrawDispatcher(1s));
        const auto failure = ledger.Failure();
        UNSCOPED_INFO((nextDraw ? "the next draw from another thread first refuses the exited thread's held root"
                                : "removal refuses the exited thread's held root"));
        REQUIRE((verdicts.size() == (nextDraw ? 2U : 1U) && verdicts[0] == NativeDrawReason::UncommittedSinkDraw &&
                 failures == std::vector<NativeDrawReason>{ NativeDrawReason::UncommittedSinkDraw } && failure &&
                 failure->find("uncommitted_sink_draw") != std::string::npos));
    }
    std::cout << "Held thread exit: queued at teardown, refused at the next draw or at removal\n";
}

// Mid-epoch guest fences complete without waiting for swap; tails continue.
void testMidEpochSegments()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
        submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, sequence, static_cast<std::uint32_t>(sequence)));
    const auto segmentFlush = assembler.FlushThrough(2);
    UNSCOPED_INFO("a flush before the swap submits a segment: " + segmentFlush.error);
    REQUIRE((segmentFlush.fence == 1 && completion.spans.size() == 1));
    {
        const auto& segment = completion.spans[0];
        UNSCOPED_INFO("the segment names its epoch, range and assumptions");
        REQUIRE((segment.segment && segment.epoch == 1 && segment.firstSequence == 1 && segment.lastSequence == 2 &&
                 segment.status == NativeFrameStatus::Provisional && segment.assumptions.clearAssumed && !segment.assumptions.impliedResolve));
        UNSCOPED_INFO("the segment holds only draws through the flushed sequence");
        REQUIRE(drawTags(segment) == std::vector<std::uint8_t>{ 1, 2 });
        const auto& frame = *segment.frame;
        UNSCOPED_INFO("the first segment starts the target and presents nothing");
        REQUIRE((frame.passes.size() == 1 && !frame.passes[0].preservePreviousAttachments && frame.passes[0].resolves.empty() &&
                 !frame.outputTextureId && !frame.returnGpuOutput && frame.textures.empty()));
    }
    UNSCOPED_INFO("a flushed sequence returns its fence without new work");
    REQUIRE((assembler.FlushThrough(1).fence == 1 && completion.spans.size() == 1));

    scanOut(assembler);
    const auto result = assembler.Swap(makeSwap(1, 3));
    UNSCOPED_INFO("the swap closes the epoch with the remaining work and whole-epoch coverage");
    REQUIRE((result.status == NativeFrameStatus::Provisional && result.hostFence == 2 && result.firstSequence == 1 &&
             result.lastSequence == 4 && result.coverage.admitted == 3 && completion.spans.size() == 2));
    const auto& last = completion.spans[1];
    UNSCOPED_INFO("the final span continues the segment's attachments and resolves the output");
    REQUIRE((!last.segment && drawTags(last) == std::vector<std::uint8_t>{ 3 } && last.frame->passes.front().preservePreviousAttachments &&
             last.frame->outputTextureId && last.frame->passes.back().resolves.size() == 1));
    UNSCOPED_INFO("a swapped sequence returns the swap's fence");
    REQUIRE((assembler.FlushThrough(3).fence == 2 && completion.spans.size() == 2));

    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 5, 4));
    scanOut(assembler);
    UNSCOPED_INFO("the next epoch starts a fresh target");
    REQUIRE((assembler.Swap(makeSwap(2, 4)).hostFence == 3 && !completion.spans[2].frame->passes.front().preservePreviousAttachments));
    const auto early = assembler.FlushThrough(9);
    UNSCOPED_INFO("a sequence not yet recorded asks for a retry without failing the session");
    REQUIRE((!early.fence && early.retry && early.error.starts_with("flush_draw_not_recorded") && !assembler.Failure()));
    NativeFrameAssemblerConfig offline;
    offline.build      = panelBuilder;
    const auto missing = NativeFrameAssembler(offline).FlushThrough(1);
    UNSCOPED_INFO("a real failure is not retried");
    REQUIRE((!missing.fence && !missing.retry && missing.error == "no_completion_source"));
}

void testCpuCompletionSource()
{
    CpuCompletionSource source;
    std::string         error;
    UNSCOPED_INFO("a span without a frame completes at submission");
    REQUIRE((source.Submit({}, error) == 1 && source.Completed() == 1 && source.WaitFor(1, 0ms)));
    NativeCompletionSpan render;
    render.frame = NativeRenderFrame{};
    UNSCOPED_INFO("a span with work to render is refused");
    REQUIRE((!source.Submit(std::move(render), error) && refusedAs(error, "cpu_completion_cannot_render") && source.Completed() == 1));

    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &source;
    NativeFrameAssembler assembler(config);
    UNSCOPED_INFO("an empty epoch completes on the CPU source");
    REQUIRE(assembler.Swap(makeSwap(1)).hostFence == 2);
    submit(assembler, submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1));
    scanOut(assembler);
    const auto refused = assembler.Swap(makeSwap(2, 1));
    UNSCOPED_INFO("an admitted epoch is never completed without a renderer");
    REQUIRE((refused.status == NativeFrameStatus::Failed && refused.detail.find("cpu_completion_cannot_render") != std::string::npos));
}

void testSolidPanelInputs()
{
    std::vector<std::uint64_t> reads;
    std::uint32_t              stride = 12;
    const ReadMemory           read   = [&](std::uint64_t address, std::uint64_t size, bool physical)
    {
        reads.push_back(address);
        UNSCOPED_INFO("only the virtual stride word is read for a foreign stride");
        REQUIRE((size == 4 && !physical));
        return std::vector<std::uint8_t>{ 0, 0, 0, static_cast<std::uint8_t>(stride) };
    };
    SolidDrawInputs inputs;
    std::string     error;
    UNSCOPED_INFO("other hooks and callers are foreign without guest reads");
    REQUIRE((!ReadSolidPanelInputs(submission(NativeDrawHook::NonindexedUp, kPanelCaller, 1, 0), read, inputs, error) &&
             !ReadSolidPanelInputs(submission(NativeDrawHook::IndexedUp, kForeignCaller, 1, 0), read, inputs, error) &&
             reads.empty() && error.empty()));
    UNSCOPED_INFO("the stride comes from r1+84 and a non-8 stride is foreign");
    REQUIRE((!ReadSolidPanelInputs(submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 0), read, inputs, error) &&
             reads == std::vector<std::uint64_t>{ kStack + 84 } && error.empty() && !inputs.solidCandidate));
    const ReadMemory unreadable = [](std::uint64_t, std::uint64_t, bool)
    {
        return std::vector<std::uint8_t>{};
    };
    UNSCOPED_INFO("an unreadable stride is named");
    REQUIRE((!ReadSolidPanelInputs(submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 0), unreadable, inputs, error) &&
             refusedAs(error, "solid_panel_stride_extent")));
    NativeDrawReplayRecipe draw;
    SolidDrawInputs        foreign;
    UNSCOPED_INFO("a foreign pair carries no rejection detail");
    REQUIRE((!BuildSolidPanelDraw(foreign, {}, draw, error) && error.empty()));
    foreign.solidCandidate = true;
    UNSCOPED_INFO("an incomplete candidate is rejected by name");
    REQUIRE((!BuildSolidPanelDraw(foreign, {}, draw, error) && refusedAs(error, "solid_panel_inputs_incomplete")));
    const auto builder = MakeSolidPanelDrawBuilder(unreadable, nullptr);
    UNSCOPED_INFO("the production builder routes foreign submissions without a catalog");
    REQUIRE((!builder(submission(NativeDrawHook::IndexedUp, kForeignCaller, 1, 0), draw, error) && error.empty()));
}

} // namespace

void testClearResolveCapture()
{
    std::map<std::uint32_t, std::uint32_t> memory;
    constexpr std::uint32_t                g = 0x10000, surface = 0x20000, color = 0x30000, stack = 0x40000;
    memory[g + 0x3098]      = surface;
    memory[g + 0x309C]      = 0;
    memory[g + 0x30A0]      = 0;
    memory[g + 0x30A4]      = 0;
    memory[g + 0x2880]      = 1024;
    memory[g + 0x31AC]      = 0;
    memory[g + 0x31A4]      = 0xFFFFFFFF;
    memory[g + 0x2ABC]      = 0;
    memory[surface + 0x24]  = (1023U << 18) | (1023U << 3);
    memory[surface + 0x18]  = 0;
    memory[g + 0x30A8]      = surface + 0x100;
    memory[surface + 0x11C] = 0x220;
    memory[surface + 0x124] = memory[surface + 0x24];
    memory[surface + 0x118] = 0;
    memory[surface + 0x1C]  = 0x120;
    memory[stack + 92]      = 0;
    for (std::uint32_t i = 0; i < 4; ++i)
        memory[color + 4 * i] = std::bit_cast<std::uint32_t>(1.0F);
    const GuestWordReader read = [&](std::uint32_t address, std::uint32_t& out)
    {
        auto found = memory.find(address);
        if (found == memory.end())
            return false;
        out = found->second;
        return true;
    };
    GuestClear clear;
    UNSCOPED_INFO("per-target clear captures guest registers and memory");
    REQUIRE(ReadGuestClear(read, { g, 0x3F, 0, 0, 0, 1024, 1024, color }, stack, 1.0, clear));
    UNSCOPED_INFO("packed extent and white depth/stencil clear decode");
    REQUIRE((clear.target.native.width == 1024 && clear.target.native.height == 1024 && clear.target.native.edramBase == 0x120 &&
             clear.clear.colorValue == std::array<float, 4>{ 1, 1, 1, 1 } && clear.clear.depth && clear.clear.stencil));
    memory[g + 0x2A18] = 0x100000;
    memory[g + 0x2A1C] = 0x1E3B0000;
    memory[g + 0x2A20] = 0x04000400;
    memory[g + 0x2A24] = 0x01000300;
    memory[g + 0x2A2C] = 0xFFFFFF00;
    memory[g + 0x2A30] = 0xFF000000;
    GuestResolveCopy copy;
    UNSCOPED_INFO("the low color clear word is required");
    REQUIRE(!ReadGuestResolveCopy(read, g, copy));
    memory[g + 0x2A34] = 0x12345678;
    UNSCOPED_INFO("mid-asm capture reads the copy words and the depth, color and low color clear words");
    REQUIRE((ReadGuestResolveCopy(read, g, copy) && copy.copyControl == 0x100000 && copy.destBase == 0x1E3B0000 &&
             copy.destPitch == 0x04000400 && copy.destInfo == 0x01000300 && copy.depthClear == 0xFFFFFF00 &&
             copy.colorClear == 0xFF000000 && copy.colorClearLow == 0x12345678));
    copy.rectWidth = copy.rectHeight = 1024;
    constexpr std::uint32_t texture = 0x50000, rectangle = 0x60000, point = 0x70000;
    copy.destFetch = { 0x88000002, 0xFE3AF006, 0x007FE3FF, 0xC14, 0, 0x200 };
    for (std::uint32_t i = 0; i < 6; ++i)
        memory[texture + 0x1C + i * 4] = copy.destFetch[i];
    memory[rectangle] = memory[rectangle + 4] = memory[point] = memory[point + 4] = 0;
    memory[rectangle + 8] = memory[rectangle + 12] = 1024;
    GuestResolveCopy captured;
    GuestTarget      target;
    UNSCOPED_INFO("resolve entry captures fetch, rectangle, point and bound source from guest registers");
    REQUIRE((ReadGuestResolveEntry(read, { g, 0, rectangle, texture, point, 0, 0, 0 }, target, captured) &&
             captured.destFetch == copy.destFetch && captured.rectWidth == 1024 && captured.rectHeight == 1024));
    UNSCOPED_INFO("unsupported resolve mip is rejected");
    REQUIRE(!ReadGuestResolveEntry(read, { g, 0, rectangle, texture, point, 1, 0, 0 }, target, captured));
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    std::string          error;
    bool                 partial = false;
    UNSCOPED_INFO("clear admits an operation mark");
    REQUIRE(assembler.Clear(clear, error) == 1);
    UNSCOPED_INFO("resolve follows clear in the shared operation order");
    REQUIRE(assembler.Resolve(clear.target, copy, false, partial, error) == 2);
    const auto flush = assembler.FlushThrough(2);
    UNSCOPED_INFO("resolve-only work flushes without a draw or swap: " + flush.error);
    REQUIRE((flush.fence == 1 && completion.spans.size() == 1));
    const auto& span = completion.spans.front();
    UNSCOPED_INFO("off-screen pass clears white and resolves into raw producer output");
    REQUIRE((span.guestResolve && span.resolveMark == 2 && span.frame && span.frame->retainOnly && span.frame->returnRawOutput &&
             !span.segment && span.frame->passes.size() == 1 && span.frame->passes.front().clears.size() == 1 &&
             span.frame->passes.back().resolves.size() == 1 && !span.frame->passes.back().preservePreviousAttachments));
    UNSCOPED_INFO("off-screen graph validates: " + error);
    REQUIRE(ValidateNativeRenderFrame(*span.frame, error));
    NativeFrameAssemblerConfig drawingConfig;
    drawingConfig.mode       = NativeDrawMode::Skip;
    drawingConfig.readTarget = read;
    drawingConfig.build      = [](const NativeDrawSubmission&, NativeDrawReplayRecipe& draw, std::string&)
    {
        draw       = makeDraw(1);
        draw.width = draw.height = 1024;
        draw.sampleCount = draw.sampleMask = 1;
        draw.viewport                      = { 0, 0, 1024, 1024, 0, 1 };
        draw.scissor                       = { 0, 0, 1024, 1024 };
        return true;
    };
    FakeCompletion drawingCompletion;
    drawingConfig.completion = &drawingCompletion;
    NativeFrameAssembler drawing(drawingConfig);
    UNSCOPED_INFO("draw source starts with clear");
    REQUIRE(drawing.Clear(clear, error) == 1);
    auto draw          = submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 1);
    draw.arguments[0]  = g; // the draw decodes the cleared target
    draw.operationMark = drawing.NumberOperation();
    drawing.Began(draw);
    UNSCOPED_INFO("dispatcher numbers draws on the same counter as clears");
    REQUIRE(drawing.Recorded(draw, drawing.Admit(draw)) == 2);
    UNSCOPED_INFO("draw into off-screen source is included before resolve");
    REQUIRE((drawing.Resolve(clear.target, copy, false, partial, error) == 3 && drawing.FlushThrough(3).fence == 1));
    UNSCOPED_INFO("off-screen pass contains admitted draw");
    REQUIRE(drawingCompletion.spans[0].frame->passes[0].DrawCount() == 1);
    error.clear();
    UNSCOPED_INFO("next clear shares the counter");
    REQUIRE(drawing.Clear(clear, error) == 4);
    draw.sequence      = 2;
    draw.operationMark = drawing.NumberOperation();
    drawing.Began(draw);
    drawing.Recorded(draw, NativeDrawReason::NoNativeRoute);
    UNSCOPED_INFO("assembler refuses a guest resolve after a skipped source draw");
    REQUIRE((!drawing.Resolve(clear.target, copy, false, partial, error) && partial && refusedAs(error, "resolve_source_partial")));
    auto other = clear.target.native;
    ++other.edramBase;
    UNSCOPED_INFO("preserve identity includes EDRAM base");
    REQUIRE(other != clear.target.native);
    // A partial clear from the origin defines that rectangle; one off it
    // is refused.
    clear.clear.rectangle.left = 8;
    UNSCOPED_INFO("partial clear fails admission visibly");
    REQUIRE((!assembler.Clear(clear, error) && refusedAs(error, "clear_form_unsupported: color rect")));
    memory.erase(g + 0x2A20);
    UNSCOPED_INFO("missing copy word is not guessed");
    REQUIRE(!ReadGuestResolveCopy(read, g, copy));
}

namespace
{

constexpr std::uint32_t kSurface = 0x20000;

GuestTarget offscreen(std::uint32_t base)
{
    GuestTarget target;
    target.native = { 1024, 1024, 1, NativeDrawReplayTargetFormat::Rgba8, base };
    return target;
}

GuestClear fullClear(const GuestTarget& target)
{
    GuestClear clear;
    clear.target           = target;
    clear.flags            = 1;
    clear.clear.rectangle  = { 0, 0, static_cast<std::int32_t>(target.native.width), static_cast<std::int32_t>(target.native.height) };
    clear.clear.color      = true;
    clear.clear.colorValue = { 1, 1, 1, 1 };
    return clear;
}

GuestResolveCopy fullCopy(const GuestTarget& target)
{
    GuestResolveCopy copy;
    copy.copyControl = 0x100000;
    copy.destBase    = 0x1E3B0000;
    copy.destInfo    = kScanoutDestInfo;
    copy.rectWidth   = target.native.width;
    copy.rectHeight  = target.native.height;
    return copy;
}

// Guest color target 0 of kGraphics as ReadGuestTarget decodes it.
GuestWordReader targetReader(std::uint32_t width, std::uint32_t height, bool msaa, std::uint32_t base)
{
    std::map<std::uint32_t, std::uint32_t> words{ { kGraphics + 0x3098, kSurface },
                                                  { kGraphics + 0x2880, 0 },
                                                  { kGraphics + 0x2ABC, 0 },
                                                  { kGraphics + 0x31A4, 0xFFFFFFFF },
                                                  { kGraphics + 0x31AC, 0 },
                                                  { kSurface + 0x24, ((width - 1) << 18) | ((height - 1) << 3) },
                                                  { kSurface + 0x18, msaa ? 1U << 16 : 0U },
                                                  { kSurface + 0x1C, base } };
    return [words](std::uint32_t address, std::uint32_t& out)
    {
        const auto found = words.find(address);
        if (found == words.end())
            return false;
        out = found->second;
        return true;
    };
}

// Panels at 1280x720x4, or a 1024x1024 single-sample draw for tag 0x40 and up.
bool shapedBuilder(const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string& detail)
{
    if (!panelBuilder(value, draw, detail))
        return false;
    if (value.arguments[7] >= 0x40)
    {
        draw.width = draw.height = 1024;
        draw.sampleCount = draw.sampleMask = 1;
        draw.viewport                      = { 0, 0, 1024, 1024, 0, 1 };
        draw.scissor                       = { 0, 0, 1024, 1024 };
    }
    return true;
}

// One dispatcher draw numbered on the operation counter clears and resolves share.
std::uint64_t record(NativeFrameAssembler& assembler, std::uint64_t sequence, std::uint32_t tag, std::optional<NativeDrawReason>* verdict = nullptr)
{
    auto value          = submission(NativeDrawHook::IndexedUp, kPanelCaller, sequence, tag);
    value.operationMark = assembler.NumberOperation();
    const auto result   = submit(assembler, value);
    if (verdict)
        *verdict = result;
    return value.operationMark;
}

const NativeRenderFramePass& resolvePass(const NativeCompletionSpan& span, std::uint64_t mark)
{
    UNSCOPED_INFO("an off-screen resolve is one self-contained producer frame");
    REQUIRE((span.frame && !span.segment && span.resolveMark == mark && span.frame->retainOnly && span.frame->passes.size() == 1 &&
             span.frame->passes[0].resolves.size() == 1 && span.frame->passes[0].resolves[0].textureId == mark &&
             !span.frame->passes[0].preservePreviousAttachments));
    return span.frame->passes[0];
}

// Resolve follows operation order and replays target clear/draws.
void testResolveSourceOrder()
{
    const auto  target = offscreen(0x300);
    const auto  clear  = fullClear(target);
    const auto  copy   = fullCopy(target);
    std::string error;
    bool        partial = false;
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO("a target may be resolved twice: " + error);
        REQUIRE((assembler.Clear(clear, error) == 1 && assembler.Resolve(target, copy, false, partial, error) == 2 &&
                 assembler.Resolve(target, copy, false, partial, error) == 3));
        const auto first = assembler.FlushThrough(3);
        UNSCOPED_INFO("one flush submits both resolve frames in op-mark order: " + first.error);
        REQUIRE((first.fence == 2 && first.error.empty() && completion.spans.size() == 2 && !assembler.Failure()));
        UNSCOPED_INFO("a repeated flush submits nothing more");
        REQUIRE((assembler.FlushThrough(3).fence == 2 && completion.spans.size() == 2));
        for (std::uint64_t mark = 2; mark <= 3; ++mark)
        {
            const auto& pass = resolvePass(completion.spans[mark - 2], mark);
            UNSCOPED_INFO("each resolve frame replays the clear rather than an empty target");
            REQUIRE((pass.clears.size() == 1 && pass.clears[0].colorValue == clear.clear.colorValue && !pass.DrawCount()));
        }
    }
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        config.readTarget = targetReader(1024, 1024, false, 0x300);
        config.build      = shapedBuilder;
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO("first resolve admitted");
        REQUIRE((assembler.Clear(clear, error) == 1 && assembler.Resolve(target, copy, false, partial, error) == 2));
        UNSCOPED_INFO("a draw between resolves keeps the source modeled: " + error);
        REQUIRE((record(assembler, 1, 0x41) == 3 && assembler.Resolve(target, copy, false, partial, error) == 4));
        UNSCOPED_INFO("one flush submits both resolve frames");
        REQUIRE((assembler.FlushThrough(4).fence == 2 && completion.spans.size() == 2 && resolvePass(completion.spans[0], 2).clears.size() == 1));
        const auto& pass = resolvePass(completion.spans[1], 4);
        UNSCOPED_INFO("the later resolve replays the clear and the draw");
        REQUIRE((pass.clears.size() == 1 && pass.DrawCount() == 1 && pass.Draw(0).vertexConstants[0] == 0x41));
    }
    {
        // A clear flushed before a swap still sources a resolve after it.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO("the clear alone flushes");
        REQUIRE((assembler.Clear(clear, error) == 1 && assembler.FlushThrough(1).fence == 1));
        UNSCOPED_INFO("the swap presents nothing");
        REQUIRE(assembler.Swap(makeSwap(1)).status == NativeFrameStatus::Empty);
        UNSCOPED_INFO("the next epoch resolves the cleared target: " + error);
        REQUIRE((assembler.Resolve(target, copy, false, partial, error) == 2 && assembler.FlushThrough(2).fence == 3));
        UNSCOPED_INFO("the resolve frame replays the earlier clear");
        REQUIRE(resolvePass(completion.spans.back(), 2).clears.size() == 1);
    }
    {
        // Flushing an earlier resolve does not unmodel a later clear.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        auto                 second = clear;
        second.clear.colorValue     = { 0, 1, 0, 1 };
        UNSCOPED_INFO("the first resolve flushes after the next clear was recorded");
        REQUIRE((assembler.Clear(clear, error) == 1 && assembler.Resolve(target, copy, false, partial, error) == 2 &&
                 assembler.Clear(second, error) == 3 && assembler.FlushThrough(2).fence == 1));
        UNSCOPED_INFO("the resolve after the flushed one is admitted: " + error);
        REQUIRE((assembler.Resolve(target, copy, false, partial, error) == 4 && assembler.FlushThrough(4).fence == 2));
        UNSCOPED_INFO("it replays the later clear");
        REQUIRE(resolvePass(completion.spans[1], 4).clears[0].colorValue == second.clear.colorValue);
    }
}

// The one pass of a presented frame that holds draws.
const NativeRenderFramePass& presentedPass(const NativeCompletionSpan& span)
{
    UNSCOPED_INFO("a presented span holds its frame");
    REQUIRE(span.frame.has_value());
    const auto found = std::ranges::find_if(span.frame->passes, [](const NativeRenderFramePass& pass)
                                            {
                                                return pass.DrawCount() != 0;
                                            });
    UNSCOPED_INFO("the presented frame holds a draw pass");
    REQUIRE(found != span.frame->passes.end());
    return *found;
}

// Field-for-field equality of two recipes. Shared payloads compare by bytes,
// since each builder call makes its own owners.
bool sameRecipe(const NativeDrawReplayRecipe& a, const NativeDrawReplayRecipe& b)
{
    const auto bytes = [](std::span<const std::uint8_t> left, std::span<const std::uint8_t> right)
    {
        return std::ranges::equal(left, right);
    };
    const auto sampler = [](const NativeDrawReplaySampler& left, const NativeDrawReplaySampler& right)
    {
        return std::tie(left.minLinear, left.magLinear, left.mipLinear, left.anisotropy, left.address, left.border, left.minLod, left.maxLod, left.mipBias) ==
               std::tie(right.minLinear, right.magLinear, right.mipLinear, right.anisotropy, right.address, right.border, right.minLod, right.maxLod, right.mipBias);
    };
    const auto face = [](const NativeDrawReplayStencilState::Face& left, const NativeDrawReplayStencilState::Face& right)
    {
        return std::tie(left.compare, left.fail, left.depthFail, left.pass) == std::tie(right.compare, right.fail, right.depthFail, right.pass);
    };
    const auto version = [&](const auto& left, const auto& right)
    {
        return !left == !right && (!left || (left->Layout() == right->Layout() && bytes(*left->Payload(), *right->Payload())));
    };
    if (std::tie(a.schemaVersion, a.sourcePath, a.outputPath, a.indices, a.vertexConstants, a.pixelConstants, a.sharedConstants, a.initialSample0, a.initialSample1, a.clearColor, a.blend, a.targetFormat, a.extraTargetCount, a.extraTargets, a.width, a.height, a.sampleCount, a.sampleMask, a.vertexStrideBytes, a.vertexAttributeCount, a.indexCount, a.textureMask, a.vertexShaderHash, a.pixelShaderHash, a.depthOnly) !=
            std::tie(b.schemaVersion, b.sourcePath, b.outputPath, b.indices, b.vertexConstants, b.pixelConstants, b.sharedConstants, b.initialSample0, b.initialSample1, b.clearColor, b.blend, b.targetFormat, b.extraTargetCount, b.extraTargets, b.width, b.height, b.sampleCount, b.sampleMask, b.vertexStrideBytes, b.vertexAttributeCount, b.indexCount, b.textureMask, b.vertexShaderHash, b.pixelShaderHash, b.depthOnly) ||
        !bytes(a.vertexShaderBytes(), b.vertexShaderBytes()) || !bytes(a.pixelShaderBytes(), b.pixelShaderBytes()) || !bytes(a.vertexBytes(), b.vertexBytes()) ||
        !a.shaderVersion != !b.shaderVersion)
        return false;
    const auto& av = a.viewport;
    const auto& bv = b.viewport;
    const auto& as = a.scissor;
    const auto& bs = b.scissor;
    if (std::tie(av.x, av.y, av.width, av.height, av.minDepth, av.maxDepth) != std::tie(bv.x, bv.y, bv.width, bv.height, bv.minDepth, bv.maxDepth) ||
        std::tie(as.left, as.top, as.right, as.bottom) != std::tie(bs.left, bs.top, bs.right, bs.bottom) || !sampler(a.sampler, b.sampler) ||
        a.samplers.size() != b.samplers.size() ||
        std::tie(a.depth.enabled, a.depth.writeEnabled, a.depth.compare, a.depth.initialClear, a.depth.initialSamples) !=
            std::tie(b.depth.enabled, b.depth.writeEnabled, b.depth.compare, b.depth.initialClear, b.depth.initialSamples) ||
        std::tie(a.rasterizer.cull, a.rasterizer.frontCounterClockwise, a.rasterizer.depthClip) !=
            std::tie(b.rasterizer.cull, b.rasterizer.frontCounterClockwise, b.rasterizer.depthClip) ||
        std::tie(a.stencil.enabled, a.stencil.reference, a.stencil.readMask, a.stencil.writeMask) !=
            std::tie(b.stencil.enabled, b.stencil.reference, b.stencil.readMask, b.stencil.writeMask) ||
        !face(a.stencil.front, b.stencil.front) || !face(a.stencil.back, b.stencil.back))
        return false;
    for (std::size_t index = 0; index < a.samplers.size(); ++index)
        if (!sampler(a.samplers[index], b.samplers[index]))
            return false;
    for (std::size_t slot = 0; slot < a.textures.size(); ++slot)
    {
        const auto& left  = a.textures[slot];
        const auto& right = b.textures[slot];
        if (std::tie(left.width, left.height, left.format, left.swizzle, left.mipLevels, left.dimension, left.frameTextureId) !=
                std::tie(right.width, right.height, right.format, right.swizzle, right.mipLevels, right.dimension, right.frameTextureId) ||
            !bytes(left.payloadBytes(), right.payloadBytes()) || !version(left.textureVersion, right.textureVersion))
            return false;
    }
    return true;
}

// Shared draws reuse recipes; refusal leaves raw pass and validation error.
void testSharedRecipes()
{
    const auto  target = offscreen(0x300);
    const auto  clear  = fullClear(target);
    const auto  copy   = fullCopy(target);
    std::string error;
    bool        partial  = false;
    const auto  expected = [](std::uint32_t tag)
    {
        NativeDrawReplayRecipe draw;
        std::string            detail;
        UNSCOPED_INFO("the builder makes the draw");
        REQUIRE(shapedBuilder(submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, tag), draw, detail));
        return draw;
    };
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        config.readTarget = targetReader(1024, 1024, false, 0x300);
        config.build      = shapedBuilder;
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO("clear, draw, resolve, draw, resolve: " + error);
        REQUIRE((assembler.Clear(clear, error) == 1 && record(assembler, 1, 0x41) == 2 && assembler.Resolve(target, copy, false, partial, error) == 3 &&
                 record(assembler, 2, 0x42) == 4 && assembler.Resolve(target, copy, false, partial, error) == 5));
        UNSCOPED_INFO("two resolve frames");
        REQUIRE((assembler.FlushThrough(5).fence == 2 && completion.spans.size() == 2));
        const auto& first  = resolvePass(completion.spans[0], 3);
        const auto& second = resolvePass(completion.spans[1], 5);
        UNSCOPED_INFO("both resolve frames hold admitted owners");
        REQUIRE((first.draws.empty() && first.admittedDraws.size() == 1 && second.draws.empty() && second.admittedDraws.size() == 2));
        UNSCOPED_INFO("the later frame shares the first draw's owner instead of a copy");
        REQUIRE(second.admittedDraws[0] == first.admittedDraws[0]);
        UNSCOPED_INFO("the shared recipes equal the builder's");
        REQUIRE((sameRecipe(first.Draw(0), expected(0x41)) && sameRecipe(second.Draw(0), expected(0x41)) && sameRecipe(second.Draw(1), expected(0x42))));
    }
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        config.build      = panelBuilder;
        NativeFrameAssembler assembler(config);
        record(assembler, 1, 1);
        record(assembler, 2, 2);
        scanOut(assembler);
        const auto result = assembler.Swap(makeSwap(1, 2));
        UNSCOPED_INFO("a presented swap frame: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == 1));
        const auto& pass = presentedPass(completion.spans[0]);
        UNSCOPED_INFO("presented draws are admitted owners equal to the builder's recipes");
        REQUIRE((pass.draws.empty() && pass.admittedDraws.size() == 2 && sameRecipe(pass.Draw(0), expected(1)) && sameRecipe(pass.Draw(1), expected(2))));
    }
    // Cull mode 3 passes the assembler's target checks but not recipe
    // validation, which the assembler repeats for a frame with a raw pass. The
    // resolve pass takes a refused draw first, the presented pass last.
    const auto refused = [](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string& detail)
    {
        if (!shapedBuilder(value, draw, detail))
            return false;
        if ((value.arguments[7] & 0x0F) == 0x0C)
            draw.rasterizer.cull = 3;
        return true;
    };
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        config.readTarget = targetReader(1024, 1024, false, 0x300);
        config.build      = refused;
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO("a refused recipe still records: " + error);
        REQUIRE((assembler.Clear(clear, error) == 1 && record(assembler, 1, 0x4C) == 2 && record(assembler, 2, 0x41) == 3 &&
                 assembler.Resolve(target, copy, false, partial, error) == 4));
        // The assembler validates the raw resolve frame before submitting it.
        const auto flushed = assembler.FlushThrough(4);
        const auto failure = assembler.Failure().value_or("");
        UNSCOPED_INFO("the raw resolve pass fails its frame with the validation error: " + failure);
        REQUIRE((flushed.fence == 0 && completion.spans.empty() && failure.find("resolve_frame_invalid: unsupported replay depth or rasterizer state") != std::string::npos));
    }
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        config.build      = refused;
        NativeFrameAssembler assembler(config);
        record(assembler, 1, 1);
        record(assembler, 2, 0x0C);
        scanOut(assembler);
        // The swap validates the raw presented frame before submitting it.
        const auto result = assembler.Swap(makeSwap(1, 2));
        UNSCOPED_INFO("the raw presented pass fails the swap frame with the validation error: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && completion.spans.empty() &&
                 result.detail.find("frame_invalid: unsupported replay depth or rasterizer state") != std::string::npos));
    }
    std::cout << "Recorded draws are shared admitted recipes; a refused recipe keeps its pass raw\n";
}

// Resolve and presented work share a span; the segment waits for its frame.
void testResolveThenSegment()
{
    const auto     target = offscreen(0x300);
    FakeCompletion completion;
    completion.gateSegments = true;
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    std::string          error;
    bool                 partial = false;
    UNSCOPED_INFO("clear, resolve and an admitted draw");
    REQUIRE((assembler.Clear(fullClear(target), error) == 1 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 2 &&
             record(assembler, 1, 9) == 3));
    const auto first = assembler.FlushThrough(3);
    UNSCOPED_INFO("the resolve frame is submitted alone: " + first.error);
    REQUIRE((!first.fence && first.retry && completion.spans.size() == 1 && !assembler.Failure()));
    const auto held = assembler.FlushThrough(3);
    UNSCOPED_INFO("the segment waits for the unfinished resolve frame: " + held.error);
    REQUIRE((!held.fence && held.retry && held.error == "segment_not_ready" && completion.spans.size() == 1 && !assembler.Failure()));
    completion.completed = 1;
    const auto done      = assembler.FlushThrough(3);
    UNSCOPED_INFO("the draw follows as a segment once the frame finished: " + done.error);
    REQUIRE((done.fence == 2 && completion.spans.size() == 2 && completion.spans[1].segment && drawTags(completion.spans[1]) == std::vector<std::uint8_t>{ 9 }));
}

void testSwapAfterResolve()
{
    const auto  target = offscreen(0x300);
    std::string error;
    bool        partial = false;
    {
        // An unfinished earlier resolve frame must not fail the swap.
        FakeCompletion completion;
        completion.gateSegments = true;
        NativeFrameAssemblerConfig config;
        config.build      = panelBuilder;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO("the first resolve frame is on the GPU");
        REQUIRE((assembler.Clear(fullClear(target), error) == 1 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 2 &&
                 assembler.FlushThrough(2).fence == 1));
        record(assembler, 1, 4);
        UNSCOPED_INFO("a second resolve in the same epoch");
        REQUIRE((assembler.Clear(fullClear(target), error) == 4 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 5));
        scanOut(assembler);
        const auto result = assembler.Swap(makeSwap(1, 1));
        UNSCOPED_INFO("the swap submits the resolve frame and its own frame: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && result.hostFence == 3 && completion.spans.size() == 3 && !assembler.Failure()));
        UNSCOPED_INFO("resolve frame first, then the presented draw");
        REQUIRE((resolvePass(completion.spans[1], 5).clears.size() == 1 && drawTags(completion.spans[2]) == std::vector<std::uint8_t>{ 4 }));
    }
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO("a resolve awaits the swap");
        REQUIRE((assembler.Clear(fullClear(target), error) == 1 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 2));
        completion.closing = true;
        const auto result  = assembler.Swap(makeSwap(1));
        UNSCOPED_INFO("a closing source stops the swap without a failure: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Stopped && result.detail == "closing" && assembler.Stopped() && !assembler.Failure()));
    }
}

// A clear the assembler cannot apply still changed the guest target.
void testRejectedClearPartial()
{
    for (const auto mode : { NativeDrawMode::Skip, NativeDrawMode::Strict })
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.mode       = mode;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        const auto           target = offscreen(0x300);
        auto                 inner  = fullClear(target);
        inner.clear.rectangle.left  = 8; // off the origin, which no rectangle models
        std::string error;
        bool        partial = false;
        UNSCOPED_INFO("a partial-rectangle clear is rejected: " + error);
        REQUIRE((assembler.Clear(fullClear(target), error) == 1 && !assembler.Clear(inner, error) && refusedAs(error, "clear_form_unsupported: color rect")));
        error.clear();
        UNSCOPED_INFO("the resolve after it reports a partial source instead of encoding");
        REQUIRE((!assembler.Resolve(target, fullCopy(target), false, partial, error) && partial && refusedAs(error, "resolve_source_partial")));
        const auto other = offscreen(0x500);
        auto       fresh = fullClear(other);
        fresh.predicate  = 1;
        UNSCOPED_INFO("a predicated clear is rejected");
        REQUIRE(!assembler.Clear(fresh, error));
        error.clear();
        UNSCOPED_INFO("a rejected clear marks its own target partial");
        REQUIRE((!assembler.Resolve(other, fullCopy(other), false, partial, error) && partial));
        UNSCOPED_INFO("nothing reached the completion source");
        REQUIRE(completion.spans.empty());
    }
}

// Segments that consumed every operation leave the swap on their target.
void testMainBaseCarried()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    config.readTarget = targetReader(1280, 720, true, 0x200);
    NativeFrameAssembler assembler(config);
    record(assembler, 1, 1);
    record(assembler, 2, 2);
    UNSCOPED_INFO("both draws flush as a segment");
    REQUIRE(assembler.FlushThrough(2).fence == 1);
    scanOut(assembler, 0x200);
    const auto result = assembler.Swap(makeSwap(1, 2));
    UNSCOPED_INFO("the swap closes the epoch: " + result.detail);
    REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == 2));
    const auto& pass = completion.spans[1].frame->passes.front();
    UNSCOPED_INFO("the final pass continues the segment's decoded target");
    REQUIRE((pass.target.edramBase == 0x200 && pass.preservePreviousAttachments && pass.resolves.size() == 1));
}

void testMainKeptAcrossResolve()
{
    const auto  target = offscreen(0x600);
    std::string error;
    bool        partial = false;
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.build      = panelBuilder;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        record(assembler, 1, 1);
        record(assembler, 2, 2);
        UNSCOPED_INFO("an off-screen resolve after presented draws");
        REQUIRE((assembler.Clear(fullClear(target), error) == 3 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 4));
        record(assembler, 3, 5);
        scanOut(assembler);
        const auto result = assembler.Swap(makeSwap(1, 3));
        UNSCOPED_INFO("resolve frame and swap frame: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == 2));
        UNSCOPED_INFO("the resolve frame holds only its own target");
        REQUIRE(!resolvePass(completion.spans[0], 4).DrawCount());
        UNSCOPED_INFO("the presented frame keeps the draws before the resolve");
        REQUIRE(drawTags(completion.spans[1]) == std::vector<std::uint8_t>{ 1, 2, 5 });
    }
    for (const auto mode : { NativeDrawMode::Strict, NativeDrawMode::Skip })
    {
        // A resolve frame ends the chain holding submitted segments; the swap
        // frame replays their draws instead of presenting without them.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.mode       = mode;
        config.build      = panelBuilder;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        record(assembler, 1, 1);
        UNSCOPED_INFO("a presented segment");
        REQUIRE(assembler.FlushThrough(1).fence == 1);
        UNSCOPED_INFO("then an off-screen resolve");
        REQUIRE((assembler.Clear(fullClear(target), error) == 2 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 3));
        record(assembler, 2, 4);
        scanOut(assembler);
        const auto result = assembler.Swap(makeSwap(1, 2));
        UNSCOPED_INFO("segment, resolve frame and a whole swap frame: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == 3 && result.detail.empty()));
        const auto& last = completion.spans.back();
        UNSCOPED_INFO("the swap frame replays the segment's draw on a fresh target");
        REQUIRE((drawTags(last) == std::vector<std::uint8_t>{ 1, 4 } && !last.frame->passes.front().preservePreviousAttachments &&
                 last.assumptions.clearAssumed));
        UNSCOPED_INFO("the replayed draw keeps its segment's fence");
        REQUIRE(assembler.FlushThrough(1).fence == 1);
    }
    {
        // The same loss through the ledger's flushes: the next segment replays.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.build      = panelBuilder;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        record(assembler, 1, 1);
        UNSCOPED_INFO("a presented segment");
        REQUIRE(assembler.FlushThrough(1).fence == 1);
        UNSCOPED_INFO("a flushed resolve frame");
        REQUIRE((assembler.Clear(fullClear(target), error) == 2 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 3 &&
                 assembler.FlushThrough(3).fence == 2));
        const auto mark = record(assembler, 2, 4);
        UNSCOPED_INFO("the next draw flushes as a segment");
        REQUIRE(assembler.FlushThrough(mark).fence == 3);
        const auto& segment = completion.spans[2];
        UNSCOPED_INFO("the new chain replays the earlier segment before its own draw");
        REQUIRE((segment.segment && drawTags(segment) == std::vector<std::uint8_t>{ 1, 4 } &&
                 !segment.frame->passes.front().preservePreviousAttachments && segment.firstSequence == mark));
        record(assembler, 3, 6);
        scanOut(assembler);
        const auto result = assembler.Swap(makeSwap(1, 3));
        UNSCOPED_INFO("the swap continues the replaying chain without replaying again: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && drawTags(completion.spans.back()) == std::vector<std::uint8_t>{ 6 } &&
                 completion.spans.back().frame->passes.front().preservePreviousAttachments));
    }
}

// The graph frame the D3D12 source would queue at the swap: every segment's
// passes and textures, then the final span's.
NativeRenderFrame graphOf(const FakeCompletion& completion)
{
    NativeRenderFrame graph;
    for (const auto& span : completion.spans)
    {
        UNSCOPED_INFO("every graph span names identity attachments");
        REQUIRE((span.frame && span.frame->identityAttachments));
        graph.passes.insert(graph.passes.end(), span.frame->passes.begin(), span.frame->passes.end());
        for (const auto& texture : span.frame->textures)
            if (std::none_of(graph.textures.begin(), graph.textures.end(), [&](const auto& known)
                             {
                                 return known.id == texture.id;
                             }))
                graph.textures.push_back(texture);
        if (!span.segment)
        {
            graph.outputTextureId      = span.frame->outputTextureId;
            graph.gammaTable           = span.frame->gammaTable;
            graph.outputSwizzle        = span.frame->outputSwizzle;
            graph.returnGpuOutput      = span.frame->returnGpuOutput;
            graph.returnResolvedOutput = span.frame->returnResolvedOutput;
            graph.retainOnly           = span.frame->retainOnly;
            graph.returnRawOutput      = span.frame->returnRawOutput;
        }
    }
    graph.identityAttachments = true;
    return graph;
}

// The off-screen pass of a graph segment that reads `mark` back.
const NativeRenderFramePass& graphPass(const NativeCompletionSpan& span, std::uint64_t mark)
{
    UNSCOPED_INFO("a graph segment names the resolve");
    REQUIRE((span.frame && span.segment && std::any_of(span.resolves.begin(), span.resolves.end(), [&](const NativeSpanResolve& resolve)
                                                       {
                                                           return resolve.mark == mark;
                                                       })));
    for (const auto& pass : span.frame->passes)
        for (const auto& resolve : pass.resolves)
            if (resolve.readback && !pass.presented &&
                std::any_of(span.resolves.begin(), span.resolves.end(), [&](const NativeSpanResolve& named)
                            {
                                return named.mark == mark && named.textureId == resolve.textureId;
                            }))
                return pass;
    throw std::runtime_error("graph segment has no readback pass for the resolve");
}

// resolveGraph joins off-screen segments; presented work has one terminal.
void testResolveGraph()
{
    const auto  target = offscreen(0x300);
    const auto  clear  = fullClear(target);
    const auto  copy   = fullCopy(target);
    std::string error;
    bool        partial = false;
    {
        // An unchanged source's second resolve records no draws.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion   = &completion;
        config.resolveGraph = true;
        config.timing       = true;
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO("a target resolved twice: " + error);
        REQUIRE((assembler.Clear(clear, error) == 1 && assembler.Resolve(target, copy, false, partial, error) == 2 &&
                 assembler.Resolve(target, copy, false, partial, error) == 3));
        const auto flushed = assembler.FlushThrough(3);
        UNSCOPED_INFO("one flush submits one segment: " + flushed.error);
        REQUIRE((flushed.fence == 1 && completion.spans.size() == 1 && !assembler.Failure()));
        const auto& span = completion.spans[0];
        UNSCOPED_INFO("the segment names both resolves in op-mark order");
        REQUIRE((span.segment && span.resolves.size() == 2 && span.resolves[0].mark == 2 && span.resolves[1].mark == 3 && span.resolves[0].copy.destBase == copy.destBase));
        const auto& first  = graphPass(span, 2);
        const auto& second = graphPass(span, 3);
        UNSCOPED_INFO("the first resolve replays the clear; the second continues its attachments");
        REQUIRE((first.clears.size() == 1 && !first.preservePreviousAttachments && second.clears.empty() && second.preservePreviousAttachments && !second.DrawCount()));
        UNSCOPED_INFO("graph destinations are frame textures");
        REQUIRE(std::none_of(span.frame->textures.begin(), span.frame->textures.end(), [](const auto& texture)
                             {
                                 return texture.persistentKey;
                             }));
        const auto stats = assembler.ResolveGraphStats();
        UNSCOPED_INFO("timing counts one replay and one continuation");
        REQUIRE((stats.replays == 1 && stats.continuations == 1 && !stats.notReady && !stats.swapBatch));
    }
    {
        // A draw between resolves is the continuation's only draw.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion   = &completion;
        config.readTarget   = targetReader(1024, 1024, false, 0x300);
        config.build        = shapedBuilder;
        config.resolveGraph = true;
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO("clear, resolve, draw, resolve: " + error);
        REQUIRE((assembler.Clear(clear, error) == 1 && assembler.Resolve(target, copy, false, partial, error) == 2 && record(assembler, 1, 0x41) == 3 &&
                 assembler.Resolve(target, copy, false, partial, error) == 4));
        UNSCOPED_INFO("one segment");
        REQUIRE((assembler.FlushThrough(4).fence == 1 && completion.spans.size() == 1));
        const auto& later = graphPass(completion.spans[0], 4);
        UNSCOPED_INFO("the continuation records only the new draw");
        REQUIRE((later.preservePreviousAttachments && later.clears.empty() && later.DrawCount() == 1 && later.Draw(0).vertexConstants[0] == 0x41));
        UNSCOPED_INFO("coverage counts the recorded draws");
        REQUIRE(completion.spans[0].coverage.admitted == 1);
        const auto stats = assembler.ResolveGraphStats();
        UNSCOPED_INFO("without timing nothing is counted");
        REQUIRE((!stats.replays && !stats.continuations));
    }
    {
        // A guest fence wait before its swap flushes presented work and a
        // resolve mid-epoch; the swap frame resumes the presented target
        // after the off-screen pass, and the epoch has one terminal.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.build        = panelBuilder;
        config.completion   = &completion;
        config.resolveGraph = true;
        NativeFrameAssembler assembler(config);
        record(assembler, 1, 1);
        UNSCOPED_INFO("a presented segment for the fence wait");
        REQUIRE(assembler.FlushThrough(1).fence == 1);
        UNSCOPED_INFO("the resolve flushes as the next segment");
        REQUIRE((assembler.Clear(fullClear(target), error) == 2 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 3 &&
                 assembler.FlushThrough(3).fence == 2));
        const auto& segment = completion.spans[1];
        UNSCOPED_INFO("the segment holds only the off-screen pass; the chain keeps the presented work");
        REQUIRE((segment.segment && segment.resolves.size() == 1 && segment.frame->passes.size() == 1 && !segment.frame->passes[0].presented));
        record(assembler, 2, 4);
        scanOut(assembler);
        const auto result = assembler.Swap(makeSwap(1, 2));
        UNSCOPED_INFO("two segments and the swap frame: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && result.hostFence == 3 && completion.spans.size() == 3 && result.detail.empty()));
        const auto& last = completion.spans.back();
        UNSCOPED_INFO("the swap frame resumes the presented target without replaying the first draw");
        REQUIRE((!last.segment && drawTags(last) == std::vector<std::uint8_t>{ 4 } && last.frame->passes.front().resumeAttachments &&
                 !last.frame->passes.front().preservePreviousAttachments));
        const auto graph = graphOf(completion);
        UNSCOPED_INFO("the epoch graph validates: " + error);
        REQUIRE(ValidateNativeRenderFrame(graph, error));
        UNSCOPED_INFO("flushed operations keep their segment fences");
        REQUIRE((assembler.FlushThrough(1).fence == 1 && assembler.FlushThrough(3).fence == 2));
    }
    {
        // Resolves still open at the swap go first as one segment; the
        // presented work before them stays in the swap frame.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.build        = panelBuilder;
        config.completion   = &completion;
        config.resolveGraph = true;
        NativeFrameAssembler assembler(config);
        record(assembler, 1, 1);
        UNSCOPED_INFO("presented work, then two resolves");
        REQUIRE((assembler.Clear(fullClear(target), error) == 2 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 3 &&
                 assembler.Resolve(target, fullCopy(target), false, partial, error) == 4));
        record(assembler, 2, 5);
        scanOut(assembler);
        const auto result = assembler.Swap(makeSwap(1, 2));
        UNSCOPED_INFO("one resolve segment and the swap frame: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == 2 && completion.spans[0].segment &&
                 completion.spans[0].resolves.size() == 2 && !completion.spans[1].segment));
        UNSCOPED_INFO("the swap frame draws the presented work on its own target");
        REQUIRE((drawTags(completion.spans[1]) == std::vector<std::uint8_t>{ 1, 5 } && !completion.spans[1].frame->passes.front().resumeAttachments));
        UNSCOPED_INFO("the epoch graph validates: " + error);
        REQUIRE(ValidateNativeRenderFrame(graphOf(completion), error));
    }
    {
        // Off-screen work alone ends its chain without presenting.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion   = &completion;
        config.resolveGraph = true;
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO("a resolve segment");
        REQUIRE((assembler.Clear(clear, error) == 1 && assembler.Resolve(target, copy, false, partial, error) == 2 && assembler.FlushThrough(2).fence == 1));
        const auto result = assembler.Swap(makeSwap(1));
        UNSCOPED_INFO("the swap ends the chain without presenting: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Empty && result.hostFence == 2 && completion.spans.size() == 2 && !assembler.Failure()));
        const auto& ending = *completion.spans[1].frame;
        UNSCOPED_INFO("a retained frame continues the off-screen attachments");
        REQUIRE((!completion.spans[1].segment && ending.retainOnly && !ending.returnGpuOutput && ending.passes.size() == 1 && ending.passes[0].preservePreviousAttachments));
        UNSCOPED_INFO("the closed chain validates: " + error);
        REQUIRE(ValidateNativeRenderFrame(graphOf(completion), error));
    }
    {
        // A source that cannot cut a segment takes the resolve as its own frame.
        FakeCompletion completion;
        completion.gateSegments = true;
        NativeFrameAssemblerConfig config;
        config.build        = panelBuilder;
        config.completion   = &completion;
        config.resolveGraph = true;
        config.timing       = true;
        NativeFrameAssembler assembler(config);
        record(assembler, 1, 1);
        scanOut(assembler);
        UNSCOPED_INFO("a swap frame left on the GPU");
        REQUIRE(assembler.Swap(makeSwap(1, 1)).status == NativeFrameStatus::Provisional);
        UNSCOPED_INFO("the resolve still flushes: " + error);
        REQUIRE((assembler.Clear(fullClear(target), error) == 3 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 4 &&
                 assembler.FlushThrough(4).fence == 2));
        const auto& fallback = completion.spans.back();
        UNSCOPED_INFO("the resolve falls back to its own identity-keyed producer frame");
        REQUIRE((!fallback.segment && fallback.resolveMark == 4 && fallback.frame->retainOnly && fallback.frame->identityAttachments && fallback.resolves.empty()));
        const auto stats = assembler.ResolveGraphStats();
        UNSCOPED_INFO("timing counts the unready fallback");
        REQUIRE((stats.notReady == 1 && !stats.replays && !stats.signed16 && !stats.swapBatch));
    }
}

// Eager graph segments submit on record when enabled; unready stays pending.
void testEagerGraphSegments()
{
    const auto  target = offscreen(0x300);
    std::string error;
    bool        partial = false;
    for (const bool gated : { false, true })
    {
        FakeCompletion completion;
        completion.gateSegments = gated;
        completion.terminal     = gated ? 1 : 0;
        NativeFrameAssemblerConfig config;
        config.completion         = &completion;
        config.resolveGraph       = true;
        config.eagerGraphSegments = true;
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO("clear and resolve: " + error);
        REQUIRE((assembler.Clear(fullClear(target), error) == 1 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 2));
        if (gated)
        {
            UNSCOPED_INFO("an unready source submits nothing at the resolve");
            REQUIRE(completion.spans.empty());
            continue;
        }
        UNSCOPED_INFO("the resolve submits its segment as it is recorded");
        REQUIRE((completion.spans.size() == 1 && completion.spans[0].segment && completion.spans[0].resolves.size() == 1 && completion.spans[0].resolves[0].mark == 2));
        UNSCOPED_INFO("the read's flush finds the segment submitted");
        REQUIRE((assembler.FlushThrough(2).fence == 1 && completion.spans.size() == 1 && !assembler.Failure()));
        UNSCOPED_INFO("the next resolve continues in its own segment: " + error);
        REQUIRE((assembler.Resolve(target, fullCopy(target), false, partial, error) == 3 && completion.spans.size() == 2 && completion.spans[1].segment));
        const auto result = assembler.Swap(makeSwap(1));
        UNSCOPED_INFO("the swap ends the chain: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Empty && !assembler.Failure()));
        UNSCOPED_INFO("the eager chain validates: " + error);
        REQUIRE(ValidateNativeRenderFrame(graphOf(completion), error));
    }
}

// Census graph swap failure still ends the chain; the next epoch presents.
void testGraphCensusRecovery()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.mode         = NativeDrawMode::Census;
    config.build        = panelBuilder;
    config.completion   = &completion;
    config.resolveGraph = true;
    NativeFrameAssembler assembler(config);
    const auto           target = offscreen(0x300);
    std::string          error;
    bool                 partial = false;
    UNSCOPED_INFO("a graph resolve segment: " + error);
    REQUIRE((assembler.Clear(fullClear(target), error) == 1 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 2 &&
             assembler.FlushThrough(2).fence == 1 && completion.spans.size() == 1 && completion.spans[0].segment));
    record(assembler, 1, 5);
    const auto failed = assembler.Swap(makeSwap(1, 1)); // presented work without a scanout
    UNSCOPED_INFO("census fails only the epoch: " + failed.detail);
    REQUIRE((failed.status == NativeFrameStatus::Failed && failed.detail.find("swap_scanout_unmodeled") != std::string::npos && !assembler.Failure()));
    UNSCOPED_INFO("the failed epoch still ends its segment chain");
    REQUIRE(completion.spans.size() == 2);
    const auto& ending = completion.spans[1];
    UNSCOPED_INFO("the ending continues the off-screen pass the segment left");
    REQUIRE((!ending.segment && ending.frame && ending.frame->retainOnly && ending.frame->passes.size() == 1 && ending.frame->passes[0].preservePreviousAttachments &&
             ending.frame->passes[0].target.edramBase == 0x300));
    record(assembler, 2, 6);
    scanOut(assembler);
    const auto later = assembler.Swap(makeSwap(2, 2));
    UNSCOPED_INFO("the next epoch presents: " + later.detail);
    REQUIRE((later.status == NativeFrameStatus::Provisional && later.hostFence && completion.spans.size() == 3));
}

// Target decode is reused unchanged and rerun after one decode word changes.
void testTargetDecodeReuse()
{
    auto                       words = std::make_shared<std::map<std::uint32_t, std::uint32_t>>(std::map<std::uint32_t, std::uint32_t>{
        { kGraphics + 0x3098, kSurface },
        { kGraphics + 0x309C, 0 },
        { kGraphics + 0x30A0, 0 },
        { kGraphics + 0x30A4, 0 },
        { kGraphics + 0x2880, 0 },
        { kGraphics + 0x2ABC, 0 },
        { kGraphics + 0x31A4, 0xFFFFFFFF },
        { kGraphics + 0x31AC, 0 },
        { kSurface + 0x24, ((1280U - 1) << 18) | ((720U - 1) << 3) },
        { kSurface + 0x18, 1U << 16 },
        { kSurface + 0x1C, 0 } });
    auto                       reads = std::make_shared<unsigned>(0), views = std::make_shared<unsigned>(0);
    auto                       page = std::make_shared<std::vector<std::uint8_t>>();
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.mode       = NativeDrawMode::Skip;
    config.build      = shapedBuilder;
    config.completion = &completion;
    config.readTarget = [words, reads](std::uint32_t address, std::uint32_t& out)
    {
        ++*reads;
        const auto found = words->find(address);
        if (found == words->end())
            return false;
        out = found->second;
        return true;
    };
    config.viewTarget = [words, views, page](std::uint32_t address, std::uint32_t size) -> std::span<const std::uint8_t>
    {
        ++*views;
        page->assign(size, 0);
        for (const auto& [at, word] : *words)
            if (at >= address && at - address <= size - 4)
                for (unsigned i = 0; i < 4; ++i)
                    (*page)[at - address + i] = static_cast<std::uint8_t>(word >> (24 - 8 * i));
        return *page;
    };
    NativeFrameAssembler            assembler(config);
    std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
    record(assembler, 1, 7, &verdict);
    UNSCOPED_INFO("a panel on the decoded presented target is admitted");
    REQUIRE(!verdict);
    const auto decodeReads = *reads;
    record(assembler, 2, 7, &verdict);
    UNSCOPED_INFO("an unchanged target reuses its decode through views alone");
    REQUIRE((!verdict && *reads == decodeReads && *views > 0));
    // Predication outside tiling limits the draw to some tiles.
    (*words)[kGraphics + 0x31AC] = 1;
    record(assembler, 3, 7, &verdict);
    UNSCOPED_INFO("a changed tiling word decodes again and refuses the draw");
    REQUIRE((verdict == NativeDrawReason::NoNativeRoute && *reads > decodeReads));
    std::cout << "Target decodes are reused while the words they read are unchanged\n";
}

// Decoded targets add routing; unreadable state does not regress shaped draws.
void testUndecodedTargets()
{
    for (const int variant : { 0, 1, 2, 3 })
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.mode       = NativeDrawMode::Skip;
        config.build      = shapedBuilder;
        config.completion = &completion;
        if (variant == 1)
            // The target words are unreadable; the tiling words are not.
            config.readTarget = [](std::uint32_t address, std::uint32_t& out)
            {
                out = address == kGraphics + 0x31A4 ? 0xFFFFFFFF : 0;
                return address == kGraphics + 0x2ABC || address == kGraphics + 0x31A4 || address == kGraphics + 0x31AC;
            };
        else if (variant == 2)
            config.readTarget = targetReader(1024, 1024, false, 0x300);
        else if (variant == 3)
            config.readTarget = targetReader(1280, 720, false, 0); // another shape on the presented base
        NativeFrameAssembler            assembler(config);
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        std::string                     error;
        bool                            partial = false;
        const auto                      target  = offscreen(0x300);
        std::uint64_t                   marks   = 0;
        if (variant == 2)
        {
            // Another base only differs once an earlier epoch named the presented one.
            GuestTarget main;
            main.native = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
            UNSCOPED_INFO("an earlier epoch clears the presented base: " + error);
            REQUIRE(assembler.Clear(fullClear(main), error) == 1);
            scanOut(assembler);
            UNSCOPED_INFO("an earlier epoch names the presented base");
            REQUIRE(assembler.Swap(makeSwap(1)).status == NativeFrameStatus::Provisional);
            marks = 2;
        }
        UNSCOPED_INFO("an off-screen source is cleared");
        REQUIRE(assembler.Clear(fullClear(target), error) == 1 + marks);
        record(assembler, 1, 7, &verdict);
        if (variant == 2)
        {
            // The guest drew into the cleared target, not the presented one.
            UNSCOPED_INFO("a panel the decode places on another base is not put on main");
            REQUIRE(verdict == NativeDrawReason::NoNativeRoute);
            UNSCOPED_INFO("the dropped draw leaves its decoded target partial");
            REQUIRE((!assembler.Resolve(target, fullCopy(target), false, partial, error) && partial && refusedAs(error, "resolve_source_partial")));
            const auto result = assembler.Swap(makeSwap(2, 1));
            UNSCOPED_INFO("the skip is labeled with its cause and nothing is presented: " + result.detail);
            REQUIRE((result.status == NativeFrameStatus::Partial && result.detail.find("frame_target_mismatch") != std::string::npos &&
                     !completion.spans.back().frame));
            continue;
        }
        UNSCOPED_INFO("a panel is admitted when its decode does not name another target");
        REQUIRE(!verdict);
        if (variant != 3)
        {
            record(assembler, 2, 0x42, &verdict);
            UNSCOPED_INFO("an off-screen draw without its decoded target is not guessed");
            REQUIRE(verdict == NativeDrawReason::NoNativeRoute);
            continue;
        }
        scanOut(assembler);
        const auto result = assembler.Swap(makeSwap(1, 1));
        UNSCOPED_INFO("the panel renders on the presented target: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && drawTags(completion.spans.back()) == std::vector<std::uint8_t>{ 7 }));
    }
}

// Undecoded guest clear writes EDRAM; later resolve must not replay pre-clear.
void testUnreadableClear()
{
    for (const bool decoded : { true, false })
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.mode       = NativeDrawMode::Skip;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        const auto           target = offscreen(0x300);
        std::string          error;
        bool                 partial = false;
        UNSCOPED_INFO("a modeled clear and resolve");
        REQUIRE((assembler.Clear(fullClear(target), error) == 1 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 2));
        assembler.ClearUnreadable(decoded ? std::optional<GuestTarget>(target) : std::nullopt);
        UNSCOPED_INFO(std::string("the next resolve reports a partial source, ") + (decoded ? "decoded" : "undecoded"));
        REQUIRE((!assembler.Resolve(target, fullCopy(target), false, partial, error) && partial && refusedAs(error, "resolve_source_partial")));
        error.clear();
        UNSCOPED_INFO("a later full clear models the source again: " + error);
        REQUIRE((assembler.Clear(fullClear(target), error) == 3 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 4 && !partial));
    }
    // A decoded clear of another target ends the aliased source.
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    const auto           target = offscreen(0x300);
    std::string          error;
    bool                 partial = false;
    UNSCOPED_INFO("a modeled clear");
    REQUIRE(assembler.Clear(fullClear(target), error) == 1);
    assembler.ClearUnreadable(offscreen(0x500));
    UNSCOPED_INFO("a resolve after another target's unreadable clear is unmodeled");
    REQUIRE((!assembler.Resolve(target, fullCopy(target), false, partial, error) && refusedAs(error, "resolve_source_unmodeled: other_source")));
}

// Frontbuffer resolves are host-only, so only the presented base and format may be scanned out.
void testFrontbufferSource()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    GuestTarget          main;
    main.native = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0x200 };
    std::string error;
    bool        partial = false;
    UNSCOPED_INFO("the presented target is cleared at its base: " + error);
    REQUIRE(assembler.Clear(fullClear(main), error) == 1);
    for (const auto& source : { GuestTarget{ { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0x300 } }, GuestTarget{ { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0x300 } }, GuestTarget{ { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgb10a2, 0x200 } } })
    {
        error.clear();
        UNSCOPED_INFO("a scanout copy of another base or format fails visibly: " + error);
        REQUIRE((!assembler.Resolve(source, scanoutCopy(), true, partial, error) && !partial && error.starts_with("resolve_frontbuffer_source")));
    }
    // Any decoded extent on the presented base and format scans out.
    for (const auto& source : { offscreen(0x200), GuestTarget{ { 1280, 720, 1, NativeDrawReplayTargetFormat::Rgba8, 0x200 } } })
    {
        error.clear();
        UNSCOPED_INFO("a scanout copy of the presented base passes: " + error);
        REQUIRE((assembler.Resolve(source, scanoutCopy(), true, partial, error) && error.empty()));
    }
    error.clear();
    UNSCOPED_INFO("the presented target is scanned out: " + error);
    REQUIRE((assembler.Resolve(main, scanoutCopy(), true, partial, error) && error.empty()));
    const auto result = assembler.Swap(makeSwap(1));
    UNSCOPED_INFO("the swap presents the cleared target: " + result.detail);
    REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.back().frame->passes.front().clears.size() == 1));
}

// Scanout maps guest 640x720 2X base 0 to presented 1280x720x4 storage.
void testFrontbufferDecodedExtent()
{
    for (const auto mode : { NativeDrawMode::Skip, NativeDrawMode::Strict })
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.mode       = mode;
        config.build      = shapedBuilder;
        config.completion = &completion;
        config.readTarget = targetReader(640, 720, true, 0);
        NativeFrameAssembler            assembler(config);
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        std::string                     error;
        bool                            partial = false;
        GuestTarget                     boot;
        boot.native = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
        record(assembler, 1, 7, &verdict);
        UNSCOPED_INFO("the draw on the decoded boot target is admitted");
        REQUIRE(!verdict);
        auto other   = boot;
        other.native = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0x300 };
        UNSCOPED_INFO("another base fails: " + error);
        REQUIRE((!assembler.Resolve(other, scanoutCopy(), true, partial, error) && error.starts_with("resolve_frontbuffer_source")));
        error.clear();
        other.native = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgb10a2, 0 };
        UNSCOPED_INFO("another format fails: " + error);
        REQUIRE((!assembler.Resolve(other, scanoutCopy(), true, partial, error) && error.starts_with("resolve_frontbuffer_source")));
        error.clear();
        UNSCOPED_INFO("the boot target is scanned out: " + error);
        REQUIRE((assembler.Resolve(boot, scanoutCopy(), true, partial, error) == 2 && error.empty()));
        const auto result = assembler.Swap(makeSwap(1, 1));
        UNSCOPED_INFO("the swap presents the draw: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && drawTags(completion.spans.back()) == std::vector<std::uint8_t>{ 7 }));
    }
}

// The presented base is unset, not 0, until a decoded operation names it.
void testPresentedBaseUnset()
{
    GuestTarget boot;
    boot.native = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0x200 };
    {
        // A decode on a non-zero base with another shape is the presented target.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.build      = shapedBuilder;
        config.completion = &completion;
        config.readTarget = targetReader(640, 720, true, 0x200);
        NativeFrameAssembler            assembler(config);
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        std::string                     error;
        bool                            partial = false;
        record(assembler, 1, 7, &verdict);
        UNSCOPED_INFO("the first decode names the presented base");
        REQUIRE(!verdict);
        auto zero   = boot;
        zero.native = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
        UNSCOPED_INFO("base 0 is not presented once 0x200 is named");
        REQUIRE(!assembler.Resolve(zero, scanoutCopy(), true, partial, error));
        error.clear();
        UNSCOPED_INFO("the adopted base scans out: " + error);
        REQUIRE(assembler.Resolve(boot, scanoutCopy(), true, partial, error) == 2);
    }
    {
        // A scanout resolve names the base when nothing did, and then holds it.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        std::string          error;
        bool                 partial = false;
        UNSCOPED_INFO("an unset base accepts the first scanout: " + error);
        REQUIRE(assembler.Resolve(boot, scanoutCopy(), true, partial, error) == 1);
        auto other   = boot;
        other.native = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0x300 };
        UNSCOPED_INFO("a different base then fails: " + error);
        REQUIRE((!assembler.Resolve(other, scanoutCopy(), true, partial, error) && error.starts_with("resolve_frontbuffer_source")));
        // That refused copy still wrote the frontbuffer; scan the named base again.
        error.clear();
        UNSCOPED_INFO("the named base scans out again: " + error);
        REQUIRE(assembler.Resolve(boot, scanoutCopy(), true, partial, error) == 2);
        const auto result = assembler.Swap(makeSwap(1));
        UNSCOPED_INFO("the swap frame uses the named base: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.back().frame->passes.front().target.edramBase == 0x200));
    }
    {
        // A draw without a decode says nothing about the base.
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.build      = shapedBuilder;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        std::string          error;
        bool                 partial = false;
        record(assembler, 1, 7);
        UNSCOPED_INFO("an undecoded draw leaves the base unset: " + error);
        REQUIRE(assembler.Resolve(boot, scanoutCopy(), true, partial, error) == 2);
    }
}

// Skip releases presented clear as partial; strict fails ledger.
void testSkippedPresentedClear()
{
    for (const auto mode : { NativeDrawMode::Skip, NativeDrawMode::Strict })
    {
        for (const bool clearFirst : { false, true })
        {
            // Variant 0 hits the presented target by shape and 1 is its unreadable
            // clear; 2 hits it by base alone; 3 is another target; 4 is an
            // unreadable clear without a decode, which could hit anything.
            for (const int variant : { 0, 1, 2, 3, 4 })
            {
                FakeCompletion             completion;
                NativeFrameAssemblerConfig config;
                config.mode       = mode;
                config.build      = shapedBuilder;
                config.completion = &completion;
                config.readTarget = targetReader(640, 720, true, 0x200);
                NativeFrameAssembler assembler(config);
                std::string          error;
                GuestTarget          main;
                main.native = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0x200 };
                if (!clearFirst)
                    record(assembler, 1, 7);
                if (variant == 1)
                    assembler.ClearUnreadable(main);
                else if (variant == 4)
                    assembler.ClearUnreadable(std::nullopt);
                else
                {
                    auto clear                 = fullClear(variant == 0 ? main : offscreen(variant == 2 ? 0x200 : 0x500));
                    clear.clear.rectangle.left = 8;
                    UNSCOPED_INFO("a partial-rectangle clear is rejected");
                    REQUIRE(!assembler.Clear(clear, error));
                }
                if (clearFirst)
                    record(assembler, 1, 7);
                scanOut(assembler, 0x200);
                const auto result  = assembler.Swap(makeSwap(1, 1));
                const bool partial = mode == NativeDrawMode::Skip && variant != 3;
                UNSCOPED_INFO(std::string("skip mode marks a lost presented clear partial and nothing else, variant ") + std::to_string(variant) +
                              (clearFirst ? " clear first: " : " draw first: ") + result.detail);
                REQUIRE((result.status == (partial ? NativeFrameStatus::Partial : NativeFrameStatus::Provisional) &&
                         (!partial || result.detail.starts_with("clear: ")) && completion.spans.back().frame));
            }
        }
    }
}

// Resolve clear leaves source unknown; replay must not use pre-clear bytes.
void testResolveClearsSource()
{
    const auto  target = offscreen(0x300);
    std::string error;
    bool        partial = false;
    // Color clear, depth clear, and the title's tile-0 scanout control (both);
    // a scanout also carries its k01 sample select.
    for (const std::uint32_t control : { 0x00100100U, 0x00100200U, 0x00100300U })
    {
        for (const bool frontbuffer : { true, false })
        {
            FakeCompletion             completion;
            NativeFrameAssemblerConfig config;
            config.completion = &completion;
            NativeFrameAssembler assembler(config);
            auto                 copy = frontbuffer ? scanoutCopy() : fullCopy(target);
            copy.copyControl          = frontbuffer ? control | 0x40 : control;
            UNSCOPED_INFO("a modeled clear");
            REQUIRE(assembler.Clear(fullClear(target), error) == 1);
            error.clear();
            const auto mark = assembler.Resolve(target, copy, frontbuffer, partial, error);
            // An off-screen form with clear bits is unsupported, yet the guest still ran it.
            UNSCOPED_INFO("the clearing resolve is admitted or refused as before: " + error);
            REQUIRE((frontbuffer ? mark == 2 && error.empty() : !mark && refusedAs(error, std::format("resolve_form_unsupported: control {:08X} samples 1", control))));
            error.clear();
            UNSCOPED_INFO(std::string("the source is unmodeled after a clearing ") + (frontbuffer ? "scanout" : "off-screen") + " resolve: " + error);
            REQUIRE((!assembler.Resolve(target, fullCopy(target), false, partial, error) && partial && refusedAs(error, "resolve_source_unmodeled: uninitialized")));
            error.clear();
            UNSCOPED_INFO("a later full clear models the source again: " + error);
            REQUIRE((assembler.Clear(fullClear(target), error) == (frontbuffer ? 3U : 2U) &&
                     assembler.Resolve(target, fullCopy(target), false, partial, error) && !partial));
        }
    }
    // Without clear bits the scanout copy leaves the source's content intact.
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    auto                 copy = scanoutCopy();
    copy.copyControl          = 0x00100040U;
    UNSCOPED_INFO("a scanout copy without clear bits keeps the source: " + error);
    REQUIRE((assembler.Clear(fullClear(target), error) == 1 && assembler.Resolve(target, copy, true, partial, error) == 2 &&
             assembler.Resolve(target, fullCopy(target), false, partial, error) == 3 && !partial));
}

void testGuestImageForwarding()
{
    FakeCompletion completion;
    completion.image = { std::nullopt, "fixture_closing", true };
    NativeFrameAssemblerConfig config;
    config.completion = &completion;
    const auto take   = NativeFrameAssembler(config).TakeGuestImage(5);
    UNSCOPED_INFO("the source's take reaches the ledger unchanged");
    REQUIRE((take.closing && take.error == "fixture_closing" && !take.image));
    const auto missing = NativeFrameAssembler(NativeFrameAssemblerConfig{}).TakeGuestImage(5);
    UNSCOPED_INFO("no source names itself");
    REQUIRE((!missing.closing && !missing.image && missing.error == "no_completion_source"));
    NativeFrameAssembler forwarding(config);
    static_cast<completion::IDrawCompletion&>(forwarding).DiscardGuestImagesThrough(7);
    UNSCOPED_INFO("the ledger's discard reaches the completion source");
    REQUIRE(completion.discarded == std::vector<std::uint64_t>{ 7 });
    NativeFrameAssembler(NativeFrameAssemblerConfig{}).DiscardGuestImagesThrough(7);
}

using Target = NativeRenderFramePass::Target;

// Guest memory of a two-tile frame as BeginTiling (0x8269CE90) leaves it: the
// 640x720 2X tile surface is saved RT0, its depth the saved depth slot.
constexpr std::uint32_t kTile = 0x20000, kTileDepth = 0x21000, kTileColor = 0x30000;

std::map<std::uint32_t, std::uint32_t> tiledWords()
{
    std::map<std::uint32_t, std::uint32_t> words{ { kGraphics + 0x3098, kTile },
                                                  { kGraphics + 0x309C, 0 },
                                                  { kGraphics + 0x30A0, 0 },
                                                  { kGraphics + 0x30A4, 0 },
                                                  { kGraphics + 0x30A8, kTileDepth },
                                                  { kGraphics + 0x2880, 0 },
                                                  { kGraphics + 0x2ABC, 0x20U << 24 },
                                                  { kGraphics + 0x31A4, 3 },
                                                  { kGraphics + 0x31AC, 1 },
                                                  { kGraphics + 0x31CC, 2 },
                                                  { kGraphics + 0x337C, 1280 },
                                                  { kGraphics + 0x3380, 720 },
                                                  { kGraphics + 0x31B8, kTile },
                                                  { kGraphics + 0x31BC, 0 },
                                                  { kGraphics + 0x31C0, 0 },
                                                  { kGraphics + 0x31C4, 0 },
                                                  { kGraphics + 0x31C8, kTileDepth },
                                                  { kTile + 0x24, (639U << 18) | (719U << 3) },
                                                  { kTile + 0x18, 1U << 16 },
                                                  { kTile + 0x1C, 0 },
                                                  { kTileDepth + 0x24, (639U << 18) | (719U << 3) },
                                                  { kTileDepth + 0x18, 1U << 16 },
                                                  { kTileDepth + 0x1C, 0x530 },
                                                  { kStack + 92, 0 },
                                                  { kTileColor, 0 },
                                                  { kTileColor + 4, 0 },
                                                  { kTileColor + 8, 0 },
                                                  { kTileColor + 12, std::bit_cast<std::uint32_t>(1.0F) } };
    const std::array<GuestRect, 2>         rects{ { { 0, 0, 640, 720 }, { 640, 0, 1280, 720 } } };
    for (std::uint32_t i = 0; i < rects.size(); ++i)
    {
        words[kGraphics + 0x31D0 + 16 * i] = rects[i].x0;
        words[kGraphics + 0x31D4 + 16 * i] = rects[i].y0;
        words[kGraphics + 0x31D8 + 16 * i] = rects[i].x1;
        words[kGraphics + 0x31DC + 16 * i] = rects[i].y1;
    }
    return words;
}

GuestWordReader wordReader(std::map<std::uint32_t, std::uint32_t> words)
{
    return [words = std::move(words)](std::uint32_t address, std::uint32_t& out)
    {
        const auto found = words.find(address);
        if (found == words.end())
            return false;
        out = found->second;
        return true;
    };
}

// The title's tile-0 predicated clear of the logical screen, flags 0x3F.
GuestClear tiledClear()
{
    GuestClear clear;
    UNSCOPED_INFO("the tiled clear decodes");
    REQUIRE(ReadGuestClear(wordReader(tiledWords()), { kGraphics, 0x3F, 0, 0, 0, 1280, 720, kTileColor }, kStack, 1.0, clear));
    return clear;
}

// SetPredication(0) (0x8269CD20) after the tile-0 clear: the draws and the
// depth-only clears that follow are unpredicated.
std::map<std::uint32_t, std::uint32_t> unpredicated(std::map<std::uint32_t, std::uint32_t> words)
{
    words[kGraphics + 0x31A4] = 0xFFFFFFFF;
    words[kGraphics + 0x31AC] = 0;
    return words;
}

// The target as tile i's scanout resolve reads it, predicated to that tile.
GuestTarget tiledTarget(std::uint32_t tile = 0)
{
    auto words                = tiledWords();
    words[kGraphics + 0x31A4] = 3U << (2 * tile);
    GuestTarget target;
    UNSCOPED_INFO("the tiled target decodes");
    REQUIRE(ReadGuestTarget(wordReader(words), kGraphics, 0, target));
    return target;
}

// RB_COLOR_CLEAR and RB_DEPTH_CLEAR that start tile 1 as the tiled clear
// started tile 0: opaque black, far depth, stencil 0.
constexpr std::uint32_t kOpaqueBlackWord = 0xFF000000, kFarDepthWord = 0xFFFFFF00;

// Tile i's frontbuffer copy as 0x8250AFEC issues it: rect, point and k01
// control, with the clear words its clear bits write.
GuestResolveCopy tileCopy(std::uint32_t tile, std::uint32_t control, std::uint32_t colorClear = kOpaqueBlackWord, std::uint32_t depthClear = kFarDepthWord)
{
    GuestResolveCopy copy;
    copy.copyControl = control;
    copy.colorClear  = colorClear;
    copy.depthClear  = depthClear;
    copy.destBase    = 0x1F000000;
    copy.destPitch   = kScanoutDestPitch;
    copy.destInfo    = kScanoutDestInfo;
    copy.rectX = copy.destX = tile * 640;
    copy.rectWidth          = 640;
    copy.rectHeight         = 720;
    return copy;
}

NativeFrameAssemblerConfig tiledConfig(FakeCompletion& completion)
{
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    config.readTarget = wordReader(unpredicated(tiledWords()));
    return config;
}

// Both tile scanouts, each predicated to its own tile; tile 0's clear words
// start tile 1.
void scanout(NativeFrameAssembler& assembler, std::uint32_t colorClear = kOpaqueBlackWord, std::uint32_t depthClear = kFarDepthWord)
{
    std::string error;
    bool        partial = false;
    UNSCOPED_INFO("both tile scanouts are admitted: " + error);
    REQUIRE((assembler.Resolve(tiledTarget(0), tileCopy(0, 0x00100340, colorClear, depthClear), true, partial, error) &&
             assembler.Resolve(tiledTarget(1), tileCopy(1, 0x00100040), true, partial, error) && error.empty()));
}

// One tiled boot frame: clear, a panel, both tile scanouts.
void tiledFrame(NativeFrameAssembler& assembler, std::uint64_t sequence, std::uint32_t tag)
{
    std::string error;
    UNSCOPED_INFO("the tiled clear is admitted: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
    record(assembler, sequence, tag, &verdict);
    UNSCOPED_INFO("the panel on the mapped target is admitted");
    REQUIRE(!verdict);
    scanout(assembler);
}

// A saved tile surface stands for the logical screen only while tiling maps.
void testTilingMapping()
{
    auto        words = tiledWords();
    GuestTarget target;
    UNSCOPED_INFO("the saved RT0 tile maps to the 1280x720x4 logical target");
    REQUIRE((ReadGuestTarget(wordReader(words), kGraphics, 0, target) && target.native == Target{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 } &&
             target.surfaceWidth == 640 && target.surfaceHeight == 720 && target.tiling && target.tiling->count == 2));
    for (const int variant : { 0, 1 })
    {
        auto changed = words;
        if (variant == 0)
            changed[kGraphics + 0x31B8] = 0x99000; // another surface was saved
        else
            changed[kGraphics + 0x2ABC] = 0; // tiling inactive
        UNSCOPED_INFO("an unsaved surface or inactive tiling keeps the true 640x720 extent");
        REQUIRE((ReadGuestTarget(wordReader(changed), kGraphics, 0, target) && target.native.width == 640 && target.native.height == 720 &&
                 !target.tiling));
    }
    auto unreadable = words;
    unreadable.erase(kGraphics + 0x2ABC);
    UNSCOPED_INFO("unreadable tiling state is not guessed");
    REQUIRE(!ReadGuestTarget(wordReader(unreadable), kGraphics, 0, target));

    const auto clear = tiledClear();
    UNSCOPED_INFO("the 0x3F clear checks depth against the mapped extents");
    REQUIRE((clear.target.native.width == 1280 && clear.predicate == 1 && clear.tiling.predicateMask == 3 && clear.clear.color &&
             clear.clear.depth && clear.clear.stencil && clear.depth && clear.depth->tiled && clear.depth->edramBase == 0x530 &&
             clear.depth->width == 1280));
    GuestClear  refused;
    std::string detail;
    auto        foreignDepth         = words;
    foreignDepth[kGraphics + 0x31C8] = 0x99000;
    UNSCOPED_INFO("a depth surface other than the saved one fails by name");
    REQUIRE((!ReadGuestClear(wordReader(foreignDepth), { kGraphics, 0x3F, 0, 0, 0, 1280, 720, kTileColor }, kStack, 1.0, refused, &detail) &&
             detail == "depth_surface_not_tiling_saved"));
    GuestClear depthOnly;
    UNSCOPED_INFO("a depth-only clear decodes the bound depth surface");
    REQUIRE((ReadGuestClear(wordReader(words), { kGraphics, 0x30, kGuestDepthOnlyClearIndex, 0, 0, 1280, 720, 0 }, kStack, 1.0, depthOnly) &&
             !depthOnly.clear.color && depthOnly.clear.depth && depthOnly.clear.stencil && depthOnly.depth &&
             depthOnly.target.native == Target{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0x530 }));
    UNSCOPED_INFO("a depth-only index with a color flag is refused");
    REQUIRE(!ReadGuestClear(wordReader(words), { kGraphics, 0x31, kGuestDepthOnlyClearIndex, 0, 0, 1280, 720, 0 }, kStack, 1.0, depthOnly));
}

// Undecodable clears name guest word/value; strict ledger reports that name.
void testClearRefusalCodes()
{
    const std::array<std::uint32_t, 8> full{ kGraphics, 0x3F, 0, 0, 0, 1280, 720, kTileColor };
    const auto                         refusal = [](const std::map<std::uint32_t, std::uint32_t>& words, const std::array<std::uint32_t, 8>& r)
    {
        GuestClear  clear;
        std::string detail;
        UNSCOPED_INFO("the clear is refused: " + detail);
        REQUIRE(!ReadGuestClear(wordReader(words), r, kStack, 1.0, clear, &detail));
        return detail;
    };
    const auto changed = [](std::uint32_t address, std::uint32_t value)
    {
        auto words     = tiledWords();
        words[address] = value;
        return words;
    };
    const auto erased = [](std::uint32_t address)
    {
        auto words = tiledWords();
        words.erase(address);
        return words;
    };
    const std::pair<std::string, std::string> cases[] = {
        { refusal(changed(kTile + 0x1C, 1U << 16), full), "clear_target_format_unsupported: RT0 format 1 color_info 00010000" },
        { refusal(changed(kTile + 0x18, 2U << 16), full), "clear_target_msaa_unsupported: RT0 msaa 2 surface_info 00020000" },
        { refusal(tiledWords(), { kGraphics, 0x2, 1, 0, 0, 1280, 720, kTileColor }), "clear_target_unbound: RT1" },
        { refusal(changed(kGraphics + 0x30A0, 0x22000), full), "clear_target_set_sparse: RT2 bound after unbound RT1" },
        { refusal(changed(kTileDepth + 0x1C, 0x10530), full), "clear_depth_format_unsupported: D24FS8 depth_info 00010530" },
        { refusal(changed(kTileDepth + 0x18, 2U << 16), full), "clear_depth_msaa_unsupported: msaa 2 surface_info 00020000" },
        { refusal(changed(kGraphics + 0x30A8, 0), full), "clear_depth_unbound" },
        { refusal(tiledWords(), { kGraphics, 0x31, kGuestDepthOnlyClearIndex, 0, 0, 1280, 720, 0 }), "clear_depth_only_flags 0x31" },
        { refusal(erased(kGraphics + 0x2ABC), full), "clear_state_unreadable: tiling" },
        { refusal(erased(kGraphics + 0x3098), full), "clear_state_unreadable: G+0x3098" },
        { refusal(erased(kTile + 0x1C), full), "clear_state_unreadable: RT0 surface 00020000" },
        { refusal(erased(kGraphics + 0x30A4), full), "clear_state_unreadable: G+0x30a4" },
        { refusal(erased(kStack + 92), full), "clear_state_unreadable: stencil at r1+92" },
        { refusal(erased(kGraphics + 0x30A8), full), "clear_state_unreadable: G+0x30a8" },
        { refusal(erased(kTileDepth + 0x24), full), "clear_state_unreadable: depth surface 00021000" },
        { refusal(erased(kTileColor + 8), full), "clear_state_unreadable: color at 00030000" },
    };
    for (const auto& [actual, expected] : cases)
    {
        UNSCOPED_INFO("refusal named: " + actual + ", expected " + expected);
        REQUIRE(actual == expected);
    }

    // Untiled, the 640x720 surface keeps its own extent; a larger depth surface
    // bound beside it is not one pass.
    auto untiled               = changed(kGraphics + 0x2ABC, 0);
    untiled[kTileDepth + 0x24] = (1023U << 18) | (1023U << 3);
    const auto mismatch        = refusal(untiled, full);
    UNSCOPED_INFO("a depth extent mismatch is named: " + mismatch);
    REQUIRE(mismatch == "clear_depth_extent_mismatch: depth 1024x1024x4 at 530, RT0 640x720x4");

    // A bound RT1 without its color bit still makes RT0's clear part of a set,
    // so RT1 must decode too.
    const auto mrt = refusal(changed(kGraphics + 0x309C, 0x22000), { kGraphics, 0x31, 0, 0, 0, 1280, 720, kTileColor });
    UNSCOPED_INFO("an undecodable RT1 is named: " + mrt);
    REQUIRE(mrt == "clear_state_unreadable: RT1 surface 00022000");
    GuestClear decoded;
    UNSCOPED_INFO("the single-RT clear still decodes");
    REQUIRE(ReadGuestClear(wordReader(tiledWords()), full, kStack, 1.0, decoded));
}

void testTiledClearAdmission()
{
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        tiledFrame(assembler, 1, 5);
        const auto result = assembler.Swap(makeSwap(1, 1));
        UNSCOPED_INFO("the tiled frame presents: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == 1));
        const auto& passes = completion.spans[0].frame->passes;
        UNSCOPED_INFO("one whole-frame clear on the logical target, then the panel");
        REQUIRE((passes.size() == 1 && passes[0].clears.size() == 1 && passes[0].target == Target{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0, 0x530 } &&
                 passes[0].clears[0].rectangle.right == 1280 && passes[0].clears[0].colorValue == std::array<float, 4>{ 0, 0, 0, 1 } &&
                 drawTags(completion.spans[0]) == std::vector<std::uint8_t>{ 5 }));
    }
    const auto expect = [](GuestClear clear, const std::string& name)
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        std::string          error;
        UNSCOPED_INFO("expected " + name + ", got " + error);
        REQUIRE((!assembler.Clear(clear, error) && refusedAs(error, "clear_form_unsupported: " + name)));
    };
    for (const std::uint32_t mask : { 0xCU, 0x30U })
    {
        auto clear                 = tiledClear();
        clear.tiling.predicateMask = mask;
        expect(clear, std::format("tiled_predicate {:#x}", mask));
    }
    for (const std::uint32_t x0 : { 600U, 700U }) // overlap, gap
    {
        auto clear               = tiledClear();
        clear.tiling.rects[1].x0 = x0;
        expect(clear, "tiled_rects");
    }
    auto narrow                  = tiledClear();
    narrow.clear.rectangle.right = 640;
    expect(narrow, "tiled_rect 0,0,640,720");
    auto unmapped = tiledClear();
    unmapped.target.tiling.reset();
    expect(unmapped, "tiled_target_unmapped");
}

void testTiledScanout()
{
    {
        // The tile scanouts alone still present the epoch, once both arrived.
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        std::string          error;
        bool                 partial = false;
        UNSCOPED_INFO("two tiles: " + error);
        REQUIRE((assembler.Resolve(tiledTarget(0), tileCopy(0, 0x00100340), true, partial, error) == 1 &&
                 assembler.Resolve(tiledTarget(1), tileCopy(1, 0x00100040), true, partial, error) == 2));
        const auto result = assembler.Swap(makeSwap(1));
        UNSCOPED_INFO("two tiles are one frontbuffer epoch: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == 1 && completion.spans[0].frame));
    }
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        std::string          error;
        bool                 partial = false;
        UNSCOPED_INFO("tile 0: " + error);
        REQUIRE(assembler.Resolve(tiledTarget(0), tileCopy(0, 0x00100340), true, partial, error));
        const auto result = assembler.Swap(makeSwap(1));
        UNSCOPED_INFO("a missing tile 1 fails the swap: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail == "tiled_scanout_incomplete: 1 of 2 tiles" && completion.spans.empty()));
    }
    // Swapped points, clear bits on the last tile, no sample select, tile 1
    // predicated to tile 0, and an unpredicated tile 0.
    for (const int variant : { 0, 1, 2, 3, 4 })
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        std::string          error;
        bool                 partial = false;
        auto                 first = tileCopy(0, 0x00100340), second = tileCopy(1, 0x00100040);
        auto                 firstTarget = tiledTarget(0), secondTarget = tiledTarget(1);
        if (variant == 0)
            std::swap(first.destX, second.destX);
        else if (variant == 1)
            second.copyControl = 0x00100340;
        else if (variant == 2)
            second.copyControl = 0x00100000;
        else if (variant == 3)
            secondTarget = tiledTarget(0);
        else
            firstTarget.tiling->predicated = 0;
        const bool admitted = assembler.Resolve(firstTarget, first, true, partial, error) && assembler.Resolve(secondTarget, second, true, partial, error);
        UNSCOPED_INFO("a malformed tile is refused: " + error);
        REQUIRE((!admitted && error.starts_with("tiled_scanout_form: tile ")));
        const auto result = assembler.Swap(makeSwap(1));
        UNSCOPED_INFO("and fails the swap: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.starts_with("tiled_scanout_form") && completion.spans.empty()));
    }
}

// Tiled scanout stays split until full clear; variant 3 leaves color.
void testTiledTargetUncleared()
{
    for (const int variant : { 0, 1, 2, 3 })
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        tiledFrame(assembler, 1, 5);
        UNSCOPED_INFO("the first tiled frame presents");
        REQUIRE(assembler.Swap(makeSwap(1, 1)).status == NativeFrameStatus::Provisional);
        std::string error;
        bool        partial = false;
        if (variant == 0)
        {
            tiledFrame(assembler, 2, 6);
            const auto result = assembler.Swap(makeSwap(2, 2));
            UNSCOPED_INFO("the next frame's predicated clear defines the target again: " + result.detail);
            REQUIRE((result.status == NativeFrameStatus::Provisional && drawTags(completion.spans.back()) == std::vector<std::uint8_t>{ 6 }));
            continue;
        }
        if (variant == 3)
        {
            // The frame then fails only for its missing scanout.
            std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
            record(assembler, 2, kNoColorTag, &verdict);
            const auto result = assembler.Swap(makeSwap(2, 2));
            UNSCOPED_INFO("a draw without color writes before the clear is not refused as uncleared color: " + result.detail);
            REQUIRE((!verdict && result.status == NativeFrameStatus::Failed && result.detail.starts_with("swap_scanout_unmodeled")));
            continue;
        }
        if (variant == 1)
            record(assembler, 2, 6);
        else
        {
            UNSCOPED_INFO("a scanout before the clear is refused: " + error);
            REQUIRE((!assembler.Resolve(tiledTarget(), tileCopy(0, 0x00100340), true, partial, error) && refusedAs(error, "tiled_target_uncleared")));
        }
        const auto result = assembler.Swap(makeSwap(2, variant == 1 ? 2 : 1));
        UNSCOPED_INFO("work on the uncleared target fails: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed &&
                 result.detail.find(variant == 1 ? "presented_draw_before_clear: tiles 2 predicated 0 mask 0xffffffff" : "tiled_target_uncleared") != std::string::npos));
    }
}

// Presented passes may use host extent; guest resolves remain at guest scale.
void testPresentedFlag()
{
    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.build      = panelBuilder;
    config.completion = &completion;
    NativeFrameAssembler assembler(config);
    const auto           target = offscreen(0x300);
    std::string          error;
    bool                 partial = false;
    record(assembler, 1, 1);
    UNSCOPED_INFO("a segment");
    REQUIRE(assembler.FlushThrough(1).fence == 1);
    UNSCOPED_INFO("a resolve frame");
    REQUIRE((assembler.Clear(fullClear(target), error) == 2 && assembler.Resolve(target, fullCopy(target), false, partial, error) == 3 &&
             assembler.FlushThrough(3).fence == 2));
    record(assembler, 2, 2);
    scanOut(assembler);
    UNSCOPED_INFO("the swap frame");
    REQUIRE((assembler.Swap(makeSwap(1, 2)).status == NativeFrameStatus::Provisional && completion.spans.size() == 3));
    const auto all = [](const NativeCompletionSpan& span, bool presented)
    {
        return std::all_of(span.frame->passes.begin(), span.frame->passes.end(), [&](const NativeRenderFramePass& pass)
                           {
                               return pass.presented == presented;
                           });
    };
    UNSCOPED_INFO("segment passes are presented");
    REQUIRE((completion.spans[0].segment && all(completion.spans[0], true)));
    UNSCOPED_INFO("resolve frame passes are not");
    REQUIRE((completion.spans[1].guestResolve && all(completion.spans[1], false)));
    UNSCOPED_INFO("swap passes are presented at the guest extent");
    REQUIRE((all(completion.spans[2], true) && completion.spans[2].frame->presentExtent == NativeRenderExtent{}));
}

// A 0x30 clear (0x8250E718) of the bound depth surface, here at `base`.
GuestClear depthOnlyClear(std::uint32_t base = 0x530, double depth = 1.0)
{
    auto words               = unpredicated(tiledWords());
    words[kTileDepth + 0x1C] = base;
    GuestClear clear;
    UNSCOPED_INFO("the depth-only clear decodes");
    REQUIRE(ReadGuestClear(wordReader(words), { kGraphics, 0x30, kGuestDepthOnlyClearIndex, 0, 0, 1280, 720, 0 }, kStack, depth, clear));
    return clear;
}

// The game-start frame form: after a tiled scanout the guest draws GFx and
// labels untiled into the 640x720 tile surface, then BeginTiling and the
// tile-0 predicated clear overwrite them before any tile reads the EDRAM.
std::map<std::uint32_t, std::uint32_t> preTilingWords()
{
    auto words                = unpredicated(tiledWords());
    words[kGraphics + 0x2ABC] = 0; // tiling inactive, no conditional or recorded mode
    words[kGraphics + 0x2880] = 0x0A010280;
    words[kGraphics + 0x28C0] = 0;
    words[kGraphics + 0x28C8] = (720U << 16) | 640;
    words[kGraphics + 0x2934] = 0x24F00770; // no stencil, depth test or depth write
    words[kGraphics + 0x28DC] = 0xF;
    words[kGraphics + 0x2A48] = 2;
    words[kGraphics + 0x2A4C] = 0;
    return words;
}

void testPresentedWritesBeforeTiledClear()
{
    enum Variant
    {
        Covered,
        CoveredDepth,
        ScissorOutside,
        ColorMaskRt1,
        WindowOffset,
        TileNarrow,
        DepthUncleared,
        ResolveFirst,
        SwapFirst,
        OtherClearFirst,
        TiledDrawFirst,
        Count
    };

    for (int variant = 0; variant < Count; ++variant)
    {
        FakeCompletion completion;
        auto           words  = std::make_shared<std::map<std::uint32_t, std::uint32_t>>(unpredicated(tiledWords()));
        auto           config = tiledConfig(completion);
        config.readTarget     = [words](std::uint32_t address, std::uint32_t& out)
        {
            const auto found = words->find(address);
            if (found == words->end())
                return false;
            out = found->second;
            return true;
        };
        NativeFrameAssembler assembler(config);
        tiledFrame(assembler, 1, 5);
        UNSCOPED_INFO("the first tiled frame presents");
        REQUIRE(assembler.Swap(makeSwap(1, 1)).status == NativeFrameStatus::Provisional);

        *words = preTilingWords();
        if (variant == ScissorOutside)
            (*words)[kGraphics + 0x28C8] = (720U << 16) | 641;
        else if (variant == ColorMaskRt1)
            (*words)[kGraphics + 0x28DC] = 0xFF;
        else if (variant == WindowOffset)
            (*words)[kGraphics + 0x28C0] = 0x00100010;
        else if (variant == CoveredDepth || variant == DepthUncleared)
            (*words)[kGraphics + 0x2934] = 4; // depth write
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        record(assembler, 2, 6, &verdict);
        UNSCOPED_INFO("the untiled panel is admitted");
        REQUIRE(!verdict);
        record(assembler, 3, 7, &verdict);
        std::uint64_t numbered = 3;

        std::string error;
        bool        partial = false;
        std::string expected;
        if (variant == ResolveFirst)
        {
            assembler.Resolve(tiledTarget(0), tileCopy(0, 0x00100340), true, partial, error);
            expected = "dead_write_unproven: then resolve";
        }
        else if (variant == SwapFirst)
            expected = "dead_write_unproven: then swap";
        else if (variant == OtherClearFirst)
        {
            assembler.Clear(depthOnlyClear(), error);
            expected = "dead_write_unproven: then clear: untiled";
        }
        else if (variant == TiledDrawFirst)
        {
            *words = unpredicated(tiledWords());
            record(assembler, ++numbered, 8);
            expected = std::format("dead_write_unproven: then indexed_up from {:#010x}", kPanelCaller);
        }
        else
        {
            *words     = unpredicated(tiledWords());
            auto clear = tiledClear();
            if (variant == TileNarrow)
            {
                clear.tiling.rects[0].x1 = 600;
                clear.tiling.rects[1].x0 = 600;
                expected                 = "dead_write_unproven: then clear: tile_0 600x720, draws 640x720";
            }
            else if (variant == DepthUncleared)
            {
                clear.flags       = 0x0F;
                clear.clear.depth = clear.clear.stencil = false;
                clear.depth.reset();
                expected = std::format("dead_write_unproven: then clear: depth {:08X} uncleared", kTileDepth);
            }
            else if (variant == ScissorOutside)
                expected = "dead_write_unproven: window_scissor 641x720 outside surface 640x720 pitch 640";
            else if (variant == ColorMaskRt1)
                expected = "dead_write_unproven: color_mask 0xff";
            else if (variant == WindowOffset)
                expected = "dead_write_unproven: window_offset 00100010";
            assembler.Clear(clear, error);
            record(assembler, ++numbered, 8);
            if (expected.empty())
                scanout(assembler);
        }
        const auto result = assembler.Swap(makeSwap(2, numbered));
        if (expected.empty())
        {
            UNSCOPED_INFO(std::format("variant {}: the covered untiled draws are dead: {}", variant, result.detail));
            REQUIRE((result.status == NativeFrameStatus::Provisional && drawTags(completion.spans.back()) == std::vector<std::uint8_t>{ 8 }));
            continue;
        }
        UNSCOPED_INFO(std::format("variant {}: expected {}, got {}", variant, expected, result.detail));
        REQUIRE((result.status == NativeFrameStatus::Failed &&
                 result.detail.find("presented_draw_before_clear: untiled; " + expected) != std::string::npos));
    }
}

// Guest memory for the ledger: RPTR and fence stores land here.
struct RingSink final : completion::IGuestProgressSink
{
    std::map<std::uint32_t, std::uint32_t> words;

    bool WritePhysical(std::uint32_t address, std::uint32_t value) override
    {
        words[address] = value;
        return true;
    }

    std::optional<std::uint32_t> ReadPhysical(std::uint32_t address) override
    {
        const auto found = words.find(address);
        return found == words.end() ? 0 : found->second;
    }

    completion::InterruptResult RaiseCallbackInterrupt(std::uint32_t) override
    {
        return completion::InterruptResult::Raised;
    }
};

// Held draws release spans; covering clear succeeds, other operations fail.
void testHeldWritesRelease()
{
    constexpr std::uint32_t kBlock = 0x1FC9C000, kWriteback = kBlock + completion::kReadPointerOffset;
    // Four spans of 0x10 dwords fill the ring; the guest kicks one span per draw.
    constexpr std::uint32_t kRing = 0x40, kSpan = 0x10, kHeld = 64;
    for (const bool covered : { true, false })
    {
        FakeCompletion completion;
        auto           words  = std::make_shared<std::map<std::uint32_t, std::uint32_t>>(unpredicated(tiledWords()));
        auto           config = tiledConfig(completion);
        config.readTarget     = [words](std::uint32_t address, std::uint32_t& out)
        {
            const auto found = words->find(address);
            if (found == words->end())
                return false;
            out = found->second;
            return true;
        };
        NativeFrameAssembler assembler(config);
        tiledFrame(assembler, 1, 5);
        UNSCOPED_INFO("the first tiled frame presents");
        REQUIRE(assembler.Swap(makeSwap(1, 1)).status == NativeFrameStatus::Provisional);
        completion.completed = completion.next;

        RingSink                               sink;
        std::vector<completion::LedgerFailure> failures;
        completion::CompletionLedger           ledger({ NativeDrawMode::Strict, &sink, &assembler, [&](const completion::LedgerFailure& failure)
                                                        {
                                                  failures.push_back(failure);
                                                        } });
        ledger.BeginRingEpoch();
        ledger.RegisterWriteback(kWriteback);
        std::uint32_t writePointer = 0;
        std::uint64_t sequence     = 1;
        // The dispatcher's fan-out for one draw, then the kick of its span once
        // the ring has room; the guest spins on RPTR, so a span that never
        // releases while the ring is full is a deadlock.
        const auto draw = [&](std::uint32_t tag)
        {
            for (int spins = 0; writePointer + kSpan - sink.words[kWriteback] > kRing; ++spins)
            {
                UNSCOPED_INFO(std::format("the guest waits for ring space forever at RPTR {:#x}", sink.words[kWriteback]));
                REQUIRE(spins < 4);
                ledger.Pump();
            }
            auto value          = submission(NativeDrawHook::IndexedUp, kPanelCaller, ++sequence, tag);
            value.operationMark = assembler.NumberOperation();
            assembler.Began(value);
            ledger.BeginDraw(value.hook, value.caller);
            const auto verdict = assembler.Admit(value);
            const auto mark    = assembler.Recorded(value, verdict);
            UNSCOPED_INFO("the draw is admitted");
            REQUIRE(!verdict);
            ledger.FinishDraw(mark, value.hook, value.caller, verdict);
            writePointer += kSpan;
            ledger.Kick(writePointer);
        };

        *words = preTilingWords();
        for (std::uint32_t i = 0; i < kHeld; ++i)
            draw(6);
        ledger.Pump();
        UNSCOPED_INFO(std::format("every held draw's span releases while the hold is open (RPTR {:#x})", sink.words[kWriteback]));
        REQUIRE((sink.words[kWriteback] == writePointer && failures.empty()));

        std::string error;
        bool        partial = false;
        if (covered)
        {
            *words = unpredicated(tiledWords());
            UNSCOPED_INFO("the covering clear: " + error);
            REQUIRE(assembler.Clear(tiledClear(), error));
            draw(8);
            scanout(assembler);
            const auto result = assembler.Swap(makeSwap(2, sequence));
            UNSCOPED_INFO("the frame completes without the dead draws: " + result.detail);
            REQUIRE((result.status == NativeFrameStatus::Provisional && drawTags(completion.spans.back()) == std::vector<std::uint8_t>{ 8 }));
            completion.completed = completion.next;
            ledger.Pump();
            UNSCOPED_INFO("the frame's spans release: " + (failures.empty() ? std::string() : failures[0].detail));
            REQUIRE((sink.words[kWriteback] == writePointer && failures.empty()));
            ledger.Close();
            continue;
        }
        assembler.Resolve(tiledTarget(0), tileCopy(0, 0x00100340), true, partial, error);
        const auto released = sink.words[kWriteback];
        draw(6);
        ledger.Pump();
        UNSCOPED_INFO("after a failed hold the next span fails strict and never releases: " + (failures.empty() ? std::string("none") : failures[0].detail));
        REQUIRE((sink.words[kWriteback] == released && failures.size() == 1 && failures[0].detail.find("dead_write_unproven: then resolve") != std::string::npos));
        UNSCOPED_INFO("the failed hold's frame is not presented");
        REQUIRE(assembler.Swap(makeSwap(2, sequence)).status == NativeFrameStatus::Failed);
        ledger.Close();
    }
}

// Depth-only clear uses its pass; an unheld depth surface fails by name.
void testDepthClears()
{
    const Target main{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0, 0x530 };
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        std::string          error;
        UNSCOPED_INFO("the tiled clear: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        record(assembler, 1, 5);
        UNSCOPED_INFO("the presented depth clear is admitted: " + error);
        REQUIRE((assembler.Clear(depthOnlyClear(0x530, 0.0), error) && error.empty()));
        record(assembler, 2, 6);
        scanout(assembler);
        const auto result = assembler.Swap(makeSwap(1, 2));
        UNSCOPED_INFO("the frame presents: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == 1));
        const auto& passes = completion.spans[0].frame->passes;
        UNSCOPED_INFO("the depth clear starts a preserving pass on the presented target");
        REQUIRE((passes.size() == 2 && passes[0].target == main && passes[1].target == main && passes[1].preservePreviousAttachments &&
                 passes[1].presented && passes[1].clears.size() == 1 && !passes[1].clears[0].color && passes[1].clears[0].depth &&
                 passes[1].clears[0].stencil && passes[1].clears[0].depthValue == 0.0F && passes[1].clears[0].rectangle.right == 1280 &&
                 drawTags(completion.spans[0]) == std::vector<std::uint8_t>{ 5, 6 }));
    }
    {
        // Before any presented clear names a depth surface, none is held.
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        std::string          error;
        UNSCOPED_INFO("an unnamed depth surface: " + error);
        REQUIRE((!assembler.Clear(depthOnlyClear(), error) && refusedAs(error, "depth_clear_target_unmodeled: 530")));
        auto undecoded = tiledClear();
        undecoded.depth.reset();
        UNSCOPED_INFO("a depth clear needs its surface: " + error);
        REQUIRE((!assembler.Clear(undecoded, error) && refusedAs(error, "clear_form_unsupported: depth_surface_undecoded")));
        UNSCOPED_INFO("the tiled clear: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        UNSCOPED_INFO("another depth base: " + error);
        REQUIRE((!assembler.Clear(depthOnlyClear(0x400), error) && refusedAs(error, "depth_clear_target_unmodeled: 400")));
        auto words                = unpredicated(tiledWords());
        words[kGraphics + 0x2ABC] = 0;
        words[kTileDepth + 0x24]  = (1023U << 18) | (1023U << 3);
        words[kTileDepth + 0x18]  = 0;
        GuestClear offscreenDepth;
        UNSCOPED_INFO("an off-screen depth surface: " + error);
        REQUIRE((ReadGuestClear(wordReader(words), { kGraphics, 0x30, kGuestDepthOnlyClearIndex, 0, 0, 1024, 1024, 0 }, kStack, 1.0, offscreenDepth) &&
                 !assembler.Clear(offscreenDepth, error) && refusedAs(error, "depth_clear_target_unmodeled: 530")));
    }
    {
        // Skip mode keeps the frame but reports it partial.
        FakeCompletion completion;
        auto           config = tiledConfig(completion);
        config.mode           = NativeDrawMode::Skip;
        NativeFrameAssembler assembler(config);
        std::string          error;
        UNSCOPED_INFO("the tiled clear: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        record(assembler, 1, 5);
        UNSCOPED_INFO("the foreign depth clear is refused");
        REQUIRE(!assembler.Clear(depthOnlyClear(0x400), error));
        scanout(assembler);
        const auto result = assembler.Swap(makeSwap(1, 1));
        UNSCOPED_INFO("a skipped presented depth clear leaves the frame partial: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Partial && refusedAs(result.detail, "clear: depth_clear_target_unmodeled: 400")));
    }
}

// A depth-tested draw must hit the depth surface its pass holds.
void testDepthDraws()
{
    const auto run = [](std::uint32_t base, bool named, bool readable) -> std::string
    {
        FakeCompletion completion;
        auto           config    = tiledConfig(completion);
        auto           words     = unpredicated(tiledWords());
        words[kTileDepth + 0x1C] = base;
        config.readTarget        = readable ? wordReader(words) : GuestWordReader{};
        NativeFrameAssembler assembler(config);
        std::string          error;
        if (named)
        {
            UNSCOPED_INFO("the tiled clear: " + error);
            REQUIRE(assembler.Clear(tiledClear(), error));
        }
        record(assembler, 1, kDepthTag);
        scanOut(assembler);
        const auto result = assembler.Swap(makeSwap(1, 1));
        if (result.status != NativeFrameStatus::Provisional)
            return result.detail;
        const auto& pass = completion.spans[0].frame->passes[0];
        UNSCOPED_INFO("the depth draw renders on the presented depth surface");
        REQUIRE((pass.target.depthEdramBase == 0x530 && pass.Draw(0).depth.enabled));
        return {};
    };
    UNSCOPED_INFO("the presented depth surface admits the draw");
    REQUIRE(run(0x530, true, true).empty());
    for (const auto& [base, named, readable, expected] : { std::tuple{ 0x400U, true, true, "frame_depth_target_mismatch" },
                                                           std::tuple{ 0x530U, false, true, "frame_depth_target_mismatch" },
                                                           std::tuple{ 0x530U, true, false, "draw_depth_unreadable" } })
    {
        const auto detail = run(base, named, readable);
        UNSCOPED_INFO(std::string("expected ") + expected + ", got " + detail);
        REQUIRE(detail.find(expected) != std::string::npos);
    }
}

// An off-screen resolve frame names the depth surface its clear hit.
void testOffscreenDepthIdentity()
{
    for (const bool depth : { false, true })
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        const auto           target = offscreen(0x300);
        auto                 clear  = fullClear(target);
        if (depth)
        {
            clear.flags       = 0x31;
            clear.clear.depth = true;
            clear.depth       = GuestDepthSurface{ 0x5000, 0x200, 0x200, 1024, 1024, 1 };
        }
        std::string error;
        bool        partial = false;
        UNSCOPED_INFO("off-screen clear and resolve: " + error);
        REQUIRE((assembler.Clear(clear, error) && assembler.Resolve(target, fullCopy(target), false, partial, error)));
        UNSCOPED_INFO("a resolve frame");
        REQUIRE((assembler.Swap(makeSwap(1)).status == NativeFrameStatus::Empty && !completion.spans.empty() && completion.spans[0].guestResolve));
        UNSCOPED_INFO("the resolve frame names the clear's depth surface");
        REQUIRE(completion.spans[0].frame->passes[0].target.depthEdramBase == (depth ? 0x200 : kNoDepthEdramBase));
    }
}

// The host models all-tile draws once; partial-tile predicates are refused.
void testDrawPredicate()
{
    const auto run = [](std::uint32_t mask, NativeDrawMode mode)
    {
        FakeCompletion completion;
        auto           config     = tiledConfig(completion);
        auto           words      = tiledWords();
        words[kGraphics + 0x31A4] = mask;
        config.readTarget         = wordReader(words);
        config.mode               = mode;
        NativeFrameAssembler assembler(config);
        std::string          error;
        UNSCOPED_INFO("the tiled clear: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        record(assembler, 1, 5);
        scanOut(assembler);
        return assembler.Swap(makeSwap(1, 1));
    };
    const auto strict = run(3, NativeDrawMode::Strict);
    UNSCOPED_INFO("a tile-0 draw fails strict: " + strict.detail);
    REQUIRE((strict.status == NativeFrameStatus::Failed && strict.detail.find("draw_predicated 0x3") != std::string::npos));
    const auto skip = run(3, NativeDrawMode::Skip);
    UNSCOPED_INFO("and is skipped in skip mode: " + skip.detail);
    REQUIRE((skip.status == NativeFrameStatus::Partial && skip.detail.find("draw_predicated 0x3") != std::string::npos));
    const auto every = run(0xF, NativeDrawMode::Strict);
    UNSCOPED_INFO("a draw on every tile is admitted: " + every.detail);
    REQUIRE(every.status == NativeFrameStatus::Provisional);
}

// The reader words of an unpredicated, untiled 1024x1024 target at `base` whose
// bound depth surface sits at `depthBase`.
GuestWordReader offscreenDepthReader(std::uint32_t base, std::uint32_t depthBase)
{
    constexpr std::uint32_t kDepthSurface = 0x00700000;
    return wordReader({ { kGraphics + 0x3098, kSurface },
                        { kGraphics + 0x30A8, kDepthSurface },
                        { kGraphics + 0x2880, 0 },
                        { kGraphics + 0x2ABC, 0 },
                        { kGraphics + 0x31A4, 0xFFFFFFFF },
                        { kGraphics + 0x31AC, 0 },
                        { kSurface + 0x24, (1023U << 18) | (1023U << 3) },
                        { kSurface + 0x18, 0 },
                        { kSurface + 0x1C, base },
                        { kDepthSurface + 0x24, (1023U << 18) | (1023U << 3) },
                        { kDepthSurface + 0x18, 0 },
                        { kDepthSurface + 0x1C, depthBase } });
}

// After tile 0 scanout, presented EDRAM stays frozen until swap.
void testScanoutInProgress()
{
    enum Variant
    {
        TiledClear,
        PresentedDraw,
        DepthClear,
        AliasClear,
        AliasDepthClear,
        AliasDraw,
        AliasDepthDraw,
        DepthClearOnColor,
        ColorClearOnDepth,
        DepthDrawOnColor,
        ColorDrawOnDepth,
        Count
    };

    for (int variant = 0; variant < Count; ++variant)
    {
        FakeCompletion completion;
        auto           config = tiledConfig(completion);
        if (variant == AliasDraw || variant == AliasDepthDraw || variant == DepthDrawOnColor || variant == ColorDrawOnDepth)
        {
            config.build = shapedBuilder;
            // Presented bases are color 0 and depth 0x530; the cross cases write one against the other.
            config.readTarget = variant == AliasDraw          ? offscreenDepthReader(0, 0x200)
                                : variant == AliasDepthDraw   ? offscreenDepthReader(0x300, 0x530)
                                : variant == DepthDrawOnColor ? offscreenDepthReader(0x300, 0)
                                                              : offscreenDepthReader(0x530, 0x200);
        }
        NativeFrameAssembler assembler(config);
        std::string          error;
        bool                 partial       = false;
        const bool           offscreenDraw = variant == AliasDraw || variant == AliasDepthDraw || variant == DepthDrawOnColor || variant == ColorDrawOnDepth;
        UNSCOPED_INFO("the tiled clear: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        // The off-screen readers decode no presented draw, so only the aliasing draw is recorded.
        if (!offscreenDraw)
            record(assembler, 1, 5);
        UNSCOPED_INFO("tile 0: " + error);
        REQUIRE(assembler.Resolve(tiledTarget(0), tileCopy(0, 0x00100340), true, partial, error));
        std::uint64_t last = 1;
        if (variant == TiledClear)
        {
            UNSCOPED_INFO("a clear between tiles: " + error);
            REQUIRE((!assembler.Clear(tiledClear(), error) && refusedAs(error, "tiled_scanout_in_progress")));
        }
        else if (variant == PresentedDraw)
            record(assembler, last = 2, 6);
        else if (variant == DepthClear)
        {
            UNSCOPED_INFO("a depth clear between tiles: " + error);
            REQUIRE((!assembler.Clear(depthOnlyClear(), error) && refusedAs(error, "tiled_scanout_in_progress")));
        }
        else if (variant == AliasClear)
        {
            // Another shape on the presented base, unpredicated.
            UNSCOPED_INFO("an aliasing color clear between tiles: " + error);
            REQUIRE((!assembler.Clear(fullClear(offscreen(0)), error) && refusedAs(error, "tiled_scanout_in_progress")));
        }
        else if (variant == AliasDepthClear)
        {
            auto clear        = fullClear(offscreen(0x300));
            clear.flags       = 0x11;
            clear.clear.depth = true;
            clear.depth       = GuestDepthSurface{ 0x00700000, 0x530, 0x530, 1024, 1024, 1 };
            UNSCOPED_INFO("an aliasing depth clear between tiles: " + error);
            REQUIRE((!assembler.Clear(clear, error) && refusedAs(error, "tiled_scanout_in_progress")));
        }
        else if (variant == DepthClearOnColor || variant == ColorClearOnDepth)
        {
            // Off-screen on the other attachment's storage: depth lands on the presented color base, color on the presented depth base.
            auto clear = fullClear(offscreen(variant == DepthClearOnColor ? 0x300 : 0x530));
            if (variant == DepthClearOnColor)
            {
                clear.flags       = 0x10;
                clear.clear.color = false;
                clear.clear.depth = true;
                clear.depth       = GuestDepthSurface{ 0x00700000, 0, 0, 1024, 1024, 1 };
            }
            UNSCOPED_INFO("a cross-attachment clear between tiles: " + error);
            REQUIRE((!assembler.Clear(clear, error) && refusedAs(error, "tiled_scanout_in_progress")));
        }
        else
        {
            std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
            record(assembler, 1, variant == AliasDraw ? 0x40 : kDepthTag, &verdict);
            UNSCOPED_INFO("the off-screen draw is admitted: " + std::to_string(variant));
            REQUIRE(!verdict);
        }
        UNSCOPED_INFO("tile 1: " + error);
        REQUIRE(assembler.Resolve(tiledTarget(1), tileCopy(1, 0x00100040), true, partial, error));
        const auto result = assembler.Swap(makeSwap(1, last));
        UNSCOPED_INFO("work between the tile scanouts fails the swap: " + std::to_string(variant) + " " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.find("tiled_scanout_in_progress") != std::string::npos));
    }
}

// Unreadable tiling state prevents proving coverage, so the draw is refused.
void testUnreadableTilingState()
{
    const auto run = [](bool tiledClearFirst, bool readerConfigured, bool readable, NativeDrawMode mode)
    {
        FakeCompletion completion;
        auto           config = tiledConfig(completion);
        auto           words  = unpredicated(tiledWords());
        if (!readable)
            words.erase(kGraphics + 0x31AC);
        config.readTarget = readerConfigured ? wordReader(words) : GuestWordReader{};
        config.mode       = mode;
        NativeFrameAssembler assembler(config);
        std::string          error;
        if (tiledClearFirst)
        {
            UNSCOPED_INFO("the tiled clear: " + error);
            REQUIRE(assembler.Clear(tiledClear(), error));
        }
        record(assembler, 1, 5);
        scanOut(assembler);
        return assembler.Swap(makeSwap(1, 1));
    };
    for (const bool tiledClearFirst : { true, false })
    {
        const auto strict = run(tiledClearFirst, true, false, NativeDrawMode::Strict);
        UNSCOPED_INFO("an unreadable predicate fails strict: " + strict.detail);
        REQUIRE((strict.status == NativeFrameStatus::Failed && strict.detail.find("draw_tiling_unreadable") != std::string::npos));
        const auto skip = run(tiledClearFirst, true, false, NativeDrawMode::Skip);
        UNSCOPED_INFO("and is skipped in skip mode: " + skip.detail);
        REQUIRE((skip.status == NativeFrameStatus::Partial && skip.detail.find("draw_tiling_unreadable") != std::string::npos));
    }
    const auto readable = run(true, true, true, NativeDrawMode::Strict);
    UNSCOPED_INFO("a readable state still admits the draw: " + readable.detail);
    REQUIRE(readable.status == NativeFrameStatus::Provisional);
    const auto none = run(true, false, true, NativeDrawMode::Strict);
    UNSCOPED_INFO("no reader keeps the undecoded path: " + none.detail);
    REQUIRE(none.status == NativeFrameStatus::Provisional);
}

bool sameRect(const NativeDrawReplayScissor& rect, std::array<std::int32_t, 4> expected)
{
    return std::array{ rect.left, rect.top, rect.right, rect.bottom } == expected;
}

// Menu frame 1728 replays scene, GFx and label draws across two 640x720 tiles;
// capture has no clears.
constexpr std::array<std::uint8_t, 2> kMenuScene{ 0x81, 0x82 };
constexpr std::array<std::uint8_t, 3> kMenuUi{ 0x11, 0x12, 0x13 };

// Panels, with a depth-tested and depth-writing scene draw for 0x80..0x8F.
bool menuBuilder(const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string& detail)
{
    if (!panelBuilder(value, draw, detail))
        return false;
    if ((value.arguments[7] & 0xF0) == 0x80)
    {
        draw.depth.enabled      = true;
        draw.depth.writeEnabled = true;
        draw.depth.compare      = 4; // LESS_EQUAL
    }
    return true;
}

NativeFrameAssemblerConfig menuConfig(FakeCompletion& completion)
{
    auto config  = tiledConfig(completion);
    config.build = menuBuilder;
    return config;
}

// One menu frame from `sequence` whose tile-0 scanout clears with `copy`'s
// words; returns the next draw sequence.
std::uint64_t menuFrame(NativeFrameAssembler& assembler, std::uint64_t sequence, const GuestResolveCopy& copy)
{
    // Frame 1728: 0x8250C768 scene draws use 0x24F00736 depth, UI uses
    // 0x24F00770; two 640x720 tiles end in 0x8250AFF0 scanouts.
    std::string error;
    UNSCOPED_INFO("the frame clears: " + error);
    REQUIRE((assembler.Clear(tiledClear(), error) && assembler.Clear(depthOnlyClear(), error)));
    for (const auto tag : kMenuScene)
    {
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        record(assembler, sequence++, tag, &verdict);
        UNSCOPED_INFO("the scene draw is admitted");
        REQUIRE(!verdict);
    }
    UNSCOPED_INFO("the depth clear between scene and UI: " + error);
    REQUIRE(assembler.Clear(depthOnlyClear(), error));
    for (const auto tag : kMenuUi)
    {
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        record(assembler, sequence++, tag, &verdict);
        UNSCOPED_INFO("the UI draw is admitted");
        REQUIRE(!verdict);
    }
    scanout(assembler, copy.colorClear, copy.depthClear);
    return sequence;
}

void testMenuFrame()
{
    using Color = std::array<float, 4>;
    const Target main{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0, 0x530 };

    // Tile 0 clear cases vary color, depth 0x400000 or stencil 0x42; SDK and
    // naive UNORM can round differently.
    struct Frame
    {
        std::uint32_t colorClear, depthClear;
        bool          split;
        Color         color;
        float         depth;
        std::uint8_t  stencil;
    };

    const auto unorm = [](std::uint32_t byte)
    {
        return float(byte) * (1.0F / 255.0F);
    };
    const std::array<Frame, 5> frames{ {
        { 0, kFarDepthWord, true, { 0, 0, 0, 0 }, 1.0F, 0 },
        { kOpaqueBlackWord, kFarDepthWord, false, { 0, 0, 0, 1 }, 1.0F, 0 },
        { 0x80660C33, kFarDepthWord, true, { unorm(0x33), unorm(0x0C), unorm(0x66), unorm(0x80) }, 1.0F, 0 },
        { kOpaqueBlackWord, 0x40000000, true, { 0, 0, 0, 1 }, 0.25F, 0 },
        { kOpaqueBlackWord, 0xFFFFFF42, true, { 0, 0, 0, 1 }, 1.0F, 0x42 },
    } };
    FakeCompletion             completion;
    NativeFrameAssembler       assembler(menuConfig(completion));
    std::uint64_t              sequence = 1, number = 0;
    // Each frame starts again from the tiled clear after the last scanout.
    for (const auto& frame : frames)
    {
        sequence          = menuFrame(assembler, sequence, tileCopy(0, 0x00100340, frame.colorClear, frame.depthClear));
        const auto result = assembler.Swap(makeSwap(++number, sequence - 1));
        UNSCOPED_INFO("the menu frame presents: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == number));
        const auto& span = completion.spans.back();
        UNSCOPED_INFO("every draw of the frame is admitted");
        REQUIRE((span.coverage.attempted == 5 && span.coverage.admitted == 5));
        const auto& passes = span.frame->passes;
        UNSCOPED_INFO("the depth clear splits the frame into two passes on the presented color and depth");
        REQUIRE((passes.size() == 2 && passes[0].target == main && passes[1].target == main && !passes[0].preservePreviousAttachments &&
                 passes[1].preservePreviousAttachments && passes[0].presented && passes[1].presented));
        const auto& first = passes[0].clears;
        UNSCOPED_INFO(std::format("frame {}: {} clears before the scene", number, first.size()));
        REQUIRE(first.size() == (frame.split ? 3U : 2U));
        // The tiled clear: opaque black, far depth, stencil 0.
        const auto& tile0 = first[0];
        UNSCOPED_INFO(std::format("frame {}: the tiled clear covers {}", number, frame.split ? "tile 0" : "the screen"));
        REQUIRE((tile0.color && tile0.depth && tile0.stencil && tile0.colorValue == Color{ 0, 0, 0, 1 } && tile0.depthValue == 1.0F &&
                 tile0.stencilValue == 0 && sameRect(tile0.rectangle, { 0, 0, frame.split ? 640 : 1280, 720 })));
        if (frame.split)
        {
            const auto& tile1 = first[1];
            UNSCOPED_INFO(std::format("frame {}: tile 1 starts from the scanout's clear words", number));
            REQUIRE((tile1.color && tile1.depth && tile1.stencil && tile1.colorValue == frame.color && tile1.depthValue == frame.depth &&
                     tile1.stencilValue == frame.stencil && sameRect(tile1.rectangle, { 640, 0, 1280, 720 })));
        }
        const auto& far = first.back();
        UNSCOPED_INFO("then the far depth clear");
        REQUIRE((!far.color && far.depth && far.depthValue == 1.0F && sameRect(far.rectangle, { 0, 0, 1280, 720 })));
        UNSCOPED_INFO("the far depth clear before the UI");
        REQUIRE((passes[1].clears.size() == 1 && !passes[1].clears[0].color && passes[1].clears[0].depth));
        UNSCOPED_INFO("the scene tests depth; the UI does not");
        REQUIRE((passes[0].DrawCount() == 2 && passes[0].Draw(0).depth.enabled && passes[0].Draw(1).depth.writeEnabled &&
                 passes[1].DrawCount() == 3 && !passes[1].Draw(0).depth.enabled));
        UNSCOPED_INFO("draws keep guest order");
        REQUIRE(drawTags(span) == std::vector<std::uint8_t>{ 0x81, 0x82, 0x11, 0x12, 0x13 });
        UNSCOPED_INFO("the scanout resolves the last pass once");
        REQUIRE((passes[0].resolves.empty() && passes[1].resolves.size() == 1));
    }
    UNSCOPED_INFO("menu frames leave the session healthy");
    REQUIRE(!assembler.Failure());
}

// Tile 1 inherits tile 0 clear; absent depth is color-only; oversize fails.
void testTiledBackgrounds()
{
    for (const bool differs : { false, true })
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        std::string          error;
        UNSCOPED_INFO("the tiled clear: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        record(assembler, 1, 5);
        UNSCOPED_INFO("a segment runs the tiled clear and the panel");
        REQUIRE((assembler.FlushThrough(2).fence == 1 && completion.spans.size() == 1 && completion.spans[0].segment));
        scanout(assembler, differs ? 0 : kOpaqueBlackWord);
        const auto result = assembler.Swap(makeSwap(1, 1));
        UNSCOPED_INFO("the frame presents: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == 2));
        const auto& pass = completion.spans[1].frame->passes.front();
        if (!differs)
        {
            UNSCOPED_INFO("a matching background continues the segment");
            REQUIRE((pass.preservePreviousAttachments && pass.clears.empty() && drawTags(completion.spans[1]).empty()));
        }
        else
        {
            UNSCOPED_INFO("a differing background replays the segment's work from tile-split clears");
            REQUIRE((!pass.preservePreviousAttachments && pass.clears.size() == 2 && sameRect(pass.clears[0].rectangle, { 0, 0, 640, 720 }) &&
                     sameRect(pass.clears[1].rectangle, { 640, 0, 1280, 720 }) && pass.clears[1].colorValue == std::array<float, 4>{ 0, 0, 0, 0 } &&
                     drawTags(completion.spans[1]) == std::vector<std::uint8_t>{ 5 }));
        }
    }
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        std::string          error;
        auto                 clear = tiledClear();
        clear.flags                = 0x0F;
        clear.clear.depth = clear.clear.stencil = false;
        clear.depth.reset();
        UNSCOPED_INFO("the color-only tiled clear: " + error);
        REQUIRE(assembler.Clear(clear, error));
        record(assembler, 1, 5);
        scanout(assembler, 0);
        const auto result = assembler.Swap(makeSwap(1, 1));
        UNSCOPED_INFO("the frame presents: " + result.detail);
        REQUIRE(result.status == NativeFrameStatus::Provisional);
        const auto& pass = completion.spans[0].frame->passes.front();
        UNSCOPED_INFO("without a depth surface tile 1 starts from the clear color alone");
        REQUIRE((pass.target.depthEdramBase == kNoDepthEdramBase && pass.clears.size() == 2 && pass.clears[1].color && !pass.clears[1].depth &&
                 !pass.clears[1].stencil && pass.clears[1].colorValue == std::array<float, 4>{ 0, 0, 0, 0 }));
    }
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        std::string          error;
        bool                 partial = false;
        UNSCOPED_INFO("the tiled clear: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        auto target             = tiledTarget(0);
        target.tiling->rects[0] = { 0, 0, 600, 720 };
        target.tiling->rects[1] = { 600, 0, 1280, 720 };
        auto copy               = tileCopy(0, 0x00100340);
        copy.rectWidth          = 600;
        UNSCOPED_INFO("a next tile larger than the clearing one is refused: " + error);
        REQUIRE((!assembler.Resolve(target, copy, true, partial, error) && refusedAs(error, "tiled_background_extent: tile 1 is 680x720, tile 0 cleared 600x720")));
        const auto result = assembler.Swap(makeSwap(1));
        UNSCOPED_INFO("and fails the swap: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail == error));
    }
}

// Tile-0 clear after work is refused: tile 1 must keep the uncleared work.
void testTiledClearAfterWork()
{
    enum Variant
    {
        AfterDraw,
        AfterClear,
        AfterDepthClear,
        Count
    };

    for (int variant = 0; variant < Count; ++variant)
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(menuConfig(completion));
        std::string          error;
        std::uint64_t        numbered = 0;
        if (variant == AfterDraw)
            record(assembler, numbered = 1, kMenuUi[0]);
        else if (variant == AfterClear)
        {
            UNSCOPED_INFO("the first tiled clear: " + error);
            REQUIRE(assembler.Clear(tiledClear(), error));
        }
        else
        {
            // An untiled scanout leaves the presented target defined.
            UNSCOPED_INFO("the frame's tiled clear: " + error);
            REQUIRE(assembler.Clear(tiledClear(), error));
            scanOut(assembler);
            UNSCOPED_INFO("a cleared frame");
            REQUIRE(assembler.Swap(makeSwap(1)).status == NativeFrameStatus::Provisional);
            UNSCOPED_INFO("the depth clear: " + error);
            REQUIRE(assembler.Clear(depthOnlyClear(), error));
        }
        UNSCOPED_INFO("the tiled clear after work is refused: " + error);
        REQUIRE((!assembler.Clear(tiledClear(), error) && refusedAs(error, "clear_form_unsupported: tiled_clear_after_work")));
        scanOut(assembler);
        const auto result = assembler.Swap(makeSwap(variant == AfterDepthClear ? 2 : 1, numbered));
        UNSCOPED_INFO("strict leaves the refusal to the ledger: " + result.detail);
        REQUIRE(result.status == NativeFrameStatus::Provisional);
    }
}

// SetPredication masks for a depth-only clear: tile 0 alone runs it.
void testDepthClearPredicated()
{
    FakeCompletion       completion;
    NativeFrameAssembler assembler(menuConfig(completion));
    std::string          error;
    auto                 words = tiledWords();
    words[kTileDepth + 0x1C]   = 0x530;
    GuestClear predicated;
    UNSCOPED_INFO("the predicated depth clear decodes");
    REQUIRE((ReadGuestClear(wordReader(words), { kGraphics, 0x30, kGuestDepthOnlyClearIndex, 0, 0, 1280, 720, 0 }, kStack, 1.0, predicated) &&
             predicated.predicate == 1));
    UNSCOPED_INFO("the tiled clear: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    UNSCOPED_INFO("a depth clear predicated to tile 0 is refused: " + error);
    REQUIRE((!assembler.Clear(predicated, error) && refusedAs(error, "clear_form_unsupported: depth_clear_predicated 0x3")));
}

void testDepthClearRect()
{
    FakeCompletion       completion;
    NativeFrameAssembler assembler(menuConfig(completion));
    std::string          error;
    GuestClear           tile;
    UNSCOPED_INFO("the tile-sized depth clear decodes");
    REQUIRE(ReadGuestClear(wordReader(unpredicated(tiledWords())), { kGraphics, 0x30, kGuestDepthOnlyClearIndex, 0, 0, 640, 720, 0 }, kStack, 1.0, tile));
    UNSCOPED_INFO("the tiled clear: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    UNSCOPED_INFO("a depth clear of part of the screen is refused: " + error);
    REQUIRE((!assembler.Clear(tile, error) && refusedAs(error, "clear_form_unsupported: depth_clear_rect 0,0,640,720")));
}

// HUD stencil rectangles cross x=640 and clear only their covered pixels.
void testStencilClearRect()
{
    const auto stencilClear = [](std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom)
    {
        GuestClear clear;
        UNSCOPED_INFO("the stencil clear decodes");
        REQUIRE(ReadGuestClear(wordReader(unpredicated(tiledWords())), { kGraphics, 0x20, kGuestDepthOnlyClearIndex, std::uint32_t(left), std::uint32_t(top), std::uint32_t(right), std::uint32_t(bottom), 0 }, kStack, 1.0, clear));
        return clear;
    };
    FakeCompletion       completion;
    NativeFrameAssembler assembler(tiledConfig(completion));
    std::string          error;
    UNSCOPED_INFO("the tiled clear: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    record(assembler, 1, 5);
    UNSCOPED_INFO("a partial stencil clear is admitted: " + error);
    REQUIRE((assembler.Clear(stencilClear(830, 624, 1190, 720), error) && error.empty()));
    record(assembler, 2, 6);
    UNSCOPED_INFO("a stencil clear past the screen is refused: " + error);
    REQUIRE((!assembler.Clear(stencilClear(830, 624, 1290, 720), error) && error.starts_with("clear_form_unsupported: ")));
    scanout(assembler);
    const auto result = assembler.Swap(makeSwap(1, 2));
    UNSCOPED_INFO("the frame presents: " + result.detail);
    REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.size() == 1));
    const auto& passes = completion.spans[0].frame->passes;
    UNSCOPED_INFO("the stencil clear keeps its rectangle in a preserving pass");
    REQUIRE((passes.size() == 2 && passes[1].preservePreviousAttachments && passes[1].clears.size() == 1 && !passes[1].clears[0].color &&
             !passes[1].clears[0].depth && passes[1].clears[0].stencil && passes[1].clears[0].rectangle.left == 830 &&
             passes[1].clears[0].rectangle.top == 624 && passes[1].clears[0].rectangle.right == 1190 && passes[1].clears[0].rectangle.bottom == 720 &&
             drawTags(completion.spans[0]) == std::vector<std::uint8_t>{ 5, 6 }));
}

// A predicated clear without active tiling selects no modeled tile.
void testPredicatedUntiled()
{
    FakeCompletion       completion;
    NativeFrameAssembler assembler(menuConfig(completion));
    std::string          error;
    auto                 words = tiledWords();
    words[kGraphics + 0x2ABC]  = 0;
    GuestClear clear;
    UNSCOPED_INFO("the untiled predicated clear decodes");
    REQUIRE((ReadGuestClear(wordReader(words), { kGraphics, 0x3F, 0, 0, 0, 640, 720, kTileColor }, kStack, 1.0, clear) && clear.predicate == 1 &&
             !clear.tiling.Active()));
    UNSCOPED_INFO("it is refused: " + error);
    REQUIRE((!assembler.Clear(clear, error) && refusedAs(error, "clear_form_unsupported: predicated_untiled 0x3")));
}

// A copy of the untiled presented frame into a texture, such as a backdrop grab.
void testResolvePresented()
{
    FakeCompletion       completion;
    NativeFrameAssembler assembler(menuConfig(completion));
    std::string          error;
    bool                 partial = false;
    UNSCOPED_INFO("the tiled clear: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    record(assembler, 1, kMenuScene[0]);
    auto target = tiledTarget(0);
    target.tiling.reset();
    auto copy       = scanoutCopy();
    copy.destBase   = 0x1E3B0000;
    const auto mark = assembler.Resolve(target, copy, false, partial, error);
    UNSCOPED_INFO("an off-screen copy of the untiled presented target is refused: " + error);
    REQUIRE((!mark && refusedAs(error, "resolve_presented_unmodeled")));
}

// The world scene's pass 3: a tiled pass on the presented 2X target whose
// per-tile resolves copy depth (k1, no clear) and then color (k01, clear
// bits on tile 0) to textures. The pass leaves presentation.
GuestTarget tiledSlot(std::uint32_t tile, std::uint32_t slot)
{
    auto target = tiledTarget(tile);
    target.slot = slot;
    return target;
}

// The harvested pass-3 copies (frame 4125): depth to fetch BD0F0096 (k_24_8,
// physical 1D0F0000), color to FD817006 (physical 1D818000), both 1280x720.
GuestResolveCopy textureCopy(std::uint32_t tile, std::uint32_t control)
{
    auto       copy   = tileCopy(tile, control);
    const bool depth  = (control & 7) == kGuestDepthSlot;
    copy.destBase     = depth ? 0x1D0F0000 : 0x1D818000;
    copy.destInfo     = depth ? 0x302 : 0x300;
    copy.destPitch    = 0x02D00500;
    copy.destFetch[1] = depth ? 0xBD0F0096 : 0xFD817006;
    copy.destFetch[2] = (1280 - 1) | ((720 - 1) << 13);
    return copy;
}

// The scanout pair after the pass: the same tiles into the 1280x720 frontbuffer.
GuestResolveCopy scanoutTile(std::uint32_t tile)
{
    auto copy         = tileCopy(tile, tile ? 0x00100040 : 0x00100340);
    copy.destFetch[1] = 0xFF6F7006;
    copy.destFetch[2] = (1280 - 1) | ((720 - 1) << 13);
    return copy;
}

// Eager tiled graph submits each graphable color/depth tile at record time.
void testEagerTiledSegments()
{
    struct Step
    {
        std::uint32_t tile, slot, control;
    };

    constexpr std::array<Step, 4> kPass{ { { 0, kGuestDepthSlot, 0x00100014 }, { 0, 0, 0x00100340 }, { 1, kGuestDepthSlot, 0x00100014 }, { 1, 0, 0x00100040 } } };
    FakeCompletion                completion;
    auto                          config = menuConfig(completion);
    config.resolveGraph                  = true;
    config.eagerGraphSegments            = true;
    NativeFrameAssembler assembler(config);
    std::string          error;
    UNSCOPED_INFO("the pass clear: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
    record(assembler, 1, kMenuScene[0], &verdict);
    UNSCOPED_INFO("the pass draw is admitted as presented work");
    REQUIRE(!verdict);
    std::uint64_t mark = 0;
    for (std::size_t n = 0; n < kPass.size(); ++n)
    {
        bool       partial = false;
        const auto target  = tiledSlot(kPass[n].tile, kPass[n].slot);
        const auto copy    = textureCopy(kPass[n].tile, kPass[n].control);
        mark               = assembler.Resolve(target, copy, assembler.ScanoutCandidate(target, copy), partial, error);
        UNSCOPED_INFO(std::format("tile resolve {} submits its segment at the resolve: {}", n, error));
        REQUIRE((mark && completion.spans.size() == n + 1 && completion.spans.back().segment && completion.spans.back().resolves.size() == 1 &&
                 completion.spans.back().resolves[0].mark == mark && !assembler.Failure()));
    }
    UNSCOPED_INFO("the read's flush finds the last segment submitted");
    REQUIRE((assembler.FlushThrough(mark).fence == kPass.size() && completion.spans.size() == kPass.size()));
}

void testTiledTexturePass()
{
    struct Step
    {
        std::uint32_t tile, slot, control;
    };

    constexpr std::array<Step, 4> kPass{ { { 0, kGuestDepthSlot, 0x00100014 }, { 0, 0, 0x00100340 }, { 1, kGuestDepthSlot, 0x00100014 }, { 1, 0, 0x00100040 } } };
    const auto                    run = [](const std::array<Step, 4>& steps, NativeFrameAssembler& assembler, std::string& error)
    {
        UNSCOPED_INFO("the pass clear: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        record(assembler, 1, kMenuScene[0], &verdict);
        UNSCOPED_INFO("the pass draw is admitted as presented work");
        REQUIRE(!verdict);
        std::vector<std::uint64_t> marks;
        for (const auto& step : steps)
        {
            bool partial = false;
            // As the hook routes it: a color tile after the pass's depth tile
            // is a texture, one before it a scanout tile.
            const auto target = tiledSlot(step.tile, step.slot);
            const auto copy   = textureCopy(step.tile, step.control);
            marks.push_back(assembler.Resolve(target, copy, assembler.ScanoutCandidate(target, copy), partial, error));
            if (!marks.back())
                break;
            completion::DrawFlush flush;
            for (unsigned attempt = 0; attempt < 4 && !flush.fence; ++attempt)
                flush = assembler.FlushThrough(marks.back());
            UNSCOPED_INFO("the resolve frame is submitted: " + flush.error);
            REQUIRE(flush.fence != 0);
        }
        return marks;
    };
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(menuConfig(completion));
        std::string          error;
        const auto           marks = run(kPass, assembler, error);
        UNSCOPED_INFO("every tile resolve of the pass is admitted: " + error);
        REQUIRE((marks.size() == 4 && marks.back() && error.empty()));
        for (std::size_t n = 0; n < marks.size(); ++n)
        {
            const auto span = std::ranges::find(completion.spans, marks[n], &NativeCompletionSpan::resolveMark);
            UNSCOPED_INFO("each resolve is its own producer");
            REQUIRE(span != completion.spans.end());
            const auto& pass    = resolvePass(*span, marks[n]);
            const auto& resolve = pass.resolves[0];
            const bool  depth   = kPass[n].slot == kGuestDepthSlot;
            UNSCOPED_INFO(std::format("resolve {} replays the pass clear, tile backgrounds and draw, and copies its tile", n));
            REQUIRE((pass.target.width == 1280 && pass.target.sampleCount == 4 && pass.target.depthEdramBase == 0x530 && pass.DrawCount() == 1 &&
                     pass.clears.size() == (kPass[n].tile ? 3U : 1U) && pass.clears[0].rectangle.right == 640 && resolve.depth == depth &&
                     resolve.sourceX == kPass[n].tile * 640 && resolve.width == 640 &&
                     resolve.samples == (depth ? std::array<std::uint32_t, 2>{ 3, 3 } : std::array<std::uint32_t, 2>{ 0, 3 }) &&
                     resolve.guestWords == (depth ? NativeGuestWordPacking::Depth24Stencil8 : NativeGuestWordPacking::Color8888) &&
                     span->guestResolve->packing == (depth ? GuestResolvePacking::Depth24Stencil8Words : GuestResolvePacking::Color8888Words)));
        }
        // The pass left presentation: a fresh tiled clear starts the frame
        // the swap shows, which holds only its own draw.
        UNSCOPED_INFO("a tiled clear after the pass is a new pass: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        record(assembler, 2, kMenuScene[1], &verdict);
        UNSCOPED_INFO("the presented draw is admitted");
        REQUIRE(!verdict);
        // Outside the pass the same tiles into the frontbuffer are scanout
        // candidates; a 1X 1280x720 source (K14) never is.
        auto oneSample = tiledTarget(0);
        oneSample.tiling.reset();
        oneSample.native.sampleCount = 1;
        auto plain                   = scanoutTile(0);
        plain.copyControl            = 0x00100000;
        UNSCOPED_INFO("the scanout pair is classified by its presented 2X source and slot");
        REQUIRE((assembler.ScanoutCandidate(tiledTarget(0), scanoutTile(0)) && assembler.ScanoutCandidate(tiledTarget(1), scanoutTile(1)) &&
                 !assembler.ScanoutCandidate(oneSample, plain) && !assembler.ScanoutCandidate(tiledSlot(0, kGuestDepthSlot), scanoutTile(0))));
        scanout(assembler);
        const auto result = assembler.Swap(makeSwap(1, 2));
        UNSCOPED_INFO("the swap presents only the frame after the pass: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && drawTags(completion.spans.back()) == std::vector<std::uint8_t>{ kMenuScene[1] }));
    }
    {
        // Presented work after the pass needs a full clear first.
        FakeCompletion       completion;
        NativeFrameAssembler assembler(menuConfig(completion));
        std::string          error;
        UNSCOPED_INFO("the pass: " + error);
        REQUIRE(run(kPass, assembler, error).back());
        record(assembler, 2, kMenuScene[1]);
        const auto result = assembler.Swap(makeSwap(1, 2));
        UNSCOPED_INFO("a presented draw over the pass's EDRAM fails: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.find("tiled_target_uncleared") != std::string::npos));
    }
    {
        // A segment that already rendered the pass as presented work.
        FakeCompletion       completion;
        NativeFrameAssembler assembler(menuConfig(completion));
        std::string          error;
        bool                 partial = false;
        UNSCOPED_INFO("the pass clear: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        const auto draw = record(assembler, 1, kMenuScene[0]);
        UNSCOPED_INFO("a guest fence cuts a segment");
        REQUIRE(assembler.FlushThrough(draw).fence != 0);
        UNSCOPED_INFO("a segmented pass is refused by name: " + error);
        REQUIRE((!assembler.Resolve(tiledSlot(0, kGuestDepthSlot), textureCopy(0, 0x00100014), false, partial, error) && refusedAs(error, "tiled_texture_pass_segmented")));
    }

    const std::array<std::pair<std::array<Step, 4>, std::string>, 4> mutants{ {
        { { { { 0, 0, 0x00100340 }, kPass[0], kPass[2], kPass[3] } }, "tiled_texture_pass_after_scanout" },
        { { { kPass[0], { 0, 0, 0x00100040 }, kPass[2], kPass[3] } }, "tiled_background_undefined: tile 1 slot 4" },
        { { { { 0, kGuestDepthSlot, 0x00100314 }, kPass[1], kPass[2], kPass[3] } }, "resolve_form_unsupported: control 00100314" },
        { { { { 0, kGuestDepthSlot, 0x00100010 }, kPass[1], kPass[2], kPass[3] } }, "resolve_form_unsupported: control 00100010" },
    } };
    for (const auto& [steps, expected] : mutants)
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(menuConfig(completion));
        std::string          error;
        const auto           marks = run(steps, assembler, error);
        UNSCOPED_INFO("a mutated pass is refused as " + expected + ": " + error);
        REQUIRE((!marks.back() && refusedAs(error, expected)));
    }
    {
        // Tile 1's rectangle under tile 0's predicate.
        FakeCompletion       completion;
        NativeFrameAssembler assembler(menuConfig(completion));
        std::string          error;
        bool                 partial = false;
        UNSCOPED_INFO("the pass clear: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        record(assembler, 1, kMenuScene[0]);
        auto copy  = textureCopy(0, 0x00100014);
        copy.rectX = copy.destX = 640;
        UNSCOPED_INFO("a rectangle outside the predicated tile is refused: " + error);
        REQUIRE((!assembler.Resolve(tiledSlot(0, kGuestDepthSlot), copy, false, partial, error) &&
                 refusedAs(error, "tiled_resolve_form: tile 0 rect 640,0,1280,720 point 640,0 mask 0x3")));
    }
}

// Offscreen copies use origin and 2X samples; other forms keep named errors.
void testOffscreenResolveForms()
{
    struct Case
    {
        std::uint32_t                samples, control, width, x;
        std::string                  refusal;
        std::array<std::uint32_t, 2> host;
    };

    const std::array<Case, 8> cases{ {
        { 4, 0x100000, 1024, 0, {}, { 0, 0 } },
        { 4, 0x100010, 1024, 0, {}, { 3, 3 } },
        { 4, 0x100040, 1024, 0, {}, { 0, 3 } },
        { 1, 0x100000, 512, 0, {}, { 0, 0 } },
        { 1, 0x100040, 1024, 0, "resolve_form_unsupported: control 00100040 samples 1", {} },
        { 4, 0x100020, 1024, 0, "resolve_form_unsupported: control 00100020 samples 4", {} },
        { 1, 0x100300, 1024, 0, "resolve_form_unsupported: control 00100300 samples 1", {} },
        { 1, 0x100000, 512, 8, "resolve_form_unsupported: rect 8,0,512x1024 point 0,0 of 0,0,1024,1024", {} },
    } };
    for (const auto& value : cases)
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        auto                 target = offscreen(0x300);
        target.native.sampleCount   = value.samples;
        auto copy                   = fullCopy(target);
        copy.copyControl            = value.control;
        copy.rectWidth              = value.width;
        copy.rectX                  = value.x;
        std::string error;
        bool        partial = false;
        UNSCOPED_INFO("the off-screen clear: " + error);
        REQUIRE(assembler.Clear(fullClear(target), error) == 1);
        const auto mark = assembler.Resolve(target, copy, false, partial, error);
        if (!value.refusal.empty())
        {
            UNSCOPED_INFO("the unmodeled resolve form is named: " + error);
            REQUIRE((!mark && refusedAs(error, value.refusal)));
            continue;
        }
        UNSCOPED_INFO(std::format("control {:08X} resolves: {}", value.control, error));
        REQUIRE((mark == 2 && assembler.FlushThrough(mark).fence));
        const auto& resolve = resolvePass(completion.spans.back(), mark).resolves[0];
        UNSCOPED_INFO(std::format("control {:08X} of {} samples resolves host samples {},{}", value.control, value.samples, value.host[0], value.host[1]));
        REQUIRE((resolve.samples == value.host && resolve.width == value.width && resolve.height == 1024 && !resolve.sourceX));
    }
}

// k_16_16 stores value/32; RGBA8 preserves bytes.
void testSourceFormatResolves()
{
    struct Case
    {
        NativeDrawReplayTargetFormat  format;
        std::uint32_t                 width, height, samples, base, control, destInfo, copyHeight;
        std::array<float, 4>          clear, host;
        GuestResolvePacking           packing;
        NativeGuestWordPacking        words;
        NativeDrawReplayTextureFormat texture;
        std::array<std::uint32_t, 2>  sampled;
    };

    // K12 uses 1X k_16_16 format 31; K20 uses 2X k_2_10_10_10 format 7 from
    // fetch 54.
    const std::array<Case, 3> cases{ {
        { NativeDrawReplayTargetFormat::Rg16Snorm, 1280, 720, 1, 0x2D0, 0x100000, 0xEF82, 720, { 16, -64, 0.5F, 2 }, { 0.5F, -1, 0.015625F, 0.0625F }, GuestResolvePacking::Float16x2Words, NativeGuestWordPacking::Float16x2, NativeDrawReplayTextureFormat::R32Uint, { 0, 0 } },
        { NativeDrawReplayTargetFormat::Rgb10a2, 640, 368, 4, 0, 0x100040, 0x380, 360, { 0.25F, 0.5F, 0.75F, 1 }, { 0.25F, 0.5F, 0.75F, 1 }, GuestResolvePacking::Color2101010Words, NativeGuestWordPacking::Color2101010, NativeDrawReplayTextureFormat::R32Uint, { 0, 3 } },
        { NativeDrawReplayTargetFormat::Rgba8, 1024, 1024, 1, 0x300, 0x100000, kScanoutDestInfo, 1024, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, GuestResolvePacking::HostRgba8, NativeGuestWordPacking::None, NativeDrawReplayTextureFormat::Rgba8, { 0, 0 } },
    } };
    for (const auto& value : cases)
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        GuestTarget          target;
        target.native          = { value.width, value.height, value.samples, value.format, value.base };
        auto clear             = fullClear(target);
        clear.clear.colorValue = value.clear;
        auto copy              = fullCopy(target);
        copy.copyControl       = value.control;
        copy.destInfo          = value.destInfo;
        copy.rectHeight        = value.copyHeight;
        std::string error;
        bool        partial = false;
        const auto  name    = std::format("format {}", static_cast<unsigned>(value.format));
        UNSCOPED_INFO(name + " clears off-screen: " + error);
        REQUIRE(assembler.Clear(clear, error) == 1);
        const auto mark = assembler.Resolve(target, copy, false, partial, error);
        UNSCOPED_INFO(name + " resolves off-screen: " + error);
        REQUIRE((mark == 2 && assembler.FlushThrough(mark).fence));
        const auto& span = completion.spans.back();
        const auto& pass = resolvePass(span, mark);
        const auto& out  = pass.resolves[0];
        UNSCOPED_INFO(name + " replays its clear as the host target holds it");
        REQUIRE((pass.target.format == value.format && pass.clears.size() == 1 && pass.clears[0].colorValue == value.host));
        UNSCOPED_INFO(name + " resolves into its packed texture");
        REQUIRE((out.guestWords == value.words && out.samples == value.sampled && out.width == value.width && out.height == value.copyHeight &&
                 span.frame->textures.size() == 1 && span.frame->textures[0].format == value.texture && span.frame->returnRawOutput));
        UNSCOPED_INFO(name + " encodes with its source's packing");
        REQUIRE((span.guestResolve && span.guestResolve->packing == value.packing));
    }
}

// K18 fog targets are depthless; tags 0x61-0x63 fail.
void testSignedTargetDraws()
{
    struct Format
    {
        NativeDrawReplayTargetFormat format;
        std::uint32_t                colorInfo, destInfo;
        const char*                  name;
    };

    for (const auto& form : { Format{ NativeDrawReplayTargetFormat::Rg16Snorm, 0x000402D0, 0xEF82, "k_16_16" },
                              Format{ NativeDrawReplayTargetFormat::Rgb10a2, 0x000202D0, 0x380, "k_2_10_10_10" } })
        for (const std::uint32_t tag : { 0x60U, 0x61U, 0x62U, 0x63U })
        {
            const std::string          name = std::format("{} draw {:#x}", form.name, tag);
            FakeCompletion             completion;
            NativeFrameAssemblerConfig config;
            config.completion = &completion;
            config.readTarget = [color = targetReader(1280, 720, false, 0x2D0), info = form.colorInfo](std::uint32_t address, std::uint32_t& out)
            {
                // RB_COLOR_INFO format 4 (k_16_16) or 2 (k_2_10_10_10) at EDRAM 0x2D0.
                if (address == kSurface + 0x1C)
                {
                    out = info;
                    return true;
                }
                return color(address, out);
            };
            config.build = [format = form.format](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string&)
            {
                const auto tag   = static_cast<std::uint8_t>(value.arguments[7]);
                draw             = makeDraw(tag);
                draw.sampleCount = draw.sampleMask = 1;
                draw.targetFormat                  = format;
                draw.depth.enabled                 = tag == 0x61;
                draw.depth.writeEnabled            = tag == 0x62;
                draw.stencil.enabled               = tag == 0x63;
                return true;
            };
            NativeFrameAssembler assembler(config);
            GuestTarget          target;
            target.native = { 1280, 720, 1, form.format, 0x2D0 };
            auto copy     = fullCopy(target);
            copy.destInfo = form.destInfo;
            std::string                     error;
            bool                            partial = false;
            std::optional<NativeDrawReason> verdict;
            UNSCOPED_INFO(name + ": the target clears: " + error);
            REQUIRE(assembler.Clear(fullClear(target), error) == 1);
            record(assembler, 1, static_cast<std::uint8_t>(tag), &verdict);
            const auto mark = assembler.Resolve(target, copy, false, partial, error);
            if (tag != 0x60)
            {
                UNSCOPED_INFO(name + " drops: " + error);
                REQUIRE((verdict && !mark && partial && refusedAs(error, "resolve_source_partial")));
                const auto result = assembler.Swap(makeSwap(2, 1));
                UNSCOPED_INFO(name + " names its cause: " + result.detail);
                REQUIRE(result.detail.find("frame_target_mismatch") != std::string::npos);
                continue;
            }
            UNSCOPED_INFO(name + " is admitted and resolves: " + error);
            REQUIRE((!verdict && mark == 3 && assembler.FlushThrough(mark).fence));
            const auto& pass = resolvePass(completion.spans.back(), mark);
            UNSCOPED_INFO(name + " replays the clear and the draw as guest words");
            REQUIRE((pass.target.format == form.format && pass.clears.size() == 1 && pass.DrawCount() == 1 && pass.Draw(0).vertexConstants[0] == 0x60 &&
                     pass.resolves[0].guestWords == (form.format == NativeDrawReplayTargetFormat::Rg16Snorm ? NativeGuestWordPacking::Float16x2 : NativeGuestWordPacking::Color2101010)));
        }
}

// Packed color clears refuse depth/stencil forms; no host depth surface.
void testPackedTargetClears()
{
    for (const auto format : { NativeDrawReplayTargetFormat::Rg16Snorm, NativeDrawReplayTargetFormat::Rgb10a2 })
        for (const auto [depth, stencil] : { std::pair{ true, false }, std::pair{ false, true }, std::pair{ true, true } })
        {
            const std::string          name = std::format("format {} clearing depth {} stencil {}", static_cast<unsigned>(format), depth, stencil);
            FakeCompletion             completion;
            NativeFrameAssemblerConfig config;
            config.completion = &completion;
            NativeFrameAssembler assembler(config);
            GuestTarget          target;
            target.native       = { 1280, 720, 1, format, 0x2D0 };
            auto clear          = fullClear(target);
            clear.flags         = 0x31;
            clear.clear.depth   = depth;
            clear.clear.stencil = stencil;
            clear.depth         = GuestDepthSurface{ 0x5000, 0x200, 0x200, 1280, 720, 1 };
            std::string error;
            UNSCOPED_INFO(name + " is refused: " + error);
            REQUIRE((!assembler.Clear(clear, error) && refusedAs(error, "clear_form_unsupported: depth_clear_packed_target")));
            clear.clear.depth = clear.clear.stencil = false;
            UNSCOPED_INFO(name + " without depth or stencil clears: " + error);
            REQUIRE(assembler.Clear(clear, error));
        }
}

// R1/R2 clears fit scissor; off-origin, oversize and presented targets refuse.
void testSubExtentClears()
{
    // Panel is 928x688, pitch 960, with a 900x675 clear.
    const auto panel = []
    {
        GuestTarget target;
        target.native     = { 928, 688, 1, NativeDrawReplayTargetFormat::Rgba8, 0 };
        auto clear        = fullClear(target);
        clear.flags       = 0x3F;
        clear.clear.depth = clear.clear.stencil = true;
        clear.depth                             = GuestDepthSurface{ 0x5000, 0x5FC, 0x5FC, 928, 688, 1 };
        clear.clear.rectangle                   = { 0, 0, 900, 675 };
        return clear;
    };
    const auto resolve = [&panel](std::uint32_t width, std::uint32_t height, std::string& error)
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        const auto           clear = panel();
        UNSCOPED_INFO("the panel clear is admitted: " + error);
        REQUIRE(assembler.Clear(clear, error) == 1);
        auto copy          = fullCopy(clear.target);
        copy.rectWidth     = width;
        copy.rectHeight    = height;
        bool       partial = false;
        const auto mark    = assembler.Resolve(clear.target, copy, false, partial, error);
        if (!mark)
            return false;
        UNSCOPED_INFO("the panel resolve flushes");
        REQUIRE(assembler.FlushThrough(mark).fence);
        const auto& pass = resolvePass(completion.spans.back(), mark);
        const auto& rect = pass.clears.at(0).rectangle;
        UNSCOPED_INFO("the host clears the panel rectangle of the whole surface and resolves inside it");
        REQUIRE((pass.target.width == 928 && pass.target.height == 688 && pass.clears.size() == 1 && rect.left == 0 && rect.top == 0 && rect.right == 900 &&
                 rect.bottom == 675 && pass.clears[0].depth && pass.clears[0].stencil && pass.resolves[0].width == width && pass.resolves[0].height == height));
        return true;
    };
    std::string error;
    UNSCOPED_INFO("the panel rectangle resolves: " + error);
    REQUIRE(resolve(900, 675, error));
    UNSCOPED_INFO("a resolve inside the rectangle: " + error);
    REQUIRE(resolve(450, 300, error));
    UNSCOPED_INFO("the surface beyond the cleared rectangle is unmodeled: " + error);
    REQUIRE((!resolve(928, 688, error) && refusedAs(error, "resolve_form_unsupported: rect 0,0,928x688 point 0,0 of 0,0,900,675")));
    UNSCOPED_INFO("a row past the cleared rectangle is unmodeled: " + error);
    REQUIRE((!resolve(900, 676, error) && refusedAs(error, "resolve_form_unsupported: rect 0,0,900x676 point 0,0 of 0,0,900,675")));

    const auto refused = [](GuestClear clear, const std::string& name)
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        std::string          error;
        UNSCOPED_INFO(name + " is refused: " + error);
        REQUIRE((!assembler.Clear(clear, error) && refusedAs(error, "clear_form_unsupported: color rect")));
    };
    auto shifted                 = panel();
    shifted.clear.rectangle.left = 8;
    refused(shifted, "a clear off the origin");
    auto past                  = panel();
    past.clear.rectangle.right = 929;
    refused(past, "a clear past the target");
    auto presented          = panel();
    presented.target.native = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    presented.depth         = GuestDepthSurface{ 0x5000, 0x530, 0x530, 1280, 720, 4 };
    refused(presented, "a partial clear of the presented target");
}

// Blur-chain overwrite requires full opaque 2X coverage; tags isolate refusals.
void testOverwrittenSources()
{
    // Tags 0x55-0x59 break masking, alpha, depth/stencil or alpha-to-coverage.
    const auto run = [](const std::vector<std::uint32_t>& tags, const std::string& refusal)
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        // A depth surface of the same 2X shape at EDRAM 0x690, which the depth
        // and stencil tests of 0x57 and 0x58 read.
        config.readTarget = [color = targetReader(640, 368, true, 0)](std::uint32_t address, std::uint32_t& out)
        {
            const std::map<std::uint32_t, std::uint32_t> depth{
                { kGraphics + 0x30A8, kSurface + 0x100 }, { kSurface + 0x11C, 0x690 }, { kSurface + 0x124, (639U << 18) | (367U << 3) }, { kSurface + 0x118, 1U << 16 }
            };
            if (const auto found = depth.find(address); found != depth.end())
            {
                out = found->second;
                return true;
            }
            return color(address, out);
        };
        config.build = [](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string&)
        {
            draw                   = makeDraw(static_cast<std::uint8_t>(value.arguments[7]));
            draw.width             = 640;
            draw.height            = 368;
            draw.viewport          = { 0, 0, 640, 368, 0, 1 };
            draw.scissor           = { 0, 0, 640, 360 };
            draw.blend.enabled     = value.arguments[7] == 0x51 || value.arguments[7] >= 0x5A;
            draw.blend.sourceColor = draw.blend.sourceAlpha = NativeDrawReplayBlendFactor::SourceAlpha;
            draw.blend.destinationColor = draw.blend.destinationAlpha = NativeDrawReplayBlendFactor::InverseSourceAlpha;
            const auto tag                                            = value.arguments[7];
            draw.sampleMask                                           = tag == 0x55 ? 1 : 9;
            draw.blend.writeMask                                      = tag == 0x56 ? 0x7 : 0xF;
            draw.depth.enabled                                        = tag == 0x57;
            draw.stencil.enabled                                      = tag == 0x58;
            draw.blend.alphaToCoverage                                = tag == 0x59;
            // 0x5B adds the source to the target: alpha 1 does not replace it.
            if (tag == 0x5B)
                draw.blend.destinationColor = draw.blend.destinationAlpha = NativeDrawReplayBlendFactor::One;
            return true;
        };
        // 0x5A and 0x5B prove alpha 1, as the fog composite's cover does.
        config.coverage = [](const NativeDrawSubmission& value, const NativeDrawReplayRecipe&) -> std::optional<NativeFrameDrawCover>
        {
            if (value.arguments[7] == 0x53)
                return std::nullopt;
            return NativeFrameDrawCover{ { 0, 0, value.arguments[7] == 0x54 ? 320 : 640, 368 }, value.arguments[7] >= 0x5A };
        };
        NativeFrameAssembler assembler(config);
        GuestTarget          target;
        target.native = { 640, 368, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
        GuestResolveCopy copy;
        copy.copyControl       = 0x100040;
        copy.destBase          = 0x1BD85000;
        copy.destInfo          = kScanoutDestInfo;
        copy.rectWidth         = 640;
        copy.rectHeight        = 360;
        std::uint64_t sequence = 0;
        for (const auto tag : tags)
        {
            std::optional<NativeDrawReason> verdict;
            record(assembler, ++sequence, tag, &verdict);
            UNSCOPED_INFO(std::format("draw {:#x} is admitted", tag));
            REQUIRE(!verdict);
        }
        std::string error;
        bool        partial = false;
        const auto  mark    = assembler.Resolve(target, copy, false, partial, error);
        if (!refusal.empty())
        {
            UNSCOPED_INFO("the uncleared source is refused as " + refusal + ": " + error);
            REQUIRE((!mark && refusedAs(error, refusal)));
            return;
        }
        UNSCOPED_INFO("the overwritten source resolves: " + error);
        REQUIRE((mark && !partial && assembler.FlushThrough(mark).fence));
        const auto& pass = resolvePass(completion.spans.back(), mark);
        UNSCOPED_INFO("the resolve replays only the draws and selects k01");
        REQUIRE((pass.clears.empty() && pass.DrawCount() == tags.size() && pass.resolves[0].samples == std::array<std::uint32_t, 2>{ 0, 3 } &&
                 pass.resolves[0].width == 640 && pass.resolves[0].height == 360));
    };
    run({ 0x50 }, {});
    run({ 0x50, 0x51 }, {});
    run({ 0x5A }, {});
    run({ 0x5B }, "resolve_source_unmodeled: uninitialized");
    run({}, "resolve_source_unmodeled: other_source");
    run({ 0x51 }, "resolve_source_unmodeled: uninitialized");
    run({ 0x53 }, "resolve_source_unmodeled: uninitialized");
    run({ 0x54 }, "resolve_form_unsupported: rect 0,0,640x360 point 0,0 of 0,0,320,360");
    for (const std::uint32_t tag : { 0x55, 0x56, 0x57, 0x58, 0x59 })
        run({ tag }, "resolve_source_unmodeled: uninitialized");
}

void putFloat(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    const auto word = std::bit_cast<std::uint32_t>(value);
    for (unsigned k = 0; k < 4; ++k)
        bytes[offset + k] = static_cast<std::uint8_t>(word >> (8 * k));
}

// P20 coverage follows half-pixel transform, CCW culling and full target.
void testSceneQuadCoverage()
{
    // Capture maps x'=2x-1-1/1280 and y'=1+1/720-2y.
    const auto quad = [](float right)
    {
        auto draw             = makeDraw(0);
        draw.vertexShaderHash = 0x3418D1CAA13CAF50ULL;
        draw.pixelShaderHash  = 0x1262103F2F06927FULL;
        draw.sampleCount      = 1;
        draw.sampleMask       = 1;
        draw.vertexData.assign(4 * 32, 0);
        const std::array<std::array<float, 2>, 4> positions{ { { 0, 0 }, { 0, 1 }, { right, 1 }, { right, 0 } } };
        for (std::size_t i = 0; i < positions.size(); ++i)
        {
            putFloat(draw.vertexData, i * 32, positions[i][0]);
            putFloat(draw.vertexData, i * 32 + 4, positions[i][1]);
            putFloat(draw.vertexData, i * 32 + 12, 1);
        }
        draw.indices    = { 0, 1, 2, 0, 2, 3 };
        draw.indexCount = 6;
        const std::array<float, 16> constants{ 2, 0, 0, -1.0007812976837158F, 0, -2, 0, 1.0013889074325562F, 0, 0, 0.00010001000191550702F, 2.3799999837592622e-07F, 0, 0, 0, 1 };
        for (std::size_t i = 0; i < constants.size(); ++i)
            putFloat(draw.vertexConstants, i * 4, constants[i]);
        putFloat(draw.sharedConstants, 292, 1.0F / 1280);
        putFloat(draw.sharedConstants, 296, -1.0F / 720);
        draw.rasterizer.cull                  = 2;
        draw.rasterizer.frontCounterClockwise = true;
        return draw;
    };
    const auto text = [](const std::optional<NativeDrawReplayScissor>& rect)
    {
        return rect ? std::format("{},{},{},{}", rect->left, rect->top, rect->right, rect->bottom) : std::string("none");
    };
    UNSCOPED_INFO("the full quad covers the target: " + text(SceneQuadCoverage(quad(1))));
    REQUIRE(text(SceneQuadCoverage(quad(1))) == "0,0,1280,720");
    for (const auto [vertex, pixel] : { std::pair{ 0xDE0359DBD47D7FABULL, 0x560C753DAD5555ADULL }, std::pair{ 0x7C1DD533708BB131ULL, 0x818E4EAF9BA41B33ULL }, std::pair{ 0x38691F81BEA4AD28ULL, 0xB8C1B55C7CE2E502ULL }, std::pair{ 0xF95D105E51CF1A19ULL, 0x884C15F5AE654F89ULL }, std::pair{ 0x66B6CC03F44ED9B7ULL, 0x645E162E54094FF6ULL } })
    {
        auto pinned             = quad(1);
        pinned.vertexShaderHash = vertex;
        pinned.pixelShaderHash  = pixel;
        UNSCOPED_INFO(std::format("the k_2_10_10_10 pass {:016X} covers the target", vertex));
        REQUIRE(text(SceneQuadCoverage(pinned)) == "0,0,1280,720");
    }
    UNSCOPED_INFO("the left half quad covers 640 columns: " + text(SceneQuadCoverage(quad(0.5F))));
    REQUIRE(text(SceneQuadCoverage(quad(0.5F))) == "0,0,640,720");
    // The rasterizer's viewport scissor: a 640-column viewport stops a quad
    // its constants stretch over the whole 1280-column target.
    auto narrow     = quad(1);
    narrow.viewport = { 0, 0, 640, 720, 0, 1 };
    putFloat(narrow.vertexConstants, 0, 4);
    putFloat(narrow.vertexConstants, 12, -1.0F - 1.0F / 640);
    putFloat(narrow.sharedConstants, 292, 1.0F / 640);
    UNSCOPED_INFO("the viewport bounds the covered columns: " + text(SceneQuadCoverage(narrow)));
    REQUIRE(text(SceneQuadCoverage(narrow)) == "0,0,640,720");
    // Edges in screen pixels under an identity transform and no half-pixel
    // offset: snapping 359.8759765625 to 359.875 leaves row 359 partial,
    // 719.999 snaps to 720, and a left edge at 0.5 leaves column 0 partial.
    const auto edges = [&](float left, float bottom)
    {
        auto draw = quad(1);
        for (const auto [vertex, x, y] : { std::tuple{ 0, left, 0.0F }, std::tuple{ 1, left, bottom }, std::tuple{ 2, 1280.0F, bottom }, std::tuple{ 3, 1280.0F, 0.0F } })
        {
            putFloat(draw.vertexData, vertex * 32, x / 640 - 1);
            putFloat(draw.vertexData, vertex * 32 + 4, 1 - y / 360);
        }
        const std::array<float, 16> identity{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        for (std::size_t i = 0; i < identity.size(); ++i)
            putFloat(draw.vertexConstants, i * 4, identity[i]);
        putFloat(draw.sharedConstants, 292, 0);
        putFloat(draw.sharedConstants, 296, 0);
        return text(SceneQuadCoverage(draw));
    };
    UNSCOPED_INFO("a bottom edge snapped inside row 359 leaves it out: " + edges(0, 359.8759765625F));
    REQUIRE(edges(0, 359.8759765625F) == "0,0,1280,359");
    UNSCOPED_INFO("a bottom edge within 1/512 of 720 snaps to it: " + edges(0, 719.999F));
    REQUIRE(edges(0, 719.999F) == "0,0,1280,720");
    UNSCOPED_INFO("a left edge inside column 0 leaves it out: " + edges(0.5F, 720));
    REQUIRE(edges(0.5F, 720) == "1,0,1280,720");
    auto unclipped                 = quad(1);
    unclipped.rasterizer.depthClip = false;
    putFloat(unclipped.vertexConstants, 2 * 16 + 12, 2);
    UNSCOPED_INFO("without depth clip a far quad still covers");
    REQUIRE(text(SceneQuadCoverage(unclipped)) == "0,0,1280,720");

    const std::vector<std::pair<std::string, std::function<void(NativeDrawReplayRecipe&)>>> broken{
        { "an unpinned vertex shader", [](auto& draw)
          {
              draw.vertexShaderHash = 0x410718F3B9741E31ULL;
          } },
        { "one triangle", [](auto& draw)
          {
              draw.indices    = { 0, 1, 2 };
              draw.indexCount = 3;
          } },
        { "overlapping triangles", [](auto& draw)
          {
              draw.indices = { 0, 1, 2, 1, 2, 3 };
          } },
        { "front faces culled", [](auto& draw)
          {
              draw.rasterizer.cull = 1;
          } },
        { "a clockwise front", [](auto& draw)
          {
              draw.rasterizer.frontCounterClockwise = false;
          } },
        { "a negative w", [](auto& draw)
          {
              putFloat(draw.vertexConstants, 3 * 16 + 12, -1);
          } },
        { "a depth past the far plane", [](auto& draw)
          {
              putFloat(draw.vertexConstants, 2 * 16 + 12, 2);
          } },
        { "a rotated quad", [](auto& draw)
          {
              putFloat(draw.vertexConstants, 4, 0.1F);
          } },
        { "integer positions", [](auto& draw)
          {
              draw.sharedConstants[324] = 1;
          } },
        { "swapped positions", [](auto& draw)
          {
              draw.sharedConstants[288] = 1;
          } },
    };
    for (const auto& [name, mutate] : broken)
    {
        auto draw = quad(1);
        mutate(draw);
        UNSCOPED_INFO(name + " proves no coverage: " + text(SceneQuadCoverage(draw)));
        REQUIRE(!SceneQuadCoverage(draw));
    }

    // RB_COLORCONTROL decides the alpha test and alpha to mask.
    const auto covered = [&](std::vector<std::uint8_t> control)
    {
        const auto           coverage = MakeSceneDrawCoverage([control](std::uint64_t address, std::uint64_t size, bool physical)
                                                              {
                                                        return address == 0x40000000 + 0x293C && size == 4 && !physical ? control : std::vector<std::uint8_t>{};
                                                              });
        NativeDrawSubmission value;
        value.arguments[0] = 0x40000000;
        const auto cover   = coverage(value, quad(1));
        UNSCOPED_INFO("a scene quad proves no alpha");
        REQUIRE((!cover || !cover->alphaOne));
        return text(cover ? std::optional(cover->rect) : std::nullopt);
    };
    UNSCOPED_INFO("a scene draw without the alpha test covers");
    REQUIRE(covered({ 0x87, 0, 0, 4 }) == "0,0,1280,720");
    UNSCOPED_INFO("the alpha test may kill");
    REQUIRE(covered({ 0x87, 0, 0, 0x0C }) == "none");
    UNSCOPED_INFO("alpha to mask may drop samples");
    REQUIRE(covered({ 0x87, 0, 0, 0x14 }) == "none");
    UNSCOPED_INFO("an unread color control proves nothing");
    REQUIRE(covered({}) == "none");
}

// Fog composite key 62663883, caller 0x8230686C.
void testSceneCompositeCoverage()
{
    // Half-pixel quad corners at 0.25/1280.25 and 0.25/720.25 cover all pixel
    // centers.
    const auto composite = [](float right)
    {
        auto draw                                      = makeDraw(0);
        draw.vertexShaderHash                          = kSceneCompositeVertexHash;
        draw.pixelShaderHash                           = 0xF6CF41BEA360546BULL;
        draw.sampleCount                               = 1;
        draw.sampleMask                                = 1;
        draw.vertexAttributeCount                      = 3;
        draw.vertexStrideBytes                         = 48;
        const auto                                left = std::bit_cast<float>(0xBF800CCDU), top = std::bit_cast<float>(0x3F8016C1U), bottom = std::bit_cast<float>(0xBF7FD27DU);
        const std::array<std::array<float, 2>, 4> positions{ { { left, top }, { right, top }, { left, bottom }, { right, bottom } } };
        draw.vertexData.assign(positions.size() * 48, 0);
        for (std::size_t i = 0; i < positions.size(); ++i)
        {
            putFloat(draw.vertexData, i * 48, positions[i][0]);
            putFloat(draw.vertexData, i * 48 + 4, positions[i][1]);
            putFloat(draw.vertexData, i * 48 + 12, 1);
        }
        draw.indices    = { 2, 1, 0, 3, 1, 2 };
        draw.indexCount = 6;
        putFloat(draw.sharedConstants, 292, 1.0F / 1280);
        putFloat(draw.sharedConstants, 296, -1.0F / 720);
        // c255.x, the c252..c255 literal record's 13th word.
        draw.pixelConstants.assign(256 * 16, 0);
        putFloat(draw.pixelConstants, 255 * 16, 1);
        return draw;
    };
    const auto full = std::bit_cast<float>(0x3F7FE666U);
    const auto text = [](const std::optional<NativeDrawReplayScissor>& rect)
    {
        return rect ? std::format("{},{},{},{}", rect->left, rect->top, rect->right, rect->bottom) : std::string("none");
    };
    UNSCOPED_INFO("every pixel center of the target is covered: " + text(SceneCompositeCoverage(composite(full))));
    REQUIRE(text(SceneCompositeCoverage(composite(full))) == "0,0,1280,720");
    UNSCOPED_INFO("the composite is not a scene quad");
    REQUIRE(!SceneQuadCoverage(composite(full)));
    // A right edge at clip x 0 lands at 640.5: pixel 640's center lies on it.
    UNSCOPED_INFO("a partial composite covers its columns: " + text(SceneCompositeCoverage(composite(0))));
    REQUIRE(text(SceneCompositeCoverage(composite(0))) == "0,0,640,720");
    const std::vector<std::pair<std::string, std::function<void(NativeDrawReplayRecipe&)>>> broken{
        { "another pixel shader", [](auto& draw)
          {
              draw.pixelShaderHash ^= 1;
          } },
        { "another vertex shader", [](auto& draw)
          {
              draw.vertexShaderHash ^= 1;
          } },
        { "the guest vertex shader's digest, which no recipe carries", [](auto& draw)
          {
              draw.vertexShaderHash = 0xF65866A40B248418ULL;
          } },
        { "a multisampled target", [](auto& draw)
          {
              draw.sampleCount = 2;
          } },
        { "a guest stride", [](auto& draw)
          {
              draw.vertexStrideBytes = 32;
          } },
        { "a third triangle", [](auto& draw)
          {
              draw.indices.push_back(0);
          } },
    };
    for (const auto& [name, mutate] : broken)
    {
        auto draw = composite(full);
        mutate(draw);
        UNSCOPED_INFO(name + " proves no coverage: " + text(SceneCompositeCoverage(draw)));
        REQUIRE(!SceneCompositeCoverage(draw));
    }

    // The guest state of the harvested draw; each change proves nothing.
    constexpr std::uint32_t                      g = 0x40000000;
    const std::map<std::uint32_t, std::uint32_t> state{
        { 0x293C, 0x87000004 },
        { 0x2938, 0x07060706 },
        { 0x28DC, 0xF },
        { 0x2934, 0x24F00730 },
        { 0x2954, 4 },
        { 0x30A8, 0 },
        { 0x28C0, 0 },
        { 0x28C4, 0 },
        { 0x28C8, 0x02D00500 },
        { 0x4BC, 0x00001414 },
    };
    const auto covered = [&](const std::map<std::uint32_t, std::uint32_t>& words, const NativeDrawReplayRecipe& draw)
    {
        const auto           coverage = MakeSceneDrawCoverage([&words](std::uint64_t address, std::uint64_t size, bool physical)
                                                              {
                                                        const auto found = words.find(static_cast<std::uint32_t>(address - g));
                                                        if (address < g || size != 4 || physical || found == words.end())
                                                            return std::vector<std::uint8_t>{};
                                                        const auto word = found->second;
                                                        return std::vector<std::uint8_t>{ std::uint8_t(word >> 24), std::uint8_t(word >> 16), std::uint8_t(word >> 8), std::uint8_t(word) };
                                                              });
        NativeDrawSubmission value;
        value.arguments[0] = g;
        const auto cover   = coverage(value, draw);
        return cover ? text(cover->rect) + (cover->alphaOne ? " alpha 1" : "") : std::string("none");
    };
    UNSCOPED_INFO("the pinned composite overwrites the target: " + covered(state, composite(full)));
    REQUIRE(covered(state, composite(full)) == "0,0,1280,720 alpha 1");
    UNSCOPED_INFO("a partial composite overwrites only its columns");
    REQUIRE(covered(state, composite(0)) == "0,0,640,720 alpha 1");
    const std::vector<std::pair<std::string, std::pair<std::uint32_t, std::uint32_t>>> changed{
        { "the alpha test", { 0x293C, 0x8700000C } },
        { "another blend", { 0x2938, 0x00010001 } },
        { "a partial color mask", { 0x28DC, 0x7 } },
        { "a depth test", { 0x2934, 0x24F00732 } },
        { "another mode", { 0x2954, 5 } },
        { "a depth surface", { 0x30A8, 0x50000000 } },
        { "a window offset", { 0x28C0, 0x00010001 } },
        { "a scissor origin", { 0x28C4, 0x00010001 } },
        { "a smaller scissor", { 0x28C8, 0x02D00280 } },
        { "a W swizzle of the texel", { 0x4BC, 0x00000C14 } },
        { "a W swizzle of 0", { 0x4BC, 0x00001014 } },
    };
    for (const auto& [name, word] : changed)
    {
        auto words         = state;
        words[word.first]  = word.second;
        const auto outcome = covered(words, composite(full));
        UNSCOPED_INFO(name + " proves nothing: " + outcome);
        REQUIRE(outcome == "none");
    }
    auto unread = state;
    unread.erase(0x4BC);
    UNSCOPED_INFO("an unread swizzle proves nothing");
    REQUIRE(covered(unread, composite(full)) == "none");
    const std::vector<std::pair<std::string, std::function<void(NativeDrawReplayRecipe&)>>> recipes{
        { "a c255.x literal of 0.5", [](auto& draw)
          {
              putFloat(draw.pixelConstants, 255 * 16, 0.5F);
          } },
        { "no literal overlay", [](auto& draw)
          {
              draw.pixelConstants.resize(224 * 16);
          } },
        { "a smaller target", [](auto& draw)
          {
              draw.width = 640;
          } },
        { "another target format", [](auto& draw)
          {
              draw.targetFormat = NativeDrawReplayTargetFormat::Rgb10a2;
          } },
    };
    for (const auto& [name, mutate] : recipes)
    {
        auto draw = composite(full);
        mutate(draw);
        UNSCOPED_INFO(name + " proves nothing: " + covered(state, draw));
        REQUIRE(covered(state, draw) == "none");
    }
}

// Draw 1895 uses 448x720 2X tiles of widths 448/416/416.
constexpr std::uint32_t            kMrt0 = 0x50263120, kMrt1 = 0x5044C6B0, kMrtDepth = 0x5044C390;
constexpr std::uint32_t            kMrtTag = 0x4D, kMrtDepthTag = 0x4E;
constexpr std::array<GuestRect, 3> kMrtRects{ { { 0, 0, 448, 720 }, { 448, 0, 864, 720 }, { 864, 0, 1280, 720 } } };

std::map<std::uint32_t, std::uint32_t> mrtWords(std::uint32_t mask = 3, std::uint32_t predicated = 1, std::uint32_t tiling = 0x21042400)
{
    std::map<std::uint32_t, std::uint32_t> words{ { kGraphics + 0x3098, kMrt0 },
                                                  { kGraphics + 0x309C, kMrt1 },
                                                  { kGraphics + 0x30A0, 0 },
                                                  { kGraphics + 0x30A4, 0 },
                                                  { kGraphics + 0x30A8, kMrtDepth },
                                                  { kGraphics + 0x2880, 0x070101E0 },
                                                  { kGraphics + 0x2934, 0x24F00770 },
                                                  { kGraphics + 0x28DC, 0xFF },
                                                  { kGraphics + 0x2ABC, tiling },
                                                  { kGraphics + 0x2A48, 2 },
                                                  { kGraphics + 0x2A4C, 0 },
                                                  { kGraphics + 0x31A4, mask },
                                                  { kGraphics + 0x31AC, predicated },
                                                  { kGraphics + 0x31CC, 3 },
                                                  { kGraphics + 0x337C, 1280 },
                                                  { kGraphics + 0x3380, 720 },
                                                  { kGraphics + 0x31B8, kMrt0 },
                                                  { kGraphics + 0x31BC, kMrt1 },
                                                  { kGraphics + 0x31C0, 0 },
                                                  { kGraphics + 0x31C4, 0 },
                                                  { kGraphics + 0x31C8, kMrtDepth },
                                                  { kStack + 92, 0 },
                                                  { kTileColor, 0 },
                                                  { kTileColor + 4, 0 },
                                                  { kTileColor + 8, 0 },
                                                  { kTileColor + 12, std::bit_cast<std::uint32_t>(1.0F) } };
    for (const auto& [surface, info] : { std::pair{ kMrt0, 0U }, std::pair{ kMrt1, 0x0002021CU }, std::pair{ kMrtDepth, 0x5E4U } })
    {
        words[surface + 0x18] = 0x070101E0;
        words[surface + 0x1C] = info;
        words[surface + 0x24] = 0x06FC1678;
    }
    for (std::uint32_t i = 0; i < kMrtRects.size(); ++i)
    {
        words[kGraphics + 0x31D0 + 16 * i] = kMrtRects[i].x0;
        words[kGraphics + 0x31D4 + 16 * i] = kMrtRects[i].y0;
        words[kGraphics + 0x31D8 + 16 * i] = kMrtRects[i].x1;
        words[kGraphics + 0x31DC + 16 * i] = kMrtRects[i].y1;
    }
    return words;
}

const Target kMrtSet{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0, kNoDepthEdramBase, 1, { NativeDrawReplayTargetFormat::Rgb10a2 }, { 0x21C } };

// 0x826A22F8's clears of the set under the tile-0 predicate: index 0 with
// depth and stencil (0x3F), index 1 color only (0x0F).
GuestClear mrtClear(std::uint32_t index, std::uint32_t flags)
{
    GuestClear  clear;
    std::string detail;
    UNSCOPED_INFO("the MRT clear decodes: " + detail);
    REQUIRE(ReadGuestClear(wordReader(mrtWords()), { kGraphics, flags, index, 0, 0, 1280, 720, kTileColor }, kStack, 1.0, clear, &detail));
    return clear;
}

// The set as tile `tile`'s resolve of `slot` reads it, predicated to that tile.
GuestTarget mrtTarget(std::uint32_t slot, std::uint32_t tile)
{
    GuestTarget target;
    std::string detail;
    UNSCOPED_INFO("the MRT source decodes: " + detail);
    REQUIRE(ReadGuestTargetSet(wordReader(mrtWords(3U << (2 * tile))), kGraphics, slot, target, &detail));
    return target;
}

// The copy words of one tile resolve: source select, k1 for depth and k01
// for color, and the clear bits the guest set on tiles 0 and 1.
GuestResolveCopy mrtCopy(std::uint32_t slot, std::uint32_t tile, std::uint32_t clearBits)
{
    GuestResolveCopy copy;
    copy.copyControl = 0x00100000 | slot | (slot == kGuestDepthSlot ? 0x10 : 0x40) | clearBits;
    copy.destBase    = 0x1E000000 + 0x100000 * slot;
    copy.rectX = copy.destX = kMrtRects[tile].x0;
    copy.rectWidth          = kMrtRects[tile].x1 - kMrtRects[tile].x0;
    copy.rectHeight         = 720;
    copy.colorClear         = 0xFF000000;
    copy.depthClear         = 0xFFFFFF00;
    return copy;
}

std::uint32_t mrtClearBits(std::uint32_t slot, std::uint32_t tile)
{
    return tile == 2 || slot == kGuestDepthSlot ? 0 : slot == 0 ? 0x300
                                                                : 0x100;
}

// Panels, or for kMrtTag a draw into RT0 and a 2_10_10_10 RT1; kMrtDepthTag
// also tests LESS against the set's depth.
bool mrtBuilder(const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string& detail)
{
    if (!panelBuilder(value, draw, detail))
        return false;
    if (value.arguments[7] == kMrtTag || value.arguments[7] == kMrtDepthTag)
    {
        draw.extraTargetCount       = 1;
        draw.extraTargets[0].format = NativeDrawReplayTargetFormat::Rgb10a2;
    }
    if (value.arguments[7] == kMrtDepthTag)
    {
        draw.depth.enabled = true;
        draw.depth.compare = 2; // LESS
    }
    return true;
}

NativeDrawShaderWords gfxWords(std::string_view vertex = kGfxVertexWordDigest)
{
    return { std::string(vertex), std::string(kGfxPixelWordDigest) };
}

// A reader whose words the test swaps as the guest state moves on. Its shader
// reader names `shaders`, or fails when that is empty; `reader` replaces it.
struct MrtRig
{
    FakeCompletion                                          completion;
    std::shared_ptr<std::map<std::uint32_t, std::uint32_t>> words   = std::make_shared<std::map<std::uint32_t, std::uint32_t>>(unpredicated(mrtWords()));
    std::shared_ptr<std::optional<NativeDrawShaderWords>>   shaders = std::make_shared<std::optional<NativeDrawShaderWords>>(gfxWords());
    std::unique_ptr<NativeFrameAssembler>                   assembler;
    std::string                                             error;
    bool                                                    partial = false;

    explicit MrtRig(NativeDrawMode mode = NativeDrawMode::Strict, std::optional<NativeDrawShaderReader> reader = std::nullopt)
    {
        NativeFrameAssemblerConfig config;
        config.mode       = mode;
        config.build      = mrtBuilder;
        config.completion = &completion;
        config.readTarget = [words = words](std::uint32_t address, std::uint32_t& out)
        {
            const auto found = words->find(address);
            if (found == words->end())
                return false;
            out = found->second;
            return true;
        };
        config.readShaders = reader.value_or([shaders = shaders](const NativeDrawSubmission&, NativeDrawShaderWords& out, std::string& failure)
                                             {
                                                 if (!*shaders)
                                                 {
                                                     failure = "fixture_shader_unreadable";
                                                     return false;
                                                 }
                                                 out = **shaders;
                                                 return true;
                                             });
        assembler          = std::make_unique<NativeFrameAssembler>(config);
    }

    std::uint64_t clear(const GuestClear& value)
    {
        error.clear();
        return assembler->Clear(value, error);
    }

    std::uint64_t resolve(std::uint32_t slot, std::uint32_t tile, std::optional<GuestResolveCopy> copy = std::nullopt)
    {
        error.clear();
        return assembler->Resolve(mrtTarget(slot, tile), copy.value_or(mrtCopy(slot, tile, mrtClearBits(slot, tile))), false, partial, error);
    }

    // Both clears, then the set's draw.
    void begin(std::uint64_t sequence = 1)
    {
        UNSCOPED_INFO("both MRT clears are admitted: " + error);
        REQUIRE((clear(mrtClear(0, 0x3F)) && clear(mrtClear(1, 0x0F))));
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        record(*assembler, sequence, kMrtTag, &verdict);
        UNSCOPED_INFO("the MRT draw is admitted");
        REQUIRE(!verdict);
    }
};

// Tiles in order, each depth, RT0, RT1, as frame 428 resolves them.
constexpr std::array<std::uint32_t, 3> kMrtSlots{ kGuestDepthSlot, 0, 1 };

// A loading-screen batch into the ended set: by default a GFx quad batch.
std::optional<NativeDrawReason> recordEnded(NativeFrameAssembler& assembler, std::uint64_t sequence, NativeDrawHook hook = NativeDrawHook::IndexedUp, std::uint32_t caller = kGfxDrawCaller)
{
    auto value          = submission(hook, caller, sequence, kMrtTag);
    value.operationMark = assembler.NumberOperation();
    return submit(assembler, value);
}

void testMrtPass()
{
    MrtRig     rig;
    const auto first = rig.clear(mrtClear(0, 0x3F));
    UNSCOPED_INFO("both MRT clears are admitted: " + rig.error);
    REQUIRE((first && rig.clear(mrtClear(1, 0x0F))));
    std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
    record(*rig.assembler, 1, kMrtTag, &verdict);
    UNSCOPED_INFO("the MRT draw is admitted into the off-screen source");
    REQUIRE(!verdict);
    std::vector<std::uint64_t> marks;
    for (std::uint32_t tile = 0; tile < 3; ++tile)
        for (const auto slot : kMrtSlots)
        {
            marks.push_back(rig.resolve(slot, tile));
            UNSCOPED_INFO(std::format("tile {} slot {} is admitted: {}", tile, slot, rig.error));
            REQUIRE((marks.back() && rig.error.empty()));
        }
    // The set is never the presented target: nothing presents, nothing waits.
    const auto result = rig.assembler->Swap(makeSwap(1, 1));
    UNSCOPED_INFO(std::format("the pass leaves presented work untouched: {} {} spans {} failure {}", NativeFrameStatusName(result.status), result.detail, rig.completion.spans.size(), rig.assembler->Failure().value_or("none")));
    REQUIRE((result.status == NativeFrameStatus::Empty && rig.completion.spans.size() == 10 && !rig.completion.spans[9].frame && !rig.assembler->Failure()));
    const std::array<std::size_t, 9> clearCounts{ 2, 2, 4, 5, 5, 7, 8, 8, 8 };
    for (std::size_t n = 0; n < marks.size(); ++n)
    {
        const auto  tile = static_cast<std::uint32_t>(n / 3), slot = kMrtSlots[n % 3];
        const auto& span   = rig.completion.spans[n];
        const auto& pass   = resolvePass(span, marks[n]);
        auto        set    = kMrtSet;
        set.depthEdramBase = 0x5E4;
        UNSCOPED_INFO("the pass replays the set and its draw");
        REQUIRE((pass.target == set && pass.DrawCount() == 1 && pass.Draw(0).extraTargetCount == 1));
        UNSCOPED_INFO(std::format("resolve {} replays {} clears, not {}", n, clearCounts[n], pass.clears.size()));
        REQUIRE(pass.clears.size() == clearCounts[n]);
        const auto& rt0 = pass.clears[0];
        const auto& rt1 = pass.clears[1];
        UNSCOPED_INFO("the paired clears cover tile 0 only");
        REQUIRE((rt0.rectangle.right == 448 && rt0.color && rt0.depth && rt0.stencil && rt0.colorTargets == 1 && rt0.colorValue == std::array<float, 4>{ 0, 0, 0, 1 } &&
                 rt1.rectangle.right == 448 && rt1.color && !rt1.depth && rt1.colorTargets == 2));
        const auto& resolve = pass.resolves[0];
        const bool  depth   = slot == kGuestDepthSlot;
        const auto  samples = depth ? std::array<std::uint32_t, 2>{ 3, 3 } : std::array<std::uint32_t, 2>{ 0, 3 };
        const auto  packing = depth ? NativeGuestWordPacking::Depth24Stencil8 : slot ? NativeGuestWordPacking::Color2101010
                                                                                     : NativeGuestWordPacking::Color8888;
        UNSCOPED_INFO(std::format("resolve {} reads its tile, slot and samples", n));
        REQUIRE((resolve.depth == depth && resolve.colorTarget == (depth ? 0 : slot) && resolve.sourceX == kMrtRects[tile].x0 && resolve.sourceY == 0 &&
                 resolve.width == kMrtRects[tile].x1 - kMrtRects[tile].x0 && resolve.height == 720 && resolve.destinationX == 0 &&
                 resolve.samples == samples && resolve.guestWords == packing));
        const auto& texture = span.frame->textures[0];
        UNSCOPED_INFO("the resolve renders guest words");
        REQUIRE((texture.id == marks[n] && texture.format == NativeDrawReplayTextureFormat::R32Uint && texture.width == resolve.width && texture.height == 720));
        const auto packed = depth ? GuestResolvePacking::Depth24Stencil8Words : slot ? GuestResolvePacking::Color2101010Words
                                                                                     : GuestResolvePacking::Color8888Words;
        UNSCOPED_INFO("the span carries the copy with its packing");
        REQUIRE((span.guestResolve && span.guestResolve->packing == packed && span.guestResolve->copyControl == mrtCopy(slot, tile, mrtClearBits(slot, tile)).copyControl));
        UNSCOPED_INFO("the resolve frame validates: " + rig.error);
        REQUIRE(ValidateNativeRenderFrame(*span.frame, rig.error));
    }
    // Tile 1's backgrounds come from tile 0's clear bits, decoded per slot layout.
    const auto& tile1      = rig.completion.spans[5].frame->passes[0].clears;
    const auto  background = [&](std::uint8_t targets, bool depth)
    {
        const auto found = std::find_if(tile1.begin(), tile1.end(), [&](const NativeRenderFramePass::Clear& clear)
                                        {
                                            return clear.rectangle.left == 448 && clear.rectangle.right == 864 && clear.depth == depth &&
                                                   (depth || (clear.color && clear.colorTargets == targets));
                                        });
        UNSCOPED_INFO("a tile-1 background is present");
        REQUIRE(found != tile1.end());
        return *found;
    };
    UNSCOPED_INFO("RT0's background is the 8888 word");
    REQUIRE((background(1, false).colorValue == std::array<float, 4>{ 0, 0, 0, 1 }));
    UNSCOPED_INFO("RT1's background is the 10:10:10:2 word");
    REQUIRE((background(2, false).colorValue == std::array<float, 4>{ 0, 0, 1008.0F / 1023.0F, 1 }));
    const auto depth = background(0, true);
    UNSCOPED_INFO("the depth background is D24S8 1.0, stencil 0");
    REQUIRE((!depth.color && depth.stencil && depth.depthValue == 1.0F && depth.stencilValue == 0));
    UNSCOPED_INFO("the ended pass is not resolved again: " + rig.error);
    REQUIRE((rig.resolve(0, 0) == 0 && refusedAs(rig.error, "resolve_source_unmodeled: other_source")));
}

// K17 uses uncleared tiled 2X; full-screen resolves, short quads unmodeled.
void testTiledOverwrite()
{
    // The MRT rig's three-tile layout with RT0 alone, as k_2_10_10_10.
    const auto words = [](std::uint32_t mask)
    {
        auto value                = unpredicated(mrtWords());
        value[kGraphics + 0x309C] = 0;
        value[kGraphics + 0x31BC] = 0;
        value[kMrt0 + 0x1C]       = 0x00020000;
        value[kGraphics + 0x31A4] = mask;
        value[kGraphics + 0x31AC] = mask == 0xFFFFFFFF ? 0 : 1;
        return value;
    };
    for (const std::int32_t right : { 1280, 864 })
    {
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        config.readTarget = wordReader(words(0xFFFFFFFF));
        config.build      = [](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string&)
        {
            draw              = makeDraw(static_cast<std::uint8_t>(value.arguments[7]));
            draw.targetFormat = NativeDrawReplayTargetFormat::Rgb10a2;
            return true;
        };
        config.coverage = [right](const NativeDrawSubmission&, const NativeDrawReplayRecipe&)
        {
            return std::optional(NativeDrawReplayScissor{ 0, 0, right, 720 });
        };
        NativeFrameAssembler            assembler(config);
        std::optional<NativeDrawReason> verdict;
        record(assembler, 1, 0x62, &verdict);
        UNSCOPED_INFO("the tiled pass's quad is admitted");
        REQUIRE(!verdict);
        for (std::uint32_t tile = 0; tile < kMrtRects.size(); ++tile)
        {
            GuestTarget target;
            std::string error;
            bool        partial = false;
            UNSCOPED_INFO("the tiled target decodes: " + error);
            REQUIRE(ReadGuestTargetSet(wordReader(words(3U << (2 * tile))), kGraphics, 0, target, &error));
            auto copy         = mrtCopy(0, tile, 0);
            copy.destInfo     = 0x380;
            const auto mark   = assembler.Resolve(target, copy, false, partial, error);
            const bool misses = std::int32_t(kMrtRects[tile].x1) > right;
            if (misses)
            {
                UNSCOPED_INFO("a tile the quad misses is unmodeled: " + error);
                REQUIRE((!mark && refusedAs(error, "resolve_source_unmodeled")));
                break;
            }
            UNSCOPED_INFO(std::format("tile {} resolves: {}", tile, error));
            REQUIRE((mark && assembler.FlushThrough(mark).fence));
            const auto& span    = completion.spans.back();
            const auto& pass    = resolvePass(span, mark);
            const auto& resolve = pass.resolves[0];
            UNSCOPED_INFO(std::format("tile {} replays the quad alone as k_2_10_10_10 words", tile));
            REQUIRE((pass.target.format == NativeDrawReplayTargetFormat::Rgb10a2 && pass.clears.empty() && pass.DrawCount() == 1 &&
                     resolve.sourceX == kMrtRects[tile].x0 && resolve.width == kMrtRects[tile].x1 - kMrtRects[tile].x0 &&
                     resolve.samples == std::array<std::uint32_t, 2>{ 0, 3 } && resolve.guestWords == NativeGuestWordPacking::Color2101010 &&
                     span.guestResolve && span.guestResolve->packing == GuestResolvePacking::Color2101010Words));
        }
    }
}

// After the last tile, still-bound guest MRT draws do not resolve again.
void testMrtEndedSetWrites()
{
    MrtRig rig;
    rig.begin();
    for (std::uint32_t tile = 0; tile < 3; ++tile)
        for (const auto slot : kMrtSlots)
        {
            UNSCOPED_INFO("resolve admitted: " + rig.error);
            REQUIRE(rig.resolve(slot, tile));
        }
    *rig.words                              = mrtWords(0xFFFFFFFF, 0, 0x00042400);
    std::optional<NativeDrawReason> verdict = recordEnded(*rig.assembler, 2);
    UNSCOPED_INFO("an untiled GFx batch into the ended set is admitted");
    REQUIRE(!verdict);
    *rig.shaders = NativeDrawShaderWords{ std::string(kLabelVertexWordDigest), std::string(kLabelPixelWordDigest) };
    verdict      = recordEnded(*rig.assembler, 3, NativeDrawHook::NonindexedUp, kLabelDrawCaller);
    UNSCOPED_INFO("and so is the label batch");
    REQUIRE(!verdict);
    const auto result = rig.assembler->Swap(makeSwap(1, 3));
    UNSCOPED_INFO("the writes add no host work: " + result.detail);
    REQUIRE((result.status == NativeFrameStatus::Empty && result.coverage.admitted == 3 && rig.completion.spans.size() == 10 && !rig.completion.spans[9].frame));
    // The swap ends the discard: the same write now needs a native route.
    record(*rig.assembler, 4, 5, &verdict);
    const auto next = rig.assembler->Swap(makeSwap(2, 4));
    UNSCOPED_INFO("after the swap the write is routed again: " + next.detail);
    REQUIRE((next.status == NativeFrameStatus::Failed && next.detail.find("frame_target_mismatch") != std::string::npos));

    // Another set's storage is not the ended one.
    MrtRig other;
    other.begin();
    for (std::uint32_t tile = 0; tile < 3; ++tile)
        for (const auto slot : kMrtSlots)
        {
            UNSCOPED_INFO("resolve admitted: " + other.error);
            REQUIRE(other.resolve(slot, tile));
        }
    *other.words                 = mrtWords(0xFFFFFFFF, 0, 0x00042400);
    (*other.words)[kMrt1 + 0x1C] = 0x00020300;
    record(*other.assembler, 2, 5, &verdict);
    const auto moved = other.assembler->Swap(makeSwap(1, 2));
    UNSCOPED_INFO("a set on other EDRAM is not discarded: " + moved.detail);
    REQUIRE((moved.status == NativeFrameStatus::Failed && moved.detail.find("frame_target_mismatch") != std::string::npos));
    // A clear ends the discard too.
    MrtRig cleared;
    cleared.begin();
    for (std::uint32_t tile = 0; tile < 3; ++tile)
        for (const auto slot : kMrtSlots)
        {
            UNSCOPED_INFO("resolve admitted: " + cleared.error);
            REQUIRE(cleared.resolve(slot, tile));
        }
    cleared.clear(mrtClear(0, 0x3F));
    *cleared.words = mrtWords(0xFFFFFFFF, 0, 0x00042400);
    record(*cleared.assembler, 2, 5, &verdict);
    const auto afterClear = cleared.assembler->Swap(makeSwap(1, 2));
    UNSCOPED_INFO("a clear ends the discard: " + afterClear.detail);
    REQUIRE((afterClear.status == NativeFrameStatus::Failed && afterClear.detail.find("frame_target_mismatch") != std::string::npos));
}

// 0x826A1D90's depth-only clear (index -1) of the bound depth surface.
GuestClear setDepthClear(const std::map<std::uint32_t, std::uint32_t>& words, std::uint32_t flags = 0x30, std::uint32_t index = kGuestDepthOnlyClearIndex)
{
    GuestClear  clear;
    std::string detail;
    UNSCOPED_INFO("the depth clear decodes: " + detail);
    REQUIRE(ReadGuestClear(wordReader(words), { kGraphics, flags, index, 0, 0, 448, 720, 0 }, kStack, 1.0, clear, &detail));
    return clear;
}

// Every tile resolved: the set ended, still bound and now untiled.
void endPass(MrtRig& rig, std::uint64_t sequence = 1)
{
    rig.begin(sequence);
    for (std::uint32_t tile = 0; tile < 3; ++tile)
        for (const auto slot : kMrtSlots)
        {
            UNSCOPED_INFO("resolve admitted: " + rig.error);
            REQUIRE(rig.resolve(slot, tile));
        }
    *rig.words = mrtWords(0xFFFFFFFF, 0, 0x00042400);
}

// Ended-set depth clears require no host work and leave the set ended.
void testMrtEndedDepthClears()
{
    {
        MrtRig rig;
        endPass(rig);
        UNSCOPED_INFO("the depth clear before the batches is admitted: " + rig.error);
        REQUIRE((rig.clear(setDepthClear(*rig.words)) && rig.error.empty()));
        UNSCOPED_INFO("a GFx batch after it is still discarded");
        REQUIRE(!recordEnded(*rig.assembler, 2));
        *rig.shaders = NativeDrawShaderWords{ std::string(kLabelVertexWordDigest), std::string(kLabelPixelWordDigest) };
        UNSCOPED_INFO("and so is the label batch");
        REQUIRE(!recordEnded(*rig.assembler, 3, NativeDrawHook::NonindexedUp, kLabelDrawCaller));
        UNSCOPED_INFO("the depth clear after the batches is admitted: " + rig.error);
        REQUIRE((rig.clear(setDepthClear(*rig.words)) && rig.error.empty()));
        // The presented pass follows: its full tiled clear, a depth-only
        // clear of the presented depth, a panel and both tile scanouts.
        *rig.words = unpredicated(tiledWords());
        UNSCOPED_INFO("the presented clears: " + rig.error);
        REQUIRE((rig.clear(tiledClear()) && rig.clear(depthOnlyClear())));
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        record(*rig.assembler, 4, 5, &verdict);
        UNSCOPED_INFO("the presented panel is admitted");
        REQUIRE(!verdict);
        scanout(*rig.assembler);
        const auto result = rig.assembler->Swap(makeSwap(1, 4));
        UNSCOPED_INFO(std::format("the depth clears add no host work and the frame presents: {} {} spans {}", NativeFrameStatusName(result.status), result.detail, rig.completion.spans.size()));
        REQUIRE((result.status == NativeFrameStatus::Provisional && result.coverage.admitted == 4 && rig.completion.spans.size() == 10 &&
                 rig.completion.spans[9].frame && drawTags(rig.completion.spans[9]) == std::vector<std::uint8_t>{ 5 } && !rig.assembler->Failure()));
    }
    {
        // Presented depth stays undefined: a color-only presented clear does
        // not repair it for a depth draw.
        MrtRig rig;
        *rig.words = unpredicated(tiledWords());
        UNSCOPED_INFO("the first presented clear: " + rig.error);
        REQUIRE(rig.clear(tiledClear()));
        scanOut(*rig.assembler);
        const auto first = rig.assembler->Swap(makeSwap(1, 0));
        UNSCOPED_INFO("the first frame: " + first.detail);
        REQUIRE(first.status == NativeFrameStatus::Provisional);
        *rig.words = unpredicated(mrtWords());
        endPass(rig, 2);
        UNSCOPED_INFO("the set's depth clear: " + rig.error);
        REQUIRE(rig.clear(setDepthClear(*rig.words)));
        *rig.words = unpredicated(tiledWords());
        GuestClear colorOnly;
        UNSCOPED_INFO("the color-only presented clear: " + rig.error);
        REQUIRE((ReadGuestClear(wordReader(tiledWords()), { kGraphics, 0x0F, 0, 0, 0, 1280, 720, kTileColor }, kStack, 1.0, colorOnly) && rig.clear(colorOnly)));
        record(*rig.assembler, 3, kDepthTag);
        const auto result = rig.assembler->Swap(makeSwap(2, 2));
        UNSCOPED_INFO("a presented depth draw still needs a full depth clear: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.find("tiled_depth_uncleared") != std::string::npos));
    }
    {
        // The clear is a recorded operation of the epoch: once the resolve
        // frames are submitted a flush through it completes, and it closes
        // the epoch's sequence range.
        MrtRig rig;
        endPass(rig);
        const auto            mark = rig.clear(setDepthClear(*rig.words));
        completion::DrawFlush flush;
        for (int call = 0; call < 12 && !flush.fence; ++call)
            flush = rig.assembler->FlushThrough(mark);
        const auto result = rig.assembler->Swap(makeSwap(1, 1));
        UNSCOPED_INFO(std::format("the depth clear is recorded: mark {} fence {} last {} {}", mark, flush.fence, result.lastSequence, result.detail));
        REQUIRE((mark && flush.fence && result.lastSequence == mark));
    }

    // Any other depth clear keeps its own verdict and ends the discard.
    const auto refused = [](auto mutate, const std::string& expected)
    {
        MrtRig rig;
        endPass(rig);
        auto words = *rig.words;
        auto clear = mutate(words);
        UNSCOPED_INFO("expected " + expected + ", got " + rig.error);
        REQUIRE((!rig.clear(clear) && refusedAs(rig.error, expected)));
        std::optional<NativeDrawReason> verdict;
        record(*rig.assembler, 2, kMrtTag, &verdict);
        const auto result = rig.assembler->Swap(makeSwap(1, 2));
        UNSCOPED_INFO("the refused clear ends the discard: " + result.detail);
        REQUIRE((verdict && result.status == NativeFrameStatus::Failed && result.detail.find("frame_target_mismatch") != std::string::npos));
    };
    refused([](std::map<std::uint32_t, std::uint32_t>& words)
            {
                words[kGraphics + 0x31AC] = 1;
                return setDepthClear(words);
            },
            "clear_form_unsupported: ended_set predicated");
    refused([](std::map<std::uint32_t, std::uint32_t>& words)
            {
                return setDepthClear(words, 0x70);
            },
            "clear_form_unsupported: ended_set flags");
    refused([](std::map<std::uint32_t, std::uint32_t>& words)
            {
                auto clear        = setDepthClear(words);
                clear.clear.color = true;
                return clear;
            },
            "clear_form_unsupported: ended_set color");
    // Under tiling the saved depth surface stands for another target.
    refused([](std::map<std::uint32_t, std::uint32_t>& words)
            {
                words[kGraphics + 0x2ABC] = 0x21042400;
                return setDepthClear(words);
            },
            "depth_clear_target_unmodeled: 5E4");
    // Another surface object on the same EDRAM, or the same object over other tiles.
    refused([](std::map<std::uint32_t, std::uint32_t>& words)
            {
                constexpr std::uint32_t other = kMrtDepth + 0x100;
                for (const auto offset : { 0x18U, 0x1CU, 0x24U })
                    words[other + offset] = words[kMrtDepth + offset];
                words[kGraphics + 0x30A8] = other;
                return setDepthClear(words);
            },
            "depth_clear_target_unmodeled: 5E4");
    refused([](std::map<std::uint32_t, std::uint32_t>& words)
            {
                words[kMrtDepth + 0x18] = 0x07010200;
                return setDepthClear(words);
            },
            "depth_clear_target_unmodeled: 5E4");
    // An index-0 depth and stencil clear through the set is a set clear.
    refused([](std::map<std::uint32_t, std::uint32_t>& words)
            {
                return setDepthClear(words, 0x30, 0);
            },
            "clear_form_unsupported: mrt unpredicated+untiled+no_color");
    {
        // A pass whose index-0 clear left stencil defines no depth to clear.
        MrtRig     rig;
        GuestClear depthOnly;
        UNSCOPED_INFO("the clears without stencil: " + rig.error);
        REQUIRE((ReadGuestClear(wordReader(mrtWords()), { kGraphics, 0x1F, 0, 0, 0, 1280, 720, kTileColor }, kStack, 1.0, depthOnly) &&
                 rig.clear(depthOnly) && rig.clear(mrtClear(1, 0x0F))));
        record(*rig.assembler, 1, kMrtTag);
        for (std::uint32_t tile = 0; tile < 3; ++tile)
            for (const std::uint32_t slot : { 0U, 1U })
            {
                UNSCOPED_INFO(std::format("tile {} slot {}: {}", tile, slot, rig.error));
                REQUIRE(rig.resolve(slot, tile));
            }
        *rig.words = mrtWords(0xFFFFFFFF, 0, 0x00042400);
        UNSCOPED_INFO("no ended depth: " + rig.error);
        REQUIRE((!rig.clear(setDepthClear(*rig.words)) && refusedAs(rig.error, "depth_clear_target_unmodeled: 5E4")));
    }
}

// MRT aliasing presented EDRAM requires a full clear before later work.
void testMrtThenPresented()
{
    for (const bool cleared : { false, true })
    {
        MrtRig rig;
        rig.begin();
        for (std::uint32_t tile = 0; tile < 3; ++tile)
            for (const auto slot : kMrtSlots)
            {
                UNSCOPED_INFO("resolve admitted: " + rig.error);
                REQUIRE(rig.resolve(slot, tile));
            }
        *rig.words = unpredicated(tiledWords());
        if (cleared)
        {
            UNSCOPED_INFO("the presented clear: " + rig.error);
            REQUIRE(rig.clear(tiledClear()));
        }
        record(*rig.assembler, 2, 5);
        if (cleared)
            scanOut(*rig.assembler);
        const auto result = rig.assembler->Swap(makeSwap(1, 2));
        if (cleared)
        {
            UNSCOPED_INFO("a full presented clear repairs the target: " + result.detail);
            REQUIRE(result.status == NativeFrameStatus::Provisional);
        }
        else
        {
            UNSCOPED_INFO("an uncleared presented draw after the pass fails: " + result.detail);
            REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.find("tiled_target_uncleared") != std::string::npos));
        }
    }
}

// Stencil-only MRT clear inherits unheld depth, so depth-tested draws fail.
void testMrtInheritedDepth()
{
    for (const bool cleared : { false, true })
    {
        MrtRig rig;
        UNSCOPED_INFO("the set's clears: " + rig.error);
        REQUIRE((rig.clear(mrtClear(0, cleared ? 0x3F : 0x23)) && rig.clear(mrtClear(1, 0x0F))));
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        record(*rig.assembler, 1, kMrtDepthTag, &verdict);
        UNSCOPED_INFO(std::format("the depth-tested draw is {}", cleared ? "admitted" : "refused"));
        REQUIRE(verdict.has_value() != cleared);
        const auto result = rig.assembler->Swap(makeSwap(1, 1));
        UNSCOPED_INFO(std::format("{}: {}", cleared ? "a cleared depth" : "inherited depth", result.detail));
        REQUIRE((cleared ? result.status != NativeFrameStatus::Failed
                         : result.status == NativeFrameStatus::Failed && result.detail.find("offscreen_depth_undefined: depth 5E4") != std::string::npos));
    }
}

void testMrtRefusals()
{
    {
        MrtRig rig;
        UNSCOPED_INFO("index 0: " + rig.error);
        REQUIRE(rig.clear(mrtClear(0, 0x3F)));
        record(*rig.assembler, 1, kMrtTag);
        const auto result = rig.assembler->Swap(makeSwap(1, 1));
        UNSCOPED_INFO("a draw before RT1's clear fails: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.find("mrt_clear_incomplete: slots 0x1") != std::string::npos));
    }
    {
        MrtRig rig;
        UNSCOPED_INFO("RT1 without RT0's clear: " + rig.error);
        REQUIRE((!rig.clear(mrtClear(1, 0x0F)) && refusedAs(rig.error, "mrt_clear_unpaired: RT1")));
        UNSCOPED_INFO("index 0: " + rig.error);
        REQUIRE(rig.clear(mrtClear(0, 0x3F)));
        auto other             = mrtClear(1, 0x0F);
        other.clear.colorValue = { 1, 0, 0, 1 };
        UNSCOPED_INFO("RT1 with another color: " + rig.error);
        REQUIRE((!rig.clear(other) && refusedAs(rig.error, "mrt_clear_unpaired: RT1")));
        UNSCOPED_INFO("the unpaired clear leaves the source partial: " + rig.error);
        REQUIRE((!rig.resolve(0, 0) && refusedAs(rig.error, "resolve_source_partial")));
    }
    {
        MrtRig rig;
        UNSCOPED_INFO("an RT0-only clear of the set: " + rig.error);
        REQUIRE((!rig.clear(mrtClear(0, 0x31)) && refusedAs(rig.error, "mrt_clear_partial: flags 0x31 count 2")));
        GuestClear  clear;
        std::string detail;
        UNSCOPED_INFO("depth on RT1's clear: " + detail);
        REQUIRE((!ReadGuestClear(wordReader(mrtWords()), { kGraphics, 0x1F, 1, 0, 0, 1280, 720, kTileColor }, kStack, 1.0, clear, &detail) &&
                 detail == "clear_mrt_flags 0x1f"));
    }
    {
        MrtRig rig;
        rig.begin();
        UNSCOPED_INFO("tile 0 depth and RT0: " + rig.error);
        REQUIRE((rig.resolve(kGuestDepthSlot, 0) && rig.resolve(0, 0)));
        UNSCOPED_INFO("tile 1 before tile 0 finished: " + rig.error);
        REQUIRE((!rig.resolve(1, 1) && refusedAs(rig.error, "tiled_resolve_order: tile 1 slot 1, expected tile 0 slot 1")));
    }
    {
        MrtRig rig;
        rig.begin();
        UNSCOPED_INFO("tile 0 depth and RT0: " + rig.error);
        REQUIRE((rig.resolve(kGuestDepthSlot, 0) && rig.resolve(0, 0)));
        UNSCOPED_INFO("a slot resolved twice: " + rig.error);
        REQUIRE((!rig.resolve(0, 0) && refusedAs(rig.error, "tiled_resolve_order: tile 0 slot 0, expected tile 0 slot 1")));
    }
    {
        MrtRig rig;
        rig.begin();
        UNSCOPED_INFO("tile 0 without RT1's clear bit: " + rig.error);
        REQUIRE((rig.resolve(kGuestDepthSlot, 0) && rig.resolve(0, 0) && rig.resolve(1, 0, mrtCopy(1, 0, 0))));
        UNSCOPED_INFO("tile 1 depth and RT0: " + rig.error);
        REQUIRE((rig.resolve(kGuestDepthSlot, 1) && rig.resolve(0, 1)));
        UNSCOPED_INFO("tile 1 RT1 has no background: " + rig.error);
        REQUIRE((!rig.resolve(1, 1) && refusedAs(rig.error, "tiled_background_undefined: tile 1 slot 1")));
    }
    {
        MrtRig rig;
        rig.begin();
        auto narrow      = mrtCopy(0, 0, 0x300);
        narrow.rectWidth = 440;
        UNSCOPED_INFO("a rect other than the tile: " + rig.error);
        REQUIRE((!rig.resolve(0, 0, narrow) && refusedAs(rig.error, "tiled_resolve_form: tile 0 rect 0,0,440,720 point 0,0 mask 0x3")));
        UNSCOPED_INFO("the refused resolve leaves the source partial: " + rig.error);
        REQUIRE((!rig.resolve(0, 0) && refusedAs(rig.error, "resolve_source_partial")));
    }
    {
        MrtRig rig;
        rig.begin();
        UNSCOPED_INFO("mask 0xC on the tile-0 rect: " + rig.error);
        REQUIRE((!rig.resolve(0, 1, mrtCopy(0, 0, 0x300)) && refusedAs(rig.error, "tiled_resolve_form: tile 1 rect 0,0,448,720 point 0,0 mask 0xc")));
    }
    {
        MrtRig rig;
        rig.begin();
        auto k2        = mrtCopy(0, 0, 0x300);
        k2.copyControl = 0x00100320;
        UNSCOPED_INFO("tile 0 depth: " + rig.error);
        REQUIRE(rig.resolve(kGuestDepthSlot, 0));
        UNSCOPED_INFO("sample select k2: " + rig.error);
        REQUIRE((!rig.resolve(0, 0, k2) && refusedAs(rig.error, "resolve_form_unsupported: control 00100320")));
    }
    {
        MrtRig rig;
        UNSCOPED_INFO("color-only clears: " + rig.error);
        REQUIRE((rig.clear(mrtClear(0, 0x03)) && rig.clear(mrtClear(1, 0x03))));
        UNSCOPED_INFO("depth without a depth clear: " + rig.error);
        REQUIRE((!rig.resolve(kGuestDepthSlot, 0) && refusedAs(rig.error, "resolve_depth_uncleared")));
    }
    {
        MrtRig rig;
        rig.begin();
        UNSCOPED_INFO("tile 0 depth: " + rig.error);
        REQUIRE(rig.resolve(kGuestDepthSlot, 0));
        record(*rig.assembler, 2, kMrtTag);
        const auto result = rig.assembler->Swap(makeSwap(1, 2));
        UNSCOPED_INFO("a draw after the first resolve fails: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.find("tiled_source_resolving") != std::string::npos));
    }
    {
        // A slot that is not the RT bound in the copy's source select.
        MrtRig rig;
        rig.begin();
        UNSCOPED_INFO("tile 0 depth and RT0: " + rig.error);
        REQUIRE((rig.resolve(kGuestDepthSlot, 0) && rig.resolve(0, 0)));
        UNSCOPED_INFO("RT1 read with RT0's select: " + rig.error);
        REQUIRE((!rig.resolve(1, 0, mrtCopy(0, 0, 0x100)) && refusedAs(rig.error, "resolve_form_unsupported: control 00100140")));
    }
}

// The set decode's refusals, each named with the slot and value.
void testMrtSetDecode()
{
    const auto refusal = [](std::map<std::uint32_t, std::uint32_t> words, std::uint32_t slot)
    {
        GuestTarget target;
        std::string detail;
        UNSCOPED_INFO("the set is refused: " + detail);
        REQUIRE(!ReadGuestTargetSet(wordReader(std::move(words)), kGraphics, slot, target, &detail));
        return detail;
    };
    const auto changed = [](std::initializer_list<std::pair<const std::uint32_t, std::uint32_t>> values)
    {
        auto words = mrtWords();
        for (const auto& [address, value] : values)
            words[address] = value;
        return words;
    };
    GuestTarget depth;
    UNSCOPED_INFO("slot 4 decodes the set with its depth surface");
    REQUIRE((ReadGuestTargetSet(wordReader(mrtWords()), kGraphics, kGuestDepthSlot, depth) && depth.native == kMrtSet && depth.slot == kGuestDepthSlot &&
             depth.colorCount == 2 && depth.surfaces == std::array<std::uint32_t, 4>{ kMrt0, kMrt1, 0, 0 } && depth.tiling));
    const std::pair<std::string, std::string> cases[] = {
        { refusal(changed({ { kMrt1 + 0x18, 0x070001E0 } }), 0), "target_set_shape_mismatch: RT1 1280x720x1, RT0 1280x720x4" },
        { refusal(changed({ { kGraphics + 0x31BC, 0 }, { kMrt1 + 0x24, (1279U << 18) | (719U << 3) } }), 0), "target_set_not_tiling_saved: RT1" },
        { refusal(changed({ { kMrt1 + 0x1C, 0x00020100 } }), 0), "target_set_edram_overlap: RT0 RT1" },
        { refusal(changed({ { kMrtDepth + 0x1C, 0x300 } }), kGuestDepthSlot), "target_set_edram_overlap: RT1 depth" },
        { refusal(changed({ { kGraphics + 0x30A8, 0 } }), kGuestDepthSlot), "depth_unbound" },
        { refusal(changed({ { kGraphics + 0x31C8, 0 } }), kGuestDepthSlot), "target_set_shape_mismatch: depth 448x720x4, RT0 1280x720x4" },
        { refusal(mrtWords(), 2), "target_unbound: RT2" },
        { refusal(changed({ { kMrt1 + 0x1C, 0x0012021C } }), 1), "target_format_unsupported: RT1 format 2 color_info 0012021C" },
        // A k_16_16 slot decodes alone, never in a set.
        { refusal(changed({ { kMrt1 + 0x1C, 0x0004021C } }), 0), "target_set_format_unsupported: RT1 format 4" },
        { refusal(changed({ { kMrt0 + 0x1C, 0x00040000 } }), 0), "target_set_format_unsupported: RT0 format 4" },
    };
    for (const auto& [actual, expected] : cases)
    {
        UNSCOPED_INFO("set refusal named: " + actual + ", expected " + expected);
        REQUIRE(actual == expected);
    }
}

// The remaining pass and tile rules.
void testMrtRules()
{
    {
        // A single-target recipe while the set is bound would write RT0 alone.
        MrtRig rig;
        rig.begin();
        record(*rig.assembler, 2, 5);
        const auto result = rig.assembler->Swap(makeSwap(1, 2));
        UNSCOPED_INFO("a panel recipe into the bound set fails: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.find("frame_target_mismatch") != std::string::npos));
    }
    {
        MrtRig rig;
        auto   unpredicated    = mrtClear(0, 0x3F);
        unpredicated.predicate = 0;
        UNSCOPED_INFO("an unpredicated set clear: " + rig.error);
        REQUIRE((!rig.clear(unpredicated) && refusedAs(rig.error, "clear_form_unsupported: mrt unpredicated")));
        UNSCOPED_INFO("the pair: " + rig.error);
        REQUIRE((rig.clear(mrtClear(0, 0x3F)) && rig.clear(mrtClear(1, 0x0F))));
        UNSCOPED_INFO("RT1 cleared twice: " + rig.error);
        REQUIRE((!rig.clear(mrtClear(1, 0x0F)) && refusedAs(rig.error, "mrt_clear_unpaired: RT1")));
    }
    {
        MrtRig rig;
        UNSCOPED_INFO("index 0: " + rig.error);
        REQUIRE(rig.clear(mrtClear(0, 0x3F)));
        record(*rig.assembler, 1, kMrtTag);
        UNSCOPED_INFO("RT1 cleared after a draw: " + rig.error);
        REQUIRE((!rig.clear(mrtClear(1, 0x0F)) && refusedAs(rig.error, "mrt_clear_unpaired: RT1")));
    }
    {
        MrtRig rig;
        UNSCOPED_INFO("index 0: " + rig.error);
        REQUIRE(rig.clear(mrtClear(0, 0x3F)));
        UNSCOPED_INFO("an uncleared color slot: " + rig.error);
        REQUIRE((!rig.resolve(1, 0) && refusedAs(rig.error, "resolve_source_unmodeled")));
    }
    {
        MrtRig rig;
        rig.begin();
        for (const auto slot : kMrtSlots)
        {
            UNSCOPED_INFO("tile 0: " + rig.error);
            REQUIRE(rig.resolve(slot, 0));
        }
        auto moved  = mrtCopy(kGuestDepthSlot, 1, 0);
        moved.destX = 0;
        UNSCOPED_INFO("a point other than the tile's: " + rig.error);
        REQUIRE((!rig.resolve(kGuestDepthSlot, 1, moved) && refusedAs(rig.error, "tiled_resolve_form: tile 1 rect 448,0,864,720 point 0,0 mask 0xc")));
    }
    for (const auto& [slot, control] : { std::pair{ kGuestDepthSlot, 0x00100214U }, std::pair{ 0U, 0x00300340U }, std::pair{ 0U, 0x00100348U } })
    {
        MrtRig rig;
        rig.begin();
        auto copy        = mrtCopy(slot, 0, 0);
        copy.copyControl = control;
        UNSCOPED_INFO("tile 0 depth: " + rig.error);
        REQUIRE((slot == kGuestDepthSlot || rig.resolve(kGuestDepthSlot, 0)));
        UNSCOPED_INFO("a clear bit on depth, copy command 3 or a reserved bit: " + rig.error);
        REQUIRE((!rig.resolve(slot, 0, copy) && refusedAs(rig.error, std::format("resolve_form_unsupported: control {:08X}", control))));
    }
    {
        // A complete tiled scanout frees the epoch for an MRT pass on other storage.
        MrtRig rig;
        *rig.words = unpredicated(tiledWords());
        tiledFrame(*rig.assembler, 1, 5);
        *rig.words = unpredicated(mrtWords());
        rig.begin(2);
        for (std::uint32_t tile = 0; tile < 3; ++tile)
            for (const auto slot : kMrtSlots)
            {
                UNSCOPED_INFO("resolve after the scanout: " + rig.error);
                REQUIRE(rig.resolve(slot, tile));
            }
        const auto result = rig.assembler->Swap(makeSwap(1, 2));
        UNSCOPED_INFO("the scanout frame still presents: " + result.detail);
        REQUIRE(result.status == NativeFrameStatus::Provisional);
    }
    {
        // Between tile scanouts the presented EDRAM stays frozen, set or not.
        MrtRig rig;
        UNSCOPED_INFO("the tiled clear: " + rig.error);
        REQUIRE(rig.clear(tiledClear()));
        UNSCOPED_INFO("tile 0: " + rig.error);
        REQUIRE(rig.assembler->Resolve(tiledTarget(0), tileCopy(0, 0x00100340), true, rig.partial, rig.error));
        UNSCOPED_INFO("a set clear between tiles: " + rig.error);
        REQUIRE((!rig.clear(mrtClear(0, 0x3F)) && refusedAs(rig.error, "tiled_scanout_in_progress")));
    }
    {
        // A refused set clear is not a presented clear, so skip mode keeps the frame whole.
        MrtRig rig(NativeDrawMode::Skip);
        *rig.words = unpredicated(tiledWords());
        tiledFrame(*rig.assembler, 1, 5);
        UNSCOPED_INFO("the presented frame");
        REQUIRE(rig.assembler->Swap(makeSwap(1, 1)).status == NativeFrameStatus::Provisional);
        UNSCOPED_INFO("the unpaired clear is refused");
        REQUIRE(!rig.clear(mrtClear(1, 0x0F)));
        const auto result = rig.assembler->Swap(makeSwap(2, 1));
        UNSCOPED_INFO(std::format("the next epoch stays whole: {} {}", NativeFrameStatusName(result.status), result.detail));
        REQUIRE(result.status == NativeFrameStatus::Empty);
    }
    {
        // A lone untiled RGB10A2 target resolves as k_2_10_10_10 words.
        auto target          = offscreen(0x300);
        target.native.format = NativeDrawReplayTargetFormat::Rgb10a2;
        FakeCompletion             completion;
        NativeFrameAssemblerConfig config;
        config.completion = &completion;
        NativeFrameAssembler assembler(config);
        std::string          error;
        bool                 partial = false;
        UNSCOPED_INFO("the 10:10:10:2 clear: " + error);
        REQUIRE(assembler.Clear(fullClear(target), error));
        const auto mark = assembler.Resolve(target, fullCopy(target), false, partial, error);
        UNSCOPED_INFO("its untiled resolve packs k_2_10_10_10 words: " + error);
        REQUIRE((mark && assembler.FlushThrough(mark).fence && completion.spans.back().guestResolve &&
                 completion.spans.back().guestResolve->packing == GuestResolvePacking::Color2101010Words));
    }
}

// MRT tiles resolve depth, RT0, then RT1; clear bits reach copied slots only.
void testMrtResolveOrder()
{
    {
        MrtRig rig;
        rig.begin();
        UNSCOPED_INFO("RT0, whose clear bits clear depth, before the depth resolve: " + rig.error);
        REQUIRE((!rig.resolve(0, 0) && refusedAs(rig.error, "tiled_resolve_order: tile 0 slot 0, expected tile 0 slot 4")));
    }
    {
        MrtRig rig;
        rig.begin();
        UNSCOPED_INFO("tile 0 depth: " + rig.error);
        REQUIRE(rig.resolve(kGuestDepthSlot, 0));
        UNSCOPED_INFO("RT1 before RT0: " + rig.error);
        REQUIRE((!rig.resolve(1, 0) && refusedAs(rig.error, "tiled_resolve_order: tile 0 slot 1, expected tile 0 slot 0")));
    }
}

// Between tile scanouts, every set operation is refused on presented EDRAM.
void testMrtBetweenTileScanouts()
{
    // RT0 at 0x300 and RT1 at the presented base 0.
    auto moved          = mrtWords();
    moved[kMrt0 + 0x1C] = 0x300;
    moved[kMrt1 + 0x1C] = 0x00020000;
    const auto between  = [](MrtRig& rig)
    {
        *rig.words = unpredicated(tiledWords());
        UNSCOPED_INFO("the tiled clear: " + rig.error);
        REQUIRE(rig.clear(tiledClear()));
        UNSCOPED_INFO("tile 0: " + rig.error);
        REQUIRE(rig.assembler->Resolve(tiledTarget(0), tileCopy(0, 0x00100340), true, rig.partial, rig.error));
    };
    {
        MrtRig rig;
        between(rig);
        GuestClear  clear;
        std::string detail;
        UNSCOPED_INFO("the moved set decodes: " + detail);
        REQUIRE(ReadGuestClear(wordReader(moved), { kGraphics, 0x3F, 0, 0, 0, 1280, 720, kTileColor }, kStack, 1.0, clear, &detail));
        UNSCOPED_INFO("a set clear with RT1 on the presented base: " + rig.error);
        REQUIRE((!rig.clear(clear) && refusedAs(rig.error, "tiled_scanout_in_progress")));
    }
    {
        MrtRig rig;
        between(rig);
        *rig.words = unpredicated(moved);
        record(*rig.assembler, 2, kMrtTag);
        const auto result = rig.assembler->Swap(makeSwap(1, 1));
        UNSCOPED_INFO("a set draw with RT1 on the presented base: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.find("tiled_scanout_in_progress") != std::string::npos));
    }
}

// The set clear that writes depth checks it against every color slot.
void testMrtClearDepthOverlap()
{
    for (const auto& [base, expected] : { std::pair{ 0x300U, "clear_target_set_edram_overlap: RT1 depth" }, std::pair{ 0x100U, "clear_target_set_edram_overlap: RT0 depth" } })
    {
        auto words              = mrtWords();
        words[kMrtDepth + 0x1C] = base;
        GuestClear  clear;
        std::string detail;
        UNSCOPED_INFO("depth overlapping a color slot: " + detail);
        REQUIRE((!ReadGuestClear(wordReader(words), { kGraphics, 0x3F, 0, 0, 0, 1280, 720, kTileColor }, kStack, 1.0, clear, &detail) && detail == expected));
        UNSCOPED_INFO("a color-only clear of the same set writes no depth: " + detail);
        REQUIRE(ReadGuestClear(wordReader(words), { kGraphics, 0x0F, 1, 0, 0, 1280, 720, kTileColor }, kStack, 1.0, clear, &detail));
    }
    // An unbound depth slot is refused even where address 0 reads as a surface.
    auto words                = mrtWords();
    words[kGraphics + 0x30A8] = 0;
    words[kGraphics + 0x31C8] = 0;
    words[0x18]               = 0x070101E0;
    words[0x1C]               = 0x5E4;
    words[0x24]               = 0x06FC1678;
    GuestTarget target;
    std::string detail;
    UNSCOPED_INFO("an unbound depth slot: " + detail);
    REQUIRE((!ReadGuestTargetSet(wordReader(words), kGraphics, kGuestDepthSlot, target, &detail) && detail == "depth_unbound"));
}

// Dead own-color writes discard; presented color/depth require full clears.
void testMrtEndedSetRestrictions()
{
    const auto ended = [](NativeDrawMode mode)
    {
        auto rig = std::make_unique<MrtRig>(mode);
        rig->begin();
        for (std::uint32_t tile = 0; tile < 3; ++tile)
            for (const auto slot : kMrtSlots)
            {
                UNSCOPED_INFO("resolve admitted: " + rig->error);
                REQUIRE(rig->resolve(slot, tile));
            }
        *rig->words = mrtWords(0xFFFFFFFF, 0, 0x00042400);
        return rig;
    };
    const auto refused = [&](std::uint32_t address, std::optional<std::uint32_t> value, const std::string& expected)
    {
        auto rig = ended(NativeDrawMode::Strict);
        if (value)
            (*rig->words)[address] = *value;
        else
            rig->words->erase(address);
        record(*rig->assembler, 2, kMrtTag);
        const auto result = rig->assembler->Swap(makeSwap(1, 2));
        UNSCOPED_INFO("expected " + expected + ", got " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.find(expected) != std::string::npos));
    };
    refused(kGraphics + 0x2934, 0x24F00776, "ended_set_write_unsupported: depth_control 24F00776 color_mask 0xff");
    refused(kGraphics + 0x2934, 0x24F00771, "ended_set_write_unsupported: depth_control 24F00771 color_mask 0xff");
    refused(kGraphics + 0x28DC, 0xFFF, "ended_set_write_unsupported: depth_control 24F00770 color_mask 0xfff");
    refused(kGraphics + 0x2934, std::nullopt, "ended_set_state_unreadable");
    // The conditional or recorded VS variant (bit 6 of byte G+0x2ABC or G+0x2ABE).
    refused(kGraphics + 0x2ABC, 0x40042400, "ended_set_conditional_or_recorded_shader: 40042400");
    refused(kGraphics + 0x2ABC, 0x00046400, "ended_set_conditional_or_recorded_shader: 00046400");
    // Any sample-count state but the device-init one may count the samples.
    refused(kGraphics + 0x2A48, 1, "ended_set_query_state: sample_count_ctl 00000001 addr 00000000");
    refused(kGraphics + 0x2A4C, 0x1FC00000, "ended_set_query_state: sample_count_ctl 00000002 addr 1FC00000");
    refused(kGraphics + 0x2A48, std::nullopt, "ended_set_state_unreadable");
    refused(kGraphics + 0x2A4C, std::nullopt, "ended_set_state_unreadable");
    // RT1 at its base with a 512-sample pitch covers more EDRAM than the set.
    refused(kMrt1 + 0x18, 0x07010200, "frame_target_mismatch");
    {
        // Skip mode drops the refused write as a raster draw, by name.
        auto rig                                = ended(NativeDrawMode::Skip);
        (*rig->words)[kGraphics + 0x2934]       = 0x24F00776;
        std::optional<NativeDrawReason> verdict = std::nullopt;
        record(*rig->assembler, 2, kMrtTag, &verdict);
        const auto result = rig->assembler->Swap(makeSwap(1, 2));
        UNSCOPED_INFO(std::format("a refused write is skipped by name: {} {}", NativeFrameStatusName(result.status), result.detail));
        REQUIRE((verdict == NativeDrawReason::NoNativeRoute && result.status != NativeFrameStatus::Failed &&
                 result.coverage.skipped[static_cast<std::size_t>(NativeDrawReason::NoNativeRoute)] == 1 &&
                 result.detail.find("ended_set_write_unsupported") != std::string::npos));
    }

    // After the pass or a tiled scanout, a presented depth draw needs a full
    // depth and stencil clear; a color-only clear repairs only color.
    const auto depthDraw = [](std::uint32_t flags, bool scanoutFirst)
    {
        MrtRig rig;
        *rig.words = unpredicated(tiledWords());
        if (scanoutFirst)
            tiledFrame(*rig.assembler, 1, 5);
        else
        {
            UNSCOPED_INFO("the first presented clear: " + rig.error);
            REQUIRE(rig.clear(tiledClear()));
            scanOut(*rig.assembler);
        }
        const auto first = rig.assembler->Swap(makeSwap(1, scanoutFirst ? 1 : 0));
        UNSCOPED_INFO("the first frame: " + first.detail);
        REQUIRE((first.status == NativeFrameStatus::Provisional || first.status == NativeFrameStatus::Empty));
        if (!scanoutFirst)
        {
            *rig.words = unpredicated(mrtWords());
            rig.begin(2);
            for (std::uint32_t tile = 0; tile < 3; ++tile)
                for (const auto slot : kMrtSlots)
                {
                    UNSCOPED_INFO("resolve admitted: " + rig.error);
                    REQUIRE(rig.resolve(slot, tile));
                }
            *rig.words = unpredicated(tiledWords());
        }
        GuestClear clear;
        UNSCOPED_INFO("the presented clear decodes");
        REQUIRE(ReadGuestClear(wordReader(tiledWords()), { kGraphics, flags, 0, 0, 0, 1280, 720, kTileColor }, kStack, 1.0, clear));
        UNSCOPED_INFO("the presented clear: " + rig.error);
        REQUIRE(rig.clear(clear));
        record(*rig.assembler, 3, kDepthTag);
        scanout(*rig.assembler);
        return rig.assembler->Swap(makeSwap(2, 2));
    };
    for (const bool scanoutFirst : { false, true })
    {
        const auto colorOnly = depthDraw(0x0F, scanoutFirst);
        UNSCOPED_INFO("a depth draw after a color-only clear fails: " + colorOnly.detail);
        REQUIRE((colorOnly.status == NativeFrameStatus::Failed && colorOnly.detail.find("tiled_depth_uncleared") != std::string::npos));
        const auto full = depthDraw(0x3F, scanoutFirst);
        UNSCOPED_INFO("a full clear repairs depth: " + full.detail);
        REQUIRE(full.status == NativeFrameStatus::Provisional);
    }
}

// Only pinned loading pairs discard; attachment state cannot exclude exports.
void testMrtEndedSetShaders()
{
    const auto ended = [](MrtRig& rig)
    {
        rig.begin();
        for (std::uint32_t tile = 0; tile < 3; ++tile)
            for (const auto slot : kMrtSlots)
            {
                UNSCOPED_INFO("resolve admitted: " + rig.error);
                REQUIRE(rig.resolve(slot, tile));
            }
        *rig.words = mrtWords(0xFFFFFFFF, 0, 0x00042400);
    };
    const NativeDrawShaderWords label{ std::string(kLabelVertexWordDigest), std::string(kLabelPixelWordDigest) };
    for (const auto& [words, hook, caller] : { std::tuple{ gfxWords(), NativeDrawHook::IndexedUp, kGfxDrawCaller },
                                               std::tuple{ label, NativeDrawHook::NonindexedUp, kLabelDrawCaller } })
    {
        MrtRig rig;
        ended(rig);
        *rig.shaders       = words;
        const auto verdict = recordEnded(*rig.assembler, 2, hook, caller);
        const auto result  = rig.assembler->Swap(makeSwap(1, 2));
        UNSCOPED_INFO("a pinned pair is discarded: " + words.vertex + " " + result.detail);
        REQUIRE((!verdict && result.status == NativeFrameStatus::Empty && result.coverage.admitted == 2));
    }

    const std::string foreign(64, 'e');
    const auto        refused = [&](std::optional<NativeDrawShaderWords> words, NativeDrawHook hook, std::uint32_t caller, const std::string& expected, std::optional<NativeDrawShaderReader> reader = std::nullopt)
    {
        MrtRig rig(NativeDrawMode::Strict, std::move(reader));
        ended(rig);
        *rig.shaders       = std::move(words);
        const auto verdict = recordEnded(*rig.assembler, 2, hook, caller);
        const auto result  = rig.assembler->Swap(makeSwap(1, 2));
        UNSCOPED_INFO("expected " + expected + ", got " + result.detail);
        REQUIRE((verdict == NativeDrawReason::NoNativeRoute && result.status == NativeFrameStatus::Failed && result.detail.find(expected) != std::string::npos));
    };
    // The words a guest that has not patched for the draw holds are no pinned VS: only the patched words are.
    refused(gfxWords(kGfxSourceVertexWordDigest), NativeDrawHook::IndexedUp, kGfxDrawCaller, std::format("ended_set_shader_unpinned: vs {} ps {}", kGfxSourceVertexWordDigest, kGfxPixelWordDigest));
    // A memory-exporting PS, or any other, behind the GFx VS.
    refused(NativeDrawShaderWords{ std::string(kGfxVertexWordDigest), foreign }, NativeDrawHook::IndexedUp, kGfxDrawCaller, std::format("ended_set_shader_unpinned: vs {} ps {}", kGfxVertexWordDigest, foreign));
    refused(NativeDrawShaderWords{ foreign, std::string(kGfxPixelWordDigest) }, NativeDrawHook::IndexedUp, kGfxDrawCaller, std::format("ended_set_shader_unpinned: vs {} ps {}", foreign, kGfxPixelWordDigest));
    // Pinned words in another pairing, hook or call site.
    refused(NativeDrawShaderWords{ std::string(kGfxVertexWordDigest), std::string(kLabelPixelWordDigest) }, NativeDrawHook::IndexedUp, kGfxDrawCaller, "ended_set_shader_unpinned");
    refused(gfxWords(), NativeDrawHook::IndexedUp, kPanelCaller, "ended_set_shader_unpinned");
    refused(gfxWords(), NativeDrawHook::NonindexedUp, kGfxDrawCaller, "ended_set_shader_unpinned");
    refused(label, NativeDrawHook::IndexedUp, kLabelDrawCaller, "ended_set_shader_unpinned");
    refused(label, NativeDrawHook::NonindexedUp, kGfxDrawCaller, "ended_set_shader_unpinned");
    // An identity that cannot be read is never assumed.
    refused(std::nullopt, NativeDrawHook::IndexedUp, kGfxDrawCaller, "ended_set_shader_unreadable: fixture_shader_unreadable");
    refused(gfxWords(), NativeDrawHook::IndexedUp, kGfxDrawCaller, "ended_set_shader_unreadable", NativeDrawShaderReader{});
    refused(gfxWords(), NativeDrawHook::IndexedUp, kGfxDrawCaller, "ended_set_shader_unreadable: fixture_shader_throw", [](const NativeDrawSubmission&, NativeDrawShaderWords&, std::string&) -> bool
            {
                throw std::runtime_error("fixture_shader_throw");
            });
    {
        // Skip mode drops the refused write as a raster draw, by name.
        MrtRig rig(NativeDrawMode::Skip);
        ended(rig);
        *rig.shaders       = NativeDrawShaderWords{ std::string(kGfxVertexWordDigest), foreign };
        const auto verdict = recordEnded(*rig.assembler, 2);
        const auto result  = rig.assembler->Swap(makeSwap(1, 2));
        UNSCOPED_INFO(std::format("an unpinned write is skipped by name: {} {}", NativeFrameStatusName(result.status), result.detail));
        REQUIRE((verdict == NativeDrawReason::NoNativeRoute && result.status == NativeFrameStatus::Partial &&
                 result.coverage.skipped[static_cast<std::size_t>(NativeDrawReason::NoNativeRoute)] == 1 &&
                 result.detail.find("ended_set_shader_unpinned") != std::string::npos));
    }
}

// Scanout 0x1414/0x01000300 swaps red and blue to present unswapped.
void testScanoutSwap()
{
    // Frontbuffers 0x1F6F8000 and 0x1F360000 alternate; later writes drop the
    // scanout form.
    constexpr std::uint32_t kCapturedSwapWord3 = 0x1414, kFrontA = 0x1F6F8000, kFrontB = 0x1F360000;
    GuestTarget             main;
    main.native   = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    using Swizzle = std::array<std::uint8_t, 4>;

    struct Epoch
    {
        std::vector<std::uint32_t> destInfos; // the epoch's untiled scanouts, in order
        std::uint32_t              destBase = kFrontA, frontbuffer = kFrontA, endian = 0;
    };

    // Clears and scans out the presented target, runs `between`, then swaps.
    const auto present = [&](NativeFrameAssembler& assembler, FakeCompletion& completion, std::uint64_t sequence, const Epoch& epoch, std::string& detail, const std::function<void(NativeFrameAssembler&)>& between = {})
    {
        std::string error;
        bool        partial = false;
        detail.clear();
        UNSCOPED_INFO("the presented target is cleared: " + error);
        REQUIRE(assembler.Clear(fullClear(main), error) != 0);
        for (const auto destInfo : epoch.destInfos)
        {
            auto copy     = scanoutCopy(destInfo);
            copy.destBase = epoch.destBase;
            if (!assembler.Resolve(main, copy, true, partial, error) && detail.empty())
                detail = error;
        }
        if (between)
            between(assembler);
        auto swap                = makeSwap(sequence);
        swap.frontbufferPhysical = epoch.frontbuffer;
        swap.fetch[1]            = epoch.frontbuffer | 6 | (epoch.endian << 6);
        swap.fetch[3]            = kCapturedSwapWord3;
        const auto result        = assembler.Swap(swap);
        if (result.status != NativeFrameStatus::Provisional)
        {
            detail = detail.empty() ? result.detail : detail;
            return std::optional<Swizzle>();
        }
        return std::optional<Swizzle>(completion.spans.back().frame->outputSwizzle);
    };
    // An off-screen 1024x1024 k_8_8_8_8 resolve to `destBase`, admitted on a
    // cleared source, refused unmodeled, or refused by the hook before Resolve.
    enum class Write
    {
        Admitted,
        Refused,
        Hook
    };
    const auto offscreenResolve = [&](NativeFrameAssembler& assembler, std::uint32_t destBase, Write write)
    {
        const auto  target = offscreen(0x300);
        auto        copy   = fullCopy(target);
        std::string error;
        bool        partial = false;
        copy.destBase       = destBase;
        copy.destPitch      = 0x04000400;
        copy.destInfo       = 0x300;
        if (write == Write::Hook)
            assembler.ResolveUnmodeled(copy);
        else if (write == Write::Admitted)
        {
            UNSCOPED_INFO("the off-screen resolve is admitted: " + error);
            REQUIRE((assembler.Clear(fullClear(target), error) && assembler.Resolve(target, copy, false, partial, error)));
        }
        else
        {
            UNSCOPED_INFO("the off-screen resolve is refused: " + error);
            REQUIRE((!assembler.Resolve(target, copy, false, partial, error) && refusedAs(error, "resolve_source_unmodeled: other_source")));
        }
    };
    // Guest write notices the assembler armed, as [base, base + length).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> armed;
    std::function<void(NativeFrameAssembler&)>           onArm;
    NativeFrameAssembler*                                armedBy = nullptr;
    const auto                                           session = [&](FakeCompletion& completion)
    {
        NativeFrameAssemblerConfig config;
        config.completion  = &completion;
        config.watchWrites = [&](std::uint32_t physical, std::uint32_t length)
        {
            armed.emplace_back(physical, length);
            if (onArm && armedBy)
                onArm(*armedBy);
        };
        return config;
    };
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(session(completion));
        std::string          detail;
        UNSCOPED_INFO("the captured swapped scanout and ZYX1 fetch present the target unswapped: " + detail);
        REQUIRE(present(assembler, completion, 1, { { kScanoutDestInfo } }, detail) == Swizzle{ 0, 1, 2, 5 });
        UNSCOPED_INFO("an unswapped scanout keeps the fetch swizzle: " + detail);
        REQUIRE(present(assembler, completion, 2, { { 0x300 } }, detail) == Swizzle{ 2, 1, 0, 5 });
        // Separate complete scanouts may differ; the last one decides.
        UNSCOPED_INFO("a later untiled scanout overwrites an earlier one's form: " + detail);
        REQUIRE((present(assembler, completion, 3, { { 0x300, kScanoutDestInfo } }, detail) == Swizzle{ 0, 1, 2, 5 } &&
                 present(assembler, completion, 4, { { kScanoutDestInfo, 0x300 } }, detail) == Swizzle{ 2, 1, 0, 5 }));
        // Each scanout arms a guest write notice over its frontbuffer view.
        UNSCOPED_INFO(std::format("scanouts arm write notices: {}", armed.size()));
        REQUIRE((armed.size() == 6 && armed.front() == std::pair{ kFrontA, 0x398000U }));
        // Alternating frontbuffers each present their own epoch's form.
        UNSCOPED_INFO("double-buffered swaps present their own scanouts: " + detail);
        REQUIRE((present(assembler, completion, 5, { { kScanoutDestInfo }, kFrontB, kFrontB }, detail) == Swizzle{ 0, 1, 2, 5 } &&
                 present(assembler, completion, 6, { { 0x300 }, kFrontA, kFrontA }, detail) == Swizzle{ 2, 1, 0, 5 }));
        // The work of a swap without its own scanout never reached its
        // frontbuffer, which an earlier epoch scanned out.
        UNSCOPED_INFO("a swap without its own scanout is refused: " + detail);
        REQUIRE((!present(assembler, completion, 7, {}, detail) && detail == "swap_scanout_unmodeled: 0x1f6f8000"));
    }
    {
        // No modeled scanout reached the frontbuffer: the first frame, or another one's.
        FakeCompletion       completion;
        NativeFrameAssembler assembler(session(completion));
        std::string          detail;
        UNSCOPED_INFO("the first frame without a scanout is refused: " + detail);
        REQUIRE((!present(assembler, completion, 1, {}, detail) && detail == "swap_scanout_unmodeled: 0x1f6f8000"));
        FakeCompletion       other;
        NativeFrameAssembler second(session(other));
        UNSCOPED_INFO("a frontbuffer no scanout wrote is refused: " + detail);
        REQUIRE((present(second, other, 1, { { kScanoutDestInfo } }, detail) && !present(second, other, 2, { {}, kFrontB, kFrontB }, detail) &&
                 detail == "swap_scanout_unmodeled: 0x1f360000"));
    }
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(session(completion));
        std::string          detail;
        UNSCOPED_INFO("a scanout to another frontbuffer than the swap's is refused: " + detail);
        REQUIRE((present(assembler, completion, 1, { { kScanoutDestInfo }, kFrontB, kFrontB }, detail) &&
                 !present(assembler, completion, 2, { { kScanoutDestInfo }, kFrontB, kFrontA }, detail) &&
                 detail == "swap_scanout_destination_mismatch: 0x1f360000 is not swap 0x1f6f8000"));
    }
    // Resolves into the frontbuffer's range after its scanout drop the form,
    // admitted or not; those ending at its base or starting at its end leave it.
    for (const auto& [destBase, write, dropped] : { std::tuple{ kFrontA + 0x1000, Write::Admitted, true }, std::tuple{ kFrontA + 0x397000, Write::Refused, true }, std::tuple{ kFrontA + 0x2000, Write::Hook, true }, std::tuple{ kFrontA - 0x400000, Write::Admitted, false }, std::tuple{ kFrontA + 0x398000, Write::Admitted, false }, std::tuple{ kFrontA + 0x398000, Write::Hook, false } })
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(session(completion));
        std::string          detail;
        const auto           swizzle = present(assembler, completion, 1, { { kScanoutDestInfo } }, detail, [&](NativeFrameAssembler& on)
                                               {
                                         offscreenResolve(on, destBase, write);
                                               });
        const auto           refused = std::format("swap_scanout_overwritten: 0x1f6f8000 by a resolve to {:#010x}", destBase);
        UNSCOPED_INFO(std::format("a resolve to {:#x} {} the scanout form: {}", destBase, dropped ? "drops" : "keeps", detail));
        REQUIRE((dropped ? !swizzle && detail == refused : swizzle == Swizzle{ 0, 1, 2, 5 }));
    }
    {
        // A refused resolve whose copy words were unread may have written anywhere.
        FakeCompletion       completion;
        NativeFrameAssembler assembler(session(completion));
        std::string          detail;
        UNSCOPED_INFO("an unread resolve drops the scanout form: " + detail);
        REQUIRE((!present(assembler, completion, 1, { { kScanoutDestInfo } }, detail, [](NativeFrameAssembler& on)
                          {
                              on.ResolveUnmodeled(std::nullopt);
                          }) &&
                 detail == "swap_scanout_overwritten: 0x1f6f8000 by an unread resolve"));
    }
    // Guest writes the notice reports: into the frontbuffer view after its
    // scanout, including one while the notice is armed, drop the form; those
    // outside it, or before the scanout, leave it.
    for (const auto& [address, length, during, dropped] : { std::tuple{ kFrontA + 0x1000, 4U, false, true }, std::tuple{ kFrontA + 0x397000, 0x1000U, false, true }, std::tuple{ kFrontA, 4U, true, true }, std::tuple{ kFrontA - 0x1000, 0x1000U, false, false }, std::tuple{ kFrontA + 0x398000, 0x1000U, false, false } })
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(session(completion));
        std::string          detail;
        armedBy = &assembler;
        if (during)
            onArm = [&](NativeFrameAssembler& on)
            {
                on.GuestWrote(address, length);
            };
        const auto swizzle = present(assembler, completion, 1, { { kScanoutDestInfo } }, detail, [&](NativeFrameAssembler& on)
                                     {
                                         if (!during)
                                             on.GuestWrote(address, length);
                                     });
        onArm              = {};
        armedBy            = nullptr;
        UNSCOPED_INFO(std::format("a guest write at {:#x} {} the scanout form: {}", address, dropped ? "drops" : "keeps", detail));
        REQUIRE((dropped ? !swizzle && detail == std::format("swap_scanout_overwritten: 0x1f6f8000 by a guest write at {:#010x}", address)
                         : swizzle == Swizzle{ 0, 1, 2, 5 }));
    }
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(session(completion));
        std::string          detail;
        assembler.GuestWrote(kFrontA, 0x398000);
        UNSCOPED_INFO("a guest write before the scanout leaves its form: " + detail);
        REQUIRE(present(assembler, completion, 1, { { kScanoutDestInfo } }, detail) == Swizzle{ 0, 1, 2, 5 });
    }
    // Endian, array, slice, format, number, exponent bias and unknown bits.
    for (const std::uint32_t destInfo : { 0x01000302U, 0x01000308U, 0x01000310U, 0x01000380U, 0x01002300U, 0x01010300U, 0x00400300U, 0x02000300U })
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(session(completion));
        std::string          detail;
        const auto           expected = std::format("resolve_frontbuffer_dest_unsupported: {:08X}", destInfo);
        UNSCOPED_INFO("an unmodeled scanout form is refused: " + detail);
        REQUIRE((!present(assembler, completion, 1, { { destInfo } }, detail) && detail == expected));
    }
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(session(completion));
        std::string          error;
        bool                 partial = false;
        auto                 copy    = scanoutCopy(kScanoutDestInfo);
        copy.destPitch               = 0x04000400;
        UNSCOPED_INFO("a scanout into another view than 1280x720 is refused: " + error);
        REQUIRE((assembler.Clear(fullClear(main), error) && !assembler.Resolve(main, copy, true, partial, error) &&
                 refusedAs(error, "resolve_frontbuffer_pitch_unsupported: 04000400")));
    }
    // An untiled scanout replaces the form only as the whole frame resolved
    // with the title's k01 control: a partial rectangle, an offset point, a
    // single-sample or depth control is refused, and fails the swap.
    for (const auto& [variant, expected] : { std::pair{ 0, "resolve_frontbuffer_form: rect 0,0,640,720 point 0,0 control 00100040" },
                                             std::pair{ 1, "resolve_frontbuffer_form: rect 0,0,1280,720 point 32,0 control 00100040" },
                                             std::pair{ 2, "resolve_frontbuffer_form: rect 0,0,1280,720 point 0,0 control 00100000" },
                                             std::pair{ 3, "resolve_frontbuffer_form: rect 0,0,1280,720 point 0,0 control 00100044" } })
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(session(completion));
        std::string          error;
        bool                 partial = false;
        UNSCOPED_INFO("the presented target is cleared: " + error);
        REQUIRE(assembler.Clear(fullClear(main), error));
        auto copy = scanoutCopy(kScanoutDestInfo);
        UNSCOPED_INFO("the whole frame is scanned out: " + error);
        REQUIRE((assembler.Resolve(main, copy, true, partial, error) && error.empty()));
        if (variant == 0)
            copy.rectWidth = 640;
        else if (variant == 1)
            copy.destX = 32;
        else
            copy.copyControl = variant == 2 ? 0x00100000 : 0x00100044;
        copy.destInfo = 0x300;
        UNSCOPED_INFO("a partial or foreign scanout is refused: " + error);
        REQUIRE((!assembler.Resolve(main, copy, true, partial, error) && refusedAs(error, expected)));
        auto swap         = makeSwap(1);
        swap.fetch[3]     = kCapturedSwapWord3;
        const auto result = assembler.Swap(swap);
        UNSCOPED_INFO("and fails the swap: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Failed && result.detail == expected));
    }
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(session(completion));
        std::string          detail;
        UNSCOPED_INFO("a byte-swapped frontbuffer fetch is refused: " + detail);
        REQUIRE((!present(assembler, completion, 1, { { kScanoutDestInfo }, kFrontA, kFrontA, 2 }, detail) &&
                 detail == "swap_frontbuffer_endian_unsupported: 2"));
    }
    // Tiles of one scanout must agree on the form and the destination.
    for (const auto& [destInfo, destBase, expected] : { std::tuple{ 0x300U, 0x1F000000U, "resolve_frontbuffer_dest_mismatch: tile 1 00000300 at 1F000000 after 01000300 at 1F000000" },
                                                        std::tuple{ kScanoutDestInfo, 0x1F014000U, "resolve_frontbuffer_dest_mismatch: tile 1 01000300 at 1F014000 after 01000300 at 1F000000" } })
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        std::string          error;
        bool                 partial = false;
        UNSCOPED_INFO("the tiled clear is admitted: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        auto second     = tileCopy(1, 0x00100040);
        second.destInfo = destInfo;
        second.destBase = destBase;
        UNSCOPED_INFO("tiles with different scanout forms or destinations are refused: " + error);
        REQUIRE((assembler.Resolve(tiledTarget(0), tileCopy(0, 0x00100340), true, partial, error) &&
                 !assembler.Resolve(tiledTarget(1), second, true, partial, error) && refusedAs(error, expected)));
    }
    // Between the tiles of one scanout, another write into its frontbuffer
    // leaves mixed content: the completed scanout records no form. Writes
    // outside it, and the tiles themselves, leave the form.
    enum class Between
    {
        None,
        Hook,
        Unread,
        Guest,
        Refused,
        Outside
    };
    for (const auto& [between, expected] : { std::pair{ Between::None, "" }, std::pair{ Between::Hook, "swap_scanout_overwritten: 0x1f000000 by a resolve to 0x1f100000" }, std::pair{ Between::Unread, "swap_scanout_overwritten: 0x1f000000 by an unread resolve" }, std::pair{ Between::Guest, "swap_scanout_overwritten: 0x1f000000 by a guest write at 0x1f200000" }, std::pair{ Between::Refused, "swap_scanout_overwritten: 0x1f000000 by a resolve to 0x1f100000" }, std::pair{ Between::Outside, "" } })
    {
        FakeCompletion completion;
        auto           config = tiledConfig(completion);
        armed.clear();
        config.watchWrites = [&](std::uint32_t physical, std::uint32_t length)
        {
            armed.emplace_back(physical, length);
        };
        NativeFrameAssembler assembler(config);
        std::string          error;
        bool                 partial = false;
        UNSCOPED_INFO("the tiled clear is admitted: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        UNSCOPED_INFO("tile 0: " + error);
        REQUIRE(assembler.Resolve(tiledTarget(0), tileCopy(0, 0x00100340), true, partial, error));
        auto copy     = tileCopy(1, 0x00100040);
        copy.destBase = 0x1F100000;
        if (between == Between::Hook)
            assembler.ResolveUnmodeled(copy);
        else if (between == Between::Unread)
            assembler.ResolveUnmodeled(std::nullopt);
        else if (between == Between::Guest)
            assembler.GuestWrote(0x1F200000, 4);
        else if (between == Between::Outside)
        {
            copy.destBase = 0x1F398000;
            assembler.ResolveUnmodeled(copy);
            assembler.GuestWrote(0x1F398000, 0x1000);
        }
        else if (between == Between::Refused)
        {
            const auto target    = offscreen(0x300);
            auto       offscreen = fullCopy(target);
            offscreen.destBase   = 0x1F100000;
            offscreen.destPitch  = 0x04000400;
            offscreen.destInfo   = 0x300;
            UNSCOPED_INFO("the off-screen resolve between the tiles is refused: " + error);
            REQUIRE((assembler.Resolve(target, offscreen, false, partial, error) == 0 && refusedAs(error, "resolve_source_unmodeled: other_source")));
        }
        error.clear();
        UNSCOPED_INFO("tile 1: " + error);
        REQUIRE((assembler.Resolve(tiledTarget(1), tileCopy(1, 0x00100040), true, partial, error) && error.empty()));
        UNSCOPED_INFO("tile 0 arms the write notice once");
        REQUIRE((armed.size() == 1 && armed[0] == std::pair{ 0x1F000000U, 0x398000U }));
        auto swap         = makeSwap(1);
        swap.fetch[3]     = kCapturedSwapWord3;
        const auto result = assembler.Swap(swap);
        if (*expected)
        {
            UNSCOPED_INFO("a write between the tiles drops the form: " + result.detail);
            REQUIRE((result.status == NativeFrameStatus::Failed && result.detail == expected));
        }
        else
        {
            UNSCOPED_INFO("the tiles alone record their form: " + result.detail);
            REQUIRE((result.status == NativeFrameStatus::Provisional && completion.spans.back().frame->outputSwizzle == Swizzle{ 0, 1, 2, 5 }));
        }
    }
}

// One recorded event of a harvest frame form: the hook's registers, the
// device block at r3 and the memory rows the harvest read.
struct HarvestEvent
{
    std::string                                                      kind;
    std::uint32_t                                                    entry = 0, caller = 0, stack = 0;
    std::array<std::uint32_t, 8>                                     r{};
    double                                                           depth = 1.0;
    std::shared_ptr<const std::vector<std::uint8_t>>                 state;
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> rows;
    std::vector<std::string>                                         roles;    // one per row
    std::vector<bool>                                                physical; // one per row
    std::string                                                      pair;     // a draw's caller-vs-ps-key record
    // The pair record's pixel literal records: first float4 and words.
    std::shared_ptr<const std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>>> pixelLiterals;

    const std::vector<std::uint8_t>* Row(std::string_view role) const
    {
        for (std::size_t i = 0; i < roles.size(); ++i)
            if (roles[i] == role)
                return &rows[i].second;
        return nullptr;
    }

    bool Read(std::uint32_t address, std::uint32_t& out) const
    {
        const auto word = [&](const std::vector<std::uint8_t>& bytes, std::size_t offset)
        {
            out = (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) | (std::uint32_t(bytes[offset + 2]) << 8) |
                  bytes[offset + 3];
            return true;
        };
        if (state && address >= r[0] && std::size_t(address - r[0]) + 4 <= state->size())
            return word(*state, address - r[0]);
        for (const auto& [base, bytes] : rows)
            if (address >= base && std::size_t(address - base) + 4 <= bytes.size())
                return word(bytes, address - base);
        return false;
    }
};

using PayloadCache = std::map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>>;

std::shared_ptr<const std::vector<std::uint8_t>> payload(const std::filesystem::path& root, const std::string& name, PayloadCache& cache)
{
    auto& slot = cache[name];
    if (!slot)
    {
        std::ifstream file(root / name, std::ios::binary);
        UNSCOPED_INFO("harvest payload opens: " + name);
        REQUIRE(file.good());
        slot = std::make_shared<const std::vector<std::uint8_t>>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    return slot;
}

std::uint32_t hexWord(const toml::node_view<const toml::node>& node)
{
    const auto text = node.value_or(std::string{});
    UNSCOPED_INFO("a harvest word is present");
    REQUIRE(!text.empty());
    return static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
}

// The pixel literal records of `<root>/<pair>/record.toml`, which the frame
// events do not carry; empty without the record.
std::shared_ptr<const std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>>> pairPixelLiterals(const std::filesystem::path& root, const std::string& pair)
{
    using Literals = std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>>;
    static std::map<std::string, std::shared_ptr<const Literals>> cache;
    const auto                                                    file = root / pair / "record.toml";
    auto&                                                         slot = cache[file.string()];
    if (slot)
        return slot;
    Literals literals;
    if (!pair.empty() && std::filesystem::exists(file))
    {
        const auto record = toml::parse_file(file.string());
        if (const auto* rows = record["pixel_literals"].as_array())
            for (const auto& row : *rows)
            {
                const auto                 view = toml::node_view<const toml::node>(row);
                std::vector<std::uint32_t> words;
                if (const auto* values = view["values"].as_array())
                    for (const auto& value : *values)
                        words.push_back(static_cast<std::uint32_t>(value.value_or<std::int64_t>(0)));
                literals.emplace_back(static_cast<std::uint32_t>(view["first_float4"].value_or<std::int64_t>(0)), std::move(words));
            }
    }
    slot = std::make_shared<const Literals>(std::move(literals));
    return slot;
}

std::vector<HarvestEvent> loadHarvestFrame(const std::filesystem::path& root, const toml::table& table, PayloadCache& cache)
{
    std::vector<HarvestEvent> events;
    const auto*               rows = table["events"].as_array();
    UNSCOPED_INFO("the frame has events");
    REQUIRE((rows && !rows->empty()));
    for (const auto& row : *rows)
    {
        const auto   view = toml::node_view<const toml::node>(row);
        HarvestEvent event;
        event.kind            = view["kind"].value_or(std::string{});
        event.entry           = hexWord(view["entry"]);
        event.caller          = hexWord(view["caller"]);
        event.stack           = hexWord(view["stack_r1"]);
        event.depth           = view["depth_f1"].value_or(1.0);
        const auto* arguments = view["arguments_r3_r10"].as_array();
        UNSCOPED_INFO("eight argument registers");
        REQUIRE((arguments && arguments->size() == 8));
        for (std::size_t i = 0; i < 8; ++i)
            event.r[i] = hexWord(toml::node_view<const toml::node>((*arguments)[i]));
        const auto state = view["state"].value_or(std::string{});
        UNSCOPED_INFO("the event has its device block");
        REQUIRE(!state.empty());
        event.state = payload(root, state, cache);
        event.pair  = view["pair"].value_or(std::string{});
        if (!event.pair.empty())
            event.pixelLiterals = pairPixelLiterals(root, event.pair);
        if (const auto* memory = view["memory"].as_array())
            for (const auto& entry : *memory)
            {
                const auto read    = toml::node_view<const toml::node>(entry);
                const auto address = hexWord(read["address"]);
                event.roles.resize(event.rows.size());
                event.roles.push_back(read["role"].value_or(std::string{}));
                event.physical.resize(event.rows.size());
                event.physical.push_back(read["physical"].value_or(false));
                if (const auto bytes = read["bytes"].value<std::string>())
                {
                    std::vector<std::uint8_t> decoded;
                    for (std::size_t i = 0; i + 1 < bytes->size(); i += 2)
                        decoded.push_back(static_cast<std::uint8_t>(std::stoul(bytes->substr(i, 2), nullptr, 16)));
                    event.rows.emplace_back(address, std::move(decoded));
                }
                else if (const auto file = read["file"].value<std::string>())
                    event.rows.emplace_back(address, *payload(root, *file, cache));
            }
        event.roles.resize(event.rows.size());
        event.physical.resize(event.rows.size());
        events.push_back(std::move(event));
    }
    return events;
}

// Replays `events` (from `first`) into one epoch and swaps it. Pre-tiling
// draws carry tag 1, draws under tiling tag 2. `skipClear` drops the clear.
NativeAssembledFrame replayHarvestEpoch(NativeFrameAssembler& assembler, const HarvestEvent*& current, std::uint8_t& tag, const std::vector<HarvestEvent>& events, std::size_t first, std::uint64_t& recorded, std::uint64_t swap, bool skipClear)
{
    std::optional<GuestResolveCopy> scanned;
    std::uint64_t                   sequence = recorded;
    for (std::size_t i = first; i < events.size(); ++i)
    {
        const auto& event = events[i];
        current           = &event;
        const auto read   = [&event](std::uint32_t address, std::uint32_t& out)
        {
            return event.Read(address, out);
        };
        std::string error;
        if (event.kind == "begin_tiling")
            tag = 2;
        else if (event.kind == "draw")
        {
            const auto hook = std::find(kNativeDrawHookAddresses.begin(), kNativeDrawHookAddresses.end(), event.entry);
            UNSCOPED_INFO(std::format("draw entry {:08X} is a hook", event.entry));
            REQUIRE(hook != kNativeDrawHookAddresses.end());
            NativeDrawSubmission value;
            value.hook          = static_cast<NativeDrawHook>(hook - kNativeDrawHookAddresses.begin());
            value.caller        = event.caller;
            value.stack         = event.stack;
            value.arguments     = event.r;
            value.stateRead     = true;
            value.sequence      = ++sequence;
            value.operationMark = assembler.NumberOperation();
            submit(assembler, value);
            ++recorded;
        }
        else if (event.kind == "clear")
        {
            if (skipClear)
                continue;
            GuestClear clear;
            UNSCOPED_INFO("the harvest clear decodes: " + error);
            REQUIRE(ReadGuestClear(read, event.r, event.stack, event.depth, clear, &error));
            UNSCOPED_INFO("the harvest clear is admitted: " + error);
            REQUIRE(assembler.Clear(clear, error) != 0);
        }
        else if (event.kind == "resolve")
        {
            GuestTarget      target;
            GuestResolveCopy copy;
            UNSCOPED_INFO("the harvest resolve decodes: " + error);
            REQUIRE((ReadGuestResolveEntry(read, event.r, target, copy, &error) && ReadGuestResolveCopy(read, event.r[0], copy)));
            // As RecordGuestResolveCopy classifies it.
            const bool front   = assembler.ScanoutCandidate(target, copy);
            bool       partial = false;
            UNSCOPED_INFO("the harvest resolve is a scanout");
            REQUIRE(front);
            UNSCOPED_INFO("the harvest scanout is admitted: " + error);
            REQUIRE((assembler.Resolve(target, copy, front, partial, error) || skipClear));
            scanned = copy;
        }
        else
        {
            UNSCOPED_INFO("unknown harvest event kind " + event.kind);
            REQUIRE(false);
        }
    }
    current    = nullptr;
    auto value = makeSwap(swap, recorded);
    if (scanned)
    {
        value.frontbufferPhysical = scanned->destBase;
        value.fetch[1]            = scanned->destBase | 6;
    }
    return assembler.Swap(value);
}

// Warm each tiled part; pre-clear draws see undefined presented storage.
int harvestFrames(const std::filesystem::path& root)
{
    PayloadCache cache;
    std::size_t  frames = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "frames"))
    {
        const auto file = entry.path() / "frame.toml";
        if (!entry.is_directory() || !std::filesystem::exists(file))
            continue;
        const auto table = toml::parse_file(file.string());
        if (!table["draw_before_clear"].value_or(false))
            continue;
        const auto events = loadHarvestFrame(root, table, cache);
        const auto tiling = std::find_if(events.begin(), events.end(), [](const HarvestEvent& event)
                                         {
                                             return event.kind == "begin_tiling";
                                         });
        UNSCOPED_INFO("the frame begins tiling");
        REQUIRE(tiling != events.end());
        const auto  name   = entry.path().filename().string();
        std::size_t before = 0, after = 0;
        for (auto it = events.begin(); it != events.end(); ++it)
            if (it->kind == "draw")
                ++(it < tiling ? before : after);

        for (const bool skipClear : { false, true })
        {
            FakeCompletion             completion;
            const HarvestEvent*        current = nullptr;
            std::uint8_t               tag     = 2;
            NativeFrameAssemblerConfig config;
            config.completion = &completion;
            config.build      = [&tag](const NativeDrawSubmission&, NativeDrawReplayRecipe& draw, std::string&)
            {
                draw = makeDraw(tag);
                return true;
            };
            config.readTarget = [&current](std::uint32_t address, std::uint32_t& out)
            {
                return current && current->Read(address, out);
            };
            NativeFrameAssembler assembler(config);
            std::uint64_t        recorded = 0;
            const auto           warm     = replayHarvestEpoch(assembler, current, tag, events, tiling - events.begin(), recorded, 1, false);
            UNSCOPED_INFO(name + ": the tiled part presents: " + warm.detail);
            REQUIRE(warm.status == NativeFrameStatus::Provisional);
            tag              = 1;
            const auto frame = replayHarvestEpoch(assembler, current, tag, events, 0, recorded, 2, skipClear);
            if (skipClear)
            {
                UNSCOPED_INFO(name + ": without the clear the untiled draws fail: " + frame.detail);
                REQUIRE((frame.status == NativeFrameStatus::Failed &&
                         frame.detail.find("presented_draw_before_clear: untiled; dead_write_unproven: then ") != std::string::npos));
                continue;
            }
            UNSCOPED_INFO(name + ": the frame presents: " + frame.detail);
            REQUIRE(frame.status == NativeFrameStatus::Provisional);
            const auto tags = drawTags(completion.spans.back());
            UNSCOPED_INFO(std::format("{}: only the {} tiled draws remain, got {}", name, after, tags.size()));
            REQUIRE((tags.size() == after && std::all_of(tags.begin(), tags.end(), [](std::uint8_t value)
                                                         {
                                                             return value == 2;
                                                         })));
        }
        std::cout << std::format("{}: {} untiled draws dead under the tile-0 clear, {} tiled draws presented\n", name, before, after);
        ++frames;
    }
    UNSCOPED_INFO("the harvest has draw-before-clear frames");
    REQUIRE(frames > 0);
    return 0;
}

// Scene recipes capture FLOAT3/FLOAT2 attributes, indices, constants, viewport,
// rasterizer state and hashes.
void sceneGeometry(const HarvestEvent& event, NativeDrawReplayRecipe& draw)
{
    // Elements FLOAT3 at 0 and FLOAT2 at 12 of stream 0.
    static constexpr std::array<std::uint8_t, 24> kQuad{ 0, 0, 0, 0, 0, 0x2A, 0x23, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0x0C, 0, 0x2C, 0x23, 0xA5, 0, 0x05, 0, 0 };
    // The composite's (0x8230686C) FLOAT3 at 0 and 12 and FLOAT2 at 24.
    static constexpr std::array<std::uint8_t, 36> kComposite{ 0, 0, 0, 0, 0, 0x2A, 0x23, 0xB9, 0, 0, 0, 0xFF, 0, 0, 0, 0x0C, 0, 0x2A, 0x23, 0xB9, 0, 3, 0, 4, 0, 0, 0, 0x18, 0, 0x2C, 0x23, 0xA5, 0, 5, 0, 0 };
    const auto*                                   stream      = event.Row("stream 0");
    const auto*                                   indices     = event.Row("indices");
    const auto*                                   declaration = event.Row("declaration");
    const auto*                                   buffer      = event.Row("index_buffer");
    const auto                                    count       = event.r[4];
    // r4 is the primitive (4, a triangle list) and r7 the index count. The
    // index buffer object's flags (+0) bit 31 select 32-bit indices, which
    // the composite uses; bits 29..30 must be that width's byte swap.
    if (event.pair.size() != 51 || !stream || !indices || !declaration || !buffer || buffer->size() < 4 || event.r[1] != 4)
        return;
    const auto width = ((*buffer)[0] & 0x80) ? 4U : 2U;
    if ((((*buffer)[0] >> 5) & 3) != (width == 4 ? 2U : 1U) || indices->size() != std::size_t(count) * width)
        return;
    const bool quad = std::ranges::equal(*declaration, kQuad), composite = std::ranges::equal(*declaration, kComposite);
    if (!quad && !composite)
        return;
    // Each element's component count; the guest packs them, the host pads
    // each to a float4 with w 1.
    const auto     elements    = quad ? std::vector<unsigned>{ 3, 2 } : std::vector<unsigned>{ 3, 3, 2 };
    const unsigned guestStride = quad ? 20 : 32, hostStride = unsigned(elements.size()) * 16;
    const auto     big = [](const std::vector<std::uint8_t>& bytes, std::size_t offset)
    {
        return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) | (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
    };
    std::vector<std::uint32_t> list;
    std::uint32_t              vertices = 0;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        list.push_back(width == 4 ? big(*indices, i * 4) : (std::uint32_t((*indices)[i * 2]) << 8) | (*indices)[i * 2 + 1]);
        vertices = std::max(vertices, list.back() + 1);
    }
    const auto               registers = GuestDrawRegisters(*event.state);
    NativeDrawReplayViewport viewport;
    NativeDrawReplayScissor  scissor;
    std::string              error;
    if (std::size_t(vertices) * guestStride > stream->size() || !DecodeNativeGameplayViewportAndScissor(registers, viewport, scissor, error))
        return;
    draw.vertexData.assign(std::size_t(vertices) * hostStride, 0);
    for (std::uint32_t v = 0; v < vertices; ++v)
    {
        std::size_t from = std::size_t(v) * guestStride;
        for (std::size_t e = 0; e < elements.size(); ++e)
        {
            const auto to = std::size_t(v) * hostStride + e * 16;
            for (unsigned c = 0; c < elements[e]; ++c, from += 4)
                putFloat(draw.vertexData, to + c * 4, std::bit_cast<float>(big(*stream, from)));
            putFloat(draw.vertexData, to + 12, 1);
        }
    }
    draw.indices              = std::move(list);
    draw.indexCount           = count;
    draw.vertexAttributeCount = std::uint32_t(elements.size());
    draw.vertexStrideBytes    = hostStride;
    draw.vertexConstants.assign(256 * 16, 0);
    for (std::size_t i = 0; i < 1024; ++i)
        putFloat(draw.vertexConstants, i * 4, std::bit_cast<float>(registers[0x4000 + i]));
    // Pixel float4 n is register 0x4400 + 4n; a literal record's first float4
    // counts from the vertex bank's 256.
    draw.pixelConstants.assign(256 * 16, 0);
    for (std::size_t i = 0; i < 1024; ++i)
        putFloat(draw.pixelConstants, i * 4, std::bit_cast<float>(registers[0x4400 + i]));
    if (event.pixelLiterals)
        for (const auto& [first, words] : *event.pixelLiterals)
            for (std::size_t i = 0; i < words.size(); ++i)
                if (first >= 256 && (std::size_t(first - 256) * 4 + i) < 1024)
                    putFloat(draw.pixelConstants, (std::size_t(first - 256) * 4 + i) * 4, std::bit_cast<float>(words[i]));
    draw.sharedConstants.assign(336, 0);
    putFloat(draw.sharedConstants, 292, 1.0F / viewport.width);
    putFloat(draw.sharedConstants, 296, -1.0F / viewport.height);
    draw.viewport                         = viewport;
    const auto raster                     = registers[0x2205];
    draw.rasterizer.cull                  = (raster & 1) ? 1 : (raster & 2) ? 2
                                                                            : 0;
    draw.rasterizer.frontCounterClockwise = (raster & 4) == 0;
    draw.rasterizer.depthClip             = (registers[0x2204] & 0x10000) == 0;
    // The scene family's recipe carries the catalog's hash of the linked,
    // fetch-patched composite vertex shader, not the guest digest.
    draw.vertexShaderHash = composite ? kSceneCompositeVertexHash : std::stoull(event.pair.substr(9, 16), nullptr, 16);
    draw.pixelShaderHash  = std::stoull(event.pair.substr(26, 16), nullptr, 16);
}

// A frame report records refusals, swap verdict, producer resolves and guest
// rectangles.
std::string surfaceRegistryMode()
{
#if defined(_WIN32)
    char*       value  = nullptr;
    std::size_t length = 0;
    std::string mode;
    if (_dupenv_s(&value, &length, "REREVVED_TEST_SURFACE_REGISTRY") == 0 && value)
        mode = value;
    std::free(value);
    return mode;
#else
    const char* value = std::getenv("REREVVED_TEST_SURFACE_REGISTRY");
    return value ? value : "";
#endif
}

struct WorldReplay
{
    std::vector<std::string>                                                    refusals;
    std::set<std::size_t>                                                       refused;
    std::vector<std::tuple<NativeRenderFrameResolve, std::size_t, std::size_t>> resolves;
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::string>>          encodes;
    // Registry mode reports physical reads, owed overlaps, waits, live overlaps
    // and slot hits.
    std::map<std::string, std::array<std::uint64_t, 6>> reads;
    std::uint64_t                                       hitDraws = 0, hitDrawRangeReads = 0;
};

// World replay feeds draw, clear and copy hooks with a stand-in family; skip
// events are omitted.
WorldReplay replayWorldFrame(const std::vector<HarvestEvent>& events, const std::vector<std::string>& tokens, const std::set<std::size_t>& skip = {})
{
    WorldReplay                replay;
    FakeCompletion             completion;
    const HarvestEvent*        current = nullptr;
    std::string                token;
    NativeFrameAssemblerConfig config;
    NativeFrameAssembler*      built          = nullptr;
    std::uint64_t              surfaceQueries = 0;
    // Misses by kNativeAliasMissNames, then unmodeled: no entry holds the
    // fetch, or its frames were not all submitted.
    std::array<std::uint64_t, kNativeAliasMissNames.size() + 1> surfaceMisses{};
    config.completion = &completion;
    config.readTarget = [&current](std::uint32_t address, std::uint32_t& out)
    {
        return current && current->Read(address, out);
    };
    config.build = [&](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string& detail)
    {
        const auto  g = value.Graphics();
        GuestTarget target;
        // RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLOR_MASK and the window
        // scissor (G+0x2934, +0x2938, +0x28DC, +0x28C4, +0x28C8).
        std::uint32_t depthControl = 0, blendControl = 0, colorMask = 0, topLeft = 0, bottomRight = 0;
        if (!ReadGuestTargetSet(config.readTarget, g, 0, target) || target.native.extraColorTargets)
        {
            detail = "fixture_target_unsupported";
            return false;
        }
        if (!current->Read(g + 0x2934, depthControl) || !current->Read(g + 0x2938, blendControl) || !current->Read(g + 0x28DC, colorMask) ||
            !current->Read(g + 0x28C4, topLeft) || !current->Read(g + 0x28C8, bottomRight))
        {
            detail = "fixture_draw_state_unread";
            return false;
        }
        const auto& native = target.native;
        draw               = makeDraw(2);
        draw.width         = native.width;
        draw.height        = native.height;
        draw.sampleCount   = native.sampleCount;
        draw.sampleMask    = native.sampleCount == 4 ? 9 : 1;
        draw.targetFormat  = native.format;
        draw.viewport      = { 0, 0, float(native.width), float(native.height), 0, 1 };
        // A tiled draw runs once on the logical target; its scissor is per tile.
        draw.scissor         = target.tiling ? NativeDrawReplayScissor{ 0, 0, std::int32_t(native.width), std::int32_t(native.height) }
                                             : NativeDrawReplayScissor{ std::int32_t(topLeft & 0x7FFF), std::int32_t((topLeft >> 16) & 0x7FFF), std::int32_t(bottomRight & 0x7FFF), std::int32_t((bottomRight >> 16) & 0x7FFF) };
        draw.depth.enabled   = (depthControl & 2) != 0;
        draw.stencil.enabled = (depthControl & 1) != 0;
        draw.blend.writeMask = static_cast<std::uint8_t>(colorMask & 0xF);
        // 0x00010001 is ONE/ZERO for color and alpha; any other control blends.
        draw.blend.enabled = blendControl != 0x00010001;
        if (draw.blend.enabled)
        {
            draw.blend.sourceColor = draw.blend.sourceAlpha = NativeDrawReplayBlendFactor::SourceAlpha;
            draw.blend.destinationColor = draw.blend.destinationAlpha = NativeDrawReplayBlendFactor::InverseSourceAlpha;
        }
        sceneGeometry(*current, draw);
        // Registry binds contiguous texture hits from slot 0; frames import
        // stored surfaces.
        std::uint32_t hits = 0;
        for (std::uint32_t slot = 0; config.surfaces && slot < draw.textures.size(); ++slot)
        {
            std::array<std::uint32_t, 6> fetch{};
            bool                         read = true;
            for (std::size_t i = 0; i < fetch.size(); ++i)
                read = read && current->Read(g + 0x480 + slot * 24 + std::uint32_t(i) * 4, fetch[i]);
            if (!read || (fetch[0] & 3) != 2 || !(fetch[1] & ~0xFFFU))
                continue;
            ++surfaceQueries;
            auto miss = NativeResolvedTextureMiss::Form;
            if (const auto texture = built->ResolvedTexture(fetch, value.operationMark, miss))
            {
                hits |= 1U << slot;
                if (draw.textureMask == (1U << slot) - 1)
                {
                    draw.textureMask |= 1U << slot;
                    draw.textures[slot] = BindNativeResolvedTexture(*texture);
                }
            }
            else
                ++surfaceMisses[std::min<std::size_t>(static_cast<std::size_t>(miss), kNativeAliasMissNames.size())];
        }
        // The family reads a fetch slot's bytes only on a miss; vertex fetch
        // 16 it reads without asking. Streams and indices it always reads.
        if (config.surfaces)
        {
            bool          allHit = true;
            std::uint64_t ranged = 0;
            for (std::size_t row = 0; row < current->rows.size(); ++row)
            {
                if (!current->physical[row])
                    continue;
                const auto& role  = current->roles[row];
                const auto& bytes = current->rows[row].second;
                std::string site  = role.starts_with("stream") ? "vertex" : role == "indices" ? "index"
                                                                                              : "other";
                int         slot  = -1;
                if (role.starts_with("texture "))
                {
                    slot = std::stoi(role.substr(8));
                    site = slot == 16 ? "texture16" : "texture";
                }
                auto& counted = replay.reads[site];
                if (slot >= 0 && slot != 16 && (hits >> slot) & 1)
                {
                    ++counted[5];
                    continue;
                }
                allHit                = allHit && !(slot >= 0 && slot != 16);
                const auto address    = current->rows[row].first;
                const auto size       = static_cast<std::uint32_t>(bytes.size());
                const auto trace      = built->ProbeResolved(address, size);
                const bool registered = config.surfaces->Overlapping(address, std::uint64_t(address) + size).has_value();
                ++counted[0];
                counted[1] += size;
                counted[2] += trace.overlapped;
                counted[3] += trace.flushed;
                counted[4] += registered;
                ranged += trace.overlapped || registered;
            }
            if (allHit && hits)
            {
                ++replay.hitDraws;
                replay.hitDrawRangeReads += ranged;
            }
        }
        return true;
    };
    config.coverage = MakeSceneDrawCoverage([&current](std::uint64_t address, std::uint64_t size, bool)
                                            {
                                                std::uint32_t word = 0;
                                                if (size != 4 || !current || address > UINT32_MAX || !current->Read(static_cast<std::uint32_t>(address), word))
                                                    return std::vector<std::uint8_t>{};
                                                return std::vector<std::uint8_t>{ std::uint8_t(word >> 24), std::uint8_t(word >> 16), std::uint8_t(word >> 8), std::uint8_t(word) };
                                            });
    // Legacy resolve frames submit at swap; graph mode submits eager segments
    // so draws bind.
    if (const auto mode = surfaceRegistryMode(); mode == "1" || mode == "graph")
    {
        config.surfaces = std::make_shared<ResolveSurfaceRegistry>();
        if (mode == "graph")
            config.resolveGraph = config.eagerGraphSegments = config.castableGuestWords = true;
    }
    NativeFrameAssembler assembler(config);
    built = &assembler;
    // The harvest records no EndTiling (0x8269D320): it follows the tile
    // resolve loop (sub_8250AC90), so a bracket's tiles commit at the first
    // operation after its run of tile resolves.
    bool       tilesPending = false;
    const auto endTiling    = [&]
    {
        if (std::exchange(tilesPending, false) && config.surfaces)
            config.surfaces->CommitTiles();
    };
    // Runs of one refusal over consecutive events: first event, text, count.
    std::vector<std::tuple<std::size_t, std::string, unsigned>> runs;
    std::optional<GuestResolveCopy>                             scanned;
    std::uint64_t                                               sequence = 0;
    std::size_t                                                 last     = 0;
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        if (skip.contains(i))
            continue;
        const auto& event = events[i];
        current           = &event;
        token             = tokens[i];
        const auto read   = [&event](std::uint32_t address, std::uint32_t& out)
        {
            return event.Read(address, out);
        };
        if (event.kind != "resolve")
            endTiling();
        const auto refuse = [&](const std::string& what)
        {
            replay.refused.insert(i);
            // A draw is named by its key; a clear or resolve by its form.
            auto text = std::format("{}: {}", event.kind == "draw" ? token.substr(0, 18) : token.substr(0, token.find(" [")), what);
            if (!runs.empty() && last + 1 == i && std::get<1>(runs.back()) == text)
                ++std::get<2>(runs.back());
            else
                runs.emplace_back(i, std::move(text), 1);
            last = i;
        };
        std::string error;
        if (event.kind == "draw")
        {
            const auto hook = std::find(kNativeDrawHookAddresses.begin(), kNativeDrawHookAddresses.end(), event.entry);
            UNSCOPED_INFO(std::format("draw entry {:08X} is a hook", event.entry));
            REQUIRE(hook != kNativeDrawHookAddresses.end());
            NativeDrawSubmission value;
            value.hook          = static_cast<NativeDrawHook>(hook - kNativeDrawHookAddresses.begin());
            value.caller        = event.caller;
            value.stack         = event.stack;
            value.arguments     = event.r;
            value.stateRead     = true;
            value.sequence      = ++sequence;
            value.operationMark = assembler.NumberOperation();
            assembler.Began(value);
            // A harvested direct 0x826A3048 draw was followed by its commit.
            if (NativeDrawDefersToCommit(value.hook, value.caller))
                assembler.Committed(value);
            const auto verdict = assembler.Admit(value);
            assembler.Recorded(value, verdict, &error);
            if (verdict)
                refuse(error.empty() ? std::string(NativeDrawReasonName(*verdict)) : error);
        }
        else if (event.kind == "clear")
        {
            // As RecordGuestClear.
            GuestClear clear;
            if (!ReadGuestClear(read, event.r, event.stack, event.depth, clear, &error))
            {
                GuestTarget                target;
                std::optional<GuestTarget> decoded;
                if (ReadGuestTarget(read, event.r[0], event.r[2], target))
                    decoded = target;
                assembler.ClearUnreadable(decoded);
                refuse("clear_state_unreadable: " + error);
            }
            else if (!assembler.Clear(clear, error))
                refuse(error);
        }
        else if (event.kind == "resolve")
        {
            // As the resolve entry and copy hooks.
            GuestTarget      target;
            GuestResolveCopy copy;
            std::string      detail;
            if (!ReadGuestResolveEntry(read, event.r, target, copy, &detail) || !ReadGuestResolveCopy(read, event.r[0], copy))
            {
                assembler.ResolveUnmodeled(ReadGuestResolveCopy(read, event.r[0], copy) ? std::optional(copy) : std::nullopt);
                refuse("resolve_state_unreadable: " + detail);
                continue;
            }
            const bool    frontbuffer = assembler.ScanoutCandidate(target, copy);
            const bool    tiled       = !frontbuffer && target.tiling.has_value();
            const auto    flags       = event.r[1];
            std::uint32_t predicate = 0, tiling = 0;
            if (tiled)
                tilesPending = true;
            else
                endTiling();
            if (tiled && (flags & ~0x377U))
                error = std::format("resolve_flags_unsupported {:#x}", flags);
            else if (!frontbuffer && !tiled && flags)
                error = "resolve_flags_unsupported";
            else if (!frontbuffer && !tiled && (!read(event.r[0] + 0x31AC, predicate) || !read(event.r[0] + 0x2ABC, tiling) || predicate || ((tiling >> 24) & 0x30)))
                error = "resolve_predicate_unsupported";
            else
            {
                bool partial = false;
                if (assembler.Resolve(target, copy, frontbuffer, partial, error))
                {
                    if (frontbuffer)
                        scanned = copy;
                    continue;
                }
                refuse(error);
                continue;
            }
            assembler.ResolveUnmodeled(copy);
            refuse(error);
        }
        else
        {
            UNSCOPED_INFO("unknown harvest event kind " + event.kind);
            REQUIRE(event.kind == "begin_tiling");
        }
    }
    endTiling();
    current    = nullptr;
    auto value = makeSwap(1, sequence);
    if (scanned)
    {
        value.frontbufferPhysical = scanned->destBase;
        value.fetch[1]            = scanned->destBase | 6;
    }
    const auto result = assembler.Swap(value);
    if (config.surfaces)
    {
        std::string misses;
        for (std::size_t i = 0; i < surfaceMisses.size(); ++i)
            if (surfaceMisses[i])
                misses += std::format(" {}={}", i < kNativeAliasMissNames.size() ? kNativeAliasMissNames[i] : "unmodeled", surfaceMisses[i]);
        std::cout << "resolve surface registry: surface_queries=" << surfaceQueries << FormatResolveSurfaceCounts(config.surfaces->TakeCounts()) << " by reason:" << misses << '\n';
        for (const auto& [pair, count] : config.surfaces->FormPairs())
            std::cout << "resolve surface form miss: " << pair.Describe(kNativeAliasFormNames) << " misses=" << count << '\n';
        std::string reads;
        for (const auto& [site, counted] : replay.reads)
            reads += std::format(" {}={}/{:.1f}MB/images={}/flushes={}/surfaces={}/bound={}", site, counted[0], double(counted[1]) / 1e6, counted[2], counted[3], counted[4], counted[5]);
        std::cout << "resolve surface reads:" << reads << " hit_draws=" << replay.hitDraws << " hit_draw_range_reads=" << replay.hitDrawRangeReads << '\n';
    }
    for (const auto& [first, text, count] : runs)
        replay.refusals.push_back(count > 1 ? std::format("{} {} x{}", first, text, count) : std::format("{} {}", first, text));
    replay.refusals.push_back(std::format("swap {}: {}", NativeFrameStatusName(result.status), result.detail));
    for (const auto& span : completion.spans)
        if (span.frame)
            for (const auto& pass : span.frame->passes)
                for (const auto& resolve : pass.resolves)
                    replay.resolves.emplace_back(resolve, pass.clears.size(), pass.DrawCount());
    for (const auto& span : completion.spans)
        if (span.guestResolve)
        {
            const auto&                     copy = *span.guestResolve;
            const std::vector<std::uint8_t> texels(std::size_t(copy.rectWidth) * copy.rectHeight * 4);
            GuestResolveImage               image;
            std::string                     error;
            if (!EncodeGuestResolve(copy, texels, copy.rectWidth * 4, image, error))
            {
                UNSCOPED_INFO("a refused guest resolve names its refusal");
                REQUIRE(!error.empty());
            }
            replay.encodes.emplace_back(copy.rectWidth, copy.rectHeight, error);
        }
    return replay;
}

// Census packet A retains named refusals and admits K11, overwritten targets,
// k_16_16 and k_2_10_10_10 forms.
struct WorldFixtures
{
    // The fixtures each harvest wrote.
    std::vector<std::pair<std::string, std::vector<std::string>>> harvests;
    std::map<std::string, std::vector<std::string>>               refusals;
    // The resolves of each frame's overwritten sources once its refused
    // operations are dropped, its k_16_16 and k_2_10_10_10 resolves, and its
    // guest resolves with an unaligned edge that ends the destination.
    std::map<std::string, std::string> overwritten, packed, padded;
};

const WorldFixtures& worldFixtures()
{
    static const WorldFixtures fixtures{
        {
            { "native-world-harvest-20261014-0001", { "D7AA1B9B851613F9-0", "EF41C8E8315C919A-0", "71148455644BDC08-0", "DB1BA325C3E6B6B1-0", "A714B59BD23D0766-0", "0547FF4FD38763A7-0" } },
            { "native-world-harvest-20261015-0001", { "66753FC1304530CE-0", "536D6C34FA08452D-0", "FE027C74196DB003-0", "373D086E7E46A11B-0", "FADA316BAEBA5E33-0", "E3AA356DAD7A4AB9-0", "16FE8B2D8D174869-0", "BB479A40A832D6E1-0", "ADC945E2B490BD6A-0" } },
        },
        {
            { "D7AA1B9B851613F9-0",
              {
                  "swap provisional: ",
              } },
            { "EF41C8E8315C919A-0",
              {
                  "swap provisional: ",
              } },
            { "71148455644BDC08-0",
              {
                  "swap provisional: ",
              } },
            { "DB1BA325C3E6B6B1-0",
              {
                  "swap provisional: ",
              } },
            { "A714B59BD23D0766-0",
              {
                  "swap provisional: ",
              } },
            { "0547FF4FD38763A7-0",
              {
                  "swap provisional: ",
              } },
            // 20261015-0001: the Hall of Glory with its achievement panels (1031,
            // 1069, 1071) and without (1035), the late world (1737, 1771, 5127),
            // the spaceship screen (2643) and the load-game menu (1554).
            { "66753FC1304530CE-0",
              {
                  "swap provisional: ",
              } },
            { "536D6C34FA08452D-0",
              {
                  "swap provisional: ",
              } },
            { "FE027C74196DB003-0",
              {
                  "swap provisional: ",
              } },
            { "373D086E7E46A11B-0",
              {
                  "swap provisional: ",
              } },
            { "FADA316BAEBA5E33-0",
              {
                  "swap provisional: ",
              } },
            { "E3AA356DAD7A4AB9-0",
              {
                  "swap provisional: ",
              } },
            { "16FE8B2D8D174869-0",
              {
                  "swap provisional: ",
              } },
            { "BB479A40A832D6E1-0",
              {
                  "swap provisional: ",
              } },
            { "ADC945E2B490BD6A-0",
              {
                  "swap provisional: ",
              } },
        },
        // P20 and the fog composite (1X, sample 0), K15 and K16 (2X, k01), and
        // K17's two tiles and K20's copies (2X, k01).
        {
            { "D7AA1B9B851613F9-0", "1280x720 samples 0,0 x2; 320x180 samples 0,3 x1; 640x360 samples 0,3 x2; 640x720 from 640,0 samples 0,3 x1; 640x720 samples 0,3 x1" },
            { "EF41C8E8315C919A-0", "1280x720 samples 0,0 x2; 320x180 samples 0,3 x1; 640x360 samples 0,3 x2; 640x720 from 640,0 samples 0,3 x1; 640x720 samples 0,3 x1" },
            { "71148455644BDC08-0", "1280x720 samples 0,0 x1" },
            { "A714B59BD23D0766-0", "1280x720 samples 0,0 x2; 320x180 samples 0,3 x1; 640x360 samples 0,3 x4; 640x720 from 640,0 samples 0,3 x2; 640x720 samples 0,3 x2" },
            { "0547FF4FD38763A7-0", "1280x720 samples 0,0 x1; 640x360 samples 0,3 x2; 640x720 from 640,0 samples 0,3 x1; 640x720 samples 0,3 x1" },
            { "66753FC1304530CE-0", "1280x720 samples 0,0 x2; 320x180 samples 0,3 x1; 640x360 samples 0,3 x2; 640x720 from 640,0 samples 0,3 x1; 640x720 samples 0,3 x1" },
            { "536D6C34FA08452D-0", "1280x720 samples 0,0 x1" },
            { "FE027C74196DB003-0", "1280x720 samples 0,0 x1" },
            { "373D086E7E46A11B-0", "1280x720 samples 0,0 x1" },
            { "FADA316BAEBA5E33-0", "1280x720 samples 0,0 x1" },
            { "E3AA356DAD7A4AB9-0", "1280x720 samples 0,0 x1" },
            { "16FE8B2D8D174869-0", "1280x720 samples 0,0 x1; 640x360 samples 0,3 x2; 640x720 from 640,0 samples 0,3 x1; 640x720 samples 0,3 x1" },
            { "BB479A40A832D6E1-0", "1280x720 samples 0,0 x1" },
        },
        {
            { "D7AA1B9B851613F9-0", "k_16_16 1280x720 from 0,0 clears 1 draws 0 x2; k_2_10_10_10 640x720 from 0,0 clears 0 draws 1 x1; k_2_10_10_10 640x720 from 640,0 clears 0 draws 1 x1" },
            { "EF41C8E8315C919A-0", "k_16_16 1280x720 from 0,0 clears 1 draws 324 x1; k_16_16 1280x720 from 0,0 clears 1 draws 405 x1; k_2_10_10_10 640x720 from 0,0 clears 0 draws 1 x1; k_2_10_10_10 640x720 from 640,0 clears 0 draws 1 x1" },
            { "71148455644BDC08-0", "k_16_16 1280x720 from 0,0 clears 1 draws 344 x1; k_16_16 1280x720 from 0,0 clears 1 draws 430 x1" },
            { "A714B59BD23D0766-0", "k_16_16 1280x720 from 0,0 clears 1 draws 316 x1; k_16_16 1280x720 from 0,0 clears 1 draws 430 x1; k_2_10_10_10 640x360 from 0,0 clears 0 draws 1 x1; k_2_10_10_10 640x360 from 0,0 clears 0 draws 2 x1; k_2_10_10_10 640x720 from 0,0 clears 0 draws 1 x2; k_2_10_10_10 640x720 from 640,0 clears 0 draws 1 x2" },
            { "0547FF4FD38763A7-0", "k_16_16 1280x720 from 0,0 clears 1 draws 316 x1; k_16_16 1280x720 from 0,0 clears 1 draws 430 x1; k_2_10_10_10 640x360 from 0,0 clears 0 draws 1 x1; k_2_10_10_10 640x360 from 0,0 clears 0 draws 2 x1; k_2_10_10_10 640x720 from 0,0 clears 0 draws 1 x1; k_2_10_10_10 640x720 from 640,0 clears 0 draws 1 x1" },
            { "66753FC1304530CE-0", "k_16_16 1280x720 from 0,0 clears 1 draws 0 x2; k_2_10_10_10 640x720 from 0,0 clears 0 draws 1 x1; k_2_10_10_10 640x720 from 640,0 clears 0 draws 1 x1" },
            { "536D6C34FA08452D-0", "k_16_16 1280x720 from 0,0 clears 1 draws 0 x2" },
            { "FE027C74196DB003-0", "k_16_16 1280x720 from 0,0 clears 1 draws 0 x2" },
            { "373D086E7E46A11B-0", "k_16_16 1280x720 from 0,0 clears 1 draws 0 x2" },
            { "FADA316BAEBA5E33-0", "k_16_16 1280x720 from 0,0 clears 1 draws 380 x1; k_16_16 1280x720 from 0,0 clears 1 draws 475 x1" },
            { "E3AA356DAD7A4AB9-0", "k_16_16 1280x720 from 0,0 clears 1 draws 0 x1; k_16_16 1280x720 from 0,0 clears 1 draws 305 x1" },
            { "16FE8B2D8D174869-0", "k_16_16 1280x720 from 0,0 clears 1 draws 0 x1; k_16_16 1280x720 from 0,0 clears 1 draws 220 x1; k_2_10_10_10 640x360 from 0,0 clears 0 draws 1 x1; k_2_10_10_10 640x360 from 0,0 clears 0 draws 2 x1; k_2_10_10_10 640x720 from 0,0 clears 0 draws 1 x1; k_2_10_10_10 640x720 from 640,0 clears 0 draws 1 x1" },
        },
        // The 320x180 copies, widened by the SDK to 320x184, and the Hall of
        // Glory panels' 900x675 resolves, widened to 904x680, into their
        // destinations' padding.
        {
            { "D7AA1B9B851613F9-0", "320x180 x1" },
            { "EF41C8E8315C919A-0", "320x180 x1" },
            { "A714B59BD23D0766-0", "320x180 x1" },
            { "66753FC1304530CE-0", "320x180 x1; 900x675 x6" },
            { "536D6C34FA08452D-0", "900x675 x2" },
            { "FE027C74196DB003-0", "900x675 x4" },
        },
    };
    return fixtures;
}

// Named harvest fixtures replay; missing expectations are reported only.
int worldHarvestFrames(const std::filesystem::path& root, std::vector<std::string> names)
{
    const auto& fixtures = worldFixtures();
    const auto  present  = [&](const std::string& name)
    {
        return std::filesystem::exists(root / "frames" / name / "frame.toml");
    };
    if (names.empty())
    {
        for (const auto& [harvest, members] : fixtures.harvests)
        {
            const auto held = std::ranges::count_if(members, present);
            if (!held)
                continue;
            for (const auto& name : members)
            {
                UNSCOPED_INFO(std::format("{} holds {} of {} fixtures of {}; {} is missing", root.string(), held, members.size(), harvest, name));
                REQUIRE(present(name));
            }
            names.insert(names.end(), members.begin(), members.end());
        }
        UNSCOPED_INFO(root.string() + " holds no known fixture; name the fixtures to replay");
        REQUIRE(!names.empty());
    }
    for (const auto& name : names)
    {
        UNSCOPED_INFO(std::format("{} has no frames/{}/frame.toml", root.string(), name));
        REQUIRE(present(name));
        // Payloads are shared within one frame; dropping them between frames
        // bounds the replay's memory to one frame.
        PayloadCache             cache;
        const auto               expected = fixtures.refusals.find(name);
        const bool               pinned   = expected != fixtures.refusals.end() && surfaceRegistryMode() != "graph";
        const auto               file     = root / "frames" / name / "frame.toml";
        const auto               table    = toml::parse_file(file.string());
        const auto               events   = loadHarvestFrame(root, table, cache);
        std::vector<std::string> tokens;
        for (const auto& row : *table["events"].as_array())
            tokens.push_back(toml::node_view<const toml::node>(row)["token"].value_or(std::string{}));
        const auto  first  = replayWorldFrame(events, tokens);
        const auto& actual = first.refusals;
        std::string text;
        for (const auto& line : actual)
            text += "\n  " + line;
        std::cout << std::format("{}: {} events, refused:{}\n", name, events.size(), text);
        if (!pinned)
            std::cout << name << ": no expectation; reported only\n";
        UNSCOPED_INFO(name + ": the refused operations changed");
        REQUIRE((!pinned || actual == expected->second));

        // Without its refused operations the frame assembles, so each
        // admitted resolve reaches its producer frame. Those of overwritten
        // sources, whose pass has no clear, copy from the origin or a tile.
        const auto clean = replayWorldFrame(events, tokens, first.refused);
        std::cout << std::format("{}: without its refusals: {} refused, {}\n", name, clean.refused.size(), clean.refusals.back());
        UNSCOPED_INFO(name + ": the frame assembles without its refusals: " + clean.refusals.back());
        REQUIRE((!pinned || (clean.refused.empty() && !clean.refusals.back().starts_with("swap failed"))));
        std::map<std::string, unsigned> forms;
        for (const auto& [resolve, clears, draws] : clean.resolves)
            if (!clears)
            {
                UNSCOPED_INFO(name + ": an overwritten source resolves its draws");
                REQUIRE((!pinned || (draws && !resolve.depth)));
                const auto from = resolve.sourceX || resolve.sourceY ? std::format(" from {},{}", resolve.sourceX, resolve.sourceY) : std::string{};
                ++forms[std::format("{}x{}{} samples {},{}", resolve.width, resolve.height, from, resolve.samples[0], resolve.samples[1])];
            }
        std::string summary;
        for (const auto& [form, count] : forms)
            summary += std::format("{}{} x{}", summary.empty() ? "" : "; ", form, count);
        std::cout << std::format("{}: overwritten resolves: {}\n", name, summary);
        const auto wanted = fixtures.overwritten.find(name);
        UNSCOPED_INFO(name + ": the overwritten resolves changed");
        REQUIRE((!pinned || summary == (wanted == fixtures.overwritten.end() ? std::string{} : wanted->second)));

        // The k_16_16 and k_2_10_10_10 resolves, packed as the guest words.
        std::map<std::string, unsigned> packedForms;
        for (const auto& [resolve, clears, draws] : clean.resolves)
            if (resolve.guestWords == NativeGuestWordPacking::Float16x2 || resolve.guestWords == NativeGuestWordPacking::Color2101010)
                ++packedForms[std::format("{} {}x{} from {},{} clears {} draws {}", resolve.guestWords == NativeGuestWordPacking::Float16x2 ? "k_16_16" : "k_2_10_10_10", resolve.width, resolve.height, resolve.sourceX, resolve.sourceY, clears, draws)];
        std::string packedSummary;
        for (const auto& [form, count] : packedForms)
            packedSummary += std::format("{}{} x{}", packedSummary.empty() ? "" : "; ", form, count);
        std::cout << std::format("{}: packed resolves: {}\n", name, packedSummary);
        const auto packedWanted = fixtures.packed.find(name);
        UNSCOPED_INFO(name + ": the packed resolves changed");
        REQUIRE((!pinned || packedSummary == (packedWanted == fixtures.packed.end() ? std::string{} : packedWanted->second)));

        // Every guest resolve encodes; those with an unaligned edge leave
        // the SDK's widened texels to the destination padding.
        std::map<std::string, unsigned> paddedForms;
        for (const auto& [width, height, refusal] : clean.encodes)
        {
            // Pinned or not, an admitted resolve that cannot encode fails the replay.
            UNSCOPED_INFO(std::format("{}: a {}x{} guest resolve does not encode: {}", name, width, height, refusal));
            REQUIRE(refusal.empty());
            if ((width | height) & 7)
                ++paddedForms[std::format("{}x{}", width, height)];
        }
        std::string paddedSummary;
        for (const auto& [form, count] : paddedForms)
            paddedSummary += std::format("{}{} x{}", paddedSummary.empty() ? "" : "; ", form, count);
        std::cout << std::format("{}: {} guest resolves encode; unaligned edges: {}\n", name, clean.encodes.size(), paddedSummary);
        const auto paddedWanted = fixtures.padded.find(name);
        UNSCOPED_INFO(name + ": the unaligned-edge resolves changed");
        REQUIRE((!pinned || paddedSummary == (paddedWanted == fixtures.padded.end() ? std::string{} : paddedWanted->second)));
    }
    return 0;
}

// Pass 4 reads pass 3 depth before ledger; destination reads follow order.
void testResolvedReads()
{
    const auto image = [](std::uint8_t value, std::uint32_t offset, std::uint32_t length)
    {
        GuestResolveImage result;
        result.physical = 0x1D0F0000;
        result.bytes.assign(64, value);
        result.runs = { { offset, length } };
        return completion::GuestImageTake{ result, {}, false };
    };
    const auto memory = []
    {
        return std::vector<std::uint8_t>(80, 0xEE);
    };
    // Bytes [8, 24) of the read are depth tile 0's, [24, 56) tile 1's.
    const auto expect = [](std::uint8_t first, std::uint8_t second)
    {
        std::vector<std::uint8_t> bytes(80, 0xEE);
        std::fill(bytes.begin() + 8, bytes.begin() + 24, first);
        std::fill(bytes.begin() + 24, bytes.begin() + 56, second);
        return bytes;
    };
    FakeCompletion       completion;
    NativeFrameAssembler assembler(menuConfig(completion));
    std::string          error;
    bool                 partial = false;
    UNSCOPED_INFO("the pass clear: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    record(assembler, 1, kMenuScene[0]);
    std::array<std::uint64_t, 3>                                 marks{};
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> steps{ { { 0, 0x00100014 }, { 0, 0x00100340 }, { 1, 0x00100014 } } };
    for (std::size_t n = 0; n < steps.size(); ++n)
    {
        const auto [tile, control] = steps[n];
        marks[n]                   = assembler.Resolve(tiledSlot(tile, (control & 7)), textureCopy(tile, control), false, partial, error);
        UNSCOPED_INFO("the pass resolves: " + error);
        REQUIRE(marks[n] != 0);
    }
    UNSCOPED_INFO("no resolve frame is submitted before a read");
    REQUIRE(completion.spans.empty());
    // Guest memory under an unwritten image has no view; beside it, it does.
    const std::vector<std::uint8_t> guest(80, 0xEE);
    const auto                      view = [&]
    {
        return std::span<const std::uint8_t>(guest);
    };
    UNSCOPED_INFO("a range under a pending image has no view and submits nothing");
    REQUIRE((assembler.ViewResolved(0x1D0EFFF8, 80, view).empty() && completion.spans.empty()));
    UNSCOPED_INFO("a range beside the images is guest memory in place");
    REQUIRE(assembler.ViewResolved(0x1D000000, 4, view).size() == 80);
    completion.peeks[marks[0]] = image(0x11, 0, 32);
    completion.peeks[marks[2]] = image(0x22, 16, 32);
    std::vector<std::uint8_t> bytes;
    UNSCOPED_INFO("a read lays both depth tiles over memory, the later tile last: " + error);
    REQUIRE((assembler.ReadResolved(0x1D0EFFF8, 80, memory, bytes, error) && bytes == expect(0x11, 0x22)));
    UNSCOPED_INFO("the read submits the resolve frames and peeks only the depth destination's images");
    REQUIRE((completion.peeked == std::vector<std::uint64_t>{ marks[0], marks[2] } &&
             std::ranges::find(completion.spans, marks[2], &NativeCompletionSpan::resolveMark) != completion.spans.end()));
    UNSCOPED_INFO("a second read reuses the images: " + error);
    REQUIRE((assembler.ReadResolved(0x1D0EFFF8, 80, memory, bytes, error) && bytes == expect(0x11, 0x22) && completion.peeked.size() == 2));
    UNSCOPED_INFO("a read past the destination is guest memory: " + error);
    REQUIRE((assembler.ReadResolved(0x1D000000, 4, memory, bytes, error) && bytes == std::vector<std::uint8_t>(80, 0xEE)));

    // The ledger's take of tile 0 keeps it laid over memory until the
    // ledger reports it written; then memory serves tile 0, including any
    // guest write after it.
    completion.image = completion.peeks[marks[0]];
    UNSCOPED_INFO("the ledger takes depth tile 0");
    REQUIRE(assembler.TakeGuestImage(marks[0]).image.has_value());
    UNSCOPED_INFO("a taken image is kept until written: " + error);
    REQUIRE((assembler.ReadResolved(0x1D0EFFF8, 80, memory, bytes, error) && bytes == expect(0x11, 0x22)));
    assembler.GuestImageWritten(marks[0]);
    UNSCOPED_INFO("a written image leaves its bytes to guest memory: " + error);
    REQUIRE((assembler.ReadResolved(0x1D0EFFF8, 80, memory, bytes, error) && bytes == expect(0xEE, 0x22)));

    // An image the source cannot give names the resolve.
    assembler.DiscardGuestImagesThrough(marks[2]);
    UNSCOPED_INFO("the ledger's discard returns the destination to guest memory: " + error);
    REQUIRE((assembler.ReadResolved(0x1D0EFFF8, 80, memory, bytes, error) && bytes == expect(0xEE, 0xEE)));
    UNSCOPED_INFO("with no image left the range is viewed in place");
    REQUIRE(assembler.ViewResolved(0x1D0EFFF8, 80, view).size() == 80);
    const auto next = assembler.Resolve(tiledSlot(1, 0), textureCopy(1, 0x00100040), false, partial, error);
    UNSCOPED_INFO("the last tile resolves: " + error);
    REQUIRE(next != 0);
    auto       copy  = textureCopy(1, 0x00100040);
    const auto start = static_cast<std::uint32_t>(copy.destBase);
    UNSCOPED_INFO("a missing image fails the read by name: " + error);
    REQUIRE((!assembler.ReadResolved(start, 4, memory, bytes, error) && error == std::format("resolve_image_unavailable: op mark {}: fixture_peek_missing", next)));
}

// Pending-texture queries submit, wait and peek nothing; later reads agree.
void testResolvedTextureQuery()
{
    const auto memory = []
    {
        return std::vector<std::uint8_t>(80, 0xEE);
    };
    FakeCompletion       completion;
    NativeFrameAssembler assembler(menuConfig(completion));
    std::string          error;
    bool                 partial = false;
    UNSCOPED_INFO("the pass clear: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    record(assembler, 1, kMenuScene[0]);
    const auto mark = assembler.Resolve(tiledSlot(0, 4), textureCopy(0, 0x00100014), false, partial, error);
    UNSCOPED_INFO("the pass resolves: " + error);
    REQUIRE(mark != 0);
    GuestResolveImage image;
    image.physical = 0x1D0F0000;
    image.bytes.assign(64, 0x11);
    image.runs             = { { 0, 32 } };
    completion.peeks[mark] = completion::GuestImageTake{ image, {}, false };
    // A 2D fetch whose base is the resolve destination.
    const std::array<std::uint32_t, 6> fetch{ 2, 0x1D0F0000 | 6, 0x009FE4FF, 0, 0, 0 };
    auto                               miss = NativeResolvedTextureMiss::Form;
    UNSCOPED_INFO("the query names the unmodeled miss");
    REQUIRE((!assembler.ResolvedTexture(fetch, mark, miss) && miss == NativeResolvedTextureMiss::Unmodeled));
    UNSCOPED_INFO("for a draw still being built too");
    REQUIRE((!assembler.ResolvedTexture(fetch, 0, miss) && miss == NativeResolvedTextureMiss::Unmodeled));
    UNSCOPED_INFO("the query submits, peeks and waits for nothing");
    REQUIRE((completion.spans.empty() && completion.peekCount == 0 && completion.waits == 0));
    std::vector<std::uint8_t> bytes;
    auto                      expect = memory();
    std::fill(expect.begin() + 8, expect.begin() + 40, 0x11);
    UNSCOPED_INFO("a read after the query submits and lays the resolve as before: " + error);
    REQUIRE((assembler.ReadResolved(0x1D0EFFF8, 80, memory, bytes, error) && bytes == expect && completion.peeked == std::vector<std::uint64_t>{ mark }));
}

// A ledger take may remove the image while read waits; the read does not fail.
void testResolvedReadDuringTake()
{
    FakeCompletion       completion;
    NativeFrameAssembler assembler(menuConfig(completion));
    std::string          error;
    bool                 partial = false;
    UNSCOPED_INFO("the pass clear: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    record(assembler, 1, kMenuScene[0]);
    const auto mark = assembler.Resolve(tiledSlot(0, kGuestDepthSlot), textureCopy(0, 0x00100014), false, partial, error);
    UNSCOPED_INFO("the depth tile resolves: " + error);
    REQUIRE(mark != 0);
    GuestResolveImage taken;
    taken.physical   = 0x1D0F0000;
    taken.bytes      = std::vector<std::uint8_t>(16, 0x44);
    completion.image = { taken, {}, false };
    std::latch inside(1), release(1);
    completion.onTake = [&]
    {
        inside.count_down();
        release.wait();
    };
    std::thread ledger([&]
                       {
                           assembler.TakeGuestImage(mark);
                       });
    inside.wait();
    std::vector<std::uint8_t> bytes;
    std::string               readError;
    bool                      read = false;
    std::thread               reader([&]
                                     {
                           read = assembler.ReadResolved(0x1D0F0000, 16, []
                                                         {
                                                             return std::vector<std::uint8_t>(16, 0);
                                                         },
                                                         bytes,
                                                         readError);
                                     });
    // The peek finds no image, as when the take already removed it.
    while (completion.peekCount.load() == 0)
        std::this_thread::yield();
    release.count_down();
    ledger.join();
    reader.join();
    UNSCOPED_INFO("a read racing the ledger's take uses the taken image: " + readError);
    REQUIRE((read && bytes == taken.bytes));
}

// Snapshot reads retain guest bytes across retirement and later overwrite.
void testResolvedReadAfterRetire()
{
    FakeCompletion       completion;
    NativeFrameAssembler assembler(menuConfig(completion));
    std::string          error;
    bool                 partial = false;
    UNSCOPED_INFO("the pass clear: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    record(assembler, 1, kMenuScene[0]);
    const auto mark = assembler.Resolve(tiledSlot(0, kGuestDepthSlot), textureCopy(0, 0x00100014), false, partial, error);
    UNSCOPED_INFO("the depth tile resolves: " + error);
    REQUIRE(mark != 0);
    GuestResolveImage resolved;
    resolved.physical      = 0x1D0F0000;
    resolved.bytes         = std::vector<std::uint8_t>(16, 0x44);
    completion.peeks[mark] = { resolved, {}, false };
    completion.image       = completion.peeks[mark];
    std::vector<std::uint8_t> memory(16, 0), bytes;
    unsigned                  reads = 0;
    const auto                read  = [&]
    {
        if (reads++ == 0)
        {
            // Between the snapshot and the read: the ledger's write, its
            // retirement, then a synchronized guest write.
            UNSCOPED_INFO("the ledger takes the image");
            REQUIRE(assembler.TakeGuestImage(mark).image.has_value());
            memory = resolved.bytes;
            assembler.GuestImageWritten(mark);
            memory.assign(16, 0x99);
        }
        return memory;
    };
    const bool done = assembler.ReadResolved(0x1D0F0000, 16, read, bytes, error);
    UNSCOPED_INFO(std::format("a read after the image retired keeps the guest's overwrite ({} reads): {}", reads, error));
    REQUIRE((done && bytes == std::vector<std::uint8_t>(16, 0x99) && reads == 2));
}

// A large read with no unwritten image over it shares the bytes the last read
// of its range returned while guest memory still holds them, and returns the
// guest's bytes, never a held buffer changed, once they differ.
NativeFrameAssemblerConfig timedConfig(FakeCompletion& completion, std::size_t budget = std::size_t(256) << 20)
{
    auto config               = menuConfig(completion);
    config.timing             = true;
    config.mirroredReadBudget = budget;
    return config;
}

void testMirroredReads()
{
    FakeCompletion            completion;
    NativeFrameAssembler      assembler(timedConfig(completion));
    std::string               error;
    constexpr auto            kSize = NativeFrameAssembler::kMirroredReadBytes;
    std::vector<std::uint8_t> memory(kSize, 0x10);
    const auto                view = [&]
    {
        return std::span<const std::uint8_t>(memory);
    };
    const auto read = [&](std::uint32_t address, std::uint32_t size)
    {
        GuestBytes bytes;
        UNSCOPED_INFO("a mirrored read: " + error);
        REQUIRE(assembler.ReadResolved(address, size, [&]
                                       {
                                           return view().first(size);
                                       },
                                       bytes,
                                       error));
        return bytes;
    };
    const auto stats = [&](std::uint64_t hits, std::uint64_t misses)
    {
        const auto mirror = assembler.ReadMirror();
        return mirror.hits == hits && mirror.misses == misses && mirror.hitBytes == hits * kSize && mirror.missBytes == misses * kSize;
    };
    auto first = read(0x10000000, kSize);
    UNSCOPED_INFO("the first read copies guest memory");
    REQUIRE((first == memory && stats(0, 1)));
    auto again = read(0x10000000, kSize);
    UNSCOPED_INFO("an unchanged range shares the last read's bytes");
    REQUIRE((again == memory && again.SharesBuffer(first) && stats(1, 1)));
    memory.back() = 0x20;
    auto changed  = read(0x10000000, kSize);
    UNSCOPED_INFO("a guest write is read, and held bytes keep their values");
    REQUIRE((changed == memory && !changed.SharesBuffer(first) && first[kSize - 1] == 0x10 && again[kSize - 1] == 0x10 && stats(1, 2)));
    const auto* kept = changed.data();
    first.clear();
    again.clear();
    changed.clear();
    memory.front()       = 0x30;
    const auto refreshed = read(0x10000000, kSize);
    UNSCOPED_INFO("bytes no reader holds refresh in place");
    REQUIRE((refreshed == memory && refreshed.data() == kept && stats(1, 3)));
    UNSCOPED_INFO("another address is another range");
    REQUIRE((!read(0x10100000, kSize).SharesBuffer(refreshed) && stats(1, 4)));
    const auto small = read(0x10000000, 64), smallAgain = read(0x10000000, 64);
    UNSCOPED_INFO("a read below the mirrored size is a plain copy");
    REQUIRE((small == std::vector<std::uint8_t>(memory.begin(), memory.begin() + 64) && !small.SharesBuffer(smallAgain) && stats(1, 4)));
    GuestBytes failed;
    UNSCOPED_INFO("an unreadable range fails by name: " + error);
    REQUIRE((!assembler.ReadResolved(0x10000000, kSize, []() -> std::span<const std::uint8_t>
                                     {
                                         throw std::runtime_error("guest_memory_uncommitted");
                                     },
                                     failed,
                                     error) &&
             error == "resolve_image_unavailable: guest_memory_uncommitted"));

    // An unwritten resolve image over the range is laid over guest memory
    // and never mirrored.
    bool partial = false;
    UNSCOPED_INFO("the pass clear: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    record(assembler, 1, kMenuScene[0]);
    const auto mark = assembler.Resolve(tiledSlot(0, kGuestDepthSlot), textureCopy(0, 0x00100014), false, partial, error);
    UNSCOPED_INFO("the depth tile resolves: " + error);
    REQUIRE(mark != 0);
    GuestResolveImage resolved;
    resolved.physical      = 0x1D0F0000;
    resolved.bytes         = std::vector<std::uint8_t>(16, 0x44);
    completion.peeks[mark] = { resolved, {}, false };
    std::ranges::fill(memory, 0xEE);
    for (int n = 0; n < 2; ++n)
    {
        const auto overlaid = read(0x1D0F0000 - 8, kSize);
        UNSCOPED_INFO("an unwritten image is laid over a large read");
        REQUIRE((overlaid.size() == kSize && overlaid[7] == 0xEE && overlaid[8] == 0x44 && overlaid[23] == 0x44 && overlaid[24] == 0xEE && stats(1, 4)));
    }
    assembler.GuestImageWritten(mark);
    const auto written = read(0x1D0F0000 - 8, kSize), writtenAgain = read(0x1D0F0000 - 8, kSize);
    UNSCOPED_INFO("a written image leaves the range to the mirror");
    REQUIRE((written == memory && writtenAgain.SharesBuffer(written) && stats(2, 5)));
}

// Held mirrors stay charged; a read that cannot fit uses a plain copy.
void testMirroredReadBudget()
{
    constexpr auto            kSize = NativeFrameAssembler::kMirroredReadBytes;
    FakeCompletion            completion;
    NativeFrameAssembler      assembler(timedConfig(completion, kSize * 3));
    std::string               error;
    std::vector<std::uint8_t> memory(kSize, 0x10);
    const auto                read = [&](NativeFrameAssembler& reader, std::uint32_t address)
    {
        GuestBytes bytes;
        UNSCOPED_INFO("a mirrored read: " + error);
        REQUIRE(reader.ReadResolved(address, kSize, [&]
                                    {
                                        return std::span<const std::uint8_t>(memory);
                                    },
                                    bytes,
                                    error));
        return bytes;
    };
    const auto retained = [&]
    {
        return assembler.ReadMirror().retainedBytes;
    };
    std::vector<GuestBytes> held;
    for (std::uint32_t n = 0; n < 3; ++n)
        held.push_back(read(assembler, 0x10000000 + n * 0x00200000));
    UNSCOPED_INFO("three held reads fill the budget");
    REQUIRE(retained() == kSize * 3);
    const auto over = read(assembler, 0x10800000), overAgain = read(assembler, 0x10800000);
    UNSCOPED_INFO("a read past the held budget is a plain copy and nothing more is retained");
    REQUIRE((over == memory && !over.SharesBuffer(overAgain) && retained() == kSize * 3));
    held.erase(held.begin());
    UNSCOPED_INFO("a dropped holder releases its buffer");
    REQUIRE(retained() == kSize * 2);
    const auto fitted = read(assembler, 0x10800000);
    UNSCOPED_INFO("the freed room mirrors the next read");
    REQUIRE((read(assembler, 0x10800000).SharesBuffer(fitted) && retained() == kSize * 3));
    held.clear();

    // Unheld buffers leave least recently read first.
    NativeFrameAssembler evicting(timedConfig(completion, kSize * 3));
    for (std::uint32_t n = 0; n < 3; ++n)
        read(evicting, 0x10000000 + n * 0x00200000);
    const auto kept = read(evicting, 0x10200000);
    read(evicting, 0x10600000);
    UNSCOPED_INFO("eviction makes room within the budget");
    REQUIRE(evicting.ReadMirror().retainedBytes == kSize * 3);
    UNSCOPED_INFO("a recently read range stays mirrored");
    REQUIRE(read(evicting, 0x10200000).SharesBuffer(kept));
    const auto refetched = read(evicting, 0x10000000);
    UNSCOPED_INFO("the least recently read range was evicted");
    REQUIRE((!refetched.SharesBuffer(kept) && evicting.ReadMirror().misses == 5 && evicting.ReadMirror().hits == 2 && evicting.ReadMirror().retainedBytes == kSize * 3));

    NativeFrameAssembler quiet(menuConfig(completion));
    const auto           first = read(quiet, 0x10000000);
    memory.back()              = 0x20;
    const auto second = read(quiet, 0x10000000), third = read(quiet, 0x10000000);
    const auto stats = quiet.ReadMirror();
    UNSCOPED_INFO("without timing the mirror counts no reads");
    REQUIRE((third.SharesBuffer(second) && !second.SharesBuffer(first) && stats.hits == 0 && stats.misses == 0 && stats.hitBytes == 0 && stats.missBytes == 0 &&
             stats.retainedBytes == kSize * 2));
}

// watchMirror re-arms after writes; each write doubles equal-compare threshold.
void testWatchedMirrorReads()
{
    // Guest/host writes notify; races and uncovered ranges still compare.
    constexpr auto            kSize = NativeFrameAssembler::kMirroredReadBytes;
    constexpr std::uint32_t   kBase = 0x1D000000;
    FakeCompletion            completion;
    auto                      config  = timedConfig(completion);
    bool                      covered = true;
    std::vector<std::uint8_t> memory(kSize, 0x10);
    std::uint32_t             arms = 0;
    std::function<void()>     duringArm;
    config.watchMirror = [&](std::uint32_t physical, std::uint32_t length)
    {
        UNSCOPED_INFO("the read's own range is armed");
        REQUIRE((physical == kBase && length == kSize));
        ++arms;
        if (duringArm)
            std::exchange(duringArm, nullptr)();
        return covered;
    };
    NativeFrameAssembler assembler(std::move(config));
    std::string          error;
    const auto           read = [&]
    {
        GuestBytes bytes;
        UNSCOPED_INFO("a watched read: " + error);
        REQUIRE(assembler.ReadResolved(kBase, kSize, [&]
                                       {
                                           return std::span<const std::uint8_t>(memory);
                                       },
                                       bytes,
                                       error));
        return bytes;
    };
    const auto watchedHits = [&]
    {
        return assembler.ReadMirror().watchedHits;
    };

    const auto first = read(), compared = read();
    UNSCOPED_INFO("an equal compare arms the range");
    REQUIRE((compared.SharesBuffer(first) && arms == 1 && watchedHits() == 0));
    memory[5] = 0x11; // unreported, as no real writer can be
    UNSCOPED_INFO("a watched range skips the compare");
    REQUIRE((read().SharesBuffer(first) && watchedHits() == 1 && arms == 1));

    assembler.GuestWrote(kBase + 4096, 4);
    auto seen = read();
    UNSCOPED_INFO("a guest store ends the watch");
    REQUIRE((seen[5] == 0x11 && !seen.SharesBuffer(first) && arms == 1));
    seen = read();
    UNSCOPED_INFO("a written watched range is not armed on its next equal compare");
    REQUIRE((arms == 1 && watchedHits() == 1));
    seen = read();
    UNSCOPED_INFO("it is armed again after two");
    REQUIRE((arms == 2 && read().SharesBuffer(seen) && watchedHits() == 2));

    memory[6] = 0x12;
    assembler.HostWriting(kBase + kSize - 4, 4);
    assembler.HostWrote(kBase + kSize - 4, 4);
    seen = read();
    UNSCOPED_INFO("a ledger word write ends the watch");
    REQUIRE((seen[6] == 0x12 && watchedHits() == 2));
    for (int n = 0; n < 4; ++n)
        read();
    UNSCOPED_INFO("the backoff doubles again");
    REQUIRE((arms == 3 && watchedHits() == 2));
    assembler.GuestWrote(kBase, 4);
    // A store of the same bytes between the arm and the compare.
    duringArm = [&]
    {
        assembler.GuestWrote(kBase, 4);
    };
    const auto armed = arms;
    for (int n = 0; n < 8 && arms == armed; ++n)
        read();
    UNSCOPED_INFO("the range is armed once more");
    REQUIRE((arms == armed + 1 && !duringArm));
    const auto racedHits = watchedHits();
    read();
    UNSCOPED_INFO("a store racing the arm leaves the range unwatched");
    REQUIRE((watchedHits() == racedHits && arms == armed + 1));

    covered = false;
    for (int n = 0; n < 16; ++n)
        read();
    UNSCOPED_INFO("an uncovered range keeps comparing");
    REQUIRE((watchedHits() == racedHits && arms > armed + 1));
    covered = true;
    for (int n = 0; n < 64 && watchedHits() == racedHits; ++n)
        read();
    UNSCOPED_INFO("a covered range is watched again");
    REQUIRE(watchedHits() > racedHits);

    // A resolve image is written while it is pending, so reads overlay it;
    // once the ledger reports it, a watched read over it compares again.
    bool partial = false;
    UNSCOPED_INFO("the pass clear: " + error);
    REQUIRE(assembler.Clear(tiledClear(), error));
    record(assembler, 1, kMenuScene[0]);
    const auto mark = assembler.Resolve(tiledSlot(0, kGuestDepthSlot), textureCopy(0, 0x00100014), false, partial, error);
    UNSCOPED_INFO("the depth tile resolves: " + error);
    REQUIRE(mark != 0);
    GuestResolveImage resolved;
    resolved.physical      = 0x1D0F0000;
    resolved.bytes         = std::vector<std::uint8_t>(16, 0x44);
    completion.peeks[mark] = { resolved, {}, false };
    const auto hitsBefore  = watchedHits();
    UNSCOPED_INFO("a pending image is overlaid, not watched");
    REQUIRE((read()[0xF0008] == 0x44 && watchedHits() == hitsBefore));
    std::fill_n(memory.begin() + 0xF0000, 16, 0x44);
    assembler.GuestImageWritten(mark);
    seen = read();
    UNSCOPED_INFO("a written image ends the watch");
    REQUIRE((seen[0xF0008] == 0x44 && watchedHits() == hitsBefore));
}

// Timed plain reads count by size; mirrored and untimed reads count nothing.
void testSmallReadCounts()
{
    constexpr auto            kSize = NativeFrameAssembler::kMirroredReadBytes;
    FakeCompletion            completion;
    std::vector<std::uint8_t> memory(kSize, 0x10);
    std::string               error;
    const auto                read = [&](NativeFrameAssembler& reader, std::uint32_t size)
    {
        GuestBytes bytes;
        UNSCOPED_INFO("a small read: " + error);
        REQUIRE((reader.ReadResolved(0x10000000, size, [&]
                                     {
                                         return std::span<const std::uint8_t>(memory).first(size);
                                     },
                                     bytes,
                                     error) &&
                 bytes.size() == size));
    };
    constexpr std::array<std::uint32_t, 9> kSizes{ 64, 4095, 4096, 65535, 65536, 262143, 262144, kSize - 1, kSize };
    // Mirroring from 1 MiB leaves every class's reads plain.
    auto timedConfigFrom1M                 = timedConfig(completion);
    timedConfigFrom1M.mirroredReadMinBytes = kSize;
    NativeFrameAssembler timed(std::move(timedConfigFrom1M));
    for (const auto size : kSizes)
        read(timed, size);
    const auto stats = timed.ReadMirror();
    UNSCOPED_INFO("each plain read counts once in its size class with its bytes");
    REQUIRE((stats.smallReads == std::array<std::uint64_t, 4>{ 2, 2, 2, 2 } &&
             stats.smallBytes == std::array<std::uint64_t, 4>{ 64 + 4095, 4096 + 65535, 65536 + 262143, 262144 + kSize - 1 } && stats.misses == 1));
    NativeFrameAssembler quiet(menuConfig(completion));
    for (const auto size : kSizes)
        read(quiet, size);
    const auto none = quiet.ReadMirror();
    UNSCOPED_INFO("without timing no small read counts");
    REQUIRE((none.smallReads == std::array<std::uint64_t, 4>{} && none.smallBytes == std::array<std::uint64_t, 4>{}));
}

// Lowering the threshold mirrors small reads; default below 64 KiB copies.
void testSmallMirroredReads()
{
    constexpr std::uint32_t   kSmall = 4096, kLarge = NativeFrameAssembler::kMirroredReadBytes, kBase = 0x1D000000;
    FakeCompletion            completion;
    std::vector<std::uint8_t> memory(kLarge * 2, 0x10);
    std::string               error;
    unsigned                  views = 0;
    const auto                read  = [&](NativeFrameAssembler& reader, std::uint32_t address, std::uint32_t size)
    {
        GuestBytes bytes;
        const auto before = views;
        UNSCOPED_INFO("a read: " + error);
        REQUIRE(reader.ReadResolved(address, size, [&]
                                    {
                                        ++views;
                                        return std::span<const std::uint8_t>(memory).subspan(address - kBase, size);
                                    },
                                    bytes,
                                    error));
        const auto from = memory.begin() + (address - kBase);
        UNSCOPED_INFO("a read views guest memory once and returns what a copy would");
        REQUIRE((views == before + 1 && bytes == std::vector<std::uint8_t>(from, from + size)));
        return bytes;
    };
    auto          config        = timedConfig(completion);
    std::uint32_t arms          = 0;
    config.mirroredReadMinBytes = kSmall;
    config.watchMirror          = [&](std::uint32_t, std::uint32_t)
    {
        ++arms;
        return true;
    };
    NativeFrameAssembler assembler(std::move(config));
    const auto           first = read(assembler, kBase, kSmall), compared = read(assembler, kBase, kSmall);
    auto                 stats = assembler.ReadMirror();
    UNSCOPED_INFO("an equal 4 KiB compare shares the bytes and arms the range");
    REQUIRE((compared.SharesBuffer(first) && arms == 1 && stats.hits == 1 && stats.misses == 1 && stats.watchedHits == 0));
    const auto watched = read(assembler, kBase, kSmall);
    stats              = assembler.ReadMirror();
    UNSCOPED_INFO("a repeat read of the watched range skips the compare");
    REQUIRE((watched.SharesBuffer(first) && stats.watchedHits == 1 && stats.watchedHitBytes == kSmall && stats.hits == 2));
    memory[8] = 0x11;
    assembler.GuestWrote(kBase + 8, 1);
    const auto written = read(assembler, kBase, kSmall);
    stats              = assembler.ReadMirror();
    UNSCOPED_INFO("a reported write forces a compare and leaves held bytes unchanged");
    REQUIRE((!written.SharesBuffer(first) && first[8] == 0x10 && stats.watchedHits == 1 && stats.misses == 2));
    read(assembler, kBase + kSmall, 64);
    stats = assembler.ReadMirror();
    UNSCOPED_INFO("only a read below the lowered size counts as a plain small read");
    REQUIRE((stats.smallReads == std::array<std::uint64_t, 4>{ 1, 0, 0, 0 } && stats.smallBytes == std::array<std::uint64_t, 4>{ 64, 0, 0, 0 }));

    // A write near the end of the longest entry ends its watch while small
    // entries follow it.
    const auto large = read(assembler, kBase + kLarge, kLarge);
    read(assembler, kBase + kLarge, kLarge);
    read(assembler, kBase + kLarge * 2 - kSmall, kSmall);
    const auto largeHits = assembler.ReadMirror().watchedHits;
    UNSCOPED_INFO("the large range is watched");
    REQUIRE((read(assembler, kBase + kLarge, kLarge).SharesBuffer(large) && assembler.ReadMirror().watchedHits == largeHits + 1));
    memory[kLarge * 2 - kSmall - 2] = 0x33;
    assembler.GuestWrote(kBase + kLarge * 2 - kSmall - 2, 1);
    UNSCOPED_INFO("a write at its end ends its watch");
    REQUIRE((!read(assembler, kBase + kLarge, kLarge).SharesBuffer(large) && assembler.ReadMirror().watchedHits == largeHits + 1));

    // Small entries count against the budget, and a held buffer is never
    // refreshed in place.
    auto budget                 = timedConfig(completion, kSmall * 3);
    budget.mirroredReadMinBytes = kSmall;
    NativeFrameAssembler    budgeted(std::move(budget));
    std::vector<GuestBytes> held;
    for (std::uint32_t n = 0; n < 3; ++n)
        held.push_back(read(budgeted, kBase + n * kSmall, kSmall));
    UNSCOPED_INFO("three held small reads fill the budget");
    REQUIRE(budgeted.ReadMirror().retainedBytes == kSmall * 3);
    memory[0]           = 0x44;
    const auto replaced = read(budgeted, kBase, kSmall);
    UNSCOPED_INFO("a held buffer keeps its bytes when its range changes");
    REQUIRE((!replaced.SharesBuffer(held[0]) && held[0][0] == 0x10 && budgeted.ReadMirror().retainedBytes == kSmall * 3));
    const auto over = read(budgeted, kBase + kSmall * 4, kSmall);
    UNSCOPED_INFO("a small read past the held budget is a plain copy");
    REQUIRE((!read(budgeted, kBase + kSmall * 4, kSmall).SharesBuffer(over) && budgeted.ReadMirror().retainedBytes == kSmall * 3));

    // A read that evicts several unheld entries takes the least recently read.
    auto lruConfig                 = timedConfig(completion, kSmall * 4);
    lruConfig.mirroredReadMinBytes = kSmall;
    NativeFrameAssembler lru(std::move(lruConfig));
    for (const std::uint32_t n : { 0u, 1u, 2u, 3u, 0u })
        read(lru, kBase + n * kSmall, kSmall);
    read(lru, kBase + kSmall * 8, kSmall * 2);
    read(lru, kBase, kSmall);
    read(lru, kBase + kSmall * 3, kSmall);
    stats = lru.ReadMirror();
    UNSCOPED_INFO("a two-entry eviction keeps the recently read ranges");
    REQUIRE((stats.hits == 3 && stats.misses == 5 && stats.retainedBytes == kSmall * 4));
    read(lru, kBase + kSmall, kSmall);
    UNSCOPED_INFO("and evicts the two oldest");
    REQUIRE(lru.ReadMirror().misses == 6);

    NativeFrameAssembler plain(timedConfig(completion));
    UNSCOPED_INFO("the default mirrors from 64 KiB");
    REQUIRE(menuConfig(completion).mirroredReadMinBytes == std::size_t(64) << 10);
    for (const std::uint32_t size : { 64u, kSmall, 0xFFFFu })
    {
        UNSCOPED_INFO("by default a read below 64 KiB is a plain copy");
        REQUIRE(!read(plain, kBase, size).SharesBuffer(read(plain, kBase, size)));
    }
    stats = plain.ReadMirror();
    UNSCOPED_INFO("by default nothing below 64 KiB is mirrored");
    REQUIRE((stats.retainedBytes == 0 && stats.hits == 0 && stats.misses == 0 && stats.smallReads == std::array<std::uint64_t, 4>{ 2, 4, 0, 0 }));
    const auto mirrored = read(plain, kBase, 0x10000);
    UNSCOPED_INFO("and a 64 KiB read is");
    REQUIRE((read(plain, kBase, 0x10000).SharesBuffer(mirrored) && plain.ReadMirror().hits == 1));
}

// HostWriting-to-HostWrote ranges cannot gain a watch from equal compares.
void testWatchedMirrorHostWriteInFlight()
{
    constexpr auto            kSize = NativeFrameAssembler::kMirroredReadBytes;
    constexpr std::uint32_t   kBase = 0x1D000000;
    FakeCompletion            completion;
    auto                      config = timedConfig(completion);
    std::vector<std::uint8_t> memory(kSize, 0x20);
    config.watchMirror = [](std::uint32_t, std::uint32_t)
    {
        return true;
    };
    NativeFrameAssembler assembler(std::move(config));
    std::string          error;
    const auto           read = [&]
    {
        GuestBytes bytes;
        UNSCOPED_INFO("a watched read: " + error);
        REQUIRE(assembler.ReadResolved(kBase, kSize, [&]
                                       {
                                           return std::span<const std::uint8_t>(memory);
                                       },
                                       bytes,
                                       error));
        return bytes;
    };
    const auto watchedHits = [&]
    {
        return assembler.ReadMirror().watchedHits;
    };
    read();
    read();
    read();
    UNSCOPED_INFO("the range is watched");
    REQUIRE(watchedHits() == 1);
    assembler.HostWriting(kBase + 64, 4);
    for (int n = 0; n < 16; ++n)
        read();
    UNSCOPED_INFO("a write in flight keeps every read comparing");
    REQUIRE(watchedHits() == 1);
    memory[64] = 0x21;
    assembler.HostWrote(kBase + 64, 4);
    UNSCOPED_INFO("the finished write is read");
    REQUIRE(read()[64] == 0x21);
    for (int n = 0; n < 16 && watchedHits() == 1; ++n)
        read();
    UNSCOPED_INFO("the range is watched again after the write");
    REQUIRE(watchedHits() == 2);
}

// Unwritten resolves never drop; the limit refuses until a write makes room.
void testUnwrittenResolveLimit()
{
    FakeCompletion                                                   completion;
    NativeFrameAssembler                                             assembler(menuConfig(completion));
    std::string                                                      error;
    std::vector<std::uint64_t>                                       marks;
    constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> kSteps{ { { 0, 0x00100014 }, { 0, 0x00100340 }, { 1, 0x00100014 }, { 1, 0x00100040 } } };
    const auto                                                       pass = [&](std::uint64_t sequence)
    {
        UNSCOPED_INFO("the pass clear: " + error);
        REQUIRE(assembler.Clear(tiledClear(), error));
        record(assembler, sequence, kMenuScene[0]);
        for (const auto [tile, control] : kSteps)
        {
            bool partial = false;
            marks.push_back(assembler.Resolve(tiledSlot(tile, control & 7), textureCopy(tile, control), false, partial, error));
            if (!marks.back())
                return false;
        }
        return true;
    };
    for (std::uint64_t n = 0; n < 64; ++n)
    {
        UNSCOPED_INFO(std::format("pass {} resolves: {}", n, error));
        REQUIRE(pass(n + 1));
    }
    UNSCOPED_INFO("the 257th unwritten resolve fails by name: " + error);
    REQUIRE((!pass(65) && refusedAs(error, "resolve_images_unwritten: 256 off-screen resolves await the ledger")));
    for (std::size_t n = 0; n < kSteps.size(); ++n)
        assembler.GuestImageWritten(marks[n]);
    UNSCOPED_INFO("written images make room: " + error);
    REQUIRE(pass(66));
}

} // namespace

// Off-screen clear invalidates presented storage; full clear restores it.
void testPresentedAlias()
{
    enum Variant
    {
        SameBase,
        OverlappingRange,
        DisjointRange,
        PresentedDepth,
        // An 80x16 clear of a surface whose allocation, not its tiles touched,
        // overlaps the presented depth; without a pitch the whole range counts.
        SmallRect,
        PitchlessRect,
        // A full presented clear of both planes, of stencil alone or of depth
        // alone after the aliasing depth clear; a draw that uses neither plane.
        DepthRedefined,
        StencilRedefined,
        DepthPlaneRedefined,
        DepthlessDraw,
        ClearedAgain,
        Count
    };

    for (int variant = 0; variant < Count; ++variant)
    {
        FakeCompletion       completion;
        NativeFrameAssembler assembler(tiledConfig(completion));
        std::string          error;
        bool                 partial = false;
        auto                 main    = tiledClear();
        // The tile surface and its depth, as a decoded 640x720 2X pitch gives them.
        main.target.ranges[0] = { 0, 720 };
        main.depth->range     = { 0x530, 0x530 + 720 };
        if (variant == ClearedAgain)
        {
            // Before the presented clear, the aliasing clear loses nothing.
            const bool aliasing = assembler.Clear(fullClear(offscreen(0)), error) != 0;
            UNSCOPED_INFO("the aliasing clear: " + error);
            REQUIRE(aliasing);
        }
        UNSCOPED_INFO("the tiled clear: " + error);
        REQUIRE(assembler.Clear(main, error));
        record(assembler, 1, 5);
        auto clear = fullClear(offscreen(variant == SameBase ? 0 : variant == OverlappingRange ? 0x100
                                                                                               : 0x300));
        if (variant == OverlappingRange)
            clear.target.ranges[0] = { 0x100, 0x200 };
        else if (variant != SameBase)
            clear.target.ranges[0] = { 0x300, 0x400 };
        const bool depthVariant = variant >= PresentedDepth && variant < ClearedAgain;
        if (depthVariant)
        {
            const bool small   = variant == SmallRect || variant == PitchlessRect;
            const auto base    = small ? 0x400U : 0x530U;
            clear.flags        = 0x11;
            clear.clear.depth  = true;
            clear.depth        = GuestDepthSurface{ 0x00700000, base, base, 1024, 1024, 1 };
            clear.depth->range = small ? GuestEdramRange{ 0x400, 0x740 } : GuestEdramRange{ 0x530, 0x630 };
            if (small)
            {
                // A 1024-sample pitch: 13 tiles a row, so 64 rows fill 832 tiles.
                clear.clear.rectangle = { 0, 0, 80, 16 };
                clear.depth->pitch = clear.target.surfacePitch = variant == SmallRect ? 1024 : 0;
            }
        }
        UNSCOPED_INFO("the off-screen clear: " + error);
        REQUIRE(assembler.Clear(clear, error));
        const bool aliased = variant == SameBase || variant == OverlappingRange;
        const bool tile0   = assembler.Resolve(tiledTarget(0), tileCopy(0, 0x00100340), true, partial, error) != 0;
        UNSCOPED_INFO(std::format("variant {}: the scanout {}: {}", variant, aliased ? "is refused" : "is admitted", error));
        REQUIRE((tile0 != aliased && (!aliased || error.starts_with("presented_edram_aliased: off-screen clear "))));
        if (aliased)
            continue;
        if (depthVariant)
        {
            // The presented color stays defined; its depth does not.
            UNSCOPED_INFO("tile 1: " + error);
            REQUIRE(assembler.Resolve(tiledTarget(1), tileCopy(1, 0x00100040), true, partial, error));
            const auto result = assembler.Swap(makeSwap(1, 1));
            UNSCOPED_INFO("the presented color presents: " + result.detail);
            REQUIRE(result.status == NativeFrameStatus::Provisional);
            FakeCompletion depthCompletion;
            auto           config = tiledConfig(depthCompletion);
            config.readTarget     = wordReader(unpredicated(tiledWords()));
            NativeFrameAssembler depthAssembler(config);
            UNSCOPED_INFO("the clears: " + error);
            REQUIRE((depthAssembler.Clear(main, error) && depthAssembler.Clear(clear, error)));
            if (variant == DepthRedefined || variant == StencilRedefined || variant == DepthPlaneRedefined)
            {
                auto redefine = depthOnlyClear();
                if (variant == StencilRedefined)
                {
                    redefine.flags       = 0x20;
                    redefine.clear.depth = false;
                }
                if (variant == DepthPlaneRedefined)
                {
                    // The depth test reads depth alone; stencil stays undefined.
                    redefine.flags         = 0x10;
                    redefine.clear.stencil = false;
                }
                UNSCOPED_INFO("the presented depth clear: " + error);
                REQUIRE(depthAssembler.Clear(redefine, error) != 0);
            }
            record(depthAssembler, 1, variant == DepthlessDraw ? 5 : kDepthTag);
            scanOut(depthAssembler);
            const auto tested = depthAssembler.Swap(makeSwap(1, 1));
            const bool fails  = variant == PresentedDepth || variant == PitchlessRect || variant == StencilRedefined;
            UNSCOPED_INFO(std::format("variant {}: a presented depth test after the depth clear {}: {}", variant, fails ? "fails" : "presents", tested.detail));
            REQUIRE((fails ? tested.status == NativeFrameStatus::Failed && tested.detail.find("presented_edram_aliased: off-screen clear") != std::string::npos
                           : tested.status == NativeFrameStatus::Provisional));
            continue;
        }
        UNSCOPED_INFO("tile 1: " + error);
        REQUIRE(assembler.Resolve(tiledTarget(1), tileCopy(1, 0x00100040), true, partial, error));
        const auto result = assembler.Swap(makeSwap(1, 1));
        UNSCOPED_INFO(std::format("variant {} presents: {}", variant, result.detail));
        REQUIRE(result.status == NativeFrameStatus::Provisional);
    }
}

// Offscreen depth starts far-cleared; depth/stencil tests need defined planes.
void testOffscreenDepthDefined()
{
    for (const bool cleared : { false, true })
    {
        FakeCompletion completion;
        auto           config = tiledConfig(completion);
        config.build          = shapedBuilder;
        config.readTarget     = offscreenDepthReader(0x300, 0x530);
        NativeFrameAssembler assembler(config);
        std::string          error;
        bool                 partial = false;
        const auto           target  = offscreen(0x300);
        auto                 clear   = fullClear(target);
        // Color and stencil, inheriting the presented depth surface's depth,
        // or with that depth cleared too.
        clear.depth         = GuestDepthSurface{ 0x00700000, 0x530, 0x530, 1024, 1024, 1 };
        clear.flags         = cleared ? 0x31 : 0x21;
        clear.clear.depth   = cleared;
        clear.clear.stencil = true;
        UNSCOPED_INFO("the off-screen clear: " + error);
        REQUIRE(assembler.Clear(clear, error));
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        record(assembler, 1, kDepthTag, &verdict);
        UNSCOPED_INFO(std::format("a depth-tested draw after a {} clear is {}", cleared ? "depth" : "stencil-only", cleared ? "admitted" : "refused"));
        REQUIRE(verdict.has_value() != cleared);
        UNSCOPED_INFO("the resolve: " + error);
        REQUIRE((assembler.Resolve(target, fullCopy(target), false, partial, error) || !cleared));
        const auto result = assembler.Swap(makeSwap(1, 1));
        UNSCOPED_INFO(std::format("{} clear: {}", cleared ? "depth" : "stencil-only", result.detail));
        REQUIRE((cleared ? result.status != NativeFrameStatus::Failed
                         : result.status == NativeFrameStatus::Failed && result.detail.find("offscreen_depth_undefined: depth 530") != std::string::npos));
    }
}

namespace
{

constexpr std::uint32_t kAliasBase = 0x1A000000, kAliasWidth = 256, kAliasHeight = 128, kAliasPitch = kAliasWidth / 32;
constexpr std::uint32_t kAliasIdentity = (0U << 1) | (1U << 4) | (2U << 7) | (3U << 10);

// A tiled 2D fetch of the destination: type 2, pitch in 32-texel units,
// k_8_8_8_8 (6) or k_2_10_10_10_AS_16_16_16_16 (54), endian k_8in32 (2).
std::array<std::uint32_t, 6> aliasFetch(std::uint32_t format)
{
    return { 2U | (kAliasPitch << 22) | (1U << 31), format | (2U << 6) | kAliasBase, (kAliasWidth - 1) | ((kAliasHeight - 1) << 13), kAliasIdentity, 0, 1U << 9 };
}

NativePendingResolve aliasResolve(std::uint64_t mark, NativeGuestWordPacking packing, std::uint32_t format = 6)
{
    NativePendingResolve resolve;
    resolve.mark      = mark;
    resolve.textureId = 1000 + mark;
    resolve.chain     = 7;
    resolve.epoch     = 1;
    resolve.destFetch = aliasFetch(format);
    resolve.destBase  = kAliasBase;
    resolve.destEnd   = kAliasBase + kAliasWidth * kAliasHeight * 4;
    resolve.width     = kAliasWidth;
    resolve.height    = kAliasHeight;
    resolve.packing   = packing;
    return resolve;
}

std::string missName(NativeResolvedTextureMiss miss)
{
    const auto which = static_cast<std::size_t>(miss);
    return which < kNativeAliasMissNames.size() ? kNativeAliasMissNames[which] : "unmodeled";
}

std::string describeAlias(const std::optional<NativeAliasLookup>& lookup)
{
    if (!lookup)
        return "none";
    if (const auto* miss = std::get_if<NativeResolvedTextureMiss>(&*lookup))
        return "miss " + missName(*miss);
    return std::format("alias {}", std::get<NativeResolvedAlias>(*lookup).texture.frameTextureId);
}

NativeResolvedAlias requireAlias(const std::optional<NativeAliasLookup>& lookup, const std::string& what)
{
    UNSCOPED_INFO(what + ": expected an alias, got " + describeAlias(lookup));
    REQUIRE((lookup && std::holds_alternative<NativeResolvedAlias>(*lookup)));
    return std::get<NativeResolvedAlias>(*lookup);
}

void requireAliasMiss(const std::optional<NativeAliasLookup>& lookup, NativeResolvedTextureMiss miss, const std::string& what)
{
    UNSCOPED_INFO(what + ": expected miss " + missName(miss) + ", got " + describeAlias(lookup));
    REQUIRE((lookup && std::holds_alternative<NativeResolvedTextureMiss>(*lookup) && std::get<NativeResolvedTextureMiss>(*lookup) == miss));
}

void aliasHits()
{
    NativeResolveAliasTable table;
    table.Register(aliasResolve(10, NativeGuestWordPacking::None));
    const auto& alias = requireAlias(table.Find(aliasFetch(6), 11, 1, 7, false), "untiled Rgba8 resolve");
    UNSCOPED_INFO("alias names the resolve's texture and mark");
    REQUIRE((alias.texture.frameTextureId == 1010 && alias.mark == 10));
    UNSCOPED_INFO("alias layout is the fetch's single 2D level");
    REQUIRE(alias.texture.layout == NativeDrawReplayTextureLayout{ kAliasWidth, kAliasHeight, NativeDrawReplayTextureFormat::Rgba8, 1, NativeDrawReplayTextureDimension::Texture2D });
    UNSCOPED_INFO("alias keeps the fetch swizzle");
    REQUIRE((alias.texture.swizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 3 }));

    // A read at or before the resolve's mark, or outside its range, has no alias.
    UNSCOPED_INFO("a read before the resolve");
    REQUIRE(!table.Find(aliasFetch(6), 10, 1, 7, false));
    UNSCOPED_INFO("op mark zero follows every resolve");
    REQUIRE(requireAlias(table.Find(aliasFetch(6), 0, 1, 7, false), "a draw still being built").texture.frameTextureId == 1010);
    auto outside = aliasFetch(6);
    outside[1]   = (outside[1] & 0xFFF) | (kAliasBase + kAliasWidth * kAliasHeight * 4);
    UNSCOPED_INFO("a read past the range");
    REQUIRE(!table.Find(outside, 11, 1, 7, false));
    // A virtual view of the same pages reads the same bytes.
    auto viewed = aliasFetch(6);
    viewed[1] |= 0xE0000000U;
    UNSCOPED_INFO("the fetch base is compared as a physical page");
    REQUIRE(requireAlias(table.Find(viewed, 11, 1, 7, false), "a virtual-view base").texture.frameTextureId == 1010);

    // Packed words alias only through castable views.
    NativeResolveAliasTable packed;
    packed.Register(aliasResolve(20, NativeGuestWordPacking::Color8888));
    packed.Register(aliasResolve(21, NativeGuestWordPacking::Color2101010, 54));
    requireAliasMiss(packed.Find(aliasFetch(54), 22, 1, 7, false), NativeResolvedTextureMiss::Packing, "Color2101010 without castable views");
    const auto& wide = requireAlias(packed.Find(aliasFetch(54), 22, 1, 7, true), "Color2101010 words as Rgb10a2");
    UNSCOPED_INFO("Color2101010 binds Rgb10a2");
    REQUIRE((wide.texture.frameTextureId == 1021 && wide.texture.layout.format == NativeDrawReplayTextureFormat::Rgb10a2));
}

void aliasSwizzles()
{
    // The copy swapped red and blue in guest bytes; the host texture holds them unswapped.
    auto resolve = aliasResolve(10, NativeGuestWordPacking::None);
    resolve.destInfo |= 1U << 24;
    NativeResolveAliasTable table;
    table.Register(resolve);
    auto fetch = aliasFetch(6);
    fetch[3]   = (0U << 1) | (2U << 4) | (4U << 7) | (5U << 10);
    UNSCOPED_INFO("swapped channel selectors exchange, constants stay");
    REQUIRE((requireAlias(table.Find(fetch, 11, 1, 7, false), "swapped").texture.swizzle == std::array<std::uint8_t, 4>{ 2, 0, 4, 5 }));
    fetch[3]          = kAliasIdentity | (6U << 10);
    std::size_t which = 0;
    requireAliasMiss(table.Find(fetch, 11, 1, 7, false, &which), NativeResolvedTextureMiss::Form, "selector 6");
    UNSCOPED_INFO("selector 6 is refused as swizzle");
    REQUIRE(which == kNativeAliasFormNames.size() - 1);
}

void aliasForms()
{
    NativeResolveAliasTable table;
    table.Register(aliasResolve(10, NativeGuestWordPacking::None));
    // Each case names the first condition it fails, as alias_form_<name> counts it.
    const std::tuple<const char*, std::string_view, std::function<void(std::array<std::uint32_t, 6>&)>> cases[] = {
        { "format 7", "format", [](auto& f)
          {
              f[1] = (f[1] & ~0x3FU) | 7;
          } },
        { "format 54 over 8888 bytes", "format", [](auto& f)
          {
              f[1] = (f[1] & ~0x3FU) | 54;
          } },
        { "not a texture fetch", "type", [](auto& f)
          {
              f[0] &= ~3U;
          } },
        { "signed channel", "signed", [](auto& f)
          {
              f[0] |= 1U << 2;
          } },
        { "other pitch", "pitch", [](auto& f)
          {
              f[0] += 1U << 22;
          } },
        { "linear", "pitch", [](auto& f)
          {
              f[0] &= ~(1U << 31);
          } },
        { "other endian", "endian", [](auto& f)
          {
              f[1] ^= 1U << 6;
          } },
        { "stacked", "stacked", [](auto& f)
          {
              f[1] |= 1U << 10;
          } },
        { "inner base", "base", [](auto& f)
          {
              f[1] += 0x1000;
          } },
        { "other width", "size", [](auto& f)
          {
              f[2] -= 1;
          } },
        { "stack depth", "stacked", [](auto& f)
          {
              f[2] |= 1U << 26;
          } },
        { "integer", "signed", [](auto& f)
          {
              f[3] |= 1;
          } },
        { "exponent bias", "exp_bias", [](auto& f)
          {
              f[3] |= 1U << 13;
          } },
        { "mip levels", "mip", [](auto& f)
          {
              f[4] |= 1U << 6;
          } },
        { "3D", "dimension", [](auto& f)
          {
              f[5] = (f[5] & ~(3U << 9)) | (2U << 9);
          } },
        { "packed mips", "packed_mips", [](auto& f)
          {
              f[5] |= 1U << 11;
          } },
    };
    for (const auto& [name, refusal, mutate] : cases)
    {
        auto fetch = aliasFetch(6);
        mutate(fetch);
        std::size_t which = kNativeAliasFormNames.size();
        requireAliasMiss(table.Find(fetch, 11, 1, 7, true, &which), NativeResolvedTextureMiss::Form, name);
        UNSCOPED_INFO(std::format("{}: refused as {}", name, which < kNativeAliasFormNames.size() ? kNativeAliasFormNames[which] : "nothing"));
        REQUIRE((which < kNativeAliasFormNames.size() && kNativeAliasFormNames[which] == refusal));
    }
    NativeResolveAliasTable packed;
    packed.Register(aliasResolve(10, NativeGuestWordPacking::Color2101010, 54));
    requireAliasMiss(packed.Find(aliasFetch(6), 11, 1, 7, true), NativeResolvedTextureMiss::Form, "8888 fetch of Color2101010 words");
    NativeResolveAliasTable unpacked;
    unpacked.Register(aliasResolve(10, NativeGuestWordPacking::Float16x2));
    requireAliasMiss(unpacked.Find(aliasFetch(6), 11, 1, 7, true), NativeResolvedTextureMiss::Form, "8888 fetch of Float16x2 words");
}

void aliasCoverage()
{
    for (const auto& [name, mutate] : std::initializer_list<std::pair<const char*, std::function<void(NativePendingResolve&)>>>{
             { "offset strip", [](auto& r)
               {
                   r.destY = 32;
                   r.height -= 32;
               } },
             { "narrow strip", [](auto& r)
               {
                   r.width = 128;
               } },
             { "column offset", [](auto& r)
               {
                   r.destX = 32;
               } },
         })
    {
        auto resolve = aliasResolve(10, NativeGuestWordPacking::None);
        mutate(resolve);
        NativeResolveAliasTable table;
        table.Register(resolve);
        requireAliasMiss(table.Find(aliasFetch(6), 11, 1, 7, false), NativeResolvedTextureMiss::Coverage, name);
    }
}

void aliasLifetimes()
{
    const auto range = [](NativeResolveAliasTable& table, std::uint64_t mark)
    {
        auto resolve        = aliasResolve(mark, NativeGuestWordPacking::None);
        resolve.watchedFrom = 40;
        table.Register(resolve);
    };
    NativeResolveAliasTable table;
    range(table, 10);
    table.Written(kAliasBase + 64, kAliasBase + 128, 40);
    requireAlias(table.Find(aliasFetch(6), 11, 1, 7, false), "a write numbered before arming");
    table.Written(kAliasBase - 64, kAliasBase, 41);
    requireAlias(table.Find(aliasFetch(6), 11, 1, 7, false), "a write beside the range");
    table.Written(kAliasBase + 64, kAliasBase + 128, 41);
    requireAliasMiss(table.Find(aliasFetch(6), 11, 1, 7, false), NativeResolvedTextureMiss::Written, "a numbered write after arming");

    NativeResolveAliasTable cleared;
    range(cleared, 10);
    cleared.Written(kAliasBase, kAliasBase + 4);
    requireAliasMiss(cleared.Find(aliasFetch(6), 11, 1, 7, false), NativeResolvedTextureMiss::Written, "a clear or unmodeled resolve");

    // A later overlapping resolve supersedes the earlier one for every reader.
    NativeResolveAliasTable later;
    range(later, 10);
    auto over     = aliasResolve(12, NativeGuestWordPacking::None);
    over.destBase = kAliasBase + 0x2000;
    later.Register(over);
    requireAliasMiss(later.Find(aliasFetch(6), 11, 1, 7, false), NativeResolvedTextureMiss::Superseded, "a read between overlapping resolves");

    // Each entry aliases only in its epoch and in the chain holding it.
    NativeResolveAliasTable retired;
    range(retired, 10);
    requireAliasMiss(retired.Find(aliasFetch(6), 11, 2, 7, false), NativeResolvedTextureMiss::Retired, "a reader in a later epoch");
    requireAliasMiss(retired.Find(aliasFetch(6), 11, 1, 8, false), NativeResolvedTextureMiss::Reset, "a reader in another chain");
    retired.EndChain(8);
    requireAlias(retired.Find(aliasFetch(6), 11, 1, 7, false), "another chain ending");
    retired.EndChain(7);
    requireAliasMiss(retired.Find(aliasFetch(6), 11, 1, 7, false), NativeResolvedTextureMiss::Reset, "a reader after its chain ended");
    requireAlias(retired.Find(aliasFetch(6), 11, 1, 0, false), "a reader while no chain is open");
    UNSCOPED_INFO("a chain adopts no unbound entry");
    REQUIRE(retired.Adopt(1, 9).empty());
    retired.Bind(10);
    UNSCOPED_INFO("the next chain of its epoch adopts the bound entry, once");
    REQUIRE((retired.Adopt(2, 9).empty() && retired.Adopt(1, 9) == std::vector<std::uint64_t>{ 10 } && retired.Adopt(1, 10).empty()));
    requireAlias(retired.Find(aliasFetch(6), 11, 1, 9, false), "a reader in the adopting chain");
    retired.Supersede(kAliasBase, kAliasBase + 4);
    retired.EndChain(9);
    UNSCOPED_INFO("an ended bound entry is still re-recorded");
    REQUIRE(retired.Adopt(1, 0) == std::vector<std::uint64_t>{ 10 });
    retired.EndEpoch(1);
    requireAliasMiss(retired.Find(aliasFetch(6), 11, 1, 0, false), NativeResolvedTextureMiss::Retired, "its epoch's end");
    UNSCOPED_INFO("an ended epoch re-records nothing");
    REQUIRE(retired.Adopt(1, 0).empty());
}

void aliasBound()
{
    const auto entry = [](std::uint64_t mark)
    {
        auto resolve     = aliasResolve(mark, NativeGuestWordPacking::None);
        resolve.destBase = kAliasBase + static_cast<std::uint32_t>(mark) * 0x10000;
        resolve.destEnd  = resolve.destBase + 0x1000;
        return resolve;
    };
    const auto at = [](std::uint64_t mark)
    {
        auto fetch = aliasFetch(6);
        fetch[1]   = (fetch[1] & 0xFFF) | (kAliasBase + static_cast<std::uint32_t>(mark) * 0x10000);
        return fetch;
    };
    NativeResolveAliasTable table;
    for (std::uint64_t mark = 1; mark <= NativeResolveAliasTable::kEntries + 1; ++mark)
    {
        table.Register(entry(mark));
        if (mark == 1)
            table.Bind(1);
    }
    UNSCOPED_INFO("the table keeps its bound");
    REQUIRE(table.Size() == NativeResolveAliasTable::kEntries);
    requireAlias(table.Find(at(1), ~std::uint64_t(0), 1, 7, false), "a bound entry is kept for its re-records");
    UNSCOPED_INFO("the oldest unbound entry is forgotten");
    REQUIRE(!table.Find(at(2), ~std::uint64_t(0), 1, 7, false));

    NativeResolveAliasTable full;
    for (std::uint64_t mark = 1; mark <= NativeResolveAliasTable::kEntries; ++mark)
    {
        full.Register(entry(mark));
        full.Bind(mark);
    }
    full.Register(entry(NativeResolveAliasTable::kEntries + 1));
    UNSCOPED_INFO("with every entry bound a new resolve is not aliased");
    REQUIRE((full.Size() == NativeResolveAliasTable::kEntries && !full.Find(at(NativeResolveAliasTable::kEntries + 1), ~std::uint64_t(0), 1, 7, false)));
}

void aliasAdmission()
{
    // A recorded resolve is reserved without a texture until a segment admits
    // it. Its watch is armed at record, so only a write numbered after the
    // record ends it, before the image lands or after.
    NativeResolveAliasTable table;
    auto                    resolve = aliasResolve(10, NativeGuestWordPacking::None);
    resolve.textureId = resolve.chain = 0;
    resolve.watchedFrom               = 4;
    table.Register(resolve);
    requireAliasMiss(table.Find(aliasFetch(6), 11, 1, 7, false), NativeResolvedTextureMiss::Unmodeled, "an unadmitted resolve");
    table.Written(kAliasBase, kAliasBase + 4, 4);
    table.Admit(10, 1010, 7);
    requireAlias(table.Find(aliasFetch(6), 11, 1, 7, false), "a write counted before the record");
    table.Landed(10);
    table.Unwritten(0, 10);
    requireAlias(table.Find(aliasFetch(6), 11, 1, 7, false), "a landed image is not unwritten");
    table.Written(kAliasBase, kAliasBase + 4, 5);
    requireAliasMiss(table.Find(aliasFetch(6), 11, 1, 7, false), NativeResolvedTextureMiss::Written, "a write after the image lands");

    NativeResolveAliasTable early;
    early.Register(resolve);
    early.Admit(10, 1010, 7);
    early.Written(kAliasBase, kAliasBase + 4, 5);
    early.Landed(10);
    requireAliasMiss(early.Find(aliasFetch(6), 11, 1, 7, false), NativeResolvedTextureMiss::Written, "a write before the image lands");

    NativeResolveAliasTable skipped;
    skipped.Register(resolve);
    skipped.Admit(10, 1010, 7);
    skipped.Unwritten(10, 10);
    requireAliasMiss(skipped.Find(aliasFetch(6), 11, 1, 7, false), NativeResolvedTextureMiss::Written, "a skipped image");

    NativeResolveAliasTable host;
    host.Register(resolve);
    host.Admit(10, 1010, 7);
    host.Landed(10);
    host.Written(kAliasBase + 8, kAliasBase + 12);
    requireAliasMiss(host.Find(aliasFetch(6), 11, 1, 7, false), NativeResolvedTextureMiss::Written, "an unnumbered host write");
}

// The off-screen copy fullCopy describes, as its destination fetch.
std::array<std::uint32_t, 6> fullCopyFetch(std::uint32_t format = 6)
{
    return { 2U | (32U << 22) | (1U << 31), format | 0x1E3B0000U, 1023 | (1023U << 13), kAliasIdentity, 0, 1U << 9 };
}

constexpr std::uint64_t kGraphTexture = std::uint64_t(1) << 62;

GuestTarget presentedTarget()
{
    GuestTarget target;
    target.native = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    return target;
}

// A k_16_16 off-screen target past the presented EDRAM, whose resolve the
// graph never takes, so it resets the chain.
GuestTarget signedTarget()
{
    GuestTarget target;
    target.native = { 64, 64, 1, NativeDrawReplayTargetFormat::Rg16Snorm, 0x900 };
    return target;
}

GuestResolveCopy signedCopy()
{
    auto copy     = fullCopy(signedTarget());
    copy.destInfo = 0xEF82;
    copy.destBase = 0x1D800000;
    return copy;
}

// The frame texture each draw of a span samples, zero for none.
std::vector<std::uint64_t> sampledTextures(const NativeCompletionSpan& span)
{
    UNSCOPED_INFO("span carries a frame");
    REQUIRE(span.frame.has_value());
    std::vector<std::uint64_t> textures;
    for (const auto& pass : span.frame->passes)
        for (std::size_t i = 0; i < pass.DrawCount(); ++i)
            textures.push_back(pass.Draw(i).textureMask ? pass.Draw(i).textures[0].frameTextureId : 0);
    return textures;
}

bool declares(const NativeRenderFrame& frame, std::uint64_t texture)
{
    return std::ranges::any_of(frame.textures, [&](const NativeRenderFrameTexture& declared)
                               {
                                   return declared.id == texture;
                               });
}

// An aliasing assembler whose draws ask for fullCopyFetch's resolved texture
// at build, as a family reader does, and bind what it returns. Tag 5 draws
// into the presented target, tag 0x40 into the off-screen one at 0x300.
struct AliasRig
{
    FakeCompletion                                       completion;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> watched;
    std::vector<NativeResolvedTextureMiss>               misses;
    std::vector<std::uint64_t>                           bound;
    bool                                                 presented = true;
    // Run once inside the next build: before its first query, and before a
    // second slot's query.
    std::function<void()> ahead, between;
    // The fetch each build and query names.
    std::array<std::uint32_t, 6>          fetch = fullCopyFetch();
    std::unique_ptr<NativeFrameAssembler> assembler;
    std::string                           error;

    explicit AliasRig(const std::function<void(NativeFrameAssemblerConfig&)>& adjust = {})
    {
        NativeFrameAssemblerConfig config;
        config.completion         = &completion;
        config.resolveGraph       = true;
        config.eagerGraphSegments = true;
        config.resolveAlias       = true;
        config.timing             = true;
        config.watchWrites        = [this](std::uint32_t physical, std::uint32_t length)
        {
            watched.emplace_back(physical, length);
        };
        config.readTarget = [this, onScreen = targetReader(1280, 720, true, 0), offScreen = targetReader(1024, 1024, false, 0x300)](std::uint32_t address, std::uint32_t& out)
        {
            return (presented ? onScreen : offScreen)(address, out);
        };
        config.build = [this](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string& detail)
        {
            if (!shapedBuilder(value, draw, detail))
                return false;
            if (ahead)
                std::exchange(ahead, {})();
            auto       miss    = NativeResolvedTextureMiss::Form;
            const auto texture = assembler->ResolvedTexture(fetch, value.operationMark, miss);
            if (texture)
            {
                draw.textureMask = 1;
                draw.textures[0] = BindNativeResolvedTexture(*texture);
            }
            bound.push_back(texture ? texture->frameTextureId : 0);
            misses.push_back(miss);
            if (between)
            {
                std::exchange(between, {})();
                const auto other = assembler->ResolvedTexture(fetch, value.operationMark, miss);
                if (other)
                {
                    draw.textureMask |= 2;
                    draw.textures[1] = BindNativeResolvedTexture(*other);
                }
                bound.push_back(other ? other->frameTextureId : 0);
                misses.push_back(miss);
            }
            return true;
        };
        if (adjust)
            adjust(config);
        assembler = std::make_unique<NativeFrameAssembler>(config);
    }

    std::uint64_t clear(const GuestTarget& target)
    {
        const auto mark = assembler->Clear(fullClear(target), error);
        UNSCOPED_INFO("a full clear: " + error);
        REQUIRE(mark != 0);
        return mark;
    }

    std::uint64_t resolve(const GuestTarget& target, const GuestResolveCopy& copy)
    {
        bool       partial = false;
        const auto mark    = assembler->Resolve(target, copy, false, partial, error);
        UNSCOPED_INFO("an off-screen resolve: " + error);
        REQUIRE(mark != 0);
        return mark;
    }

    // An off-screen graph resolve of the target at 0x300 to fullCopyFetch.
    std::uint64_t produce()
    {
        auto copy      = fullCopy(offscreen(0x300));
        copy.destFetch = fullCopyFetch();
        clear(offscreen(0x300));
        return resolve(offscreen(0x300), copy);
    }

    // One admitted draw; returns the texture it bound, zero for a miss.
    std::uint64_t draw(std::uint64_t sequence, bool onScreen)
    {
        presented                               = onScreen;
        std::optional<NativeDrawReason> verdict = NativeDrawReason::NoNativeRoute;
        record(*assembler, sequence, onScreen ? 5 : 0x40, &verdict);
        UNSCOPED_INFO("the draw is admitted");
        REQUIRE(!verdict);
        return bound.back();
    }

    // The table's answer outside a build, where a found alias is off-screen.
    NativeResolvedTextureMiss query(std::uint64_t opMark = 0)
    {
        auto       miss  = NativeResolvedTextureMiss::Form;
        const auto found = assembler->ResolvedTexture(fetch, opMark, miss);
        UNSCOPED_INFO("only a presented draw being built binds");
        REQUIRE(!found);
        return miss;
    }

    std::uint64_t missCount(NativeResolvedTextureMiss miss) const
    {
        return assembler->ResolveGraphStats().aliasMisses[static_cast<std::size_t>(miss)];
    }

    NativeAssembledFrame swap(std::uint64_t numbered)
    {
        scanOut(*assembler);
        return assembler->Swap(makeSwap(1, numbered));
    }
};

// Skipping a submitted image exposes older guest bytes; written images persist.
void surfaceSkippedImage()
{
    for (const bool skip : { false, true })
    {
        AliasRig rig([](NativeFrameAssemblerConfig& config)
                     {
                         config.castableGuestWords = true;
                         config.surfaces           = std::make_shared<ResolveSurfaceRegistry>();
                     });
        UNSCOPED_INFO("the resolve submits its segment: " + rig.error);
        REQUIRE(rig.produce() == 2);
        UNSCOPED_INFO("the admitted entry is found outside a build");
        REQUIRE(rig.query() == NativeResolvedTextureMiss::Offscreen);
        if (skip)
            rig.assembler->GuestImageSkipped(2);
        else
            rig.assembler->GuestImageWritten(2);
        UNSCOPED_INFO((skip ? "a skipped image ends the entry" : "a written image keeps the entry"));
        REQUIRE(rig.query() == (skip ? NativeResolvedTextureMiss::Unmodeled : NativeResolvedTextureMiss::Offscreen));
    }
    std::cout << "A skipped resolve image ends its surface entry\n";
}

// Owed resolves bind only within a submitted build; swap imports them.
void surfaceUnsubmittedResolve()
{
    const auto surfaces = std::make_shared<ResolveSurfaceRegistry>();
    AliasRig   rig([&](NativeFrameAssemblerConfig& config)
                   {
                     config.castableGuestWords = true;
                     config.surfaces           = surfaces;
                   });
    // The last terminal frame is unfinished, so no segment is ready.
    rig.completion.gateSegments = true;
    rig.completion.terminal     = 1;
    UNSCOPED_INFO("the resolve waits in the open epoch: " + rig.error);
    REQUIRE((rig.produce() == 2 && rig.completion.spans.empty()));
    UNSCOPED_INFO("outside a build the entry misses and nothing submits");
    REQUIRE((rig.query() == NativeResolvedTextureMiss::Unmodeled && rig.completion.spans.empty()));
    rig.clear(presentedTarget());
    UNSCOPED_INFO("the build binds the entry's texture");
    REQUIRE(rig.draw(1, true) == ResolveSurfaceTexture(2));
    UNSCOPED_INFO("the query submitted the resolve's frame and peeked nothing");
    REQUIRE((rig.completion.spans.size() == 1 && rig.completion.spans[0].resolveMark == 2 && !rig.completion.spans[0].segment && rig.completion.peekCount == 0));
    const auto counts = surfaces->TakeCounts();
    UNSCOPED_INFO("one submit before the hit, after the miss outside the build");
    REQUIRE((counts.submits == 1 && counts.hits == 1 && counts.missUnmodeled == 1));
    const auto swapped = rig.swap(1);
    UNSCOPED_INFO("the epoch presents: " + swapped.detail);
    REQUIRE(swapped.status == NativeFrameStatus::Provisional);
    UNSCOPED_INFO("the swap's frame imports the surface");
    REQUIRE(declares(*rig.completion.spans.back().frame, ResolveSurfaceTexture(2)));
    UNSCOPED_INFO("no failure");
    REQUIRE(!rig.assembler->Failure());
    std::cout << "An unsubmitted surface entry binds after its resolve submits\n";
}

// A held readback that counts its encodes.
struct FakeHeldImage final : IHeldGuestImage
{
    GuestResolveImage             image;
    std::uint64_t                 bytes = 0;
    mutable std::atomic<unsigned> encodes{ 0 };
    bool                          fails = false;

    FakeHeldImage(std::uint8_t value, std::uint64_t held)
    : bytes(held)
    {
        image.physical = 0x1E3B0000;
        image.bytes.assign(16, value);
    }

    completion::GuestImageTake Encode() const override
    {
        ++encodes;
        if (fails)
            return { std::nullopt, "fixture_encode_failed", false };
        return { image, {}, false };
    }

    std::uint64_t Bytes() const noexcept override
    {
        return bytes;
    }
};

// An AliasRig whose assembler holds images: a surface registry and a write
// gate that records each image written through it, as the ledger's would.
struct HoldRig
{
    std::shared_ptr<ResolveSurfaceRegistry> surfaces = std::make_shared<ResolveSurfaceRegistry>();
    std::vector<GuestResolveImage>          written;
    unsigned                                gates = 0;
    AliasRig                                rig;

    HoldRig()
    : rig([this](NativeFrameAssemblerConfig& config)
          {
              config.castableGuestWords = true;
              config.surfaces           = surfaces;
          })
    {
        rig.assembler->SetGuestImageWrites([this](const std::function<void(const completion::GuestImageWriter&)>& body)
                                           {
                                               ++gates;
                                               body([this](const GuestResolveImage& image)
                                                    {
                                                        written.push_back(image);
                                                        return true;
                                                    });
                                               return true;
                                           });
    }

    // An admitted surface resolve whose readback the source will hand over held.
    std::pair<std::uint64_t, std::shared_ptr<FakeHeldImage>> held(std::uint8_t value, std::uint64_t bytes = 4096)
    {
        const auto mark            = rig.produce();
        auto       image           = std::make_shared<FakeHeldImage>(value, bytes);
        rig.completion.holds[mark] = image;
        return { mark, image };
    }

    // A read of the destination's first bytes, which returns guest memory.
    bool read(std::string& error)
    {
        std::vector<std::uint8_t> bytes;
        return rig.assembler->ReadResolved(0x1E3B0000, 16, []
                                           {
                                               return std::vector<std::uint8_t>(16, 0xEE);
                                           },
                                           bytes,
                                           error) &&
               bytes == std::vector<std::uint8_t>(16, 0xEE);
    }
};

// A held surface image stays unencoded until the ledger gate writes it.
void surfaceHeldImageRead()
{
    HoldRig rig;
    const auto [mark, image] = rig.held(0x11);
    const auto take          = rig.rig.assembler->TakeGuestImage(mark);
    UNSCOPED_INFO("the take holds the live entry's image: " + take.error);
    REQUIRE((take.held && !take.image && take.error.empty() && take.before.empty()));
    UNSCOPED_INFO("a hold encodes and writes nothing");
    REQUIRE((image->encodes == 0 && rig.written.empty()));
    auto counts = rig.surfaces->TakeCounts();
    UNSCOPED_INFO("the held bytes are the gauge");
    REQUIRE((counts.heldBytes == 4096 && counts.flushes == 0 && counts.lockReadbacks == 0));
    UNSCOPED_INFO("the timing line carries the hold counters");
    REQUIRE(FormatResolveSurfaceCounts({}).find(" surface_submits=0 lock_readbacks=0 surface_flushes=0 surface_readback_mb=0.0") != std::string::npos);
    UNSCOPED_INFO("the held image's entry stays live");
    REQUIRE(rig.rig.query() == NativeResolvedTextureMiss::Offscreen);
    rig.rig.assembler->DiscardGuestImagesThrough(mark);
    std::string error;
    const bool  read = rig.read(error);
    UNSCOPED_INFO("the read returns guest memory after the write: " + error);
    REQUIRE(read);
    UNSCOPED_INFO("the read wrote the held image once, through the gate");
    REQUIRE((image->encodes == 1 && rig.written.size() == 1 && rig.written[0].bytes == image->image.bytes && rig.gates == 1));
    counts = rig.surfaces->TakeCounts();
    UNSCOPED_INFO("the write counts as a flush and releases the bytes");
    REQUIRE((counts.flushes == 1 && counts.lockReadbacks == 0 && counts.heldBytes == 0));
    const bool again = rig.read(error);
    UNSCOPED_INFO("a second read writes nothing: " + error);
    REQUIRE((again && rig.written.size() == 1));
    UNSCOPED_INFO("no failure");
    REQUIRE(!rig.rig.assembler->Failure());
    std::cout << "A held surface image lands before a read of its destination\n";
}

// A lock over a held image writes it; taking under the lock encodes it once.
void surfaceHeldImageLock()
{
    HoldRig rig;
    const auto [mark, image] = rig.held(0x22);
    UNSCOPED_INFO("the take holds the image");
    REQUIRE(rig.rig.assembler->TakeGuestImage(mark).held);
    const ResolveSurfaceLockRanges ranges{ { { 0x1E3B0000, 0x1E3B0010 }, { 0, 0 } } };
    rig.surfaces->Locked(7, ranges, false);
    rig.surfaces->Settle(&ranges);
    UNSCOPED_INFO("the lock wrote the held image");
    REQUIRE((image->encodes == 1 && rig.written.size() == 1 && rig.written[0].bytes == image->image.bytes));
    auto counts = rig.surfaces->TakeCounts();
    UNSCOPED_INFO("the write counts as a lock readback");
    REQUIRE((counts.lockReadbacks == 1 && counts.flushes == 0 && counts.heldBytes == 0));
    const auto [next, nextImage] = rig.held(0x33);
    const auto take              = rig.rig.assembler->TakeGuestImage(next);
    UNSCOPED_INFO("a take under the open lock encodes now: " + take.error);
    REQUIRE((!take.held && take.image && take.image->bytes == nextImage->image.bytes && nextImage->encodes == 1));
    counts = rig.surfaces->TakeCounts();
    UNSCOPED_INFO("and counts as a lock readback");
    REQUIRE((counts.lockReadbacks == 1 && counts.flushes == 0));
    UNSCOPED_INFO("the ledger, not the gate, writes a take's own image");
    REQUIRE(rig.written.size() == 1);
    UNSCOPED_INFO("no failure");
    REQUIRE(!rig.rig.assembler->Failure());
    std::cout << "A lock writes the held images it names\n";
}

// A partial resolve 512 x 512 over the held image's destination: its entry
// ends owed, not covered.
std::uint64_t partialResolve(HoldRig& rig)
{
    auto copy       = fullCopy(offscreen(0x300));
    copy.destFetch  = fullCopyFetch();
    copy.rectWidth  = 512;
    copy.rectHeight = 512;
    rig.rig.clear(offscreen(0x300));
    return rig.rig.resolve(offscreen(0x300), copy);
}

// Full cover drops owed held images; partial cover waits for read/lock.
void surfaceHeldImageEndings()
{
    {
        HoldRig rig;
        const auto [mark, image] = rig.held(0x44);
        UNSCOPED_INFO("the take holds the image");
        REQUIRE(rig.rig.assembler->TakeGuestImage(mark).held);
        rig.rig.produce();
        UNSCOPED_INFO("the covering resolve drops the held image unwritten");
        REQUIRE((image->encodes == 0 && rig.written.empty()));
        const auto counts = rig.surfaces->TakeCounts();
        UNSCOPED_INFO("the drop releases the bytes");
        REQUIRE((counts.heldBytes == 0 && counts.flushes == 0));
        UNSCOPED_INFO("no failure");
        REQUIRE(!rig.rig.assembler->Failure());
    }
    for (const bool lock : { false, true })
    {
        HoldRig rig;
        const auto [mark, image] = rig.held(0x55);
        UNSCOPED_INFO("the take holds the image");
        REQUIRE(rig.rig.assembler->TakeGuestImage(mark).held);
        const auto next = partialResolve(rig);
        UNSCOPED_INFO("the partial resolve leaves the owed image held, unwritten");
        REQUIRE((image->encodes == 0 && rig.written.empty() && rig.gates == 0));
        auto counts = rig.surfaces->TakeCounts();
        UNSCOPED_INFO("it counts as an overlap and stays owed");
        REQUIRE((counts.overlaps == 1 && counts.owed == 1 && counts.flushes == 0 && counts.heldBytes == 4096));
        if (lock)
        {
            const ResolveSurfaceLockRanges ranges{ { { 0x1E3B0000, 0x1E3B0010 }, { 0, 0 } } };
            rig.surfaces->Locked(7, ranges, false);
            rig.surfaces->Settle(&ranges);
            UNSCOPED_INFO("a lock of the owed range writes it");
            REQUIRE((image->encodes == 1 && rig.written.size() == 1 && rig.written[0].bytes == image->image.bytes));
            counts = rig.surfaces->TakeCounts();
            UNSCOPED_INFO("as a lock readback");
            REQUIRE((counts.lockReadbacks == 1 && counts.flushes == 0 && counts.owed == 0 && counts.heldBytes == 0));
        }
        else
        {
            rig.rig.completion.holds[next] = std::make_shared<FakeHeldImage>(0x99, 4096);
            UNSCOPED_INFO("the newer image is held too");
            REQUIRE(rig.rig.assembler->TakeGuestImage(next).held);
            std::string error;
            const bool  read = rig.read(error);
            UNSCOPED_INFO("the read returns guest memory after the writes: " + error);
            REQUIRE(read);
            UNSCOPED_INFO("a read writes the owed image, then the newer one over it");
            REQUIRE((rig.written.size() == 2 && rig.written[0].bytes == image->image.bytes && rig.written[1].bytes == std::vector<std::uint8_t>(16, 0x99)));
            counts = rig.surfaces->TakeCounts();
            UNSCOPED_INFO("both count as flushes");
            REQUIRE((counts.flushes == 2 && counts.owed == 0 && counts.heldBytes == 0));
        }
        UNSCOPED_INFO("no failure");
        REQUIRE(!rig.rig.assembler->Failure());
    }
    {
        HoldRig rig;
        const auto [mark, image] = rig.held(0x56);
        UNSCOPED_INFO("the take holds the image");
        REQUIRE(rig.rig.assembler->TakeGuestImage(mark).held);
        const auto partial = partialResolve(rig);
        static_cast<void>(rig.surfaces->TakeCounts());
        rig.rig.produce();
        UNSCOPED_INFO("the next covering version drops the owed image unwritten");
        REQUIRE((image->encodes == 0 && rig.written.empty()));
        const auto counts = rig.surfaces->TakeCounts();
        UNSCOPED_INFO("the drop counts and releases the bytes");
        REQUIRE((counts.owedDropped == 1 && counts.flushes == 0 && counts.heldBytes == 0));
        // Its 512 x 512 range is not the full resolve's, which leaves it owed.
        UNSCOPED_INFO("the untaken partial image stays owed");
        REQUIRE((counts.owed == 1 && rig.surfaces->Holds(partial) == ResolveSurfaceHold::Hold));
        UNSCOPED_INFO("no failure");
        REQUIRE(!rig.rig.assembler->Failure());
    }
    {
        // A whole resolve of a longer range writes every byte of the older
        // one: the held image is dropped, not owed.
        HoldRig rig;
        const auto [mark, image] = rig.held(0x66);
        UNSCOPED_INFO("the take holds the image");
        REQUIRE(rig.rig.assembler->TakeGuestImage(mark).held);
        auto copy         = fullCopy(offscreen(0x300));
        copy.destFetch    = fullCopyFetch();
        copy.destPitch    = (1056U << 16) | 1024;
        copy.destFetch[2] = 1023 | (1055U << 13);
        rig.rig.clear(offscreen(0x300));
        rig.rig.resolve(offscreen(0x300), copy);
        UNSCOPED_INFO("the whole resolve drops the held image inside it unwritten");
        REQUIRE((image->encodes == 0 && rig.written.empty()));
        const auto counts = rig.surfaces->TakeCounts();
        UNSCOPED_INFO("the drop counts as covered");
        REQUIRE((counts.droppedCovered == 1 && counts.overlaps == 0 && counts.flushes == 0 && counts.heldBytes == 0));
        UNSCOPED_INFO("no failure");
        REQUIRE(!rig.rig.assembler->Failure());
    }
    std::cout << "A held image is dropped when covered and stays owed under a partial resolve\n";
}

// Dropped-before-take images end unencoded; reads still use guest memory.
void surfaceImagesBeforeTake()
{
    // The destination's first bytes as a read returns them; empty on failure.
    const auto readBytes = [](HoldRig& rig, std::string& error)
    {
        std::vector<std::uint8_t> bytes;
        if (!rig.rig.assembler->ReadResolved(0x1E3B0000, 16, []
                                             {
                                                 return std::vector<std::uint8_t>(16, 0xEE);
                                             },
                                             bytes,
                                             error))
            bytes.clear();
        return bytes;
    };
    {
        // The next version supersedes the first before its take.
        HoldRig rig;
        const auto [first, image]        = rig.held(0x12);
        const auto [second, next]        = rig.held(0x13);
        rig.rig.completion.peeks[second] = completion::GuestImageTake{ next->image, {}, false };
        std::string error;
        UNSCOPED_INFO("a read before the takes skips the dropped image and peeks the next: " + error);
        REQUIRE((readBytes(rig, error) == std::vector<std::uint8_t>(16, 0x13) && rig.rig.completion.peeked == std::vector<std::uint64_t>{ second }));
        const auto take = rig.rig.assembler->TakeGuestImage(first);
        UNSCOPED_INFO("the superseded image ends at its take unencoded: " + take.error);
        REQUIRE((take.held && !take.image && take.error.empty() && take.before.empty() && image->encodes == 0));
        UNSCOPED_INFO("the next version is held");
        REQUIRE((rig.rig.assembler->TakeGuestImage(second).held && next->encodes == 0));
        auto counts = rig.surfaces->TakeCounts();
        UNSCOPED_INFO("nothing lands; the next version's bytes are held");
        REQUIRE((counts.flushes == 0 && counts.heldBytes == 4096));
        const bool read = rig.read(error);
        UNSCOPED_INFO("a read writes only the next version: " + error);
        REQUIRE((read && rig.written.size() == 1 && rig.written[0].bytes == next->image.bytes && rig.rig.completion.peekCount == 1));
        UNSCOPED_INFO("no failure");
        REQUIRE(!rig.rig.assembler->Failure());
    }
    {
        // A guest store queues the drop before the take, also of a peeked image.
        HoldRig rig;
        const auto [mark, image]       = rig.held(0x14);
        rig.rig.completion.peeks[mark] = completion::GuestImageTake{ image->image, {}, false };
        std::string error;
        UNSCOPED_INFO("a read overlays the pending image: " + error);
        REQUIRE(readBytes(rig, error) == std::vector<std::uint8_t>(16, 0x14));
        rig.rig.assembler->GuestWrote(0x1E3B0004, 4);
        const bool read = rig.read(error);
        UNSCOPED_INFO("a read after the store takes guest memory: " + error);
        REQUIRE((read && rig.rig.completion.peekCount == 1 && rig.written.empty()));
        const auto take = rig.rig.assembler->TakeGuestImage(mark);
        UNSCOPED_INFO("the take ends it unencoded: " + take.error);
        REQUIRE((take.held && !take.image && image->encodes == 0 && rig.written.empty()));
        const auto counts = rig.surfaces->TakeCounts();
        UNSCOPED_INFO("nothing lands or stays held");
        REQUIRE((counts.flushes == 0 && counts.heldBytes == 0 && !rig.rig.assembler->Failure()));
    }
    {
        // An entry ended owed before its take.
        HoldRig rig;
        const auto [mark, image] = rig.held(0x15);
        partialResolve(rig);
        const auto take = rig.rig.assembler->TakeGuestImage(mark);
        UNSCOPED_INFO("the take holds the owed image: " + take.error);
        REQUIRE((take.held && !take.image && image->encodes == 0 && rig.written.empty()));
        const auto counts = rig.surfaces->TakeCounts();
        UNSCOPED_INFO("it stays owed on the host");
        REQUIRE((counts.flushes == 0 && counts.heldBytes == 4096 && counts.owed == 1 && !rig.rig.assembler->Failure()));
    }
    std::cout << "Images dropped or owed before their take\n";
}

// Guest store or host write drops a held image; later read/lock writes nothing.
void surfaceHeldImageStores()
{
    for (const bool host : { false, true })
    {
        HoldRig rig;
        const auto [mark, image] = rig.held(0x77);
        UNSCOPED_INFO("the take holds the image");
        REQUIRE(rig.rig.assembler->TakeGuestImage(mark).held);
        if (host)
            rig.rig.assembler->HostWriting(0x1E3B0004, 4);
        else
            rig.rig.assembler->GuestWrote(0x1E3B0004, 4);
        std::string error;
        const bool  read = rig.read(error);
        UNSCOPED_INFO(std::string(host ? "a host write" : "a guest store") + " leaves the image unwritten for a read: " + error);
        REQUIRE((read && rig.written.empty() && image->encodes == 0));
        const ResolveSurfaceLockRanges ranges{ { { 0x1E3B0000, 0x1E3B0010 }, { 0, 0 } } };
        rig.surfaces->Locked(7, ranges, false);
        rig.surfaces->Settle(&ranges);
        if (host)
            rig.rig.assembler->HostWrote(0x1E3B0004, 4);
        const auto counts = rig.surfaces->TakeCounts();
        UNSCOPED_INFO("the settle drops it");
        REQUIRE((rig.written.empty() && image->encodes == 0 && counts.heldBytes == 0 && counts.flushes == 0 && counts.lockReadbacks == 0));
        UNSCOPED_INFO("no failure");
        REQUIRE(!rig.rig.assembler->Failure());
    }
    std::cout << "A store over a held image drops it at its notice\n";
}

// Older images land in mark order; encode failure fails session, not take.
void surfaceHeldImageBefore()
{
    for (const bool fails : { false, true })
    {
        HoldRig rig;
        const auto [mark, image] = rig.held(0x88);
        UNSCOPED_INFO("the take holds the image");
        REQUIRE(rig.rig.assembler->TakeGuestImage(mark).held);
        image->fails = fails;
        // The partial resolve leaves it owed and held.
        const auto next = partialResolve(rig);
        UNSCOPED_INFO("the partial resolve wrote nothing");
        REQUIRE((rig.written.empty() && image->encodes == 0));
        rig.rig.completion.holds[next] = std::make_shared<FakeHeldImage>(0x99, NativeFrameAssembler::kHeldBytes + 1);
        const auto take                = rig.rig.assembler->TakeGuestImage(next);
        UNSCOPED_INFO("the newer image is taken: " + take.error);
        REQUIRE((take.image && take.image->bytes == std::vector<std::uint8_t>(16, 0x99) && take.error.empty()));
        const auto counts = rig.surfaces->TakeCounts();
        if (fails)
        {
            const auto failure = rig.rig.assembler->Failure();
            UNSCOPED_INFO("the failed older image fails the session");
            REQUIRE((take.before.empty() && failure && failure->starts_with(std::format("held_image_unwritten: op mark {}", mark))));
            UNSCOPED_INFO("only the newer image counts");
            REQUIRE((counts.flushes == 1 && counts.heldBytes == 0));
        }
        else
        {
            UNSCOPED_INFO("the older held image comes first");
            REQUIRE((take.before.size() == 1 && take.before[0].first == mark && take.before[0].second.bytes == image->image.bytes));
            UNSCOPED_INFO("both count as flushes");
            REQUIRE((counts.flushes == 2 && counts.heldBytes == 0 && !rig.rig.assembler->Failure()));
        }
        // The ledger reports what it wrote: the older image first.
        if (!fails)
            rig.rig.assembler->GuestImageWritten(mark);
        rig.rig.assembler->GuestImageWritten(next);
        std::string error;
        const bool  read = rig.read(error);
        UNSCOPED_INFO("no image is left owed to a read: " + error);
        REQUIRE((read && rig.rig.completion.peekCount == 0));
    }
    std::cout << "A written image hands over the older held images under it\n";
}

// Beyond the held bytes bound the take encodes the image for the ledger.
void surfaceHeldImageBound()
{
    HoldRig rig;
    const auto [mark, image] = rig.held(0x66, NativeFrameAssembler::kHeldBytes + 1);
    const auto take          = rig.rig.assembler->TakeGuestImage(mark);
    UNSCOPED_INFO("an image past the bound is encoded for the ledger: " + take.error);
    REQUIRE((!take.held && take.image && image->encodes == 1 && rig.written.empty()));
    const auto counts = rig.surfaces->TakeCounts();
    UNSCOPED_INFO("and counts as a flush");
    REQUIRE((counts.flushes == 1 && counts.heldBytes == 0));
    std::cout << "A held image past the bound is written when taken\n";
}

// Record arms resolve watches; writes and epoch end invalidate entries.
void aliasWiring()
{
    for (const bool castable : { false, true })
    {
        AliasRig rig([&](NativeFrameAssemblerConfig& config)
                     {
                         config.castableGuestWords = castable;
                     });
        auto&    assembler = *rig.assembler;
        UNSCOPED_INFO("the resolve submits its segment when recorded: " + rig.error);
        REQUIRE((rig.produce() == 2 && rig.completion.spans.size() == 1));
        UNSCOPED_INFO("graph segments allow typed views only with castable storage");
        REQUIRE(rig.completion.spans[0].frame->typedGuestWords == castable);
        UNSCOPED_INFO("the record arms its destination");
        REQUIRE((rig.watched.size() == 1 && rig.watched[0].first == 0x1E3B0000 && rig.watched[0].second >= 1024 * 1024 * 4));
        UNSCOPED_INFO("an alias found outside a presented build is refused as offscreen");
        REQUIRE((rig.query() == NativeResolvedTextureMiss::Offscreen && rig.missCount(NativeResolvedTextureMiss::Offscreen) == 1));
        UNSCOPED_INFO("a read before the resolve has none");
        REQUIRE(rig.query(2) == NativeResolvedTextureMiss::Unmodeled);
        auto miss = NativeResolvedTextureMiss::Unmodeled;
        UNSCOPED_INFO("another format");
        REQUIRE((!assembler.ResolvedTexture(fullCopyFetch(7), 0, miss) && miss == NativeResolvedTextureMiss::Form && rig.missCount(NativeResolvedTextureMiss::Form) == 1));
        const auto forms = assembler.ResolveGraphStats().aliasForms;
        UNSCOPED_INFO("the form miss counts as alias_form_format");
        REQUIRE((forms[0] == 1 && std::reduce(forms.begin(), forms.end(), std::uint64_t{}) == 1));
        assembler.GuestImageWritten(2);
        UNSCOPED_INFO("the landed image");
        REQUIRE(rig.query() == NativeResolvedTextureMiss::Offscreen);
        assembler.GuestWrote(0x1E3B0000 + 0x1000, 4);
        UNSCOPED_INFO("a guest write after the record");
        REQUIRE(rig.query() == NativeResolvedTextureMiss::Written);

        // A later resolve over the range supersedes it for draws between them.
        UNSCOPED_INFO("a second resolve: " + rig.error);
        REQUIRE(rig.produce() == 4);
        UNSCOPED_INFO("each draw sees the resolve before it");
        REQUIRE((rig.query(4) == NativeResolvedTextureMiss::Written && rig.query() == NativeResolvedTextureMiss::Offscreen));
        UNSCOPED_INFO("a draw between overlapping resolves");
        REQUIRE((rig.produce() == 6 && rig.query(6) == NativeResolvedTextureMiss::Superseded));
        assembler.GuestImageSkipped(6);
        UNSCOPED_INFO("a skipped image leaves the old bytes");
        REQUIRE(rig.query() == NativeResolvedTextureMiss::Written);
        UNSCOPED_INFO("a fourth resolve: " + rig.error);
        REQUIRE(rig.produce() == 8);
        assembler.ResolveUnmodeled(std::nullopt);
        UNSCOPED_INFO("an unread resolve may write anywhere");
        REQUIRE(rig.query() == NativeResolvedTextureMiss::Superseded);
    }
    {
        // A guest write between the record and the image's landing.
        AliasRig rig;
        rig.produce();
        rig.assembler->GuestWrote(0x1E3B0000, 4);
        rig.assembler->GuestImageWritten(2);
        UNSCOPED_INFO("a guest write before the image lands");
        REQUIRE(rig.query() == NativeResolvedTextureMiss::Written);
    }
    {
        // A host write over the destination, such as ledger progress.
        AliasRig rig;
        rig.produce();
        rig.assembler->GuestImageWritten(2);
        rig.assembler->HostWriting(0x1E3B0000 + 16, 4);
        rig.assembler->HostWrote(0x1E3B0000 + 16, 4);
        UNSCOPED_INFO("a host write after the image lands");
        REQUIRE(rig.query() == NativeResolvedTextureMiss::Written);
    }
    {
        // Without a write watch nothing proves the bytes stay the image's.
        AliasRig rig([](NativeFrameAssemblerConfig& config)
                     {
                         config.watchWrites = nullptr;
                     });
        rig.produce();
        UNSCOPED_INFO("an unwatched destination");
        REQUIRE(rig.query() == NativeResolvedTextureMiss::Written);
    }
    {
        // Without eager segments the resolve waits unadmitted for its flush.
        AliasRig rig([](NativeFrameAssemblerConfig& config)
                     {
                         config.eagerGraphSegments = false;
                         config.timing             = false;
                     });
        UNSCOPED_INFO("a held resolve: " + rig.error);
        REQUIRE((rig.produce() == 2 && rig.completion.spans.empty()));
        UNSCOPED_INFO("an unadmitted resolve");
        REQUIRE(rig.query() == NativeResolvedTextureMiss::Unmodeled);
        UNSCOPED_INFO("the flush submits the segment");
        REQUIRE(rig.assembler->FlushThrough(2).fence == 1);
        UNSCOPED_INFO("the admitted resolve");
        REQUIRE(rig.query() == NativeResolvedTextureMiss::Offscreen);
        UNSCOPED_INFO("without timing nothing is counted");
        REQUIRE(rig.missCount(NativeResolvedTextureMiss::Offscreen) == 0);
        rig.assembler->DiscardGuestImagesThrough(2);
        UNSCOPED_INFO("a discarded image is never written");
        REQUIRE(rig.query() == NativeResolvedTextureMiss::Written);
    }
    {
        // Without native_resolve_alias the table stays empty.
        AliasRig rig([](NativeFrameAssemblerConfig& config)
                     {
                         config.resolveAlias = false;
                     });
        rig.produce();
        UNSCOPED_INFO("aliasing off");
        REQUIRE((rig.query() == NativeResolvedTextureMiss::Unmodeled && !rig.completion.spans[0].frame->typedGuestWords && rig.watched.empty()));
    }
}

// Presented draws bind within their epoch's chain; offscreen/later do not.
void aliasBinding()
{
    AliasRig rig;
    UNSCOPED_INFO("the producer: " + rig.error);
    REQUIRE(rig.produce() == 2);
    UNSCOPED_INFO("an off-screen draw reads the bytes");
    REQUIRE((!rig.draw(1, false) && rig.misses.back() == NativeResolvedTextureMiss::Offscreen));
    rig.clear(presentedTarget());
    UNSCOPED_INFO("a presented draw binds the resolve's texture");
    REQUIRE((rig.draw(2, true) == kGraphTexture + 2 && rig.assembler->ResolveGraphStats().aliasHits == 1));
    const auto swapped = rig.swap(2);
    UNSCOPED_INFO("the epoch presents: " + swapped.detail);
    REQUIRE(swapped.status == NativeFrameStatus::Provisional);
    const auto& frame = *rig.completion.spans.back().frame;
    UNSCOPED_INFO("the terminal frame samples the chain's texture");
    REQUIRE((!rig.completion.spans.back().segment && sampledTextures(rig.completion.spans.back()) == std::vector<std::uint64_t>{ kGraphTexture + 2 } && !declares(frame, kGraphTexture + 2)));
    UNSCOPED_INFO("an open chain re-records nothing");
    REQUIRE(!rig.assembler->ResolveGraphStats().aliasReproduced);
    UNSCOPED_INFO("the next epoch does not bind");
    REQUIRE((rig.query() == NativeResolvedTextureMiss::Retired && !rig.draw(3, true) && rig.misses.back() == NativeResolvedTextureMiss::Retired));
    UNSCOPED_INFO("no failure");
    REQUIRE(!rig.assembler->Failure());
}

// What starts a frame: the re-recorded producer of `mark`, without a readback.
void requireReproduced(const NativeCompletionSpan& span, std::uint64_t mark, const std::string& what)
{
    UNSCOPED_INFO(what + ": a frame with a leading resolve pass");
    REQUIRE((span.frame && !span.frame->passes.empty() && span.frame->passes.front().resolves.size() == 1));
    const auto& resolve = span.frame->passes.front().resolves[0];
    UNSCOPED_INFO(what + ": the producer re-records into its texture without a readback");
    REQUIRE((resolve.textureId == kGraphTexture + mark && !resolve.readback && declares(*span.frame, kGraphTexture + mark) &&
             span.resolves.empty() == !span.segment));
}

// Legacy FlushThrough ends graph; next chain records orphan producers.
void aliasFlushReset()
{
    for (const bool nextChain : { false, true })
    {
        AliasRig rig;
        auto&    assembler = *rig.assembler;
        rig.produce();
        rig.clear(presentedTarget());
        UNSCOPED_INFO("the presented draw binds");
        REQUIRE(rig.draw(1, true) == kGraphTexture + 2);
        UNSCOPED_INFO("the presented draw joins the chain's segment");
        REQUIRE(assembler.FlushThrough(4).fence);
        rig.clear(signedTarget());
        const auto            reset = rig.resolve(signedTarget(), signedCopy());
        completion::DrawFlush flush;
        for (unsigned attempt = 0; attempt < 4 && !flush.fence; ++attempt)
            flush = assembler.FlushThrough(reset);
        UNSCOPED_INFO("the k_16_16 resolve is a legacy frame: " + flush.error);
        REQUIRE((flush.fence && rig.completion.spans.back().resolveMark == reset && !rig.completion.spans.back().segment));
        UNSCOPED_INFO("a presented draw binds the orphaned resolve while no chain is open");
        REQUIRE(rig.draw(2, true) == kGraphTexture + 2);
        if (nextChain)
        {
            auto copy     = fullCopy(offscreen(0x300));
            copy.destBase = 0x1D000000;
            rig.clear(offscreen(0x300));
            UNSCOPED_INFO("a graph resolve opens the next chain");
            REQUIRE((rig.resolve(offscreen(0x300), copy) && rig.completion.spans.back().segment));
            requireReproduced(rig.completion.spans.back(), 2, "the next chain's first segment");
            UNSCOPED_INFO("the adopted resolve binds in the next chain");
            REQUIRE(rig.draw(3, true) == kGraphTexture + 2);
        }
        else
        {
            UNSCOPED_INFO("the orphaned resolve stays live");
            REQUIRE(rig.query() == NativeResolvedTextureMiss::Offscreen);
        }
        const auto swapped = rig.swap(nextChain ? 3 : 2);
        UNSCOPED_INFO("the epoch presents: " + swapped.detail);
        REQUIRE(swapped.status == NativeFrameStatus::Provisional);
        if (!nextChain)
            requireReproduced(rig.completion.spans.back(), 2, "the chainless terminal frame");
        UNSCOPED_INFO("one producer is re-recorded once");
        REQUIRE((rig.assembler->ResolveGraphStats().aliasReproduced == 1 && !assembler.Failure()));
    }
    {
        // An unbound resolve of the ended chain is refused in the next one.
        AliasRig rig;
        rig.produce();
        rig.clear(signedTarget());
        const auto            reset = rig.resolve(signedTarget(), signedCopy());
        completion::DrawFlush flush;
        for (unsigned attempt = 0; attempt < 4 && !flush.fence; ++attempt)
            flush = rig.assembler->FlushThrough(reset);
        UNSCOPED_INFO("the legacy resolve frame: " + flush.error);
        REQUIRE(flush.fence);
        auto copy     = fullCopy(offscreen(0x300));
        copy.destBase = 0x1D000000;
        rig.clear(offscreen(0x300));
        rig.resolve(offscreen(0x300), copy);
        UNSCOPED_INFO("the unbound orphan is refused as reset");
        REQUIRE((rig.query() == NativeResolvedTextureMiss::Reset && !rig.assembler->ResolveGraphStats().aliasReproduced));
    }
}

// A nongraph swap resolve ends its chain; terminal re-records its producer.
void aliasSwapReset()
{
    AliasRig rig;
    rig.produce();
    rig.clear(presentedTarget());
    UNSCOPED_INFO("the presented draw binds");
    REQUIRE(rig.draw(1, true) == kGraphTexture + 2);
    rig.clear(signedTarget());
    const auto reset   = rig.resolve(signedTarget(), signedCopy());
    const auto swapped = rig.swap(1);
    UNSCOPED_INFO("the epoch presents: " + swapped.detail);
    REQUIRE(swapped.status == NativeFrameStatus::Provisional);
    const auto& spans = rig.completion.spans;
    UNSCOPED_INFO("the segment, the fallback resolve frame and the terminal frame");
    REQUIRE((spans.size() == 3 && spans[1].resolveMark == reset));
    requireReproduced(spans[2], 2, "the terminal frame after the fallback");
    UNSCOPED_INFO("the terminal frame samples the re-recorded texture");
    REQUIRE(sampledTextures(spans[2]) == std::vector<std::uint64_t>{ kGraphTexture + 2 });
    UNSCOPED_INFO("one producer is re-recorded");
    REQUIRE((rig.assembler->ResolveGraphStats().aliasReproduced == 1 && !rig.assembler->Failure()));
}

// Abandoned census chains re-record submitted producers in the open epoch.
void aliasAbandonedEpoch()
{
    AliasRig rig([](NativeFrameAssemblerConfig& config)
                 {
                     config.mode = NativeDrawMode::Census;
                 });
    rig.produce();
    rig.clear(presentedTarget());
    UNSCOPED_INFO("the presented draw binds");
    REQUIRE(rig.draw(1, true) == kGraphTexture + 2);
    UNSCOPED_INFO("the bound draw is submitted in the chain");
    REQUIRE(rig.assembler->FlushThrough(4).fence);
    rig.completion.refuse = true;
    const auto refused    = rig.draw(2, true);
    UNSCOPED_INFO("a later segment is refused and its census flush abandoned");
    REQUIRE((refused == kGraphTexture + 2 && !rig.assembler->FlushThrough(5).fence));
    rig.completion.refuse = false;
    UNSCOPED_INFO("the open epoch keeps its resolve");
    REQUIRE(rig.query() == NativeResolvedTextureMiss::Offscreen);
    auto copy     = fullCopy(offscreen(0x300));
    copy.destBase = 0x1D000000;
    rig.clear(offscreen(0x300));
    rig.resolve(offscreen(0x300), copy);
    requireReproduced(rig.completion.spans.back(), 2, "the next chain, which replays the submitted bound draw");
    UNSCOPED_INFO("the producer is re-recorded once");
    REQUIRE(rig.assembler->ResolveGraphStats().aliasReproduced == 1);
    const auto swapped = rig.swap(2);
    UNSCOPED_INFO("the epoch ends: " + swapped.detail);
    REQUIRE(swapped.status != NativeFrameStatus::Failed);
}

// Swap during a bound draw's recording makes the next epoch fail by name.
void aliasEpochCrossed()
{
    AliasRig rig;
    rig.produce();
    rig.clear(presentedTarget());
    auto value          = submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 5);
    value.operationMark = rig.assembler->NumberOperation();
    rig.assembler->Began(value);
    UNSCOPED_INFO("the draw binds while it is built");
    REQUIRE(rig.bound.back() == kGraphTexture + 2);
    scanOut(*rig.assembler);
    UNSCOPED_INFO("the epoch closes before the draw is numbered");
    REQUIRE(rig.assembler->Swap(makeSwap(1)).status != NativeFrameStatus::Failed);
    const auto verdict = rig.assembler->Admit(value);
    rig.assembler->Recorded(value, verdict);
    scanOut(*rig.assembler);
    const auto crossed = rig.assembler->Swap(makeSwap(2, 1));
    UNSCOPED_INFO("the next epoch fails by name: " + crossed.detail);
    REQUIRE((crossed.status == NativeFrameStatus::Failed && crossed.detail.find("alias_epoch_crossed: bound in epoch 1") != std::string::npos));
}

// After swap, another-slot resolve is retired; crossing epochs fails by name.
void aliasEpochSlots()
{
    AliasRig rig;
    rig.produce();
    rig.clear(presentedTarget());
    rig.between = [&rig]
    {
        scanOut(*rig.assembler);
        UNSCOPED_INFO("the epoch closes during the build");
        REQUIRE(rig.assembler->Swap(makeSwap(1)).status != NativeFrameStatus::Failed);
        rig.produce();
    };
    // Live builds see op mark 0, after every recorded operation.
    auto value          = submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 5);
    value.operationMark = 0;
    rig.assembler->Began(value);
    UNSCOPED_INFO("the second slot does not bind the next epoch's resolve");
    REQUIRE((rig.bound == std::vector<std::uint64_t>{ kGraphTexture + 2, 0 } && rig.misses.back() == NativeResolvedTextureMiss::Retired));
    rig.assembler->Recorded(value, rig.assembler->Admit(value));
    scanOut(*rig.assembler);
    const auto crossed = rig.assembler->Swap(makeSwap(2, 1));
    UNSCOPED_INFO("the next epoch fails by name: " + crossed.detail);
    REQUIRE((crossed.status == NativeFrameStatus::Failed && crossed.detail.find("alias_epoch_crossed: bound in epoch 1") != std::string::npos));
}

// A swap during build leaves stale tiled state unable to bind the next epoch.
void aliasEpochBuilt()
{
    AliasRig rig;
    rig.produce();
    rig.clear(presentedTarget());
    rig.ahead = [&rig]
    {
        scanOut(*rig.assembler);
        UNSCOPED_INFO("the epoch closes during the build");
        REQUIRE(rig.assembler->Swap(makeSwap(1)).status != NativeFrameStatus::Failed);
        rig.produce();
    };
    auto value          = submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 5);
    value.operationMark = 0;
    rig.assembler->Began(value);
    UNSCOPED_INFO("the draw does not bind the next epoch's resolve");
    REQUIRE((rig.bound.back() == 0 && rig.misses.back() == NativeResolvedTextureMiss::Retired));
    rig.assembler->Recorded(value, rig.assembler->Admit(value));
    UNSCOPED_INFO("the next epoch's resolve stays live");
    REQUIRE(rig.query() == NativeResolvedTextureMiss::Offscreen);
}

// Census skips after epoch close replay nothing, so crossing cannot fail.
void aliasEpochSkipped()
{
    AliasRig rig([](NativeFrameAssemblerConfig& config)
                 {
                     config.mode = NativeDrawMode::Census;
                 });
    rig.produce();
    rig.clear(presentedTarget());
    auto value          = submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, 5);
    value.operationMark = rig.assembler->NumberOperation();
    rig.assembler->Began(value);
    UNSCOPED_INFO("the draw binds while it is built");
    REQUIRE(rig.bound.back() == kGraphTexture + 2);
    scanOut(*rig.assembler);
    UNSCOPED_INFO("the epoch closes before the draw is numbered");
    REQUIRE(rig.assembler->Swap(makeSwap(1)).status != NativeFrameStatus::Failed);
    rig.assembler->Recorded(value, NativeDrawReason::NoNativeRoute);
    scanOut(*rig.assembler);
    const auto next = rig.assembler->Swap(makeSwap(2, 1));
    UNSCOPED_INFO("a skipped draw does not cross: " + next.detail);
    REQUIRE((next.status != NativeFrameStatus::Failed && next.detail.find("alias_epoch_crossed") == std::string::npos));
}

// Packed 2_10_10_10 needs castable storage; otherwise format 54 misses.
void aliasCastable()
{
    for (const bool castable : { false, true })
    {
        AliasRig rig([&](NativeFrameAssemblerConfig& config)
                     {
                         config.castableGuestWords = castable;
                     });
        rig.fetch = { 2U | (20U << 22) | (1U << 31), 54U | (2U << 6) | 0x1E3B0000U, 639 | (359U << 13), kAliasIdentity, 0, 1U << 9 };
        GuestTarget source;
        source.native    = { 640, 368, 4, NativeDrawReplayTargetFormat::Rgb10a2, 0x300 };
        auto copy        = fullCopy(source);
        copy.copyControl = 0x100040;
        copy.destInfo    = 0x382; // format 7, endian k_8in32 as the fetch reads it
        copy.destPitch   = (360U << 16) | 640U;
        copy.rectHeight  = 360;
        copy.destFetch   = rig.fetch;
        rig.clear(source);
        const auto  mark   = rig.resolve(source, copy);
        const auto& span   = rig.completion.spans.back();
        const bool  packed = span.frame && std::ranges::any_of(span.frame->passes, [](const NativeRenderFramePass& pass)
                                                               {
                                                                  return std::ranges::any_of(pass.resolves, [](const auto& resolve)
                                                                                             {
                                                                                                 return resolve.guestWords == NativeGuestWordPacking::Color2101010;
                                                                                             });
                                                               });
        UNSCOPED_INFO("the resolve writes Color2101010 words into a segment");
        REQUIRE((span.segment && packed && span.frame->typedGuestWords == castable));
        GuestResolveImage image;
        std::string       error;
        const bool        encodes = span.resolves.size() == 1 && EncodeGuestResolve(span.resolves[0].copy, std::vector<std::uint8_t>(640 * 360 * 4), 640 * 4, image, error);
        UNSCOPED_INFO("the guest encoder takes the copy: " + error);
        REQUIRE(encodes);
        rig.clear(presentedTarget());
        const auto bound = rig.draw(1, true);
        if (castable)
        {
            UNSCOPED_INFO("with castable storage the packed resolve binds");
            REQUIRE(bound == kGraphTexture + mark);
        }
        else
        {
            UNSCOPED_INFO("without castable storage it misses as packing");
            REQUIRE((!bound && rig.misses.back() == NativeResolvedTextureMiss::Packing && rig.missCount(NativeResolvedTextureMiss::Packing) == 1));
        }
    }
}

// Before presented work or scanout, draws cannot bind the tiled pass.
void aliasTiledPass()
{
    const auto run = [](bool duringBuild, bool alias)
    {
        FakeCompletion completion;
        auto           config     = menuConfig(completion);
        config.resolveGraph       = true;
        config.eagerGraphSegments = true;
        config.resolveAlias       = alias;
        config.watchWrites        = [](std::uint32_t, std::uint32_t)
        {
        };
        NativeFrameAssembler* current = nullptr;
        bool                  bound   = false;
        config.build                  = [&current, &bound, inner = config.build](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string& detail)
        {
            if (!inner(value, draw, detail))
                return false;
            auto miss = NativeResolvedTextureMiss::Form;
            if (const auto texture = current->ResolvedTexture(fullCopyFetch(), value.operationMark, miss))
            {
                draw.textureMask = 1;
                draw.textures[0] = BindNativeResolvedTexture(*texture);
                bound            = true;
            }
            return true;
        };
        NativeFrameAssembler assembler(config);
        current = &assembler;
        std::string error;
        bool        partial = false;
        auto        copy    = fullCopy(offscreen(0x300));
        copy.destFetch      = fullCopyFetch();
        UNSCOPED_INFO("the producer: " + error);
        REQUIRE((assembler.Clear(fullClear(offscreen(0x300)), error) && assembler.Resolve(offscreen(0x300), copy, false, partial, error)));
        if (duringBuild)
        {
            // Built with no presented work yet, numbered after the clear.
            auto value          = submission(NativeDrawHook::IndexedUp, kPanelCaller, 1, kMenuScene[0]);
            value.operationMark = 0;
            assembler.Began(value);
            UNSCOPED_INFO("the pass clear: " + error);
            REQUIRE(assembler.Clear(tiledClear(), error));
            assembler.Recorded(value, assembler.Admit(value));
        }
        else
        {
            UNSCOPED_INFO("the pass clear: " + error);
            REQUIRE(assembler.Clear(tiledClear(), error));
            record(assembler, 1, kMenuScene[0]);
        }
        UNSCOPED_INFO("the pass draw does not bind the resolve");
        REQUIRE(!bound);
        error.clear();
        const bool formed = assembler.Resolve(tiledSlot(0, kGuestDepthSlot), textureCopy(0, 0x00100014), false, partial, error);
        return std::pair{ formed, error };
    };
    const auto pass = run(false, true);
    UNSCOPED_INFO("the pass forms: " + pass.second);
    REQUIRE(pass.first);
    for (const bool duringBuild : { false, true })
    {
        const auto aliased = run(duringBuild, true);
        const auto plain   = run(duringBuild, false);
        UNSCOPED_INFO("the pass resolves as without aliasing: " + aliased.second + " versus " + plain.second);
        REQUIRE(aliased == plain);
    }
}

} // namespace

// Alias only proven destinations; each missing eligibility fact names its miss.
void testResolveAliasTable()
{
    aliasHits();
    aliasSwizzles();
    aliasForms();
    aliasCoverage();
    aliasLifetimes();
    aliasBound();
    aliasAdmission();
    aliasWiring();
    surfaceSkippedImage();
    surfaceUnsubmittedResolve();
    surfaceHeldImageRead();
    surfaceHeldImageLock();
    surfaceHeldImageEndings();
    surfaceImagesBeforeTake();
    surfaceHeldImageBound();
    surfaceHeldImageStores();
    surfaceHeldImageBefore();
    aliasBinding();
    aliasFlushReset();
    aliasSwapReset();
    aliasAbandonedEpoch();
    aliasEpochCrossed();
    aliasEpochSlots();
    aliasEpochBuilt();
    aliasEpochSkipped();
    aliasTiledPass();
    aliasCastable();
}

// Replays the world harvest's frame forms. With no names every captured frame
// replays; a malformed or missing frame.toml or payload names itself instead of
// ending the process unhandled.
void runHarvest(const std::filesystem::path& root, std::vector<std::string> names)
{
    try
    {
        if (!names.empty())
        {
            CHECK(worldHarvestFrames(root, std::move(names)) == 0);
            return;
        }
        const auto dead = harvestFrames(root);
        CHECK((dead ? dead : worldHarvestFrames(root, {})) == 0);
    }
    catch (const std::exception& exception)
    {
        FAIL("harvest " << root.string() << ": " << exception.what());
    }
}

TEST_CASE("native_frame_assembler", "[ctest][native-frame-assembler][timeout-60]")
{
    testClearResolveCapture();
    testOneEpochOneFrame();
    testEpochOrdering();
    testDrawThreads();
    testStrictRejection();
    testCensusRecovery();
    testSkipPartial();
    testEmptyEpoch();
    testDeferredSwapEpoch();
    testDisplayedImage();
    testGammaAdoption();
    testWriteNotice();
    testSwapAndCompletionFailures();
    testDispatcherSeam();
    testHeldThreadExit();
    testMidEpochSegments();
    testCpuCompletionSource();
    testSolidPanelInputs();
    testResolveSourceOrder();
    testSharedRecipes();
    testResolveThenSegment();
    testSwapAfterResolve();
    testRejectedClearPartial();
    testMainBaseCarried();
    testMainKeptAcrossResolve();
    testResolveGraph();
    testEagerGraphSegments();
    testGraphCensusRecovery();
    testTargetDecodeReuse();
    testUndecodedTargets();
    testUnreadableClear();
    testFrontbufferSource();
    testFrontbufferDecodedExtent();
    testScanoutSwap();
    testPresentedBaseUnset();
    testSkippedPresentedClear();
    testResolveClearsSource();
    testGuestImageForwarding();
    testTilingMapping();
    testClearRefusalCodes();
    testTiledClearAdmission();
    testTiledScanout();
    testTiledTargetUncleared();
    testPresentedWritesBeforeTiledClear();
    testHeldWritesRelease();
    testPresentedFlag();
    testDepthClears();
    testDepthDraws();
    testPresentedAlias();
    testOffscreenDepthDefined();
    testMrtInheritedDepth();
    testOffscreenDepthIdentity();
    testDrawPredicate();
    testScanoutInProgress();
    testUnreadableTilingState();
    testMenuFrame();
    testTiledBackgrounds();
    testTiledClearAfterWork();
    testDepthClearPredicated();
    testDepthClearRect();
    testStencilClearRect();
    testPredicatedUntiled();
    testResolvePresented();
    testTiledTexturePass();
    testEagerTiledSegments();
    testResolvedReads();
    testResolvedTextureQuery();
    testResolvedReadDuringTake();
    testResolvedReadAfterRetire();
    testMirroredReads();
    testMirroredReadBudget();
    testWatchedMirrorReads();
    testWatchedMirrorHostWriteInFlight();
    testSmallReadCounts();
    testSmallMirroredReads();
    testUnwrittenResolveLimit();
    testOffscreenResolveForms();
    testSourceFormatResolves();
    testSignedTargetDraws();
    testPackedTargetClears();
    testSubExtentClears();
    testTiledOverwrite();
    testOverwrittenSources();
    testSceneQuadCoverage();
    testSceneCompositeCoverage();
    testMrtPass();
    testMrtEndedSetWrites();
    testMrtEndedDepthClears();
    testMrtThenPresented();
    testMrtRefusals();
    testMrtSetDecode();
    testMrtRules();
    testMrtResolveOrder();
    testMrtBetweenTileScanouts();
    testMrtClearDepthOverlap();
    testMrtEndedSetRestrictions();
    testMrtEndedSetShaders();
    testResolveAliasTable();
}

TEST_CASE("native_frame_assembler_harvest", "[.][renderer-research][native-frame-assembler][timeout-300]")
{
    const char* variable = std::getenv("REREVVED_WORLD_HARVEST");
    if (!variable || !*variable)
    {
        SKIP("Set REREVVED_WORLD_HARVEST to a world harvest with frame forms to run this check");
    }
    runHarvest(variable, {});
}

// Replays chosen world harvest frames:
//   native-frame-assembler.harvest [--root <dir>] [--fixtures <list>] [frame...]
// The root defaults to REREVVED_WORLD_HARVEST.
TEST_CASE("native_frame_assembler_harvest_selected", "[.][manual][native-frame-assembler]")
{
    auto&                    args     = rerevved::tests::CurrentManualArgv();
    const char*              variable = std::getenv("REREVVED_WORLD_HARVEST");
    std::filesystem::path    root     = variable ? variable : "";
    std::vector<std::string> names;
    for (int i = 1; i <= args.argc(); ++i)
    {
        const std::string_view argument = args[i];
        UNSCOPED_INFO(std::string(argument) + " needs a path");
        REQUIRE(!((argument == "--root" || argument == "--fixtures") && i == args.argc()));
        if (argument == "--root")
            root = args.argv()[++i];
        else if (argument == "--fixtures")
        {
            std::ifstream list(args.argv()[++i]);
            UNSCOPED_INFO(std::format("the fixture list {} opens", args.argv()[i]));
            REQUIRE(list.good());
            for (std::string line; std::getline(list, line);)
            {
                line             = line.substr(0, line.find('#'));
                const auto first = line.find_first_not_of(" \t\r");
                if (first != std::string::npos)
                    names.push_back(line.substr(first, line.find_last_not_of(" \t\r") + 1 - first));
            }
        }
        else
            names.emplace_back(argument);
    }
    if (root.empty())
    {
        SKIP("Set REREVVED_WORLD_HARVEST to a world harvest with frame forms to run this check");
    }
    runHarvest(root, std::move(names));
}

REREVVED_MANUAL_HELPER(nativeFrameAssemblerHarvest, "native-frame-assembler.harvest", "native_frame_assembler_harvest_selected")
