#include "plume_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

#include <plume_d3d12.h>
#include <rex/logging.h>

namespace plume
{

std::unique_ptr<RenderInterface> CreateD3D12Interface();

} // namespace plume

namespace rerevved::gpu
{

namespace
{

int healthRank(HostHealth health) noexcept
{
    switch (health)
    {
        case HostHealth::Healthy:
            return 0;
        case HostHealth::Failed:
            return 1;
        case HostHealth::Unproved:
            return 2;
        case HostHealth::DeviceLost:
            return 3;
    }
    return 0;
}

} // namespace

HostResult MapWaitResult(const plume::RenderWaitResult& result, const char* operation) noexcept
{
    HostResult mapped;
    mapped.nativeCode = result.nativeCode;
    mapped.operation  = operation;
    switch (result.status)
    {
        case plume::RenderWaitStatus::SIGNALED:
            mapped.status = HostStatus::Ok;
            break;
        case plume::RenderWaitStatus::TIMEOUT:
            mapped.status = HostStatus::Timeout;
            break;
        case plume::RenderWaitStatus::DEVICE_LOST:
            mapped.status = HostStatus::DeviceLost;
            break;
        default:
            mapped.status = HostStatus::Failed;
            break;
    }
    return mapped;
}

HostResult MapSubmitResult(const plume::RenderSubmitResult& result, const char* operation) noexcept
{
    HostResult mapped;
    mapped.nativeCode = result.nativeCode;
    mapped.operation  = operation;
    switch (result.status)
    {
        case plume::RenderSubmitStatus::SUBMITTED:
            mapped.status = HostStatus::Ok;
            break;
        case plume::RenderSubmitStatus::DEVICE_LOST:
            mapped.status = HostStatus::DeviceLost;
            break;
        default:
            mapped.status = HostStatus::Failed;
            break;
    }
    return mapped;
}

HostResult MapPresentResult(const plume::RenderPresentResult& result, const char* operation) noexcept
{
    HostResult mapped;
    mapped.nativeCode = result.nativeCode;
    mapped.operation  = operation;
    switch (result.status)
    {
        case plume::RenderPresentStatus::PRESENTED:
            mapped.status = HostStatus::Ok;
            break;
        case plume::RenderPresentStatus::OCCLUDED:
            mapped.status = HostStatus::Occluded;
            break;
        case plume::RenderPresentStatus::OUT_OF_DATE_OR_SUBOPTIMAL:
            mapped.status = HostStatus::OutOfDate;
            break;
        case plume::RenderPresentStatus::DEVICE_LOST:
            mapped.status = HostStatus::DeviceLost;
            break;
        default:
            mapped.status = HostStatus::Failed;
            break;
    }
    return mapped;
}

struct PlumeHost::Impl
{
    struct Slot
    {
        std::unique_ptr<plume::RenderCommandList>  list;
        std::unique_ptr<plume::RenderCommandFence> fence;
        bool                                       recording = false;
        bool                                       inFlight  = false;
    };

    struct Fault
    {
        bool          armed     = false;
        HostOperation operation = HostOperation::Submit;
        HostStatus    status    = HostStatus::Failed;
        std::uint32_t skip      = 0;
    };

    // Reverse declaration order is destruction order: work slots first, the
    // interface last. The last slot is a fence marker for present work.
    std::function<void()>                                       beforeResize;
    Config                                                      config;
    std::unique_ptr<plume::RenderInterface>                     renderInterface;
    std::unique_ptr<plume::RenderDevice>                        device;
    std::unique_ptr<plume::RenderCommandQueue>                  queue;
    std::unique_ptr<plume::RenderSwapChain>                     swapChain;
    std::vector<std::unique_ptr<plume::RenderFramebuffer>>      framebuffers;
    std::unique_ptr<plume::RenderCommandSemaphore>              acquireSemaphore;
    std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> releaseSemaphores;
    std::vector<Slot>                                           slots;
    plume::D3D12Device*                                         d3dDevice = nullptr;
    std::atomic<HostHealth>                                     health{ HostHealth::Healthy };
    Fault                                                       fault;
    bool                                                        presentPending    = false;
    bool                                                        frameLatencyReady = false;

    void raise(HostHealth next) noexcept
    {
        auto current = health.load();
        while (healthRank(next) > healthRank(current) && !health.compare_exchange_weak(current, next))
        {
        }
    }

    // A failed submit or wait leaves GPU work unaccounted for; any other
    // failure is terminal without outstanding work.
    void observe(HostOperation operation, const HostResult& result) noexcept
    {
        switch (result.status)
        {
            case HostStatus::DeviceLost:
                raise(HostHealth::DeviceLost);
                break;
            case HostStatus::Timeout:
                raise(HostHealth::Unproved);
                break;
            case HostStatus::Failed:
                raise(operation == HostOperation::Present ? HostHealth::Failed : HostHealth::Unproved);
                break;
            default:
                break;
        }
    }

    HostResult terminal(const char* operation) const noexcept
    {
        HostResult result;
        result.status    = health == HostHealth::DeviceLost ? HostStatus::DeviceLost : HostStatus::Failed;
        result.operation = operation;
        return result;
    }

    bool takeFault(HostOperation operation, HostStatus& status) noexcept
    {
        if (!fault.armed || fault.operation != operation)
        {
            return false;
        }
        if (fault.skip != 0)
        {
            --fault.skip;
            return false;
        }
        fault.armed = false;
        status      = fault.status;
        return true;
    }

    bool createFramebuffers()
    {
        framebuffers.clear();
        for (std::uint32_t index = 0; index < swapChain->getTextureCount(); ++index)
        {
            const plume::RenderTexture*  color = swapChain->getTexture(index);
            plume::RenderFramebufferDesc desc;
            desc.colorAttachments      = &color;
            desc.colorAttachmentsCount = 1;
            auto framebuffer           = device->createFramebuffer(desc);
            if (!framebuffer)
            {
                framebuffers.clear();
                return false;
            }
            framebuffers.push_back(std::move(framebuffer));
        }
        return true;
    }

    // A present is queued after the fence of the work that drew it, so the
    // slot fences alone do not prove the backbuffer is idle. An empty marker
    // submitted afterwards completes only once the present work is done.
    HostResult retirePresentWork(OperationDeadline& deadline)
    {
        HostResult done;
        done.status    = HostStatus::Ok;
        done.operation = "retire present work";
        // A removed device has no work left to retire; a reported loss on a live
        // device still needs the marker.
        if (!presentPending || FAILED(d3dDevice->d3d->GetDeviceRemovedReason()))
        {
            return done;
        }
        const std::size_t index  = slots.size() - 1;
        auto&             marker = slots[index];
        if (auto waited = waitSlot(index, deadline, DeadlinePhase::Host); !waited.Ok())
        {
            return waited;
        }
        marker.list->begin();
        marker.list->end();
        const plume::RenderCommandList* lists[]   = { marker.list.get() };
        const auto                      submitted = queue->executeCommandListsChecked(lists, 1, nullptr, 0, nullptr, 0, marker.fence.get());
        marker.inFlight                           = true;
        auto result                               = MapSubmitResult(submitted, "retire present work");
        observe(HostOperation::Submit, result);
        if (!result.Ok())
        {
            return result;
        }
        result = waitSlot(index, deadline, DeadlinePhase::Host);
        if (result.Ok())
        {
            presentPending = false;
        }
        return result;
    }

    HostResult waitSlot(std::size_t slotIndex, OperationDeadline& deadline, DeadlinePhase phase)
    {
        auto& slot = slots[slotIndex];
        if (!slot.inFlight)
        {
            HostResult done;
            done.status    = HostStatus::Ok;
            done.operation = "wait for slot fence";
            return done;
        }
        HostStatus injected = HostStatus::Ok;
        if (takeFault(HostOperation::Wait, injected))
        {
            HostResult result;
            result.status    = injected;
            result.operation = "wait for slot fence (injected)";
            observe(HostOperation::Wait, result);
            return result;
        }
        const auto allotted = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.Allot(config.waitCap));
        const auto start    = OperationDeadline::Clock::now();
        const auto waited   = queue->waitForCommandFence(slot.fence.get(), static_cast<std::uint64_t>(allotted.count()));
        deadline.Charge(phase, OperationDeadline::Clock::now() - start);
        auto result = MapWaitResult(waited, "wait for slot fence");
        if (result.Ok())
        {
            slot.inFlight = false;
        }
        observe(HostOperation::Wait, result);
        return result;
    }
};

PlumeHost::PlumeHost()
: impl_(std::make_unique<Impl>())
{
}

PlumeHost::~PlumeHost() = default;

std::unique_ptr<PlumeHost> PlumeHost::Create(const Config& config, std::string& error)
{
    error.clear();
    if (!config.nativeWindow || config.bufferCount < 2 || config.workSlots == 0 || config.workSlots > 8)
    {
        error = "presentation host requires a window, at least two buffers and one to eight work slots";
        return nullptr;
    }
    std::unique_ptr<PlumeHost> host(new PlumeHost);
    auto&                      impl = *host->impl_;
    impl.config                     = config;
    impl.renderInterface            = plume::CreateD3D12Interface();
    if (!impl.renderInterface)
    {
        error = "no usable D3D12 device: Plume could not create its D3D12 interface";
        return nullptr;
    }
    impl.device = impl.renderInterface->createDevice();
    if (!impl.device)
    {
        error = "no usable D3D12 device: Plume could not create a D3D12 device";
        return nullptr;
    }
    impl.d3dDevice = dynamic_cast<plume::D3D12Device*>(impl.device.get());
    if (!impl.d3dDevice || !impl.d3dDevice->d3d)
    {
        error = "Plume device is not a usable D3D12 device";
        return nullptr;
    }
    impl.queue           = impl.device->createCommandQueue(plume::RenderCommandListType::DIRECT);
    const auto* d3dQueue = dynamic_cast<plume::D3D12CommandQueue*>(impl.queue.get());
    if (!d3dQueue || !d3dQueue->d3d)
    {
        error = "Plume could not create a direct queue";
        return nullptr;
    }
    if (!IsWindow(reinterpret_cast<HWND>(config.nativeWindow)))
    {
        error = "no usable window: the presentation HWND is invalid";
        return nullptr;
    }
    impl.swapChain = impl.queue->createSwapChain(plume::RenderSwapChainDesc(
        reinterpret_cast<plume::RenderWindow>(config.nativeWindow), plume::RenderFormat::R8G8B8A8_UNORM, config.bufferCount, true, config.maxFrameLatency));
    if (!impl.swapChain || impl.swapChain->isEmpty())
    {
        // A swap chain whose construction failed has no texture entries, and
        // its destructor indexes them, so it is intentionally never destroyed.
        (void)impl.swapChain.release();
        error = "Plume could not create a swap chain for the window";
        return nullptr;
    }
    impl.swapChain->setVsyncEnabled(config.vsync);
    impl.acquireSemaphore = impl.device->createCommandSemaphore();
    for (std::uint32_t index = 0; index < impl.swapChain->getTextureCount(); ++index)
    {
        impl.releaseSemaphores.push_back(impl.device->createCommandSemaphore());
    }
    for (std::uint32_t index = 0; index <= config.workSlots; ++index)
    {
        Impl::Slot slot;
        slot.list            = impl.queue->createCommandList();
        slot.fence           = impl.device->createCommandFence();
        const auto* d3dList  = dynamic_cast<plume::D3D12CommandList*>(slot.list.get());
        const auto* d3dFence = dynamic_cast<plume::D3D12CommandFence*>(slot.fence.get());
        if (!d3dList || !d3dList->d3d || !d3dFence || !d3dFence->d3d)
        {
            error = "Plume could not create a command list or fence";
            return nullptr;
        }
        impl.slots.push_back(std::move(slot));
    }
    if (!impl.acquireSemaphore || !impl.createFramebuffers())
    {
        error = "Plume could not create presentation framebuffers";
        return nullptr;
    }
    return host;
}

plume::RenderDevice& PlumeHost::Device() noexcept
{
    return *impl_->device;
}

plume::RenderCommandQueue& PlumeHost::Queue() noexcept
{
    return *impl_->queue;
}

plume::RenderSwapChain& PlumeHost::SwapChain() noexcept
{
    return *impl_->swapChain;
}

ID3D12Device* PlumeHost::NativeDevice() const noexcept
{
    return impl_->d3dDevice->d3d;
}

plume::RenderFramebuffer* PlumeHost::Framebuffer(std::uint32_t textureIndex) noexcept
{
    return textureIndex < impl_->framebuffers.size() ? impl_->framebuffers[textureIndex].get() : nullptr;
}

plume::RenderCommandSemaphore* PlumeHost::AcquireSemaphore() noexcept
{
    return impl_->acquireSemaphore.get();
}

plume::RenderCommandSemaphore* PlumeHost::ReleaseSemaphore(std::uint32_t textureIndex) noexcept
{
    return textureIndex < impl_->releaseSemaphores.size() ? impl_->releaseSemaphores[textureIndex].get() : nullptr;
}

HostResult PlumeHost::BeginSlot(std::size_t slot, OperationDeadline& deadline)
{
    auto& impl = *impl_;
    if (slot >= impl.config.workSlots || impl.slots[slot].recording)
    {
        HostResult result;
        result.operation = "begin slot";
        return result;
    }
    if (impl.health != HostHealth::Healthy)
    {
        return impl.terminal("begin slot");
    }
    if (auto result = impl.waitSlot(slot, deadline, DeadlinePhase::Host); !result.Ok())
    {
        return result;
    }
    impl.slots[slot].list->begin();
    impl.slots[slot].recording = true;
    HostResult result;
    result.status    = HostStatus::Ok;
    result.operation = "begin slot";
    return result;
}

plume::RenderCommandList& PlumeHost::SlotList(std::size_t slot) noexcept
{
    return *impl_->slots[slot].list;
}

HostResult PlumeHost::SubmitSlot(std::size_t slotIndex, plume::RenderCommandSemaphore* wait, plume::RenderCommandSemaphore* signal)
{
    auto& impl = *impl_;
    if (slotIndex >= impl.config.workSlots || !impl.slots[slotIndex].recording)
    {
        HostResult result;
        result.operation = "submit slot";
        return result;
    }
    auto& slot = impl.slots[slotIndex];
    slot.list->end();
    slot.recording = false;
    if (impl.health != HostHealth::Healthy)
    {
        return impl.terminal("submit slot");
    }
    HostStatus injected = HostStatus::Ok;
    if (impl.takeFault(HostOperation::Submit, injected))
    {
        HostResult result;
        result.status    = injected;
        result.operation = "submit slot (injected)";
        impl.observe(HostOperation::Submit, result);
        return result;
    }
    const plume::RenderCommandList* lists[]   = { slot.list.get() };
    plume::RenderCommandSemaphore*  waits[]   = { wait };
    plume::RenderCommandSemaphore*  signals[] = { signal };
    const auto                      submitted = impl.queue->executeCommandListsChecked(lists, 1, wait ? waits : nullptr, wait ? 1U : 0U, signal ? signals : nullptr, signal ? 1U : 0U, slot.fence.get());
    // Even a failed submit may have issued earlier steps; the fence keeps the
    // failure for the next wait.
    slot.inFlight = true;
    auto result   = MapSubmitResult(submitted, "ExecuteCommandLists");
    impl.observe(HostOperation::Submit, result);
    return result;
}

HostResult PlumeHost::WaitSlot(std::size_t slot, OperationDeadline& deadline, DeadlinePhase phase)
{
    if (slot >= impl_->config.workSlots)
    {
        HostResult result;
        result.operation = "wait slot";
        return result;
    }
    return impl_->waitSlot(slot, deadline, phase);
}

bool PlumeHost::SlotInFlight(std::size_t slot) const noexcept
{
    return slot < impl_->config.workSlots && impl_->slots[slot].inFlight;
}

HostResult PlumeHost::Drain(OperationDeadline& deadline)
{
    auto&      impl = *impl_;
    HostResult worst;
    worst.status    = HostStatus::Ok;
    worst.operation = "drain";
    const auto rank = [](HostStatus status)
    {
        switch (status)
        {
            case HostStatus::Ok:
                return 0;
            case HostStatus::Failed:
                return 1;
            case HostStatus::Timeout:
                return 2;
            default:
                return 3;
        }
    };
    // Every in-flight slot is visited against the one shared deadline.
    for (std::size_t slot = 0; slot < impl.config.workSlots; ++slot)
    {
        if (!impl.slots[slot].inFlight)
        {
            continue;
        }
        auto result = impl.waitSlot(slot, deadline, DeadlinePhase::Host);
        if (rank(result.status) > rank(worst.status))
        {
            worst = result;
        }
    }
    if (auto retired = impl.retirePresentWork(deadline); rank(retired.status) > rank(worst.status))
    {
        worst = retired;
    }
    return worst;
}

HostResult PlumeHost::AcquireBackbuffer(std::uint32_t& textureIndex, OperationDeadline& deadline)
{
    auto& impl = *impl_;
    if (impl.health != HostHealth::Healthy)
    {
        return impl.terminal("acquire backbuffer");
    }
    // A synthetic present result consumes no DXGI frame. Keep its acquired
    // latency permit until a real Present consumes it.
    if (!impl.frameLatencyReady)
    {
        const auto start    = OperationDeadline::Clock::now();
        const auto allotted = deadline.Allot(impl.config.waitCap);
        const auto paced    = MapWaitResult(impl.swapChain->wait(static_cast<std::uint64_t>(allotted.count())), "frame latency wait");
        deadline.Charge(DeadlinePhase::Host, OperationDeadline::Clock::now() - start);
        if (!paced.Ok())
        {
            impl.observe(HostOperation::Wait, paced);
            return paced;
        }
        impl.frameLatencyReady = true;
    }
    HostResult result;
    result.operation = "acquire backbuffer";
    if (impl.swapChain->acquireTexture(impl.acquireSemaphore.get(), &textureIndex) && textureIndex < impl.framebuffers.size())
    {
        result.status = HostStatus::Ok;
    }
    impl.observe(HostOperation::Present, result);
    return result;
}

HostResult PlumeHost::Present(std::uint32_t textureIndex)
{
    auto& impl = *impl_;
    if (impl.health != HostHealth::Healthy)
    {
        return impl.terminal("present");
    }
    HostStatus injected = HostStatus::Ok;
    if (impl.takeFault(HostOperation::Present, injected))
    {
        HostResult result;
        result.status    = injected;
        result.operation = "present (injected)";
        impl.observe(HostOperation::Present, result);
        return result;
    }
    plume::RenderCommandSemaphore* release[] = { ReleaseSemaphore(textureIndex) };
    const auto                     presented = impl.swapChain->presentChecked(textureIndex, release[0] ? release : nullptr, release[0] ? 1U : 0U);
    impl.presentPending                      = true;
    impl.frameLatencyReady                   = false;
    auto result                              = MapPresentResult(presented, "present");
    impl.observe(HostOperation::Present, result);
    return result;
}

HostResult PlumeHost::ResizeSwapChain(OperationDeadline& deadline, SurfaceExtent& actual, bool& performed)
{
    auto& impl = *impl_;
    performed  = false;
    if (impl.health != HostHealth::Healthy)
    {
        return impl.terminal("resize swap chain");
    }
    RECT client{};
    if (!GetClientRect(impl.swapChain->getWindow(), &client) || client.right <= client.left || client.bottom <= client.top)
    {
        HostResult skipped;
        skipped.status    = HostStatus::Ok;
        skipped.operation = "resize swap chain";
        return skipped;
    }
    // Every backbuffer user must retire before the buffers are released.
    if (auto drained = Drain(deadline); !drained.Ok())
    {
        return drained;
    }
    impl.framebuffers.clear();
    HostResult result;
    result.operation = "resize swap chain";
    if (impl.beforeResize)
        std::exchange(impl.beforeResize, {})();
    // The host only creates D3D12 swap chains. Unlike resize(), the checked
    // call keeps the buffers and their size when the client area is empty.
    const auto resized = static_cast<plume::D3D12SwapChain&>(*impl.swapChain).resizeChecked();
    result.nativeCode  = resized.nativeCode;
    switch (resized.status)
    {
        case plume::RenderResizeStatus::RESIZED:
            break;
        case plume::RenderResizeStatus::ZERO_AREA:
            // The area emptied after the check above; restore the views and let the session suspend.
            if (impl.createFramebuffers())
                result.status = HostStatus::Ok;
            else
                impl.raise(HostHealth::Failed);
            return result;
        case plume::RenderResizeStatus::DEVICE_LOST:
            result.status = HostStatus::DeviceLost;
            impl.raise(HostHealth::DeviceLost);
            return result;
        default:
            impl.raise(HostHealth::Failed);
            return result;
    }
    if (!impl.createFramebuffers())
    {
        impl.raise(HostHealth::Failed);
        return result;
    }
    actual.width  = impl.swapChain->getWidth();
    actual.height = impl.swapChain->getHeight();
    performed     = true;
    result.status = HostStatus::Ok;
    return result;
}

std::uint32_t PlumeHost::PresentInterval() const noexcept
{
    const auto& chain = static_cast<const plume::D3D12SwapChain&>(*impl_->swapChain);
    return (chain.isVsyncEnabled() && !chain.desc.enablePresentWait) ? 1U : 0U;
}

HostHealth PlumeHost::Health() const noexcept
{
    return impl_->health;
}

bool PlumeHost::RetainsOwnership() const noexcept
{
    const auto health = impl_->health.load();
    return health == HostHealth::Unproved || (health == HostHealth::DeviceLost && SUCCEEDED(NativeDevice()->GetDeviceRemovedReason()));
}

void PlumeHost::FinalizeDrain(bool drained) noexcept
{
    // A fence sentinel or injected status is not proof of actual removal.
    if (FAILED(NativeDevice()->GetDeviceRemovedReason()))
        impl_->health.store(HostHealth::DeviceLost);
    else if (!drained || RetainsOwnership())
        impl_->health.store(HostHealth::Unproved);
}

void PlumeHost::NoteDeviceLost() noexcept
{
    impl_->raise(HostHealth::DeviceLost);
}

void PlumeHost::NoteUnproved() noexcept
{
    impl_->raise(HostHealth::Unproved);
}

void PlumeHost::NoteFailed() noexcept
{
    impl_->raise(HostHealth::Failed);
}

bool PlumeHost::PollDeviceLost() noexcept
{
    if (FAILED(impl_->d3dDevice->d3d->GetDeviceRemovedReason()))
    {
        impl_->raise(HostHealth::DeviceLost);
    }
    return impl_->health == HostHealth::DeviceLost;
}

void PlumeHost::beforeResizeForTesting(std::function<void()> callback)
{
    impl_->beforeResize = std::move(callback);
}

void PlumeHost::InjectFault(HostOperation operation, HostStatus status, std::uint32_t skip) noexcept
{
    impl_->fault = { true, operation, status, skip };
}

} // namespace rerevved::gpu
