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

void reportUnsupported(const char* operation)
{
    REXGPU_ERROR("NativeGuestGpuService: {} is unsupported", operation);
}

struct MmioBridge
{
    std::mutex              mutex;
    std::condition_variable idle;
    NativeGuestGpuService*  service           = nullptr;
    uint32_t                callbacksInFlight = 0;
};

MmioBridge& getMmioBridge()
{
    // The SDK permits only one process-wide MMIO handler. A process-lifetime
    // bridge remains valid after that handler has copied its callback context.
    static MmioBridge bridge;
    return bridge;
}

bool bindMmioBridge(NativeGuestGpuService* service)
{
    auto&           bridge = getMmioBridge();
    std::lock_guard lock(bridge.mutex);
    if (bridge.service || bridge.callbacksInFlight != 0)
    {
        return false;
    }
    bridge.service = service;
    return true;
}

void unbindMmioBridge(NativeGuestGpuService* service)
{
    auto&            bridge = getMmioBridge();
    std::unique_lock lock(bridge.mutex);
    if (bridge.service != service)
    {
        return;
    }
    bridge.service = nullptr;
    bridge.idle.wait(lock, [&bridge]
                     {
                         return bridge.callbacksInFlight == 0;
                     });
}

class MmioCallbackLease
{
public:
    explicit MmioCallbackLease(void* callbackContext)
    : bridge(static_cast<MmioBridge*>(callbackContext))
    {
        if (!bridge)
        {
            return;
        }
        std::lock_guard lock(bridge->mutex);
        boundService = bridge->service;
        if (boundService)
        {
            ++bridge->callbacksInFlight;
        }
    }

    ~MmioCallbackLease()
    {
        if (!boundService)
        {
            return;
        }
        std::lock_guard lock(bridge->mutex);
        --bridge->callbacksInFlight;
        if (bridge->callbacksInFlight == 0)
        {
            bridge->idle.notify_all();
        }
    }

    NativeGuestGpuService* service() const
    {
        return boundService;
    }

private:
    MmioBridge*            bridge       = nullptr;
    NativeGuestGpuService* boundService = nullptr;
};

} // namespace

struct NativeGuestGpuService::Impl
{
    mutable std::mutex mutex;

    rex::runtime::FunctionDispatcher* functionDispatcher = nullptr;
    rex::system::KernelState*         kernelState        = nullptr;
    rex::memory::Memory*              memory             = nullptr;

    rex::system::object_ref<rex::system::XHostThread> vblankWorkerThread;
    std::atomic<bool>                                 vblankWorkerRunning{ false };

    bool active          = false;
    bool mmioRegistered  = false;
    bool shutdownStarted = false;

    uint32_t interruptCallback = 0;
    uint32_t interruptUserData = 0;

    std::array<uint32_t, 0x10000 / 4> registerValues{};

    bool unknownMmioLogged   = false;
    bool heldWriteLogged     = false;
    bool missingThreadLogged = false;
};

NativeGuestGpuService::NativeGuestGpuService()
: impl(std::make_unique<Impl>())
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
    rex::runtime::FunctionDispatcher* functionDispatcher,
    rex::system::KernelState*         kernelState)
{
    if (!functionDispatcher || !kernelState)
    {
        return X_STATUS_INVALID_PARAMETER;
    }

    std::unique_lock lock(impl->mutex);
    if (impl->active || impl->mmioRegistered || impl->vblankWorkerThread ||
        impl->shutdownStarted)
    {
        return X_STATUS_INVALID_PARAMETER;
    }

    auto* memory = functionDispatcher->memory();
    if (!memory)
    {
        return X_STATUS_UNSUCCESSFUL;
    }

    impl->functionDispatcher = functionDispatcher;
    impl->kernelState        = kernelState;
    impl->memory             = memory;
    impl->vblankWorkerRunning.store(true, std::memory_order_release);

    try
    {
        impl->vblankWorkerThread = rex::system::object_ref<rex::system::XHostThread>(
            new rex::system::XHostThread(kernelState, 128 * 1024, 0, [this]()
                                         {
                                             return runVblankWorker();
                                         }));
        impl->vblankWorkerThread->set_name("GPU VSync");
    }
    catch (...)
    {
        impl->vblankWorkerRunning.store(false, std::memory_order_release);
        impl->active             = false;
        impl->functionDispatcher = nullptr;
        impl->kernelState        = nullptr;
        impl->memory             = nullptr;
        impl->vblankWorkerThread.reset();
        return X_STATUS_UNSUCCESSFUL;
    }

    const X_STATUS createStatus = impl->vblankWorkerThread->Create();
    if (XFAILED(createStatus))
    {
        impl->vblankWorkerRunning.store(false, std::memory_order_release);
        impl->functionDispatcher = nullptr;
        impl->kernelState        = nullptr;
        impl->memory             = nullptr;
        impl->vblankWorkerThread.reset();
        return createStatus;
    }

    if (!bindMmioBridge(this))
    {
        auto worker = impl->vblankWorkerThread;
        impl->vblankWorkerRunning.store(false, std::memory_order_release);
        lock.unlock();
        worker->Wait(0, 0, 0, nullptr);
        lock.lock();
        impl->functionDispatcher = nullptr;
        impl->kernelState        = nullptr;
        impl->memory             = nullptr;
        impl->vblankWorkerThread.reset();
        return X_STATUS_UNSUCCESSFUL;
    }

    if (!memory->AddVirtualMappedRange(
            kGpuMmioBaseAddress,
            kGpuMmioAddressMask,
            kGpuMmioSize,
            &getMmioBridge(),
            reinterpret_cast<rex::runtime::MMIOReadCallback>(readRegisterThunk),
            reinterpret_cast<rex::runtime::MMIOWriteCallback>(writeRegisterThunk)))
    {
        auto worker = impl->vblankWorkerThread;
        impl->vblankWorkerRunning.store(false, std::memory_order_release);
        lock.unlock();
        unbindMmioBridge(this);
        worker->Wait(0, 0, 0, nullptr);
        lock.lock();
        impl->functionDispatcher = nullptr;
        impl->kernelState        = nullptr;
        impl->memory             = nullptr;
        impl->vblankWorkerThread.reset();
        return X_STATUS_UNSUCCESSFUL;
    }

    impl->mmioRegistered = true;
    impl->active         = true;
    REXGPU_INFO("NativeGuestGpuService: vblank active; ring command consumption disabled");

    return X_STATUS_SUCCESS;
}

void NativeGuestGpuService::SetInterruptCallback(uint32_t callback, uint32_t userData)
{
    {
        std::lock_guard lock(impl->mutex);
        if (impl->shutdownStarted)
        {
            return;
        }
        impl->interruptCallback = callback;
        impl->interruptUserData = userData;
    }
    REXGPU_INFO("NativeGuestGpuService: interrupt callback={:08X} user_data={:08X}",
                callback,
                userData);
}

void NativeGuestGpuService::InitializeRingBuffer(uint32_t ptr, uint32_t sizeLog2)
{
    {
        std::lock_guard lock(impl->mutex);
        if (impl->shutdownStarted)
        {
            return;
        }
    }
    REXGPU_INFO("NativeGuestGpuService: ring captured ptr={:08X} size_log2={}",
                ptr,
                sizeLog2);
}

void NativeGuestGpuService::EnableReadPointerWriteBack(uint32_t ptr,
                                                       uint32_t blockSizeLog2)
{
    {
        std::lock_guard lock(impl->mutex);
        if (impl->shutdownStarted)
        {
            return;
        }
    }
    REXGPU_INFO("NativeGuestGpuService: read-pointer writeback held ptr={:08X} block_size_log2={}",
                ptr,
                blockSizeLog2);
}

void NativeGuestGpuService::InitializeShaderStorage(const std::filesystem::path&,
                                                    uint32_t,
                                                    bool)
{
    reportUnsupported("InitializeShaderStorage");
}

bool NativeGuestGpuService::PauseAndResetGpuWritePointer()
{
    reportUnsupported("PauseAndResetGpuWritePointer");
    return false;
}

void NativeGuestGpuService::ResumeGpu()
{
    reportUnsupported("ResumeGpu");
}

uint32_t NativeGuestGpuService::readRegisterThunk(void*, void* callbackContext, uint32_t addr)
{
    MmioCallbackLease lease(callbackContext);
    if (!lease.service())
    {
        return 0;
    }
    return lease.service()->readRegister(addr);
}

void NativeGuestGpuService::writeRegisterThunk(void*, void* callbackContext, uint32_t addr, uint32_t value)
{
    MmioCallbackLease lease(callbackContext);
    if (lease.service())
    {
        lease.service()->writeRegister(addr, value);
    }
}

uint32_t NativeGuestGpuService::readRegister(uint32_t addr)
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
            rex::system::X_VIDEO_MODE videoMode;
            rex::kernel::xboxkrnl::VdQueryVideoMode(&videoMode);
            return std::min(uint32_t(videoMode.display_height), uint32_t(0x0FFF));
        }
        case kInterruptStatus:
            return 1;
        case kAvivoD1ModeViewportSize:
        {
            rex::system::X_VIDEO_MODE videoMode;
            rex::kernel::xboxkrnl::VdQueryVideoMode(&videoMode);
            const uint32_t width  = std::min(uint32_t(videoMode.display_width), uint32_t(0x0FFF));
            const uint32_t height = std::min(uint32_t(videoMode.display_height), uint32_t(0x0FFF));
            return (width << 16) | height;
        }
        default:
            break;
    }

    bool     reportUnknown = false;
    uint32_t value         = 0;
    {
        std::lock_guard lock(impl->mutex);
        value = impl->registerValues[reg];
        if (!impl->unknownMmioLogged)
        {
            impl->unknownMmioLogged = true;
            reportUnknown           = true;
        }
    }
    if (reportUnknown)
    {
        REXGPU_WARN("NativeGuestGpuService: unknown GPU register read {:04X}", reg);
    }
    return value;
}

void NativeGuestGpuService::writeRegister(uint32_t addr, uint32_t value)
{
    const uint32_t reg           = (addr & 0xFFFF) / 4;
    bool           reportUnknown = false;
    bool           reportHeld    = false;
    {
        std::lock_guard lock(impl->mutex);
        impl->registerValues[reg] = value;
        if (reg == kCpRbWptr)
        {
            if (!impl->heldWriteLogged)
            {
                impl->heldWriteLogged = true;
                reportHeld            = true;
            }
        }
        else if (reg != kAvivoD1GraphPrimarySurfaceAddress)
        {
            if (!impl->unknownMmioLogged)
            {
                impl->unknownMmioLogged = true;
                reportUnknown           = true;
            }
        }
    }

    if (reportHeld)
    {
        REXGPU_ERROR(
            "NativeGuestGpuService: holding CP_RB_WPTR write {:08X}; command consumption is unavailable",
            value);
    }
    if (reportUnknown)
    {
        REXGPU_WARN("NativeGuestGpuService: unknown GPU register write {:04X}={:08X}", reg, value);
    }
}

int NativeGuestGpuService::runVblankWorker()
{
    rex::system::X_VIDEO_MODE videoMode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&videoMode);
    const double   refreshRateHz      = std::max(1.0, double(float(videoMode.refresh_rate)));
    const uint64_t guestTickFrequency = rex::chrono::Clock::guest_tick_frequency();
    const uint64_t vsyncIntervalTicks = std::max(
        uint64_t(1), uint64_t(double(guestTickFrequency) / refreshRateHz));
    uint64_t lastFrameTime = rex::chrono::Clock::QueryGuestTickCount();

    while (impl->vblankWorkerRunning.load(std::memory_order_acquire))
    {
        const uint64_t currentTime   = rex::chrono::Clock::QueryGuestTickCount();
        const uint64_t intervalTicks = vsyncIntervalTicks;
        uint32_t       catchUpCount  = 0;
        while (currentTime - lastFrameTime >= intervalTicks &&
               catchUpCount < kMaxVblankCatchUp &&
               impl->vblankWorkerRunning.load(std::memory_order_acquire))
        {
            markVblank();
            lastFrameTime += intervalTicks;
            ++catchUpCount;
        }
        if (catchUpCount == kMaxVblankCatchUp &&
            currentTime - lastFrameTime >= intervalTicks)
        {
            lastFrameTime = currentTime;
        }
        rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
}

void NativeGuestGpuService::markVblank()
{
    uint32_t                          callback           = 0;
    uint32_t                          userData           = 0;
    rex::runtime::FunctionDispatcher* functionDispatcher = nullptr;
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->active || !impl->interruptCallback || !impl->functionDispatcher)
        {
            return;
        }
        callback           = impl->interruptCallback;
        userData           = impl->interruptUserData;
        functionDispatcher = impl->functionDispatcher;
    }

    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread)
    {
        bool report = false;
        {
            std::lock_guard lock(impl->mutex);
            if (!impl->missingThreadLogged)
            {
                impl->missingThreadLogged = true;
                report                    = true;
            }
        }
        if (report)
        {
            REXGPU_WARN("NativeGuestGpuService: vblank has no current XThread");
        }
        return;
    }

    thread->SetActiveCpu(kDefaultCpu);
    uint64_t args[] = { 0, userData };
    functionDispatcher->ExecuteInterrupt(thread->thread_state(), callback, args, std::size(args));
}

void NativeGuestGpuService::Shutdown()
{
    rex::system::object_ref<rex::system::XHostThread> worker;
    {
        std::lock_guard lock(impl->mutex);
        impl->shutdownStarted   = true;
        impl->interruptCallback = 0;
        impl->interruptUserData = 0;
        impl->active            = false;
        impl->vblankWorkerRunning.store(false, std::memory_order_release);
        worker = impl->vblankWorkerThread;
    }

    unbindMmioBridge(this);

    if (worker)
    {
        worker->Wait(0, 0, 0, nullptr);
    }

    std::lock_guard lock(impl->mutex);
    impl->vblankWorkerThread.reset();
    impl->functionDispatcher = nullptr;
    impl->kernelState        = nullptr;
    impl->memory             = nullptr;
}

} // namespace rerevved::gpu
