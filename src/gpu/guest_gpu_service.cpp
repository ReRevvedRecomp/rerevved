#include "gpu/guest_gpu_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

#include <rex/chrono/clock.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

namespace rerevved::gpu
{

using rex::X_STATUS;

namespace
{

constexpr uint32_t kGpuMmioBaseAddress = 0x7FC80000;
constexpr uint32_t kGpuMmioAddressMask = 0xFFFF0000;
constexpr uint32_t kGpuMmioSize        = 0x0000FFFF;

constexpr uint32_t kRbEdramTiming                     = 0x0F00;
constexpr uint32_t kRbBcControl                       = 0x0F01;
constexpr uint32_t kR500D1ModeVCounter                = 0x194C;
constexpr uint32_t kInterruptStatus                   = 0x1951;
constexpr uint32_t kAvivoD1ModeViewportSize           = 0x1961;
constexpr uint32_t kCpRbWptr                          = 0x01C5;
constexpr uint32_t kAvivoD1GraphPrimarySurfaceAddress = 0x1844;

constexpr uint32_t kDefaultCpu       = 2;
constexpr uint32_t kMaxVblankCatchUp = 4;

void ReportUnsupported(const char* operation)
{
    REXGPU_ERROR("NativeGuestGpuService: {} is unsupported", operation);
}

struct MmioBridge
{
    std::mutex              mutex;
    std::condition_variable idle;
    NativeGuestGpuService*  service             = nullptr;
    uint32_t                callbacks_in_flight = 0;
};

MmioBridge& GetMmioBridge()
{
    // The SDK permits only one process-wide MMIO handler. A process-lifetime
    // bridge remains valid after that handler has copied its callback context.
    static MmioBridge bridge;
    return bridge;
}

bool BindMmioBridge(NativeGuestGpuService* service)
{
    auto&           bridge = GetMmioBridge();
    std::lock_guard lock(bridge.mutex);
    if (bridge.service || bridge.callbacks_in_flight != 0)
    {
        return false;
    }
    bridge.service = service;
    return true;
}

void UnbindMmioBridge(NativeGuestGpuService* service)
{
    auto&            bridge = GetMmioBridge();
    std::unique_lock lock(bridge.mutex);
    if (bridge.service != service)
    {
        return;
    }
    bridge.service = nullptr;
    bridge.idle.wait(lock, [&bridge]
                     {
                         return bridge.callbacks_in_flight == 0;
                     });
}

class MmioCallbackLease
{
public:
    explicit MmioCallbackLease(void* callback_context)
    : bridge_(static_cast<MmioBridge*>(callback_context))
    {
        if (!bridge_)
        {
            return;
        }
        std::lock_guard lock(bridge_->mutex);
        service_ = bridge_->service;
        if (service_)
        {
            ++bridge_->callbacks_in_flight;
        }
    }

    ~MmioCallbackLease()
    {
        if (!service_)
        {
            return;
        }
        std::lock_guard lock(bridge_->mutex);
        --bridge_->callbacks_in_flight;
        if (bridge_->callbacks_in_flight == 0)
        {
            bridge_->idle.notify_all();
        }
    }

    NativeGuestGpuService* service() const
    {
        return service_;
    }

private:
    MmioBridge*            bridge_  = nullptr;
    NativeGuestGpuService* service_ = nullptr;
};

} // namespace

struct NativeGuestGpuService::Impl
{
    mutable std::mutex mutex;

    rex::runtime::FunctionDispatcher* function_dispatcher = nullptr;
    rex::system::KernelState*         kernel_state        = nullptr;
    rex::memory::Memory*              memory              = nullptr;

    rex::system::object_ref<rex::system::XHostThread> vblank_worker_thread;
    std::atomic<bool>                                 vblank_worker_running{ false };

    bool active           = false;
    bool mmio_registered  = false;
    bool shutdown_started = false;

    uint32_t interrupt_callback  = 0;
    uint32_t interrupt_user_data = 0;

    std::array<uint32_t, 0x10000 / 4> register_values{};

    bool unknown_mmio_logged   = false;
    bool held_write_logged     = false;
    bool missing_thread_logged = false;
};

NativeGuestGpuService::NativeGuestGpuService()
: impl_(std::make_unique<Impl>())
{
}

NativeGuestGpuService::~NativeGuestGpuService()
{
    Shutdown();
}

rex::X_STATUS NativeGuestGpuService::SetupPresentation(rex::ui::WindowedAppContext*)
{
    // The native renderer owns presentation outside this guest service.
    return X_STATUS_SUCCESS;
}

rex::X_STATUS NativeGuestGpuService::SetupGuestGpu(
    rex::runtime::FunctionDispatcher* function_dispatcher,
    rex::system::KernelState*         kernel_state)
{
    if (!function_dispatcher || !kernel_state)
    {
        return X_STATUS_INVALID_PARAMETER;
    }

    std::unique_lock lock(impl_->mutex);
    if (impl_->active || impl_->mmio_registered || impl_->vblank_worker_thread ||
        impl_->shutdown_started)
    {
        return X_STATUS_INVALID_PARAMETER;
    }

    auto* memory = function_dispatcher->memory();
    if (!memory)
    {
        return X_STATUS_UNSUCCESSFUL;
    }

    impl_->function_dispatcher = function_dispatcher;
    impl_->kernel_state        = kernel_state;
    impl_->memory              = memory;
    impl_->vblank_worker_running.store(true, std::memory_order_release);

    try
    {
        impl_->vblank_worker_thread = rex::system::object_ref<rex::system::XHostThread>(
            new rex::system::XHostThread(kernel_state, 128 * 1024, 0, [this]()
                                         {
                                             return RunVblankWorker();
                                         }));
        impl_->vblank_worker_thread->set_name("GPU VSync");
    }
    catch (...)
    {
        impl_->vblank_worker_running.store(false, std::memory_order_release);
        impl_->active              = false;
        impl_->function_dispatcher = nullptr;
        impl_->kernel_state        = nullptr;
        impl_->memory              = nullptr;
        impl_->vblank_worker_thread.reset();
        return X_STATUS_UNSUCCESSFUL;
    }

    const X_STATUS create_status = impl_->vblank_worker_thread->Create();
    if (XFAILED(create_status))
    {
        impl_->vblank_worker_running.store(false, std::memory_order_release);
        impl_->function_dispatcher = nullptr;
        impl_->kernel_state        = nullptr;
        impl_->memory              = nullptr;
        impl_->vblank_worker_thread.reset();
        return create_status;
    }

    if (!BindMmioBridge(this))
    {
        auto worker = impl_->vblank_worker_thread;
        impl_->vblank_worker_running.store(false, std::memory_order_release);
        lock.unlock();
        worker->Wait(0, 0, 0, nullptr);
        lock.lock();
        impl_->function_dispatcher = nullptr;
        impl_->kernel_state        = nullptr;
        impl_->memory              = nullptr;
        impl_->vblank_worker_thread.reset();
        return X_STATUS_UNSUCCESSFUL;
    }

    if (!memory->AddVirtualMappedRange(
            kGpuMmioBaseAddress,
            kGpuMmioAddressMask,
            kGpuMmioSize,
            &GetMmioBridge(),
            reinterpret_cast<rex::runtime::MMIOReadCallback>(ReadRegisterThunk),
            reinterpret_cast<rex::runtime::MMIOWriteCallback>(WriteRegisterThunk)))
    {
        auto worker = impl_->vblank_worker_thread;
        impl_->vblank_worker_running.store(false, std::memory_order_release);
        lock.unlock();
        UnbindMmioBridge(this);
        worker->Wait(0, 0, 0, nullptr);
        lock.lock();
        impl_->function_dispatcher = nullptr;
        impl_->kernel_state        = nullptr;
        impl_->memory              = nullptr;
        impl_->vblank_worker_thread.reset();
        return X_STATUS_UNSUCCESSFUL;
    }

    impl_->mmio_registered = true;
    impl_->active          = true;
    REXGPU_INFO("NativeGuestGpuService: vblank active; ring command consumption disabled");

    return X_STATUS_SUCCESS;
}

void NativeGuestGpuService::SetInterruptCallback(uint32_t callback, uint32_t user_data)
{
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->shutdown_started)
        {
            return;
        }
        impl_->interrupt_callback  = callback;
        impl_->interrupt_user_data = user_data;
    }
    REXGPU_INFO("NativeGuestGpuService: interrupt callback={:08X} user_data={:08X}",
                callback,
                user_data);
}

void NativeGuestGpuService::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2)
{
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->shutdown_started)
        {
            return;
        }
    }
    REXGPU_INFO("NativeGuestGpuService: ring captured ptr={:08X} size_log2={}",
                ptr,
                size_log2);
}

void NativeGuestGpuService::EnableReadPointerWriteBack(uint32_t ptr,
                                                       uint32_t block_size_log2)
{
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->shutdown_started)
        {
            return;
        }
    }
    REXGPU_INFO("NativeGuestGpuService: read-pointer writeback held ptr={:08X} block_size_log2={}",
                ptr,
                block_size_log2);
}

void NativeGuestGpuService::InitializeShaderStorage(const std::filesystem::path&,
                                                    uint32_t,
                                                    bool)
{
    ReportUnsupported("InitializeShaderStorage");
}

bool NativeGuestGpuService::PauseAndResetGpuWritePointer()
{
    ReportUnsupported("PauseAndResetGpuWritePointer");
    return false;
}

void NativeGuestGpuService::ResumeGpu()
{
    ReportUnsupported("ResumeGpu");
}

uint32_t NativeGuestGpuService::ReadRegisterThunk(void*, void* callback_context, uint32_t addr)
{
    MmioCallbackLease lease(callback_context);
    if (!lease.service())
    {
        return 0;
    }
    return lease.service()->ReadRegister(addr);
}

void NativeGuestGpuService::WriteRegisterThunk(void*, void* callback_context, uint32_t addr, uint32_t value)
{
    MmioCallbackLease lease(callback_context);
    if (lease.service())
    {
        lease.service()->WriteRegister(addr, value);
    }
}

uint32_t NativeGuestGpuService::ReadRegister(uint32_t addr)
{
    const uint32_t reg = (addr & 0xFFFF) / 4;

    switch (reg)
    {
        case kRbEdramTiming:
            return 0x08100748;
        case kRbBcControl:
            return 0x0000200E;
        case kR500D1ModeVCounter:
        {
            rex::system::X_VIDEO_MODE video_mode;
            rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
            return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
        }
        case kInterruptStatus:
            return 1;
        case kAvivoD1ModeViewportSize:
        {
            rex::system::X_VIDEO_MODE video_mode;
            rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
            const uint32_t width  = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
            const uint32_t height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
            return (width << 16) | height;
        }
        default:
            break;
    }

    bool     report_unknown = false;
    uint32_t value          = 0;
    {
        std::lock_guard lock(impl_->mutex);
        value = impl_->register_values[reg];
        if (!impl_->unknown_mmio_logged)
        {
            impl_->unknown_mmio_logged = true;
            report_unknown             = true;
        }
    }
    if (report_unknown)
    {
        REXGPU_WARN("NativeGuestGpuService: unknown GPU register read {:04X}", reg);
    }
    return value;
}

void NativeGuestGpuService::WriteRegister(uint32_t addr, uint32_t value)
{
    const uint32_t reg            = (addr & 0xFFFF) / 4;
    bool           report_unknown = false;
    bool           report_held    = false;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->register_values[reg] = value;
        if (reg == kCpRbWptr)
        {
            if (!impl_->held_write_logged)
            {
                impl_->held_write_logged = true;
                report_held              = true;
            }
        }
        else if (reg != kAvivoD1GraphPrimarySurfaceAddress)
        {
            if (!impl_->unknown_mmio_logged)
            {
                impl_->unknown_mmio_logged = true;
                report_unknown             = true;
            }
        }
    }

    if (report_held)
    {
        REXGPU_ERROR(
            "NativeGuestGpuService: holding CP_RB_WPTR write {:08X}; command consumption is unavailable",
            value);
    }
    if (report_unknown)
    {
        REXGPU_WARN("NativeGuestGpuService: unknown GPU register write {:04X}={:08X}", reg, value);
    }
}

int NativeGuestGpuService::RunVblankWorker()
{
    rex::system::X_VIDEO_MODE video_mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
    const double   refresh_rate_hz      = std::max(1.0, double(float(video_mode.refresh_rate)));
    const uint64_t guest_tick_frequency = rex::chrono::Clock::guest_tick_frequency();
    const uint64_t vsync_interval_ticks = std::max(
        uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
    uint64_t last_frame_time = rex::chrono::Clock::QueryGuestTickCount();

    while (impl_->vblank_worker_running.load(std::memory_order_acquire))
    {
        const uint64_t current_time   = rex::chrono::Clock::QueryGuestTickCount();
        const uint64_t interval_ticks = vsync_interval_ticks;
        uint32_t       catch_up_count = 0;
        while (current_time - last_frame_time >= interval_ticks &&
               catch_up_count < kMaxVblankCatchUp &&
               impl_->vblank_worker_running.load(std::memory_order_acquire))
        {
            MarkVblank();
            last_frame_time += interval_ticks;
            ++catch_up_count;
        }
        if (catch_up_count == kMaxVblankCatchUp &&
            current_time - last_frame_time >= interval_ticks)
        {
            last_frame_time = current_time;
        }
        rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
}

void NativeGuestGpuService::MarkVblank()
{
    uint32_t                          callback            = 0;
    uint32_t                          user_data           = 0;
    rex::runtime::FunctionDispatcher* function_dispatcher = nullptr;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->active || !impl_->interrupt_callback || !impl_->function_dispatcher)
        {
            return;
        }
        callback            = impl_->interrupt_callback;
        user_data           = impl_->interrupt_user_data;
        function_dispatcher = impl_->function_dispatcher;
    }

    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread)
    {
        bool report = false;
        {
            std::lock_guard lock(impl_->mutex);
            if (!impl_->missing_thread_logged)
            {
                impl_->missing_thread_logged = true;
                report                       = true;
            }
        }
        if (report)
        {
            REXGPU_WARN("NativeGuestGpuService: vblank has no current XThread");
        }
        return;
    }

    thread->SetActiveCpu(kDefaultCpu);
    uint64_t args[] = { 0, user_data };
    function_dispatcher->ExecuteInterrupt(thread->thread_state(), callback, args, std::size(args));
}

void NativeGuestGpuService::Shutdown()
{
    rex::system::object_ref<rex::system::XHostThread> worker;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->shutdown_started    = true;
        impl_->interrupt_callback  = 0;
        impl_->interrupt_user_data = 0;
        impl_->active              = false;
        impl_->vblank_worker_running.store(false, std::memory_order_release);
        worker = impl_->vblank_worker_thread;
    }

    UnbindMmioBridge(this);

    if (worker)
    {
        worker->Wait(0, 0, 0, nullptr);
    }

    std::lock_guard lock(impl_->mutex);
    impl_->vblank_worker_thread.reset();
    impl_->function_dispatcher = nullptr;
    impl_->kernel_state        = nullptr;
    impl_->memory              = nullptr;
}

} // namespace rerevved::gpu
