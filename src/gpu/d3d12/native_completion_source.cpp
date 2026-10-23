#include "native_completion_source.h"

#include <rex/logging.h>

#include <algorithm>
#include <exception>
#include <format>
#include <iterator>
#include <limits>
#include <thread>

namespace rerevved::gpu
{

using guest_d3d::kNativeCompletionClosing;
using guest_d3d::NativeCompletionSpan;

namespace
{

// Encodes a completed raw readback as the resolve's guest bytes, or names why
// it cannot be. A graph resolve reads its segment's readback handle, which
// the segment's fence already covers; a segment result has no raw output.
void encodeImage(const guest_d3d::GuestResolveCopy&                copy,
                 const std::shared_future<NativeDrawReplayResult>& work,
                 const std::shared_ptr<const NativeRawReadback>&   readback,
                 completion::GuestImageTake&                       take)
{
    try
    {
        const auto& result = work.get();
        const auto  encode = [&](const std::vector<std::uint8_t>& texels, std::uint32_t rowPitch)
        {
            guest_d3d::GuestResolveImage image;
            std::string                  error;
            if (guest_d3d::EncodeGuestResolve(copy, texels, rowPitch, image, error))
                take.image = std::move(image);
            else
                take.error = "guest_resolve_encode_failed: " + error;
        };
        std::vector<std::uint8_t> texels;
        std::uint32_t             rowPitch = 0;
        std::string               error;
        if (!result.success)
            take.error = "guest_resolve_readback_failed: " + result.error;
        else if (readback && !readback->Read(texels, rowPitch, error))
            take.error = "guest_resolve_readback_failed: " + error;
        else if (readback)
            encode(texels, rowPitch);
        else if (result.rawOutput.empty())
            take.error = "guest_resolve_readback_missing";
        else
            encode(result.rawOutput, result.rawRowPitch);
    }
    catch (const std::exception& exception)
    {
        take.error = std::string("guest_resolve_readback_failed: ") + exception.what();
    }
    catch (...)
    {
        take.error = "guest_resolve_readback_failed";
    }
}

// Times one encode into `counts` when `timed`.
template <typename Counts>
void timedEncode(bool timed, Counts& counts, const guest_d3d::GuestResolveCopy& copy, const std::shared_future<NativeDrawReplayResult>& work, const std::shared_ptr<const NativeRawReadback>& readback, completion::GuestImageTake& take)
{
    if (!timed)
    {
        encodeImage(copy, work, readback, take);
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    encodeImage(copy, work, readback, take);
    counts.nanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
    counts.encodes.fetch_add(1, std::memory_order_relaxed);
}

// The readback a recorded prefix returned for a resolve destination.
std::shared_ptr<const NativeRawReadback> findReadback(const NativeDrawReplayResult& result, std::uint64_t textureId)
{
    for (const auto& [id, readback] : result.rawReadbacks)
        if (id == textureId)
            return readback;
    return {};
}

} // namespace

// A taken readback whose encode waits for a guest lock or a flush.
class D3D12CompletionSource::HeldImage final : public guest_d3d::IHeldGuestImage
{
public:
    HeldImage(GuestImage image, std::shared_ptr<EncodeCounts> counts, bool timed)
    : image(std::move(image))
    , counts(std::move(counts))
    , timed(timed)
    {
    }

    completion::GuestImageTake Encode() const override
    {
        completion::GuestImageTake take;
        timedEncode(timed, *counts, image.copy, image.work, image.readback, take);
        return take;
    }

    std::uint64_t Bytes() const noexcept override
    {
        try
        {
            if (image.readback)
                return image.readback->Bytes();
            return image.work.valid() ? image.work.get().rawOutput.size() : 0;
        }
        catch (...)
        {
            return 0;
        }
    }

private:
    GuestImage                    image;
    std::shared_ptr<EncodeCounts> counts;
    bool                          timed = false;
};

D3D12CompletionSource::D3D12CompletionSource(D3D12CompletionSourceConfig value)
: config(std::move(value))
{
}

D3D12CompletionSource::~D3D12CompletionSource() = default;

std::uint64_t D3D12CompletionSource::Submit(NativeCompletionSpan span, std::string& error)
{
    std::lock_guard serialized(submitMutex);
    // A failure while closing is the close itself, not a renderer failure.
    const auto refuse = [&](std::string detail)
    {
        error = closed.load(std::memory_order_acquire) ? std::string(kNativeCompletionClosing) : std::move(detail);
        return std::uint64_t(0);
    };
    if (closed.load(std::memory_order_acquire))
        return refuse({});
    if (failed.load(std::memory_order_acquire))
        return refuse("completion_failed: " + Failure().value_or(std::string{}));
    if (!config.producer)
        return refuse("no_frame_producer");
    if (!span.resolves.empty() && (!span.segment || !span.frame || span.guestResolve))
        return refuse("graph_resolves_outside_segment");
    if (span.guestResolve || !span.resolves.empty())
    {
        // A cached peek encodes the readback already held under this mark.
        std::vector<std::uint64_t> marks;
        if (span.guestResolve)
            marks.push_back(span.resolveMark);
        for (const auto& resolve : span.resolves)
            marks.push_back(resolve.mark);
        std::lock_guard lock(pendingMutex);
        for (std::size_t index = 0; index < marks.size(); ++index)
            if (images.contains(marks[index]) || peeked.contains(marks[index]) || pendingResolve(marks[index]) ||
                std::find(marks.begin(), marks.begin() + index, marks[index]) != marks.begin() + index)
                return refuse("resolve_mark_reused");
    }
    // Each graph resolve names a pass resolve that reads its destination back.
    for (const auto& resolve : span.resolves)
        if (std::none_of(span.frame->passes.begin(), span.frame->passes.end(), [&](const NativeRenderFramePass& pass)
                         {
                             return std::any_of(pass.resolves.begin(), pass.resolves.end(), [&](const NativeRenderFrameResolve& value)
                                                {
                                                    return value.readback && value.textureId == resolve.textureId;
                                                });
                         }))
            return refuse("graph_resolve_without_readback");
    if (span.epoch < epoch)
        return refuse("span_epoch_not_increasing");
    if (span.epoch != epoch)
    {
        // Each epoch's segments end with its final span.
        if (epochPassCount)
            return refuse("segments_without_final_span");
        epoch = span.epoch;
    }

    Pending entry;
    entry.value        = lastValue + 1;
    entry.resolveMark  = span.resolveMark;
    entry.guestResolve = span.guestResolve;
    entry.resolves     = std::move(span.resolves);
    if (!span.frame)
    {
        if (span.segment)
            return refuse("segment_without_frame");
        if (epochPassCount)
            return refuse("final_span_drops_segments");
    }
    else
    {
        auto& frame = *span.frame;
        if (frame.passes.empty())
            return refuse("span_without_passes");
        // Preserve continues only the target the last recorded segment ended with.
        const auto& first = frame.passes.front();
        if (first.preservePreviousAttachments && (!epochPassCount || !epochLastTarget || *epochLastTarget != GetNativeRenderFrameTarget(first)))
            return refuse("preserve_target_mismatch");
        if (span.segment)
        {
            if (frame.returnGpuOutput || frame.returnResolvedOutput || frame.outputTextureId || !frame.outputPath.empty())
                return refuse("segment_with_output");
            // An earlier terminal may still be in flight: the producer records
            // in call order, and completion follows the pending order below.
            const std::size_t                            count = frame.passes.size();
            std::optional<NativeRenderFramePass::Target> last;
            if (count)
                last = GetNativeRenderFrameTarget(frame.passes.back());
            std::shared_future<NativeDrawReplayResult> recording;
            // A refused segment leaves the count: the renderer marks its chain
            // failed, so the next terminal fails to finish it.
            if (!config.producer->QueuePasses(std::move(frame), recording, error))
                return refuse(error);
            epochPassCount += count;
            if (last)
                epochLastTarget = last;
            entry.kind = Kind::Prefix;
            entry.work = std::move(recording);
        }
        else
        {
            // The terminal carries only its own passes; the renderer joins the
            // recorded prefix in front of them.
            entry.chained               = epochPassCount != 0;
            frame.continuedPrefixPasses = std::exchange(epochPassCount, 0);
            epochLastTarget.reset();
            const bool diagnose = config.observeOutput && frame.returnGpuOutput && diagnosticOutputs < 2;
            entry.diagnose      = diagnose;
            if (diagnose)
                frame.returnResolvedOutput = true;
            std::shared_future<NativeDrawReplayResult> completion;
            if (!config.producer->QueueFrame(std::move(frame), completion, error))
                return refuse(error);
            if (diagnose)
                ++diagnosticOutputs;
            entry.kind = Kind::Terminal;
            entry.work = std::move(completion);
        }
    }
    lastValue = entry.value;
    {
        std::lock_guard lock(pendingMutex);
        pending.push_back(std::move(entry));
    }
    return lastValue;
}

std::optional<std::string> D3D12CompletionSource::advance() const
{
    std::unique_lock lock(pendingMutex, std::try_to_lock);
    if (!lock.owns_lock() || closed.load(std::memory_order_acquire))
        return std::nullopt;
    std::optional<std::uint64_t> fence;
    while (!pending.empty())
    {
        auto& entry = pending.front();
        if (entry.kind != Kind::Inherit)
        {
            if (!fence)
                fence = config.producer->CompletedFence();
            if (*fence == std::numeric_limits<std::uint64_t>::max())
                return std::string("native_device_lost");
            if (entry.work.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                break;
            const auto& result = entry.work.get();
            // Census mode completes a failed span without its image.
            const auto skip = [&](std::string detail)
            {
                skippedFailures.push_back(std::move(detail));
                completed.store(entry.value, std::memory_order_release);
                pending.pop_front();
            };
            if (!result.success)
            {
                auto detail = std::string(entry.kind == Kind::Prefix ? "native_prefix_failed: " : "native_frame_failed: ") + result.error;
                // Census skips only no-submit admission refusals outside a
                // prefix chain; other failures stop the stream.
                if (!config.census || entry.kind == Kind::Prefix || entry.chained || result.submittedFenceValue)
                    return detail;
                skip(std::move(detail));
                continue;
            }
            if (entry.kind == Kind::Prefix)
            {
                if (!result.completionFenceValue)
                {
                    if (!config.census)
                        return std::string("native_prefix_fence_missing");
                    skip("native_prefix_fence_missing");
                    continue;
                }
                if (*fence < result.completionFenceValue)
                    break;
            }
            else if (entry.diagnose && result.presenterToken && !result.resolvedOutput.empty())
            {
                outputObservations.push_back(entry.work);
            }
            // A discard may precede the readback; its image is never taken.
            if (entry.guestResolve && entry.resolveMark > discardedThrough)
                images.insert_or_assign(entry.resolveMark, GuestImage{ *entry.guestResolve, entry.work, {} });
            // A segment's graph resolves exist once its fence was reached.
            for (const auto& resolve : entry.resolves)
                if (resolve.mark > discardedThrough)
                    images.insert_or_assign(resolve.mark, GuestImage{ resolve.copy, entry.work, findReadback(result, resolve.textureId) });
            if (entry.guestResolve || !entry.resolves.empty())
            {
                if (images.size() > kGuestImageAlarm && !alarmLogged)
                {
                    alarmLogged         = true;
                    std::uint64_t bytes = 0;
                    for (const auto& [mark, image] : images)
                        bytes += image.readback ? image.readback->Bytes() : image.work.get().rawOutput.size();
                    REXLOG_WARN("Native guest resolve images: {} untaken readbacks hold {} bytes; the ledger has not taken or dropped them",
                                images.size(),
                                bytes);
                }
            }
        }
        completed.store(entry.value, std::memory_order_release);
        pending.pop_front();
    }
    return std::nullopt;
}

completion::GuestImageTake D3D12CompletionSource::TakeGuestImage(std::uint64_t mark)
{
    completion::GuestImageTake take;
    GuestImage                 entry;
    // Ledger application encodes outside both polling and the ledger lock.
    // The ledger records any error at that Resolve record.
    if (takeImage(mark, take, entry))
        encode(entry, take);
    return take;
}

completion::GuestImageTake D3D12CompletionSource::HoldGuestImage(std::uint64_t mark, std::shared_ptr<const guest_d3d::IHeldGuestImage>& held)
{
    completion::GuestImageTake take;
    GuestImage                 entry;
    held.reset();
    if (takeImage(mark, take, entry))
        held = std::make_shared<HeldImage>(std::move(entry), encodeCounts, config.timeEncodes);
    return take;
}

bool D3D12CompletionSource::takeImage(std::uint64_t mark, completion::GuestImageTake& take, GuestImage& entry)
{
    {
        std::lock_guard lock(pendingMutex);
        if (closed.load(std::memory_order_acquire))
        {
            take.closing = true;
            return false;
        }
        const auto found = images.find(mark);
        if (found == images.end())
        {
            if (mark <= discardedThrough)
                take.error = std::format("guest_image_discarded: op mark {} at or below discard point {}", mark, discardedThrough);
            else if (pendingResolve(mark))
                take.error = std::format("guest_image_incomplete: op mark {}", mark);
            else
                take.error = std::format("guest_image_missing: op mark {}", mark);
            return false;
        }
        entry = std::move(found->second);
        images.erase(images.begin(), std::next(found));
        discardedThrough   = std::max(discardedThrough, mark - 1);
        const auto encoded = peeked.find(mark);
        if (encoded != peeked.end())
            take.image = std::move(encoded->second);
        peeked.erase(peeked.begin(), peeked.upper_bound(mark));
        return !take.image;
    }
}

completion::GuestImageTake D3D12CompletionSource::PeekGuestImage(std::uint64_t mark, std::chrono::milliseconds timeout)
{
    completion::GuestImageTake                 take;
    std::optional<guest_d3d::GuestResolveCopy> copy;
    std::shared_future<NativeDrawReplayResult> work;
    std::shared_ptr<const NativeRawReadback>   readback;
    bool                                       graph     = false; // from a segment still pending
    std::uint64_t                              textureId = 0;
    {
        std::lock_guard lock(pendingMutex);
        if (closed.load(std::memory_order_acquire))
        {
            take.closing = true;
            return take;
        }
        if (const auto encoded = peeked.find(mark); encoded != peeked.end())
        {
            take.image = encoded->second;
            return take;
        }
        if (const auto found = images.find(mark); found != images.end())
        {
            copy     = found->second.copy;
            work     = found->second.work;
            readback = found->second.readback;
        }
        else if (mark > discardedThrough)
        {
            const Pending* owner = nullptr;
            if (const auto* resolve = pendingResolve(mark, &owner, &textureId))
            {
                copy  = *resolve;
                work  = owner->work;
                graph = textureId != 0;
            }
        }
        if (!copy)
        {
            take.error = mark <= discardedThrough ? std::format("guest_image_discarded: op mark {} at or below discard point {}", mark, discardedThrough)
                                                  : std::format("guest_image_missing: op mark {}", mark);
            return take;
        }
    }
    // Cached, closed and missing peeks return above without reading the clock.
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    // native_frame_timing: time the wait on a pending segment, every exit.
    struct PeekWait
    {
        D3D12CompletionSource*                self;
        std::chrono::steady_clock::time_point started;

        void finish()
        {
            if (!self)
                return;
            self->graphPeekNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
            self->graphPeeks.fetch_add(1, std::memory_order_relaxed);
            self = nullptr;
        }

        ~PeekWait()
        {
            finish();
        }
    } peekWait{ graph && config.timeEncodes ? this : nullptr, deadline - timeout };

    if (work.wait_until(deadline) != std::future_status::ready)
    {
        take.error = std::format("guest_image_incomplete: op mark {}", mark);
        return take;
    }
    if (peekWait.self)
        graphPeekRecordingNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - peekWait.started).count()),
                                                std::memory_order_relaxed);
    if (graph && work.get().success)
    {
        // A pending segment's readback is valid once the stream fence reaches
        // the value its recording named.
        const auto& result = work.get();
        const auto  fence  = config.producer->WaitFence(result.completionFenceValue, deadline);
        if (fence == std::numeric_limits<std::uint64_t>::max())
        {
            take.error = "guest_resolve_readback_failed: native_device_lost";
            return take;
        }
        if (fence < result.completionFenceValue)
        {
            take.error = std::format("guest_image_incomplete: op mark {}", mark);
            return take;
        }
        readback = findReadback(result, textureId);
    }
    peekWait.finish(); // the encode is timed with the other encodes
    encode(GuestImage{ *copy, work, readback }, take);
    if (take.image)
    {
        // Keep it for the take unless a take or discard removed the image meanwhile.
        std::lock_guard lock(pendingMutex);
        const bool      live = images.contains(mark) || pendingResolve(mark);
        if (live && mark > discardedThrough && !closed.load(std::memory_order_acquire))
            peeked.insert_or_assign(mark, *take.image);
    }
    return take;
}

const guest_d3d::GuestResolveCopy* D3D12CompletionSource::pendingResolve(std::uint64_t mark, const Pending** owner, std::uint64_t* textureId) const
{
    for (const auto& item : pending)
    {
        const guest_d3d::GuestResolveCopy* found = item.guestResolve && item.resolveMark == mark ? &*item.guestResolve : nullptr;
        for (const auto& resolve : item.resolves)
            if (!found && resolve.mark == mark)
            {
                found = &resolve.copy;
                if (textureId)
                    *textureId = resolve.textureId;
            }
        if (found)
        {
            if (owner)
                *owner = &item;
            return found;
        }
    }
    return nullptr;
}

void D3D12CompletionSource::encode(const GuestImage& image, completion::GuestImageTake& take)
{
    timedEncode(config.timeEncodes, *encodeCounts, image.copy, image.work, image.readback, take);
}

void D3D12CompletionSource::DiscardGuestImagesThrough(std::uint64_t mark)
{
    std::lock_guard lock(pendingMutex);
    images.erase(images.begin(), images.upper_bound(mark));
    peeked.erase(peeked.begin(), peeked.upper_bound(mark));
    discardedThrough = std::max(discardedThrough, mark);
}

void D3D12CompletionSource::ObserveCompletedOutputs()
{
    std::vector<std::shared_future<NativeDrawReplayResult>> observations;
    {
        std::lock_guard lock(pendingMutex);
        observations.swap(outputObservations);
    }
    for (const auto& observation : observations)
    {
        try
        {
            config.observeOutput(observation.get());
        }
        catch (...)
        {
            // An observational log must not fail completed host work.
        }
    }
}

void D3D12CompletionSource::fail(std::string detail) const
{
    if (failed.exchange(true, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard lock(failureMutex);
        failure = detail;
    }
    if (!config.failed)
        return;
    try
    {
        config.failed(detail);
    }
    catch (...)
    {
    }
}

std::uint64_t D3D12CompletionSource::Completed() const
{
    if (!closed.load(std::memory_order_acquire) && !failed.load(std::memory_order_acquire))
    {
        try
        {
            if (auto detail = advance())
                fail(std::move(*detail));
        }
        catch (...)
        {
            fail("native_completion_poll_failed");
        }
    }
    return completed.load(std::memory_order_acquire);
}

bool D3D12CompletionSource::WaitFor(std::uint64_t value, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        if (Completed() >= value)
            return true;
        if (closed.load(std::memory_order_acquire) || failed.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void D3D12CompletionSource::Close()
{
    std::lock_guard lock(pendingMutex);
    closed.store(true, std::memory_order_release);
    images.clear();
    peeked.clear();
}

std::optional<std::string> D3D12CompletionSource::Failure() const
{
    std::lock_guard lock(failureMutex);
    return failure;
}

std::optional<std::string> D3D12CompletionSource::TakeSkippedFailure()
{
    std::lock_guard lock(pendingMutex);
    if (skippedFailures.empty())
        return std::nullopt;
    auto detail = std::move(skippedFailures.front());
    skippedFailures.pop_front();
    return detail;
}

} // namespace rerevved::gpu
