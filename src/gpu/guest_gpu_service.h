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

    rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
    rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                                rex::system::KernelState*         kernel_state) override;

    bool has_presentation() const override
    {
        return false;
    }

    void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
    void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
    void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;
    void InitializeShaderStorage(const std::filesystem::path& cache_root,
                                 uint32_t                     title_id,
                                 bool                         blocking) override;
    bool PauseAndResetGpuWritePointer() override;
    void ResumeGpu() override;

    void Shutdown() override;

private:
    struct Impl;

    static uint32_t ReadRegisterThunk(void* ppc_context, void* callback_context, uint32_t addr);
    static void     WriteRegisterThunk(void* ppc_context, void* callback_context, uint32_t addr, uint32_t value);

    uint32_t ReadRegister(uint32_t addr);
    void     WriteRegister(uint32_t addr, uint32_t value);
    int      RunVblankWorker();
    void     MarkVblank();

    std::unique_ptr<Impl> impl_;
};

} // namespace rerevved::gpu
