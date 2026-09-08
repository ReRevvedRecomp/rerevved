#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace rerevved::gpu
{

class NativeGuestGpuService final : public rex::system::IGraphicsSystem
{
public:
    NativeGuestGpuService();
    ~NativeGuestGpuService() override;

    rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* appContext) override;
    rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* functionDispatcher,
                                rex::system::KernelState*         kernelState) override;

    bool has_presentation() const override
    {
        return false;
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData) override;
    void InitializeRingBuffer(uint32_t ptr, uint32_t sizeLog2) override;
    void EnableReadPointerWriteBack(uint32_t ptr, uint32_t blockSizeLog2) override;
    void InitializeShaderStorage(const std::filesystem::path& cacheRoot,
                                 uint32_t                     titleId,
                                 bool                         blocking) override;
    bool PauseAndResetGpuWritePointer() override;
    void ResumeGpu() override;

    void Shutdown() override;

private:
    struct Impl;

    static uint32_t readRegisterThunk(void* ppcContext, void* callbackContext, uint32_t addr);
    static void     writeRegisterThunk(void* ppcContext, void* callbackContext, uint32_t addr, uint32_t value);

    uint32_t readRegister(uint32_t addr);
    void     writeRegister(uint32_t addr, uint32_t value);
    int      runVblankWorker();
    void     markVblank();

    std::unique_ptr<Impl> impl;
};

} // namespace rerevved::gpu
