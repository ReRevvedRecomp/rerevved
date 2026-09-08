#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>

#include <rex/rex_app.h>

#include "fence_trace_finalization_gate.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/renderer_backend.h"
#include "native_renderer_coverage.h"

// Defined by the generated module init (generated/default/rerevved_init.cpp).
extern const rex::PPCImageInfo PPCImageConfig;

namespace rerevved
{

class App : public rex::ReXApp
{
public:
    using rex::ReXApp::ReXApp;

    static std::unique_ptr<rex::ui::WindowedApp> Create(
        rex::ui::WindowedAppContext& ctx)
    {
        return std::unique_ptr<App>(new App(ctx, "rerevved", PPCImageConfig));
    }

protected:
    void OnPreSetup(rex::RuntimeConfig& config) override;

    std::filesystem::path GetDefaultUserDataRoot() const override;

    void OnConfigurePaths(rex::PathConfig& paths) override;

    std::optional<rex::system::ProfileCopySpecification> GetProfileCopySpecification() const override;

    bool SetupEnvironment() override;

    bool SetupPresentation() override;

    std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig&               defaults,
                                                   std::function<void(rex::PathConfig)> resume) override;

    void OnPostSetup() override;

    void OnGuestThreadExit(rex::system::XThread* thread) override;

    void OnShutdown() override;

    bool OnWindowCloseRequested() override;

    void OnWindowFocusChanged(bool focused) override;

    void OnWindowPixelSizeChanged(uint32_t pixelWidth, uint32_t pixelHeight) override;

    void OnKeyDown(rex::ui::KeyEvent& event) override;

private:
    bool recordCoverageCheckpoint(bool finalSegment);
    void finalizeCoverage(rerevved::native_renderer::ExitClass exitClass);
    void finalizePassiveTrace();
    void finalizeFenceTrace();

    std::atomic<bool>                                 coverageStarted{ false };
    std::atomic<bool>                                 coverageFinalizeStarted{ false };
    std::mutex                                        coverageCheckpointMutex;
    bool                                              coverageBindRegistered = false;
    bool                                              windowFocused          = false;
    uint32_t                                          coverageMarkCount      = 0;
    std::atomic<bool>                                 passiveTraceStarted{ false };
    std::atomic<bool>                                 passiveTraceFinalizeStarted{ false };
    std::filesystem::path                             passiveTraceOutputPath;
    std::atomic<bool>                                 fenceTraceStarted{ false };
    rerevved::diagnostics::FenceTraceFinalizationGate fenceTraceFinalization;
    std::filesystem::path                             fenceTraceOutputPath;
    rerevved::gpu::RendererBackend                    rendererBackend =
        rerevved::gpu::RendererBackend::Xenos;
    rerevved::gpu::NativeRendererD3D12 nativeRenderer;
};

} // namespace rerevved
