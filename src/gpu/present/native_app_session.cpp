#include "native_app_session.h"
#include "native_app_route.h"
#if REREVVED_DEVELOPER
#include "native_cadence_trace.h"
#endif

#include <d3d12.h>
#include <fstream>
#include <limits>
#include <rex/logging.h>
#include <thread>
#include <utility>

namespace rerevved::gpu
{

#if REREVVED_DEVELOPER
namespace
{

class TraceFile final : public NativeAppEventSink
{
public:
    bool Open(const std::filesystem::path& path)
    {
        file_.open(path);
        return bool(file_);
    }

    void Event(const char* name, std::uint64_t frame, std::uint64_t value) override
    {
        std::lock_guard lock(mutex_);
        file_ << "[[event]]\nname = '" << name << "'\nframe = " << frame << "\nvalue = " << value << "\n";
    }

    void Flush() override
    {
        std::lock_guard lock(mutex_);
        file_.flush();
    }

private:
    std::mutex    mutex_;
    std::ofstream file_;
};

} // namespace

std::shared_ptr<NativeAppEventSink> OpenNativeAppTraceFile(const std::filesystem::path& path, std::string& error)
{
    if (std::filesystem::exists(path))
    {
        error = "native_present_trace must name a new file";
        return {};
    }
    auto file = std::make_shared<TraceFile>();
    if (!file->Open(path))
    {
        error = "could not open native_present_trace";
        return {};
    }
    return file;
}
#endif

std::shared_ptr<NativeAppSession> NativeAppSession::Create(void* window, std::optional<NativeRenderFrame> frame, std::shared_ptr<NativeAppEventSink> events, std::string& error)
{
    return create(window, std::move(frame), false, std::move(events), error);
}

std::shared_ptr<NativeAppSession> NativeAppSession::CreateGuest(void* window, std::shared_ptr<NativeAppEventSink> events, std::string& error)
{
    return create(window, std::nullopt, true, std::move(events), error);
}

std::shared_ptr<NativeAppSession> NativeAppSession::create(void* window, std::optional<NativeRenderFrame> frame, bool guest, std::shared_ptr<NativeAppEventSink> events, std::string& error)
{
    auto state     = std::make_shared<NativeAppSession>();
    state->events_ = std::move(events);
    NativePresentationConfig config;
    config.nativeWindow = window;
    config.idleCanvas   = !frame.has_value();
    if (!window)
    {
        error = "no usable window: native window creation failed";
        state->Event("unavailable");
        return {};
    }
    state->session_ = NativePresentation::Create(config, error);
    if (!state->session_)
    {
        state->Event(error.starts_with("no usable") ? "unavailable" : "startup_failed");
        return {};
    }
    state->device_ = state->session_->Device();
    state->device_->AddRef(); // Keep the device alive through UI-thread overlay teardown.
    state->frame_ = std::move(frame);
    state->guest_ = guest;
    if (!state->frame_ && !guest)
    {
        state->Event("startup_idle");
        return state;
    }
    const auto publisher = state->session_->Publisher(1);
    // One producer, one copied gamma table per frame, and either one in-flight
    // fixture frame or the guest frames a completion source queues. The
    // publisher carries the generation with each offer under the gate lock.
    if (!state->producer_.StartTitleDrawStream(error, state->device_, [owner = state.get(), publisher](auto* resource, auto* fence, auto value)
                                               {
                                                   auto lease = publisher(resource, fence, value);
                                                   if (lease)
                                                   {
#if REREVVED_DEVELOPER
                                                       cadence::Publish(lease->Token(), lease->GammaGeneration());
#endif
                                                       owner->Event("published", lease->Token(), lease->GammaGeneration());
                                                   }
                                                   return lease;
                                               }))
    {
        state->Event("startup_failed");
        return {};
    }
    if (guest)
    {
        state->fence_ = state->producer_.AcquireStreamFence();
        if (!state->fence_)
        {
            error = "native guest producer has no stream fence";
            state->Event("startup_failed");
            return {};
        }
    }
    state->Event(guest ? "startup_guest" : "startup");
    return state;
}

NativeAppSession::~NativeAppSession()
{
    producer_.Shutdown();
    session_.reset();
    if (fence_)
        fence_->Release();
    if (device_)
        device_->Release();
}

bool NativeAppSession::QueueFrame(NativeRenderFrame frame, std::shared_future<NativeDrawReplayResult>& completion, std::string& error)
{
    completion = {};
    if (!guest_)
    {
        error = "native session has no guest producer";
        return false;
    }
    std::shared_ptr<NativeRenderFrameTicket> ticket;
    if (!producer_.QueueRenderFrame(std::move(frame), ticket, error))
        return false;
    completion = ticket->Completion();
    return true;
}

bool NativeAppSession::QueuePasses(NativeRenderFrame passes, std::shared_future<NativeDrawReplayResult>& recording, std::string& error)
{
    recording = {};
    if (!guest_)
    {
        error = "native session has no guest producer";
        return false;
    }
    return producer_.QueueRenderFramePasses(std::move(passes), recording, error);
}

std::uint64_t NativeAppSession::CompletedFence() const
{
    // Without a fence nothing can complete; report it as lost.
    return fence_ ? fence_->GetCompletedValue() : std::numeric_limits<std::uint64_t>::max();
}

std::uint64_t NativeAppSession::WaitFence(std::uint64_t value, std::chrono::steady_clock::time_point deadline) const
{
    return fence_ ? WaitStreamFence(fence_, value, deadline) : std::numeric_limits<std::uint64_t>::max();
}

#if REREVVED_DEVELOPER
NativeDrawReplayTimingStats NativeAppSession::ProducerTiming() const noexcept
{
    return producer_.ReplayTimingStats();
}

NativeDrawReplayCacheStats NativeAppSession::ProducerCache() const noexcept
{
    return producer_.ReplayCacheStats();
}

void NativeAppSession::EnableProducerTiming(bool enabled) noexcept
{
    producer_.SetPipelineTiming(enabled);
}
#endif

bool NativeAppSession::ProducerCastableGuestWords() const noexcept
{
    return producer_.CastableGuestWordTextures();
}

void NativeAppSession::Event(const char* name, std::uint64_t frame, std::uint64_t value)
{
    if (events_)
        events_->Event(name, frame, value);
}

void NativeAppSession::Start(Tick tick)
{
    std::thread([self = shared_from_this(), tick = std::move(tick)]
                {
                    self->run(tick);
                })
        .detach();
}

void NativeAppSession::SetOverlayMailbox(std::shared_ptr<NativeOverlayMailbox> mailbox)
{
    session_->SetOverlayMailbox(std::move(mailbox));
}

void NativeAppSession::FlushTrace()
{
    if (events_)
        events_->Flush();
}

void NativeAppSession::Advance()
{
    std::lock_guard lock(mutex_);
    advance_ = true;
    wake_.notify_all();
}

void NativeAppSession::RequestExtent(SurfaceExtent extent)
{
    std::lock_guard lock(mutex_);
    // The session can drop its own request for an empty client area, so a
    // repeated extent (a restore) is still forwarded; only the trace is deduplicated.
    session_->RequestExtent(extent);
    if (extent == requestedExtent_)
        return;
    requestedExtent_ = extent;
    zeroExtent_      = !extent.Valid();
    Event("resize", extent.width, extent.height);
}

void NativeAppSession::SetMinimized(bool minimized)
{
    std::lock_guard lock(mutex_);
    if (minimized_ == minimized)
        return;
    minimized_ = minimized;
    session_->SetMinimized(minimized);
    Event(minimized ? "suspend" : "restore");
}

void NativeAppSession::RequestRepaint()
{
    std::lock_guard lock(mutex_);
    repaint_ = true;
}

void NativeAppSession::run(Tick tick)
{
    NativeAppTrace trace;
    SurfaceExtent  lastExtent;
    for (;;)
    {
        bool minimized, repaint;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this]
                       {
                           return advance_ || stopping_;
                       });
            if (stopping_)
                break;
            advance_  = false;
            minimized = minimized_ || zeroExtent_;
            repaint   = std::exchange(repaint_, false);
        }
        PresentResult result;
        if (!minimized && frame_)
        {
            const auto submitted = producer_.SubmitRenderFrame(*frame_);
            if (!submitted.success)
            {
                Event("producer_failed");
                tick(result);
                continue;
            }
        }
        auto deadline = OperationDeadline::After(std::chrono::seconds(5));
        deadline      = OperationDeadline::At(std::min(deadline.Expiry(), drain_.GetCompletion().deadline.Expiry()));
#if REREVVED_DEVELOPER
        const auto cadenceBegin = cadence::Now();
#endif
        result = session_->PumpOnce(deadline);
        if (result.outcome == PresentOutcome::NoFrame && repaint)
            result = session_->Repaint(deadline);
#if REREVVED_DEVELOPER
        // Before the trace below drops unchanged outcomes.
        cadence::Present(cadenceBegin, result);
#endif
        if (events_)
        {
            trace.Observe(result, !frame_, [this](const char* name, std::uint64_t frame, std::uint64_t value)
                          {
                              Event(name, frame, value);
                          });
            const auto extent = session_->SwapChainExtent();
            if (extent != lastExtent)
                Event("surface", extent.width, extent.height);
            lastExtent = extent;
        }
        tick(result);
        // Hidden/occluded windows do not provide a blocking vsync cadence.
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(16), [this]
                       {
                           return stopping_;
                       });
    }
    const bool drained = drain_.Run([this]
                                    {
                                        producer_.Shutdown();
                                        Event("producer_stopped");
                                    },
                                    [this](OperationDeadline& deadline)
                                    {
                                        return session_->Shutdown(deadline).drained;
                                    });
    {
        std::lock_guard lock(mutex_);
        drained_  = drained;
        finished_ = true;
    }
    wake_.notify_all();
}

NativeAppDrain::Completion NativeAppSession::Close(std::function<void(bool)> completed)
{
    bool started;
    {
        std::lock_guard lock(mutex_);
        started = drain_.Start([this](OperationDeadline& deadline)
                               {
                                   session_->CloseAdmission(deadline);
                                   Event("close_admission");
                               },
                               [this]
                               {
                                   stopping_ = true;
                                   producer_.RequestDrawStreamStop();
                               });
    }
    const auto completion = drain_.GetCompletion();
    if (started)
    {
        wake_.notify_all();
        std::thread([self = shared_from_this(), deadline = completion.deadline]
                    {
                        std::unique_lock lock(self->mutex_);
                        const bool       finished = self->wake_.wait_until(lock, deadline.Expiry(), [&]
                                                                           {
                                                                         return self->finished_;
                                                                           });
                        const bool       drained  = finished && self->drained_;
                        lock.unlock();
                        if (!drained)
                        {
                            // A timed-out producer/driver may still reference these owners.
                            (void)new std::shared_ptr<NativeAppSession>(self);
                        }
                        self->Event("shutdown", 0, drained ? 1 : 0);
#if REREVVED_DEVELOPER
                        std::string cadenceError;
                        if (!CloseNativeCadenceTrace(cadenceError))
                            REXLOG_ERROR("{}", cadenceError);
#endif
                        self->FlushTrace();
                        self->drain_.Complete(drained);
                    })
            .detach();
    }
    if (completed)
        drain_.Subscribe(std::move(completed));
    return completion;
}

} // namespace rerevved::gpu
