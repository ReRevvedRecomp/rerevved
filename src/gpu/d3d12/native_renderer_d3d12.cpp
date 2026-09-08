#include "native_renderer_d3d12.h"

#include "gpu/diagnostics/native_renderer_guest_state.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include <rex/logging.h>
#include <rex/ui/window.h>

#if defined(_WIN32)
#include <array>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#endif

namespace rerevved::gpu
{

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

namespace
{

constexpr std::uint32_t kFrameCount         = 2;
constexpr DWORD         kFenceWaitTimeoutMs = 5000;

bool logFailure(const char* operation, HRESULT result)
{
    REXLOG_ERROR("Native D3D12 {} failed: HRESULT 0x{:08X}", operation, static_cast<std::uint32_t>(result));
    return false;
}

void enableDred()
{
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
    const HRESULT                                    result = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
    if (FAILED(result))
    {
        REXLOG_WARN("Native D3D12 DRED settings unavailable: HRESULT 0x{:08X}",
                    static_cast<std::uint32_t>(result));
        return;
    }
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    REXLOG_INFO("Native D3D12 DRED enabled");
}

} // namespace
#endif

struct NativeRendererD3D12::Impl
{
    std::thread             rendererThread;
    std::mutex              stateMutex;
    std::condition_variable stateCv;
    bool                    stopRequested           = false;
    bool                    initializationDone      = false;
    bool                    initializationSucceeded = false;
    bool                    resizePending           = false;
    std::uint32_t           requestedWidth          = 0;
    std::uint32_t           requestedHeight         = 0;
    bool                    runtimeFailed           = false;
    std::function<void()>   requestDeferredQuit;
    std::atomic<bool>       initialized{ false };
    std::atomic<bool>       gpuObjectsAbandoned{ false };

#if defined(_WIN32)
    ComPtr<IDXGIFactory6>                                   factory;
    ComPtr<IDXGIAdapter1>                                   adapter;
    ComPtr<ID3D12Device>                                    device;
    ComPtr<ID3D12CommandQueue>                              queue;
    ComPtr<IDXGISwapChain3>                                 swapChain;
    ComPtr<ID3D12DescriptorHeap>                            rtvHeap;
    std::array<ComPtr<ID3D12Resource>, kFrameCount>         backBuffers;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> allocators;
    ComPtr<ID3D12GraphicsCommandList>                       commandList;
    ComPtr<ID3D12Fence>                                     fence;
    std::array<std::uint64_t, kFrameCount>                  fenceValues{};
    std::uint64_t                                           nextFenceValue = 1;
    HANDLE                                                  fenceEvent     = nullptr;
    std::uint32_t                                           rtvStride      = 0;
    std::uint32_t                                           width          = 0;
    std::uint32_t                                           height         = 0;

    bool waitForFence(std::uint64_t value)
    {
        if (value == 0 || fence->GetCompletedValue() >= value)
        {
            return true;
        }
        const HRESULT result = fence->SetEventOnCompletion(value, fenceEvent);
        if (FAILED(result))
        {
            return logDeviceRemoval("fence event", result);
        }
        const DWORD waitResult =
            WaitForSingleObject(fenceEvent, kFenceWaitTimeoutMs);
        if (waitResult == WAIT_TIMEOUT)
        {
            REXLOG_ERROR("Native D3D12 fence wait timed out after {} ms",
                         kFenceWaitTimeoutMs);
            const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
            if (FAILED(reason))
            {
                REXLOG_ERROR("Native D3D12 device removed after fence timeout: HRESULT 0x{:08X}",
                             static_cast<std::uint32_t>(reason));
            }
            return false;
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            REXLOG_ERROR("Native D3D12 fence wait failed: Win32 error {}",
                         GetLastError());
            return false;
        }
        return true;
    }

    bool waitForGpu()
    {
        const std::uint64_t value  = nextFenceValue++;
        const HRESULT       result = queue->Signal(fence.Get(), value);
        if (FAILED(result))
        {
            return logDeviceRemoval("queue signal", result);
        }
        return waitForFence(value);
    }

    void releaseBackBuffers()
    {
        for (auto& buffer : backBuffers)
        {
            buffer.Reset();
        }
    }

    bool createBackBuffers()
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE heapStart =
            rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t index = 0; index < kFrameCount; ++index)
        {
            const HRESULT result =
                swapChain->GetBuffer(index, IID_PPV_ARGS(&backBuffers[index]));
            if (FAILED(result))
            {
                return logFailure("swap-chain buffer", result);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE handle = heapStart;
            handle.ptr += static_cast<SIZE_T>(index) * rtvStride;
            device->CreateRenderTargetView(backBuffers[index].Get(), nullptr, handle);
        }
        return true;
    }

    bool logDeviceRemoval(const char* operation, HRESULT result)
    {
        REXLOG_ERROR("Native D3D12 {} failed: HRESULT 0x{:08X}", operation, static_cast<std::uint32_t>(result));
        const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
        if (FAILED(reason))
        {
            REXLOG_ERROR("Native D3D12 device removed: HRESULT 0x{:08X}",
                         static_cast<std::uint32_t>(reason));
        }
        return false;
    }

    bool initializeOnRendererThread(std::uintptr_t nativeWindow,
                                    std::uint32_t  initialWidth,
                                    std::uint32_t  initialHeight);
    bool resizeOnRendererThread(std::uint32_t width, std::uint32_t height);
    bool presentOnRendererThread();
    void shutdownOnRendererThread();
#endif
};

NativeRendererD3D12::NativeRendererD3D12()
: impl(std::make_unique<Impl>())
{
}

NativeRendererD3D12::~NativeRendererD3D12()
{
    Shutdown();
}

#if defined(_WIN32)

bool NativeRendererD3D12::Impl::initializeOnRendererThread(
    std::uintptr_t nativeWindow,
    std::uint32_t  initialWidth,
    std::uint32_t  initialHeight)
{
    const HWND hwnd = reinterpret_cast<HWND>(nativeWindow);
    if (!hwnd || initialWidth == 0 || initialHeight == 0)
    {
        REXLOG_ERROR("Native D3D12 surface has no drawable HWND extent");
        return false;
    }
    width  = initialWidth;
    height = initialHeight;

    const auto failInitialization = [this]()
    {
        shutdownOnRendererThread();
        return false;
    };

    enableDred();
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        logFailure("factory creation", result);
        return failInitialization();
    }

    for (std::uint32_t index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> candidate;
        result = factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (result == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(result))
        {
            logFailure("adapter enumeration", result);
            return failInitialization();
        }

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(candidate->GetDesc1(&description)) ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }

        ComPtr<ID3D12Device> candidateDevice;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(),
                                        D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&candidateDevice))))
        {
            adapter = std::move(candidate);
            device  = std::move(candidateDevice);
            device->SetName(L"ReRevved native renderer device");
            REXLOG_INFO("Native D3D12 adapter selected: vendor={:04X} device={:04X}",
                        description.VendorId,
                        description.DeviceId);
            break;
        }
    }
    if (!device)
    {
        REXLOG_ERROR("Native D3D12 found no compatible hardware adapter");
        return failInitialization();
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result         = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    if (FAILED(result))
    {
        logFailure("command queue creation", result);
        return failInitialization();
    }
    queue->SetName(L"ReRevved native renderer direct queue");

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width       = width;
    swapChainDesc.Height      = height;
    swapChainDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.SampleDesc  = { 1, 0 };
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> newSwapChain;
    result = factory->CreateSwapChainForHwnd(
        queue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &newSwapChain);
    if (FAILED(result))
    {
        logFailure("swap chain creation", result);
        return failInitialization();
    }
    result = newSwapChain.As(&swapChain);
    if (FAILED(result))
    {
        logFailure("swap chain interface", result);
        return failInitialization();
    }
    result = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(result))
    {
        logFailure("window association", result);
        return failInitialization();
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kFrameCount;
    result                 = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));
    if (FAILED(result))
    {
        logFailure("RTV heap creation", result);
        return failInitialization();
    }
    rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!createBackBuffers())
    {
        return failInitialization();
    }

    for (std::uint32_t index = 0; index < kFrameCount; ++index)
    {
        auto& allocator = allocators[index];
        result          = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (FAILED(result))
        {
            logFailure("command allocator creation", result);
            return failInitialization();
        }
        allocator->SetName(index == 0 ? L"ReRevved native frame allocator 0"
                                      : L"ReRevved native frame allocator 1");
    }
    result = device->CreateCommandList(0,
                                       D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocators[0].Get(),
                                       nullptr,
                                       IID_PPV_ARGS(&commandList));
    if (FAILED(result))
    {
        logFailure("command list creation", result);
        return failInitialization();
    }
    commandList->SetName(L"ReRevved native frame command list");
    result = commandList->Close();
    if (FAILED(result))
    {
        logFailure("initial command list close", result);
        return failInitialization();
    }
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result))
    {
        logFailure("fence creation", result);
        return failInitialization();
    }
    fence->SetName(L"ReRevved native frame fence");
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent)
    {
        REXLOG_ERROR("Native D3D12 fence event creation failed: Win32 error {}",
                     GetLastError());
        return failInitialization();
    }

    initialized.store(true, std::memory_order_release);
    REXLOG_INFO("Native D3D12 initialized: {}x{} frames={}", width, height, kFrameCount);
    if (!presentOnRendererThread())
    {
        shutdownOnRendererThread();
        return false;
    }
    REXLOG_INFO("Native D3D12 diagnostic frame presented");
    return true;
}

bool NativeRendererD3D12::Impl::resizeOnRendererThread(std::uint32_t newWidth,
                                                       std::uint32_t newHeight)
{
    if (newWidth == 0 || newHeight == 0 || !initialized.load(std::memory_order_acquire))
    {
        return newWidth == 0 || newHeight == 0;
    }
    if (newWidth == width && newHeight == height)
    {
        return true;
    }
    if (!waitForGpu())
    {
        return false;
    }

    releaseBackBuffers();
    fenceValues.fill(0);
    const HRESULT result = swapChain->ResizeBuffers(
        kFrameCount, newWidth, newHeight, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(result))
    {
        logDeviceRemoval("swap chain resize", result);
        return false;
    }
    width  = newWidth;
    height = newHeight;
    if (!createBackBuffers())
    {
        return false;
    }
    REXLOG_INFO("Native D3D12 resized: {}x{}", newWidth, newHeight);
    return presentOnRendererThread();
}

bool NativeRendererD3D12::Impl::presentOnRendererThread()
{
    if (!initialized.load(std::memory_order_acquire))
    {
        return false;
    }

    const std::uint32_t frameIndex = swapChain->GetCurrentBackBufferIndex();
    if (!waitForFence(fenceValues[frameIndex]))
    {
        return false;
    }

    HRESULT result = allocators[frameIndex]->Reset();
    if (FAILED(result))
    {
        logFailure("command allocator reset", result);
        return false;
    }
    result = commandList->Reset(allocators[frameIndex].Get(), nullptr);
    if (FAILED(result))
    {
        logFailure("command list reset", result);
        return false;
    }

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource   = backBuffers[frameIndex].Get();
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &toRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frameIndex) * rtvStride;
    constexpr float clearColor[4] = { 0.015F, 0.02F, 0.04F, 1.0F };
    commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    commandList->ResourceBarrier(1, &toPresent);

    result = commandList->Close();
    if (FAILED(result))
    {
        logFailure("command list close", result);
        return false;
    }
    ID3D12CommandList* lists[] = { commandList.Get() };
    queue->ExecuteCommandLists(1, lists);

    const std::uint64_t fenceValue = nextFenceValue++;
    result                         = queue->Signal(fence.Get(), fenceValue);
    if (FAILED(result))
    {
        logDeviceRemoval("frame signal", result);
        return false;
    }
    fenceValues[frameIndex] = fenceValue;

    result = swapChain->Present(1, 0);
    if (FAILED(result))
    {
        logDeviceRemoval("present", result);
        return false;
    }
    return true;
}

void NativeRendererD3D12::Impl::shutdownOnRendererThread()
{
    bool abandonGpuObjects = false;
    if (initialized.load(std::memory_order_acquire) && queue && fence && !waitForGpu())
    {
        const HRESULT reason = device ? device->GetDeviceRemovedReason() : E_FAIL;
        abandonGpuObjects    = !FAILED(reason);
        if (abandonGpuObjects)
        {
            // A live device with an uncompleted fence may still reference every
            // submitted object. Let process teardown reclaim them instead of
            // releasing storage that the GPU may still be using.
            REXLOG_ERROR("Native D3D12 drain failed on a live device; abandoning GPU objects until process exit");
        }
    }
    initialized.store(false, std::memory_order_release);
    if (abandonGpuObjects)
    {
        (void)commandList.Detach();
        for (auto& allocator : allocators)
        {
            (void)allocator.Detach();
        }
        for (auto& backBuffer : backBuffers)
        {
            (void)backBuffer.Detach();
        }
        (void)rtvHeap.Detach();
        (void)swapChain.Detach();
        (void)queue.Detach();
        (void)fence.Detach();
        (void)device.Detach();
        (void)adapter.Detach();
        (void)factory.Detach();
        fenceEvent = nullptr;
        gpuObjectsAbandoned.store(true, std::memory_order_release);
        return;
    }
    commandList.Reset();
    for (auto& allocator : allocators)
    {
        allocator.Reset();
    }
    releaseBackBuffers();
    rtvHeap.Reset();
    swapChain.Reset();
    queue.Reset();
    fence.Reset();
    device.Reset();
    adapter.Reset();
    factory.Reset();
    if (fenceEvent)
    {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
    fenceValues.fill(0);
    nextFenceValue = 1;
    width          = 0;
    height         = 0;
}

#endif

void NativeRendererD3D12::rendererThreadMain(std::uintptr_t nativeWindow,
                                             std::uint32_t  width,
                                             std::uint32_t  height)
{
#if defined(_WIN32)
    const bool initializationSucceeded =
        impl->initializeOnRendererThread(nativeWindow, width, height);
    {
        std::lock_guard lock(impl->stateMutex);
        impl->initializationSucceeded = initializationSucceeded;
        impl->initializationDone      = true;
    }
    impl->stateCv.notify_all();
    if (!initializationSucceeded)
    {
        return;
    }

    for (;;)
    {
        bool          resize          = false;
        std::uint32_t requestedWidth  = 0;
        std::uint32_t requestedHeight = 0;
        {
            std::unique_lock lock(impl->stateMutex);
            impl->stateCv.wait(lock, [this]()
                               {
                                   return impl->stopRequested || impl->resizePending;
                               });
            if (impl->stopRequested)
            {
                break;
            }
            resize              = impl->resizePending;
            requestedWidth      = impl->requestedWidth;
            requestedHeight     = impl->requestedHeight;
            impl->resizePending = false;
        }

        bool requestSucceeded = true;
        if (resize)
        {
            requestSucceeded = impl->resizeOnRendererThread(
                requestedWidth, requestedHeight);
        }
        if (!requestSucceeded)
        {
            handleRendererFailure();
            break;
        }
    }
    impl->shutdownOnRendererThread();
#else
    (void)nativeWindow;
    (void)width;
    (void)height;
    {
        std::lock_guard lock(impl->stateMutex);
        impl->initializationSucceeded = false;
        impl->initializationDone      = true;
    }
    impl->stateCv.notify_all();
#endif
    impl->initialized.store(false, std::memory_order_release);
}

void NativeRendererD3D12::handleRendererFailure()
{
    std::function<void()> requestQuit;
    {
        std::lock_guard lock(impl->stateMutex);
        if (impl->runtimeFailed)
        {
            return;
        }
        impl->runtimeFailed = true;
        impl->stopRequested = true;
        requestQuit         = impl->requestDeferredQuit;
    }
    REXLOG_ERROR("Native D3D12 renderer entered terminal failure; requesting deferred quit");
    impl->stateCv.notify_all();
    if (requestQuit)
    {
        requestQuit();
    }
}

bool NativeRendererD3D12::Initialize(rex::ui::Window& window)
{
#if !defined(_WIN32)
    (void)window;
    REXLOG_ERROR("The native renderer currently requires Windows D3D12");
    return false;
#else
    if (impl->initialized.load(std::memory_order_acquire))
    {
        return true;
    }
    if (impl->gpuObjectsAbandoned.load(std::memory_order_acquire))
    {
        REXLOG_ERROR("Native D3D12 cannot reinitialize after abandoning in-flight GPU objects");
        return false;
    }
    if (impl->rendererThread.joinable())
    {
        impl->rendererThread.join();
    }

    const HWND hwnd   = static_cast<HWND>(window.GetNativeWindowHandle());
    const auto width  = window.GetActualPhysicalWidth();
    const auto height = window.GetActualPhysicalHeight();
    if (!hwnd)
    {
        REXLOG_ERROR("Native D3D12 could not acquire the ReRevved HWND surface");
        return false;
    }
    if (width == 0 || height == 0)
    {
        REXLOG_ERROR("Native D3D12 surface has no drawable pixel extent");
        return false;
    }

    ResetGuestDevicePublication();
    ResetGuestTextureObservation();
    ResetGuestSwapCorrelation();
    auto* appContext = &window.app_context();
    {
        std::lock_guard lock(impl->stateMutex);
        impl->stopRequested           = false;
        impl->initializationDone      = false;
        impl->initializationSucceeded = false;
        impl->resizePending           = false;
        impl->requestedWidth          = width;
        impl->requestedHeight         = height;
        impl->runtimeFailed           = false;
        impl->requestDeferredQuit     = [appContext]()
        {
            appContext->RequestDeferredQuit();
        };
    }

    impl->rendererThread = std::thread(
        &NativeRendererD3D12::rendererThreadMain,
        this,
        reinterpret_cast<std::uintptr_t>(hwnd),
        width,
        height);
    {
        std::unique_lock lock(impl->stateMutex);
        impl->stateCv.wait(lock, [this]()
                           {
                               return impl->initializationDone;
                           });
        if (!impl->initializationSucceeded)
        {
            lock.unlock();
            impl->rendererThread.join();
            return false;
        }
    }
    return true;
#endif
}

bool NativeRendererD3D12::Resize(std::uint32_t width, std::uint32_t height)
{
    // A minimized window has no drawable extent. Keep the last valid request
    // and let the next non-zero callback wake the renderer.
    if (width == 0 || height == 0)
    {
        return true;
    }
    std::lock_guard lock(impl->stateMutex);
    if (!impl->initialized.load(std::memory_order_acquire) ||
        impl->stopRequested || impl->runtimeFailed)
    {
        return false;
    }

    // The UI thread only updates this mailbox; the renderer consumes the
    // latest dimensions and performs all DXGI/D3D12 work itself.
    impl->requestedWidth  = width;
    impl->requestedHeight = height;
    impl->resizePending   = true;
    impl->stateCv.notify_one();
    return true;
}

void NativeRendererD3D12::Shutdown()
{
    if (!impl->rendererThread.joinable())
    {
        std::lock_guard lock(impl->stateMutex);
        impl->initialized.store(false, std::memory_order_release);
        impl->requestDeferredQuit = {};
        impl->stateCv.notify_all();
        return;
    }
    {
        std::lock_guard lock(impl->stateMutex);
        impl->stopRequested = true;
        impl->resizePending = false;
    }
    impl->stateCv.notify_all();
    if (impl->rendererThread.get_id() != std::this_thread::get_id())
    {
        impl->rendererThread.join();
    }
    std::lock_guard lock(impl->stateMutex);
    impl->requestDeferredQuit = {};
}

bool NativeRendererD3D12::Initialized() const noexcept
{
    return impl->initialized.load(std::memory_order_acquire);
}

} // namespace rerevved::gpu
