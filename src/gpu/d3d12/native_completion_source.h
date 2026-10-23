#pragma once

#include "gpu/guest_d3d/native_frame_assembler.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rerevved::gpu
{

// The renderer stream a completion source drives. The guest-mode presentation
// session implements it over its producer; tests supply a fake.
class INativeFrameProducer
{
public:
    virtual ~INativeFrameProducer() = default;
    // A terminal frame. Its completion settles after native completion and,
    // for a GPU output, after the lease was published.
    virtual bool QueueFrame(NativeRenderFrame frame, std::shared_future<NativeDrawReplayResult>& completion, std::string& error) = 0;
    // The producer records requests in call order, so prefix passes queued
    // after a terminal frame start a new chain once that frame was recorded,
    // whether or not it completed.
    virtual bool QueuePasses(NativeRenderFrame passes, std::shared_future<NativeDrawReplayResult>& recording, std::string& error) = 0;
    // The stream fence's completed value; UINT64_MAX after device removal.
    virtual std::uint64_t CompletedFence() const = 0;

    // Waits until CompletedFence reaches value or the deadline passes, and
    // returns CompletedFence. This default polls; a producer with the fence
    // object waits on its completion event instead.
    virtual std::uint64_t WaitFence(std::uint64_t value, std::chrono::steady_clock::time_point deadline) const
    {
        for (;;)
        {
            const auto fence = CompletedFence();
            if (fence >= value || std::chrono::steady_clock::now() >= deadline)
                return fence;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

struct D3D12CompletionSourceConfig
{
    std::shared_ptr<INativeFrameProducer> producer;
    // Called once for a device or runtime failure, from whichever thread
    // observed it. It may log or post UI work, but must not enter the ledger.
    std::function<void(const std::string&)> failed;
    // Requests gamma readback for only the first two terminal GPU outputs.
    // Delivered by ObserveCompletedOutputs after the output lease was published.
    std::function<void(const NativeDrawReplayResult&)> observeOutput;
    // Census skips only no-submit failures outside a prefix chain, queuing
    // TakeSkippedFailure without an image. Other host-work failure or device
    // loss fails the source and stops renderer stream.
    bool census = false;
    // native_frame_timing: count guest image encodes and their time.
    bool timeEncodes = false;
};

// Submit values strictly increase: prefix segments then sole-publishing
// terminal; frameless spans complete with predecessor. Completed never waits
// for GPU. Close serializes polling, freezes value, prevents polls.
class D3D12CompletionSource final : public guest_d3d::INativeCompletionSource
{
public:
    explicit D3D12CompletionSource(D3D12CompletionSourceConfig config);
    ~D3D12CompletionSource() override;

    D3D12CompletionSource(const D3D12CompletionSource&)            = delete;
    D3D12CompletionSource& operator=(const D3D12CompletionSource&) = delete;

    std::uint64_t Submit(guest_d3d::NativeCompletionSpan span, std::string& error) override;
    std::uint64_t Completed() const override;
    // Encodes the image the resolve at opMark read back. After Close the take
    // reports closing, not a failure. It also discards every older image: the
    // ledger applies Resolve records in op-mark order, so none is taken later.
    completion::GuestImageTake TakeGuestImage(std::uint64_t opMark) override;
    // Takes the image as TakeGuestImage does, leaving its encode to `held`,
    // which counts it with the source's encodes when it runs.
    completion::GuestImageTake HoldGuestImage(std::uint64_t opMark, std::shared_ptr<const guest_d3d::IHeldGuestImage>& held) override;
    // Encodes a kept or in-flight image without taking it, so the ledger's
    // later take, and those of older images, still succeed. The take reuses
    // the peeked encoding: both encode the same completed readback.
    completion::GuestImageTake PeekGuestImage(std::uint64_t opMark, std::chrono::milliseconds timeout) override;
    // Frees images at or below opMark, including readbacks still in flight;
    // taking one then fails as discarded.
    void DiscardGuestImagesThrough(std::uint64_t opMark) override;
    bool WaitFor(std::uint64_t value, std::chrono::milliseconds timeout) override;

    // Swap queues resolves ahead of the ledger, which may hold their spans on
    // a guest wait, so untaken images are kept until taken or discarded. Beyond
    // this many (about 4 MiB each at 1024x1024) one warning reports the memory.
    static constexpr std::size_t kGuestImageAlarm = 16;

    // Drain diagnostic results outside the ledger lock and the polling path.
    // Call on the completion worker after Pump; callbacks hold no source lock.
    void ObserveCompletedOutputs();

    // Refuses later Submit calls with kNativeCompletionClosing. Completed()
    // stops advancing, so spans still held never release.
    void                       Close();
    std::optional<std::string> Failure() const;
    // Census mode: the oldest host-work failure not yet taken.
    std::optional<std::string> TakeSkippedFailure();

    // Guest resolve encodes by takes and peeks, and their total time; zero
    // unless the config times encodes.
    std::pair<std::uint64_t, std::uint64_t> GuestImageEncodes() const noexcept
    {
        return { encodeCounts->encodes.load(std::memory_order_relaxed), encodeCounts->nanoseconds.load(std::memory_order_relaxed) };
    }

    // Peeks of graph resolves whose segment was still pending, and their
    // total wait; zero unless the config times encodes.
    std::pair<std::uint64_t, std::uint64_t> GraphPeekWaits() const noexcept
    {
        return { graphPeeks.load(std::memory_order_relaxed), graphPeekNanoseconds.load(std::memory_order_relaxed) };
    }

    // The part of GraphPeekWaits spent before the segment's recording settled;
    // the rest waited for its GPU fence.
    std::uint64_t GraphPeekRecordingNanoseconds() const noexcept
    {
        return graphPeekRecordingNanoseconds.load(std::memory_order_relaxed);
    }

private:
    enum class Kind : std::uint8_t
    {
        Inherit,  // no host work; completes with its predecessor
        Prefix,   // completes when the stream fence reaches the recorded value
        Terminal, // completes when the frame's completion settles
    };

    struct Pending
    {
        std::uint64_t                              value    = 0;
        Kind                                       kind     = Kind::Inherit;
        bool                                       diagnose = false;
        bool                                       chained  = false; // a terminal that closes its epoch's prefix
        std::shared_future<NativeDrawReplayResult> work;
        std::uint64_t                              resolveMark = 0;
        std::optional<guest_d3d::GuestResolveCopy> guestResolve;
        std::vector<guest_d3d::NativeSpanResolve>  resolves; // a segment's graph resolves
    };

    // A completed raw readback awaiting the ledger's Resolve record: a
    // terminal's raw output, or a segment's readback of one graph resolve.
    struct GuestImage
    {
        guest_d3d::GuestResolveCopy                copy;
        std::shared_future<NativeDrawReplayResult> work;
        std::shared_ptr<const NativeRawReadback>   readback;
    };

    // Shared with held images, which may encode after the source is gone.
    struct EncodeCounts
    {
        std::atomic<std::uint64_t> encodes{ 0 }, nanoseconds{ 0 };
    };

    class HeldImage;

    // Under pendingMutex: whether a pending span still produces `mark`, which
    // resolve copy it is, and for a graph resolve its destination texture.
    const guest_d3d::GuestResolveCopy* pendingResolve(std::uint64_t mark, const Pending** owner = nullptr, std::uint64_t* textureId = nullptr) const;

    std::optional<std::string> advance() const;
    void                       fail(std::string detail) const;
    void                       encode(const GuestImage& image, completion::GuestImageTake& take);
    // Removes the image at `mark` and every older one; true when `entry`
    // still needs its encode, false when `take` is complete.
    bool takeImage(std::uint64_t mark, completion::GuestImageTake& take, GuestImage& entry);

    D3D12CompletionSourceConfig   config;
    mutable std::mutex            submitMutex;
    std::shared_ptr<EncodeCounts> encodeCounts = std::make_shared<EncodeCounts>();
    std::atomic<std::uint64_t>    graphPeeks{ 0 }, graphPeekNanoseconds{ 0 }, graphPeekRecordingNanoseconds{ 0 };
    std::uint64_t                 lastValue         = 0;
    unsigned                      diagnosticOutputs = 0;
    // The open epoch's recorded segment passes: their count, which the
    // terminal continues, and the target the last one ended with. The
    // renderer's prefix state keeps the passes, textures and typed flag.
    std::uint64_t                                epoch          = 0;
    std::size_t                                  epochPassCount = 0;
    std::optional<NativeRenderFramePass::Target> epochLastTarget;

    mutable std::mutex                          pendingMutex;
    mutable std::deque<Pending>                 pending;
    mutable std::map<std::uint64_t, GuestImage> images;
    // Encoded peeks of images not yet taken, keyed by op mark.
    std::map<std::uint64_t, guest_d3d::GuestResolveImage> peeked;
    // Op marks at or below this were discarded or precede a taken image.
    mutable std::uint64_t                                           discardedThrough = 0;
    mutable bool                                                    alarmLogged      = false;
    mutable std::vector<std::shared_future<NativeDrawReplayResult>> outputObservations;
    mutable std::atomic<std::uint64_t>                              completed{ 0 };
    std::atomic<bool>                                               closed{ false };
    mutable std::atomic<bool>                                       failed{ false };
    mutable std::mutex                                              failureMutex;
    mutable std::optional<std::string>                              failure;
    mutable std::deque<std::string>                                 skippedFailures; // census mode, under pendingMutex
};

} // namespace rerevved::gpu
