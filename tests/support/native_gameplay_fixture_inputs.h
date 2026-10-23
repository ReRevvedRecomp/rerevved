#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>

#include <rex/graphics/draw_capture_mailbox.h>

#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/native_gameplay_snapshot.h"

namespace rerevved::gpu::diagnostics
{

// Owns guest CPU inputs only. The snapshot supplies ordered guest state and
// texture-use identities; its host texture payload fields remain empty.
class NativeGameplayLiveInputs
{
public:
    static constexpr std::size_t MaxOwnedBytes = 512U * 1024U * 1024U;
    NativeGameplayLiveInputs();
    ~NativeGameplayLiveInputs();
    NativeGameplayLiveInputs(const NativeGameplayLiveInputs&)            = delete;
    NativeGameplayLiveInputs& operator=(const NativeGameplayLiveInputs&) = delete;
    NativeGameplayLiveInputs(NativeGameplayLiveInputs&&) noexcept;
    NativeGameplayLiveInputs& operator=(NativeGameplayLiveInputs&&) noexcept;

    using EventConsumer = std::function<bool(
        const rex::graphics::diagnostic::DrawCaptureFrameEvent&,
        std::span<const std::uint32_t>,
        std::span<const rex::graphics::diagnostic::DrawCaptureTexture>,
        const std::map<std::uint32_t, NativeDrawReplayTexture>&,
        std::string&)>;
    // The consumer prepares native work before compact diagnostic encoding.
    // It borrows this single event and admitted register bank synchronously.
    void SetEventConsumer(EventConsumer consumer);

    struct EventTimings
    {
        bool          enabled             = false;
        std::uint64_t callbackNanoseconds = 0, collectionNanoseconds = 0, consumeNanoseconds = 0, encodingNanoseconds = 0;
        std::uint64_t reclaimNanoseconds = 0, reclaims = 0;
        std::uint64_t drawCallbacks = 0, copyCallbacks = 0, encodedEvents = 0, consumerCalls = 0, clockReads = 0, incompleteEvents = 0;
    };

    // Borrow the caller's existing outer clocks; reclamation is a nested scope.
    void                ResetEventTimings(bool enabled);
    void                BeginEventTiming(std::chrono::steady_clock::time_point begin, bool draw);
    void                EndEventTiming(std::chrono::steady_clock::time_point end);
    const EventTimings& Timings() const;
    // Reclaims older owners before a new allocation would exceed their shared
    // input allowance. The callback returns the remaining collector allowance.
    void               SetOwnedByteBudget(std::size_t bytes, std::function<std::size_t(std::size_t)> reclaim = {});
    static std::size_t BoundaryByteBound();

    bool Begin(const rex::graphics::diagnostic::DrawCaptureFrameInputSwapView& boundary,
               std::string&                                                    error);
    bool BeginPrepared(const rex::graphics::diagnostic::DrawCaptureFrameInputSwapView& boundary,
                       std::string&                                                    error);
    bool Draw(const rex::graphics::diagnostic::DrawCaptureFrameInputDrawView& draw,
              std::string&                                                    error);
    // Owns complete CPU inputs before original host submission.
    bool PreparedDraw(const rex::graphics::diagnostic::DrawCaptureFrameInputDrawView& draw,
                      std::string&                                                    error);
    bool PreparedCopy(const rex::graphics::diagnostic::DrawCaptureFrameInputCopyView& copy,
                      std::string&                                                    error);
    // Observe copies before Begin to retain the diagnostic producer history.
    // Current CPU/GPU ownership comes from each SDK texture view.
    bool Copy(const rex::graphics::diagnostic::DrawCaptureFrameInputCopyView& copy,
              bool                                                            collecting,
              std::string&                                                    error,
              bool                                                            captureClearOnly = false);
    bool Finish(const rex::graphics::diagnostic::DrawCaptureFrameInputSwapView& swap,
                std::string&                                                    error);
    bool PreparedSwap(const rex::graphics::diagnostic::DrawCaptureFrameInputSwapView& swap,
                      std::string&                                                    error);
    // Retire the completed inputs only after presentation copying has finished.
    // The next boundary comes from owned state, never a retained SDK view.
    bool BeginAfterCompletion(std::string& error);
    // Keeps the completed owner's diagnostics intact while beginning from its
    // owned boundary and exact validated pixel versions.
    bool BeginAfterCompletion(const NativeGameplayLiveInputs& completed, std::string& error);

    const NativeGameplaySnapshot&                           Snapshot() const;
    const std::map<std::uint32_t, NativeDrawReplayTexture>& Textures() const;
    std::size_t                                             OwnedBytes() const;
    bool                                                    WriteOwnership(const std::filesystem::path& path, std::string& error) const;
    bool                                                    WriteSnapshot(const std::filesystem::path& directory, std::string& error) const;

private:
    bool CollectSwap(const rex::graphics::diagnostic::DrawCaptureFrameInputSwapView& swap,
                     std::string&                                                    error,
                     bool                                                            prepared);
    bool CollectCopy(const rex::graphics::diagnostic::DrawCaptureFrameInputCopyView& copy,
                     bool                                                            collecting,
                     std::string&                                                    error,
                     bool                                                            captureClearOnly,
                     bool                                                            prepared);
    bool CollectDraw(const rex::graphics::diagnostic::DrawCaptureFrameInputDrawView& draw,
                     std::string&                                                    error,
                     bool                                                            prepared);
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Explicit completion publishes the latest CPU frame, including all writes to
// the selected page.
class NativeGameplayPriorCapture
{
public:
    NativeGameplayPriorCapture(std::uint32_t page, std::filesystem::path directory);
    void Draw(const rex::graphics::diagnostic::DrawCaptureFrameInputDrawView& draw);
    void Copy(const rex::graphics::diagnostic::DrawCaptureFrameInputCopyView& copy);
    void Swap(const rex::graphics::diagnostic::DrawCaptureFrameInputSwapView& swap, bool finish = false);

    std::uint32_t Page() const
    {
        return page;
    }

    bool Done() const
    {
        return done;
    }

    const std::string& Error() const
    {
        return failure;
    }

    const NativeGameplayLiveInputs* Captured() const
    {
        return captured.get();
    }

    const std::string& CapturedError() const
    {
        return capturedFailure;
    }

    std::unique_ptr<NativeGameplayLiveInputs> TakeCaptured()
    {
        return std::move(captured);
    }

private:
    std::unique_ptr<NativeGameplayLiveInputs> inputs;
    std::unique_ptr<NativeGameplayLiveInputs> captured;
    std::filesystem::path                     directory;
    std::uint32_t                             page;
    const bool                                discoverTerrain;
    bool                                      candidate  = false;
    bool                                      terrain    = false;
    bool                                      collecting = false;
    bool                                      found      = false;
    bool                                      done       = false;
    std::string                               failure;
    std::string                               capturedFailure;
};

} // namespace rerevved::gpu::diagnostics
