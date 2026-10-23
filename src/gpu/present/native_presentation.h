#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "native_output_lease.h"

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace rerevved::gpu
{

class NativeOverlayMailbox;

namespace detail
{

struct NativePresentationTestAccess;

}

// Terminal results of one host operation. Occluded and OutOfDate are present
// results only; Timeout means the device is alive and the outcome is unproved.
enum class HostStatus : std::uint8_t
{
    Ok,
    Timeout,
    DeviceLost,
    Failed,
    Occluded,
    OutOfDate
};

enum class HostOperation : std::uint8_t
{
    Submit,
    Wait,
    Present
};

// Healthy is normal; Failed has no outstanding GPU work. DeviceLost still
// requires actual removal before undrained owners release. Unproved retains
// every potentially referenced owner until process exit.
enum class HostHealth : std::uint8_t
{
    Healthy,
    DeviceLost,
    Unproved,
    Failed
};

struct SurfaceExtent
{
    std::uint32_t width  = 0;
    std::uint32_t height = 0;

    bool Valid() const noexcept
    {
        return width != 0 && height != 0;
    }

    friend bool operator==(const SurfaceExtent&, const SurfaceExtent&) = default;
};

// Window state as the presentation layer sees it. A zero extent or a minimized
// window suspends presentation; neither ever becomes a resize target.
class SurfaceModel
{
public:
    void RequestExtent(SurfaceExtent extent) noexcept
    {
        ++extentRequests_;
        if (!extent.Valid())
        {
            ++zeroExtentRequests_;
            zeroExtent_ = true;
            return;
        }
        if (zeroExtent_)
        {
            repaint_ = true;
        }
        zeroExtent_  = false;
        latestValid_ = extent;
    }

    void SetMinimized(bool minimized) noexcept
    {
        if (minimized_ && !minimized)
        {
            repaint_ = true;
        }
        minimized_ = minimized;
    }

    bool Suspended() const noexcept
    {
        return minimized_ || zeroExtent_;
    }

    bool CanPresent() const noexcept
    {
        return !Suspended() && applied_.Valid();
    }

    // The latest valid requested extent when it differs from the applied one.
    std::optional<SurfaceExtent> PendingResize() const noexcept
    {
        if (!latestValid_.Valid() || latestValid_ == applied_)
        {
            return std::nullopt;
        }
        return latestValid_;
    }

    // The swap chain reports the extent it actually reached for a request. A
    // newer request that arrived meanwhile stays pending.
    void MarkApplied(SurfaceExtent actual, SurfaceExtent request) noexcept
    {
        applied_ = actual;
        if (latestValid_ == request)
        {
            latestValid_ = actual;
        }
        ++generation_;
        repaint_ = true;
    }

    bool TakeRepaint() noexcept
    {
        const bool repaint = repaint_;
        repaint_           = false;
        return repaint;
    }

    SurfaceExtent Applied() const noexcept
    {
        return applied_;
    }

    SurfaceExtent LatestValid() const noexcept
    {
        return latestValid_;
    }

    std::uint64_t Generation() const noexcept
    {
        return generation_;
    }

    std::uint64_t ExtentRequests() const noexcept
    {
        return extentRequests_;
    }

    std::uint64_t ZeroExtentRequests() const noexcept
    {
        return zeroExtentRequests_;
    }

private:
    SurfaceExtent applied_{};
    SurfaceExtent latestValid_{};
    bool          zeroExtent_         = false;
    bool          minimized_          = false;
    bool          repaint_            = false;
    std::uint64_t generation_         = 0;
    std::uint64_t extentRequests_     = 0;
    std::uint64_t zeroExtentRequests_ = 0;
};

struct LetterboxRect
{
    std::uint32_t x      = 0;
    std::uint32_t y      = 0;
    std::uint32_t width  = 0;
    std::uint32_t height = 0;

    bool Empty() const noexcept
    {
        return width == 0 || height == 0;
    }

    friend bool operator==(const LetterboxRect&, const LetterboxRect&) = default;
};

// Largest centered rectangle of the source aspect inside the target.
inline LetterboxRect ComputeLetterbox(SurfaceExtent source, SurfaceExtent target) noexcept
{
    if (!source.Valid() || !target.Valid())
    {
        return {};
    }
    const auto fit = [](std::uint64_t value, std::uint64_t numerator, std::uint64_t denominator, std::uint64_t limit)
    {
        const std::uint64_t scaled = (value * numerator + denominator / 2) / denominator;
        return static_cast<std::uint32_t>(scaled < 1 ? 1 : (scaled > limit ? limit : scaled));
    };
    LetterboxRect rect;
    if (static_cast<std::uint64_t>(target.width) * source.height <= static_cast<std::uint64_t>(target.height) * source.width)
    {
        rect.width  = target.width;
        rect.height = fit(target.width, source.height, source.width, target.height);
    }
    else
    {
        rect.height = target.height;
        rect.width  = fit(target.height, source.width, source.height, target.width);
    }
    rect.x = (target.width - rect.width) / 2;
    rect.y = (target.height - rect.height) / 2;
    return rect;
}

// R10G10B10A2 layout: red in the low ten bits.
constexpr std::uint32_t PackRgb10A2(std::uint32_t red, std::uint32_t green, std::uint32_t blue, std::uint32_t alpha2 = 3U) noexcept
{
    return (red & 1023U) | ((green & 1023U) << 10U) | ((blue & 1023U) << 20U) | ((alpha2 & 3U) << 30U);
}

// Round-to-nearest UNORM10 to UNORM8. No input falls on a rounding tie.
constexpr std::uint8_t QuantizeUnorm10To8(std::uint32_t value10) noexcept
{
    return static_cast<std::uint8_t>((510U * (value10 & 1023U) + 1023U) / 2046U);
}

// Entry layout read by the producer's gamma pass.
constexpr std::uint32_t PackGammaEntry(std::uint32_t red10, std::uint32_t green10, std::uint32_t blue10) noexcept
{
    return ((red10 & 1023U) << 20U) | ((green10 & 1023U) << 10U) | (blue10 & 1023U);
}

enum class PresentOutcome : std::uint8_t
{
    Presented,
    Occluded,
    NoFrame,
    Suspended,
    OutOfDate,
    DeviceLost,
    Timeout,
    Failed
};

struct PresentResult
{
    PresentOutcome outcome    = PresentOutcome::Failed;
    std::uint64_t  frameId    = 0;
    std::int32_t   nativeCode = 0;
    const char*    operation  = "";
    // Whether a new frame was copied for this result, and whether the shown
    // frame had been shown before.
    std::uint64_t gammaGeneration = 0;
    std::uint64_t overlaySequence = 0;
    std::uint32_t overlayDraws    = 0;
    bool          newFrame        = false;
    bool          repeat          = false;
    // The swap chain generation the pump ran against; zero only when it is unknown.
    std::uint64_t surfaceGeneration = 0;
};

// The first successful presentation of a title frame, as the ledger counts
// presented frames: repeats and the idle canvas show no new guest frame. A
// repaint can be that first one when the frame's own present was occluded.
inline bool ShowsNewFrame(const PresentResult& result) noexcept
{
    return result.outcome == PresentOutcome::Presented && !result.repeat && result.frameId != 0;
}

struct PresentationCounters
{
    std::uint64_t offered               = 0;
    std::uint64_t rejected              = 0;
    std::uint64_t dropped               = 0;
    std::uint64_t droppedWhileSuspended = 0;
    std::uint64_t claimed               = 0;
    std::uint64_t stale                 = 0;
    std::uint64_t copySubmitted         = 0;
    std::uint64_t copyRetired           = 0;
    std::uint64_t copyFailed            = 0;
    std::uint64_t hostSubmissions       = 0;
    // Distinct frames with a successful Present, separate from distinct
    // completed frames and from repeated presentations of one frame.
    std::uint64_t presentedFrames      = 0;
    std::uint64_t repeats              = 0;
    std::uint64_t occluded             = 0;
    std::uint64_t outOfDate            = 0;
    std::uint64_t presentFailed        = 0;
    std::uint64_t resizes              = 0;
    std::uint64_t resizeDrains         = 0;
    std::uint64_t suspendedPumps       = 0;
    std::uint64_t lastOfferedFrameId   = 0;
    std::uint64_t lastRetiredFrameId   = 0;
    std::uint64_t lastPresentedFrameId = 0;
    std::uint64_t surfaceGeneration    = 0;
    std::uint64_t gammaGeneration      = 0;
    std::uint32_t presentInterval      = 1;
};

// Counting rules shared by the session and its model test.
class PresentationLedger
{
public:
    void OnOffered(std::uint64_t frameId, std::uint64_t surfaceGeneration, std::uint64_t gammaGeneration) noexcept
    {
        ++counters_.offered;
        counters_.lastOfferedFrameId = frameId;
        counters_.surfaceGeneration  = surfaceGeneration;
        counters_.gammaGeneration    = gammaGeneration;
    }

    void OnRejected() noexcept
    {
        ++counters_.rejected;
    }

    void OnDropped(bool whileSuspended) noexcept
    {
        ++(whileSuspended ? counters_.droppedWhileSuspended : counters_.dropped);
    }

    void OnClaimed() noexcept
    {
        ++counters_.claimed;
    }

    void OnStale() noexcept
    {
        ++counters_.stale;
    }

    void OnCopySubmitted() noexcept
    {
        ++counters_.copySubmitted;
    }

    void OnCopyRetired(std::uint64_t frameId) noexcept
    {
        ++counters_.copyRetired;
        counters_.lastRetiredFrameId = frameId;
    }

    void OnCopyFailed() noexcept
    {
        ++counters_.copyFailed;
    }

    void OnHostSubmitted() noexcept
    {
        ++counters_.hostSubmissions;
    }

    void OnSuspendedPump() noexcept
    {
        ++counters_.suspendedPumps;
    }

    void OnResize(bool drained) noexcept
    {
        ++counters_.resizes;
        if (drained)
        {
            ++counters_.resizeDrains;
        }
    }

    // Returns true when the presentation repeated an already presented frame.
    bool OnPresent(std::uint64_t frameId, HostStatus status) noexcept
    {
        switch (status)
        {
            case HostStatus::Ok:
                if (frameId != 0 && frameId == counters_.lastPresentedFrameId)
                {
                    ++counters_.repeats;
                    return true;
                }
                ++counters_.presentedFrames;
                counters_.lastPresentedFrameId = frameId;
                return false;
            case HostStatus::Occluded:
                ++counters_.occluded;
                return false;
            case HostStatus::OutOfDate:
                ++counters_.outOfDate;
                return false;
            default:
                ++counters_.presentFailed;
                return false;
        }
    }

    void SetPresentInterval(std::uint32_t interval) noexcept
    {
        counters_.presentInterval = interval;
    }

    const PresentationCounters& Counters() const noexcept
    {
        return counters_;
    }

private:
    PresentationCounters counters_{};
};

struct NativePresentationConfig
{
    // Opaque window handle: an HWND on Windows.
    void*         nativeWindow    = nullptr;
    std::uint32_t bufferCount     = 2;
    std::uint32_t maxFrameLatency = 2;
    std::size_t   maxQueuedOffers = 2;
    // Nearest filtering keeps integer scaling exact; bilinear is the default.
    bool nearestScaling = false;
    bool vsync          = true;
    // Before the first copied frame, present a cleared black canvas with
    // overlays on repaint or overlay change. Such results carry frame ID 0 and
    // no new frame, and do not enter the frame ledger.
    bool idleCanvas = false;
    // Upper bound for any single bounded wait, further limited by the
    // caller's shared deadline.
    std::chrono::milliseconds waitCap{ 5000 };
};

struct NativePresentationShutdown
{
    bool       drained  = false;
    bool       retained = false;
    HostHealth health   = HostHealth::Healthy;
};

class NativePresentation
{
public:
    static std::unique_ptr<NativePresentation> Create(const NativePresentationConfig& config, std::string& error);
    ~NativePresentation();

    NativePresentation(const NativePresentation&)            = delete;
    NativePresentation& operator=(const NativePresentation&) = delete;

    // The device the producer must run on. Valid while the session lives.
    ID3D12Device* Device() const noexcept;
    // Attach before starting the presentation worker.
    void   SetOverlayMailbox(std::shared_ptr<NativeOverlayMailbox> mailbox);
    size_t OverlayTextureCount() const;
    size_t OverlayGeometryAllocations() const;
    size_t OverlayPendingTextureUploads() const;
    // Offer entry for the producer. It outlives the session safely: after
    // shutdown it rejects every offer.
    NativeTitleOutputPublisher Publisher(std::optional<std::uint64_t> gammaGeneration = std::nullopt) const;

    void SetGammaGeneration(std::uint64_t generation);
    // Thread-safe latches for window events.
    void RequestExtent(SurfaceExtent extent);
    void SetMinimized(bool minimized);

    // Host-thread operations. Each wait shares the caller's deadline.
    PresentResult PumpOnce(OperationDeadline& deadline);
    PresentResult Repaint(OperationDeadline& deadline);
    // Stops admission and cancels unclaimed offers, and caps every producer
    // wait on every outstanding lease, including claimed copies, at the shared
    // deadline. Run before stopping the producer.
    void CloseAdmission(OperationDeadline& deadline);
    // Closes admission, drains the host and ends every pending lease. On an
    // unproved drain it retains the whole GPU ownership set instead of
    // releasing it. Run after the producer stopped using the device.
    NativePresentationShutdown Shutdown(OperationDeadline& deadline);

    PresentationCounters Counters() const;
    HostHealth           Health() const;
    bool                 OwnershipRetained() const;
    SurfaceExtent        SwapChainExtent() const;
    SurfaceExtent        DisplayExtent() const;

    // Diagnostics. Both block on bounded waits.
    bool CaptureDisplay(std::vector<std::uint32_t>& packed, SurfaceExtent& extent, OperationDeadline& deadline, std::string& error);
    // Arms a capture of the next presented backbuffer, taken before Present.
    void ArmBackbufferCapture();
    bool TakeBackbufferCapture(std::vector<std::uint8_t>& rgba, SurfaceExtent& extent);
    // Test hook: the host reports a fixed status for an operation after
    // skipping the given number of successful ones.
    void InjectHostFault(HostOperation operation, HostStatus status, std::uint32_t skip = 0);

private:
    friend struct detail::NativePresentationTestAccess;
    ID3D12CommandQueue* commandQueueForTesting() const;
    NativePresentation();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rerevved::gpu
