#include "gpu/guest_gpu_service.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

using rex::X_STATUS;

namespace
{

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "guest_gpu_service_test: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    rerevved::gpu::NativeGuestGpuService service;

    Require(service.SetupPresentation(nullptr) == X_STATUS_SUCCESS,
            "presentation setup must acknowledge external presentation");
    Require(service.SetupPresentation(nullptr) == X_STATUS_SUCCESS,
            "presentation setup must be idempotent");
    Require(service.SetupGuestGpu(nullptr, nullptr) == X_STATUS_INVALID_PARAMETER,
            "guest GPU setup must reject null dependencies");
    Require(!service.has_presentation(), "presentation must remain unavailable");
    Require(service.provider() == nullptr, "provider must remain null");
    Require(service.presenter() == nullptr, "presenter must remain null");

    constexpr uint32_t kCallback               = 0x12345678;
    constexpr uint32_t kUserData               = 0x89ABCDEF;
    constexpr uint32_t kRingPointer            = 0x10002000;
    constexpr uint32_t kRingSizeLog2           = 17;
    constexpr uint32_t kWritebackPointer       = 0x20003000;
    constexpr uint32_t kWritebackBlockSizeLog2 = 6;

    service.SetInterruptCallback(kCallback, kUserData);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(kWritebackPointer, kWritebackBlockSizeLog2);
    service.InitializeShaderStorage({}, 0, false);
    Require(!service.PauseAndResetGpuWritePointer(),
            "GPU write-pointer reset must fail closed");
    service.ResumeGpu();

    Require(!service.has_presentation(), "optional calls must not create presentation");

    service.SetInterruptCallback(0, 0);

    service.Shutdown();
    service.Shutdown();
    service.SetInterruptCallback(kCallback, kUserData);
    Require(!service.has_presentation(), "shutdown must remain idempotent");

    std::cout << "guest_gpu_service_test: PASS\n";
    return 0;
}
