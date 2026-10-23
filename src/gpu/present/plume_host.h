#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "native_presentation.h"

namespace rerevved::gpu
{

namespace detail
{

struct PlumeHostTestAccess;

}

struct HostResult
{
    HostStatus   status     = HostStatus::Failed;
    std::int32_t nativeCode = 0;
    const char*  operation  = "";

    bool Ok() const noexcept
    {
        return status == HostStatus::Ok;
    }
};

HostResult MapWaitResult(const plume::RenderWaitResult& result, const char* operation) noexcept;
HostResult MapSubmitResult(const plume::RenderSubmitResult& result, const char* operation) noexcept;
HostResult MapPresentResult(const plume::RenderPresentResult& result, const char* operation) noexcept;

// Plume waits use the shared deadline and never reuse a slot while submission
// is unproved.
class PlumeHost
{
public:
    struct Config
    {
        // An HWND on Windows.
        void*         nativeWindow    = nullptr;
        std::uint32_t bufferCount     = 2;
        std::uint32_t maxFrameLatency = 2;
        std::uint32_t workSlots       = 3;
        bool          vsync           = true;
        // Upper bound for one bounded wait, further limited by the caller's shared deadline.
        std::chrono::milliseconds waitCap{ 5000 };
    };

    static std::unique_ptr<PlumeHost> Create(const Config& config, std::string& error);
    ~PlumeHost();

    PlumeHost(const PlumeHost&)            = delete;
    PlumeHost& operator=(const PlumeHost&) = delete;

    plume::RenderDevice&       Device() noexcept;
    plume::RenderCommandQueue& Queue() noexcept;
    plume::RenderSwapChain&    SwapChain() noexcept;
    // The native handle behind Plume's device, for the raw producer and the
    // interop resources that Plume has no format for.
    ID3D12Device* NativeDevice() const noexcept;

    plume::RenderFramebuffer*      Framebuffer(std::uint32_t textureIndex) noexcept;
    plume::RenderCommandSemaphore* AcquireSemaphore() noexcept;
    plume::RenderCommandSemaphore* ReleaseSemaphore(std::uint32_t textureIndex) noexcept;

    // Waits for the slot's previous submission, then begins recording.
    HostResult                BeginSlot(std::size_t slot, OperationDeadline& deadline);
    plume::RenderCommandList& SlotList(std::size_t slot) noexcept;
    HostResult                SubmitSlot(std::size_t slot, plume::RenderCommandSemaphore* wait, plume::RenderCommandSemaphore* signal);
    HostResult                WaitSlot(std::size_t slot, OperationDeadline& deadline, DeadlinePhase phase);
    bool                      SlotInFlight(std::size_t slot) const noexcept;
    HostResult                Drain(OperationDeadline& deadline);

    HostResult AcquireBackbuffer(std::uint32_t& textureIndex, OperationDeadline& deadline);
    HostResult Present(std::uint32_t textureIndex);
    // Retires every backbuffer user, then resizes to the client area. A zero
    // client area performs nothing.
    HostResult ResizeSwapChain(OperationDeadline& deadline, SurfaceExtent& actual, bool& performed);
    // The DXGI sync interval Present passes. With present wait enabled, Plume
    // presents at interval 0 and the frame-latency wait paces instead.
    std::uint32_t PresentInterval() const noexcept;

    HostHealth Health() const noexcept;
    bool       RetainsOwnership() const noexcept;
    void       FinalizeDrain(bool drained) noexcept;
    // Folds an observation made outside the host, such as a producer fence
    // that reports device removal, into the health state.
    void NoteDeviceLost() noexcept;
    void NoteUnproved() noexcept;
    void NoteFailed() noexcept;
    // Reports whether the device has been removed.
    bool PollDeviceLost() noexcept;

    // Test hook: the next matching operation, after `skip` successful ones,
    // reports `status` instead of reaching Plume.
    void InjectFault(HostOperation operation, HostStatus status, std::uint32_t skip = 0) noexcept;

private:
    friend struct detail::PlumeHostTestAccess;
    void beforeResizeForTesting(std::function<void()> callback);
    PlumeHost();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rerevved::gpu
