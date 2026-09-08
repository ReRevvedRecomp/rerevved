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

bool LogFailure(const char* operation, HRESULT result)
{
    REXLOG_ERROR("Native D3D12 {} failed: HRESULT 0x{:08X}", operation, static_cast<std::uint32_t>(result));
    return false;
}

void EnableDred()
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
    std::thread             renderer_thread;
    std::mutex              state_mutex;
    std::condition_variable state_cv;
    bool                    stop_requested           = false;
    bool                    initialization_done      = false;
    bool                    initialization_succeeded = false;
    bool                    resize_pending           = false;
    std::uint32_t           requested_width          = 0;
    std::uint32_t           requested_height         = 0;
    bool                    runtime_failed           = false;
    std::function<void()>   request_deferred_quit;
    std::atomic<bool>       initialized{ false };
    std::atomic<bool>       gpu_objects_abandoned{ false };

#if defined(_WIN32)
    ComPtr<IDXGIFactory6>                                   factory;
    ComPtr<IDXGIAdapter1>                                   adapter;
    ComPtr<ID3D12Device>                                    device;
    ComPtr<ID3D12CommandQueue>                              queue;
    ComPtr<IDXGISwapChain3>                                 swap_chain;
    ComPtr<ID3D12DescriptorHeap>                            rtv_heap;
    std::array<ComPtr<ID3D12Resource>, kFrameCount>         back_buffers;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> allocators;
    ComPtr<ID3D12GraphicsCommandList>                       command_list;
    ComPtr<ID3D12Fence>                                     fence;
    std::array<std::uint64_t, kFrameCount>                  fence_values{};
    std::uint64_t                                           next_fence_value = 1;
    HANDLE                                                  fence_event      = nullptr;
    std::uint32_t                                           rtv_stride       = 0;
    std::uint32_t                                           width            = 0;
    std::uint32_t                                           height           = 0;

    bool WaitForFence(std::uint64_t value)
    {
        if (value == 0 || fence->GetCompletedValue() >= value)
        {
            return true;
        }
        const HRESULT result = fence->SetEventOnCompletion(value, fence_event);
        if (FAILED(result))
        {
            return LogDeviceRemoval("fence event", result);
        }
        const DWORD wait_result =
            WaitForSingleObject(fence_event, kFenceWaitTimeoutMs);
        if (wait_result == WAIT_TIMEOUT)
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
        if (wait_result != WAIT_OBJECT_0)
        {
            REXLOG_ERROR("Native D3D12 fence wait failed: Win32 error {}",
                         GetLastError());
            return false;
        }
        return true;
    }

    bool WaitForGpu()
    {
        const std::uint64_t value  = next_fence_value++;
        const HRESULT       result = queue->Signal(fence.Get(), value);
        if (FAILED(result))
        {
            return LogDeviceRemoval("queue signal", result);
        }
        return WaitForFence(value);
    }

    void ReleaseBackBuffers()
    {
        for (auto& buffer : back_buffers)
        {
            buffer.Reset();
        }
    }

    bool CreateBackBuffers()
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE heap_start =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t index = 0; index < kFrameCount; ++index)
        {
            const HRESULT result =
                swap_chain->GetBuffer(index, IID_PPV_ARGS(&back_buffers[index]));
            if (FAILED(result))
            {
                return LogFailure("swap-chain buffer", result);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_start;
            handle.ptr += static_cast<SIZE_T>(index) * rtv_stride;
            device->CreateRenderTargetView(back_buffers[index].Get(), nullptr, handle);
        }
        return true;
    }

    bool LogDeviceRemoval(const char* operation, HRESULT result)
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

    bool InitializeOnRendererThread(std::uintptr_t native_window,
                                    std::uint32_t  initial_width,
                                    std::uint32_t  initial_height);
    bool ResizeOnRendererThread(std::uint32_t width, std::uint32_t height);
    bool PresentOnRendererThread();
    void ShutdownOnRendererThread();
#endif
};

NativeRendererD3D12::NativeRendererD3D12()
: impl_(std::make_unique<Impl>())
{
}

NativeRendererD3D12::~NativeRendererD3D12()
{
    Shutdown();
}

#if defined(_WIN32)

bool NativeRendererD3D12::Impl::InitializeOnRendererThread(
    std::uintptr_t native_window,
    std::uint32_t  initial_width,
    std::uint32_t  initial_height)
{
    const HWND hwnd = reinterpret_cast<HWND>(native_window);
    if (!hwnd || initial_width == 0 || initial_height == 0)
    {
        REXLOG_ERROR("Native D3D12 surface has no drawable HWND extent");
        return false;
    }
    width  = initial_width;
    height = initial_height;

    const auto fail_initialization = [this]()
    {
        ShutdownOnRendererThread();
        return false;
    };

    EnableDred();
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        LogFailure("factory creation", result);
        return fail_initialization();
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
            LogFailure("adapter enumeration", result);
            return fail_initialization();
        }

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(candidate->GetDesc1(&description)) ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }

        ComPtr<ID3D12Device> candidate_device;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(),
                                        D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&candidate_device))))
        {
            adapter = std::move(candidate);
            device  = std::move(candidate_device);
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
        return fail_initialization();
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result          = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(result))
    {
        LogFailure("command queue creation", result);
        return fail_initialization();
    }
    queue->SetName(L"ReRevved native renderer direct queue");

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
    swap_chain_desc.Width       = width;
    swap_chain_desc.Height      = height;
    swap_chain_desc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.SampleDesc  = { 1, 0 };
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = kFrameCount;
    swap_chain_desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> new_swap_chain;
    result = factory->CreateSwapChainForHwnd(
        queue.Get(), hwnd, &swap_chain_desc, nullptr, nullptr, &new_swap_chain);
    if (FAILED(result))
    {
        LogFailure("swap chain creation", result);
        return fail_initialization();
    }
    result = new_swap_chain.As(&swap_chain);
    if (FAILED(result))
    {
        LogFailure("swap chain interface", result);
        return fail_initialization();
    }
    result = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(result))
    {
        LogFailure("window association", result);
        return fail_initialization();
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = kFrameCount;
    result                  = device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result))
    {
        LogFailure("RTV heap creation", result);
        return fail_initialization();
    }
    rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!CreateBackBuffers())
    {
        return fail_initialization();
    }

    for (std::uint32_t index = 0; index < kFrameCount; ++index)
    {
        auto& allocator = allocators[index];
        result          = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (FAILED(result))
        {
            LogFailure("command allocator creation", result);
            return fail_initialization();
        }
        allocator->SetName(index == 0 ? L"ReRevved native frame allocator 0"
                                      : L"ReRevved native frame allocator 1");
    }
    result = device->CreateCommandList(0,
                                       D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocators[0].Get(),
                                       nullptr,
                                       IID_PPV_ARGS(&command_list));
    if (FAILED(result))
    {
        LogFailure("command list creation", result);
        return fail_initialization();
    }
    command_list->SetName(L"ReRevved native frame command list");
    result = command_list->Close();
    if (FAILED(result))
    {
        LogFailure("initial command list close", result);
        return fail_initialization();
    }
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result))
    {
        LogFailure("fence creation", result);
        return fail_initialization();
    }
    fence->SetName(L"ReRevved native frame fence");
    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event)
    {
        REXLOG_ERROR("Native D3D12 fence event creation failed: Win32 error {}",
                     GetLastError());
        return fail_initialization();
    }

    initialized.store(true, std::memory_order_release);
    REXLOG_INFO("Native D3D12 initialized: {}x{} frames={}", width, height, kFrameCount);
    if (!PresentOnRendererThread())
    {
        ShutdownOnRendererThread();
        return false;
    }
    REXLOG_INFO("Native D3D12 diagnostic frame presented");
    return true;
}

bool NativeRendererD3D12::Impl::ResizeOnRendererThread(std::uint32_t new_width,
                                                       std::uint32_t new_height)
{
    if (new_width == 0 || new_height == 0 || !initialized.load(std::memory_order_acquire))
    {
        return new_width == 0 || new_height == 0;
    }
    if (new_width == width && new_height == height)
    {
        return true;
    }
    if (!WaitForGpu())
    {
        return false;
    }

    ReleaseBackBuffers();
    fence_values.fill(0);
    const HRESULT result = swap_chain->ResizeBuffers(
        kFrameCount, new_width, new_height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(result))
    {
        LogDeviceRemoval("swap chain resize", result);
        return false;
    }
    width  = new_width;
    height = new_height;
    if (!CreateBackBuffers())
    {
        return false;
    }
    REXLOG_INFO("Native D3D12 resized: {}x{}", new_width, new_height);
    return PresentOnRendererThread();
}

bool NativeRendererD3D12::Impl::PresentOnRendererThread()
{
    if (!initialized.load(std::memory_order_acquire))
    {
        return false;
    }

    const std::uint32_t frame_index = swap_chain->GetCurrentBackBufferIndex();
    if (!WaitForFence(fence_values[frame_index]))
    {
        return false;
    }

    HRESULT result = allocators[frame_index]->Reset();
    if (FAILED(result))
    {
        LogFailure("command allocator reset", result);
        return false;
    }
    result = command_list->Reset(allocators[frame_index].Get(), nullptr);
    if (FAILED(result))
    {
        LogFailure("command list reset", result);
        return false;
    }

    D3D12_RESOURCE_BARRIER to_render_target{};
    to_render_target.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_render_target.Transition.pResource   = back_buffers[frame_index].Get();
    to_render_target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_render_target.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    to_render_target.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_list->ResourceBarrier(1, &to_render_target);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frame_index) * rtv_stride;
    constexpr float clear_color[4] = { 0.015F, 0.02F, 0.04F, 1.0F };
    command_list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);

    D3D12_RESOURCE_BARRIER to_present = to_render_target;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_present.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    command_list->ResourceBarrier(1, &to_present);

    result = command_list->Close();
    if (FAILED(result))
    {
        LogFailure("command list close", result);
        return false;
    }
    ID3D12CommandList* lists[] = { command_list.Get() };
    queue->ExecuteCommandLists(1, lists);

    const std::uint64_t fence_value = next_fence_value++;
    result                          = queue->Signal(fence.Get(), fence_value);
    if (FAILED(result))
    {
        LogDeviceRemoval("frame signal", result);
        return false;
    }
    fence_values[frame_index] = fence_value;

    result = swap_chain->Present(1, 0);
    if (FAILED(result))
    {
        LogDeviceRemoval("present", result);
        return false;
    }
    return true;
}

void NativeRendererD3D12::Impl::ShutdownOnRendererThread()
{
    bool abandon_gpu_objects = false;
    if (initialized.load(std::memory_order_acquire) && queue && fence && !WaitForGpu())
    {
        const HRESULT reason = device ? device->GetDeviceRemovedReason() : E_FAIL;
        abandon_gpu_objects  = !FAILED(reason);
        if (abandon_gpu_objects)
        {
            // A live device with an uncompleted fence may still reference every
            // submitted object. Let process teardown reclaim them instead of
            // releasing storage that the GPU may still be using.
            REXLOG_ERROR("Native D3D12 drain failed on a live device; abandoning GPU objects until process exit");
        }
    }
    initialized.store(false, std::memory_order_release);
    if (abandon_gpu_objects)
    {
        (void)command_list.Detach();
        for (auto& allocator : allocators)
        {
            (void)allocator.Detach();
        }
        for (auto& back_buffer : back_buffers)
        {
            (void)back_buffer.Detach();
        }
        (void)rtv_heap.Detach();
        (void)swap_chain.Detach();
        (void)queue.Detach();
        (void)fence.Detach();
        (void)device.Detach();
        (void)adapter.Detach();
        (void)factory.Detach();
        fence_event = nullptr;
        gpu_objects_abandoned.store(true, std::memory_order_release);
        return;
    }
    command_list.Reset();
    for (auto& allocator : allocators)
    {
        allocator.Reset();
    }
    ReleaseBackBuffers();
    rtv_heap.Reset();
    swap_chain.Reset();
    queue.Reset();
    fence.Reset();
    device.Reset();
    adapter.Reset();
    factory.Reset();
    if (fence_event)
    {
        CloseHandle(fence_event);
        fence_event = nullptr;
    }
    fence_values.fill(0);
    next_fence_value = 1;
    width            = 0;
    height           = 0;
}

#endif

void NativeRendererD3D12::RendererThreadMain(std::uintptr_t native_window,
                                             std::uint32_t  width,
                                             std::uint32_t  height)
{
#if defined(_WIN32)
    const bool initialization_succeeded =
        impl_->InitializeOnRendererThread(native_window, width, height);
    {
        std::lock_guard lock(impl_->state_mutex);
        impl_->initialization_succeeded = initialization_succeeded;
        impl_->initialization_done      = true;
    }
    impl_->state_cv.notify_all();
    if (!initialization_succeeded)
    {
        return;
    }

    for (;;)
    {
        bool          resize           = false;
        std::uint32_t requested_width  = 0;
        std::uint32_t requested_height = 0;
        {
            std::unique_lock lock(impl_->state_mutex);
            impl_->state_cv.wait(lock, [this]()
                                 {
                                     return impl_->stop_requested || impl_->resize_pending;
                                 });
            if (impl_->stop_requested)
            {
                break;
            }
            resize                = impl_->resize_pending;
            requested_width       = impl_->requested_width;
            requested_height      = impl_->requested_height;
            impl_->resize_pending = false;
        }

        bool request_succeeded = true;
        if (resize)
        {
            request_succeeded = impl_->ResizeOnRendererThread(
                requested_width, requested_height);
        }
        if (!request_succeeded)
        {
            HandleRendererFailure();
            break;
        }
    }
    impl_->ShutdownOnRendererThread();
#else
    (void)native_window;
    (void)width;
    (void)height;
    {
        std::lock_guard lock(impl_->state_mutex);
        impl_->initialization_succeeded = false;
        impl_->initialization_done      = true;
    }
    impl_->state_cv.notify_all();
#endif
    impl_->initialized.store(false, std::memory_order_release);
}

void NativeRendererD3D12::HandleRendererFailure()
{
    std::function<void()> request_quit;
    {
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->runtime_failed)
        {
            return;
        }
        impl_->runtime_failed = true;
        impl_->stop_requested = true;
        request_quit          = impl_->request_deferred_quit;
    }
    REXLOG_ERROR("Native D3D12 renderer entered terminal failure; requesting deferred quit");
    impl_->state_cv.notify_all();
    if (request_quit)
    {
        request_quit();
    }
}

bool NativeRendererD3D12::Initialize(rex::ui::Window& window)
{
#if !defined(_WIN32)
    (void)window;
    REXLOG_ERROR("The native renderer currently requires Windows D3D12");
    return false;
#else
    if (impl_->initialized.load(std::memory_order_acquire))
    {
        return true;
    }
    if (impl_->gpu_objects_abandoned.load(std::memory_order_acquire))
    {
        REXLOG_ERROR("Native D3D12 cannot reinitialize after abandoning in-flight GPU objects");
        return false;
    }
    if (impl_->renderer_thread.joinable())
    {
        impl_->renderer_thread.join();
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
    auto* app_context = &window.app_context();
    {
        std::lock_guard lock(impl_->state_mutex);
        impl_->stop_requested           = false;
        impl_->initialization_done      = false;
        impl_->initialization_succeeded = false;
        impl_->resize_pending           = false;
        impl_->requested_width          = width;
        impl_->requested_height         = height;
        impl_->runtime_failed           = false;
        impl_->request_deferred_quit    = [app_context]()
        {
            app_context->RequestDeferredQuit();
        };
    }

    impl_->renderer_thread = std::thread(
        &NativeRendererD3D12::RendererThreadMain,
        this,
        reinterpret_cast<std::uintptr_t>(hwnd),
        width,
        height);
    {
        std::unique_lock lock(impl_->state_mutex);
        impl_->state_cv.wait(lock, [this]()
                             {
                                 return impl_->initialization_done;
                             });
        if (!impl_->initialization_succeeded)
        {
            lock.unlock();
            impl_->renderer_thread.join();
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
    std::lock_guard lock(impl_->state_mutex);
    if (!impl_->initialized.load(std::memory_order_acquire) ||
        impl_->stop_requested || impl_->runtime_failed)
    {
        return false;
    }

    // The UI thread only updates this mailbox; the renderer consumes the
    // latest dimensions and performs all DXGI/D3D12 work itself.
    impl_->requested_width  = width;
    impl_->requested_height = height;
    impl_->resize_pending   = true;
    impl_->state_cv.notify_one();
    return true;
}

void NativeRendererD3D12::Shutdown()
{
    if (!impl_->renderer_thread.joinable())
    {
        std::lock_guard lock(impl_->state_mutex);
        impl_->initialized.store(false, std::memory_order_release);
        impl_->request_deferred_quit = {};
        impl_->state_cv.notify_all();
        return;
    }
    {
        std::lock_guard lock(impl_->state_mutex);
        impl_->stop_requested = true;
        impl_->resize_pending = false;
    }
    impl_->state_cv.notify_all();
    if (impl_->renderer_thread.get_id() != std::this_thread::get_id())
    {
        impl_->renderer_thread.join();
    }
    std::lock_guard lock(impl_->state_mutex);
    impl_->request_deferred_quit = {};
}

bool NativeRendererD3D12::initialized() const noexcept
{
    return impl_->initialized.load(std::memory_order_acquire);
}

} // namespace rerevved::gpu
