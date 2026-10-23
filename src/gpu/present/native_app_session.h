#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "gpu/d3d12/native_completion_source.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "native_app_drain.h"
#include "native_presentation.h"

namespace rerevved::gpu
{

// Receives the session's lifecycle events from any thread; the shipping app
// installs none, and the smoke target installs a collector.
class NativeAppEventSink
{
public:
    virtual ~NativeAppEventSink()                                                  = default;
    virtual void Event(const char* name, std::uint64_t frame, std::uint64_t value) = 0;

    virtual void Flush()
    {
    }
};

#if REREVVED_DEVELOPER
// A sink that appends each event to a new TOML file named by native_present_trace.
std::shared_ptr<NativeAppEventSink> OpenNativeAppTraceFile(const std::filesystem::path& path, std::string& error);
#endif

// Worker callbacks post to the UI thread; they never access the window directly.
class NativeAppSession : public std::enable_shared_from_this<NativeAppSession>, public INativeFrameProducer
{
public:
    using Tick = std::function<void(PresentResult)>;
    // Without a frame the session starts no producer and presents an idle
    // black canvas with overlays; it never reports a guest frame.
    static std::shared_ptr<NativeAppSession> Create(void* window, std::optional<NativeRenderFrame> frame, std::shared_ptr<NativeAppEventSink> events, std::string& error);
    // Guest boot: the producer runs on the session's device and guest frames
    // arrive through INativeFrameProducer. The run loop only presents; until
    // the first guest frame is published it shows the idle canvas.
    static std::shared_ptr<NativeAppSession> CreateGuest(void* window, std::shared_ptr<NativeAppEventSink> events, std::string& error);
    ~NativeAppSession() override;

    bool                       QueueFrame(NativeRenderFrame frame, std::shared_future<NativeDrawReplayResult>& completion, std::string& error) override;
    bool                       QueuePasses(NativeRenderFrame passes, std::shared_future<NativeDrawReplayResult>& recording, std::string& error) override;
    std::uint64_t              CompletedFence() const override;
    std::uint64_t              WaitFence(std::uint64_t value, std::chrono::steady_clock::time_point deadline) const override;
    void                       Start(Tick tick);
    void                       SetOverlayMailbox(std::shared_ptr<NativeOverlayMailbox> mailbox);
    void                       Advance();
    void                       RequestExtent(SurfaceExtent extent);
    void                       SetMinimized(bool minimized);
    void                       RequestRepaint();
    NativeAppDrain::Completion Close(std::function<void(bool)> completed = {});
    void                       Event(const char* name, std::uint64_t frame = 0, std::uint64_t value = 0);
    void                       FlushTrace();

#if REREVVED_DEVELOPER
    // The guest producer's counters, readable from any thread.
    NativeDrawReplayTimingStats ProducerTiming() const noexcept;
    NativeDrawReplayCacheStats  ProducerCache() const noexcept;
    // native_frame_timing: also time the producer's pipeline creations.
    void EnableProducerTiming(bool enabled) noexcept;
#endif
    // Whether the guest producer's device creates castable guest-word
    // resolve textures; settled once the session is created.
    bool ProducerCastableGuestWords() const noexcept;

private:
    static std::shared_ptr<NativeAppSession> create(void* window, std::optional<NativeRenderFrame> frame, bool guest, std::shared_ptr<NativeAppEventSink> events, std::string& error);
    void                                     run(Tick tick);
    std::unique_ptr<NativePresentation>      session_;
    NativeRendererD3D12                      producer_;
    ID3D12Device*                            device_ = nullptr;
    // The producer's queue fence, held so completion polls outlive the stream.
    ID3D12Fence*                        fence_ = nullptr;
    std::optional<NativeRenderFrame>    frame_;
    bool                                guest_ = false;
    std::mutex                          mutex_;
    std::condition_variable             wake_;
    bool                                advance_    = true;
    bool                                minimized_  = false;
    bool                                zeroExtent_ = false;
    bool                                repaint_    = false;
    bool                                stopping_   = false;
    bool                                finished_   = false;
    bool                                drained_    = false;
    NativeAppDrain                      drain_;
    std::optional<SurfaceExtent>        requestedExtent_;
    std::shared_ptr<NativeAppEventSink> events_;
};

} // namespace rerevved::gpu
