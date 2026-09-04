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

class ReRevvedApp : public rex::ReXApp
{
public:
    using rex::ReXApp::ReXApp;

    static std::unique_ptr<rex::ui::WindowedApp> Create(
        rex::ui::WindowedAppContext& ctx)
    {
        return std::unique_ptr<ReRevvedApp>(new ReRevvedApp(ctx, "rerevved", PPCImageConfig));
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

    void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override;

    void OnKeyDown(rex::ui::KeyEvent& event) override;

private:
    bool RecordCoverageCheckpoint(bool final_segment);
    void FinalizeCoverage(rerevved::native_renderer::ExitClass exit_class);
    void FinalizePassiveTrace();
    void FinalizeFenceTrace();

    std::atomic<bool>                                 coverage_started_{ false };
    std::atomic<bool>                                 coverage_finalize_started_{ false };
    std::mutex                                        coverage_checkpoint_mutex_;
    bool                                              coverage_bind_registered_ = false;
    bool                                              window_focused_           = false;
    uint32_t                                          coverage_mark_count_      = 0;
    std::atomic<bool>                                 passive_trace_started_{ false };
    std::atomic<bool>                                 passive_trace_finalize_started_{ false };
    std::filesystem::path                             passive_trace_output_path_;
    std::atomic<bool>                                 fence_trace_started_{ false };
    rerevved::diagnostics::FenceTraceFinalizationGate fence_trace_finalization_;
    std::filesystem::path                             fence_trace_output_path_;
    rerevved::gpu::RendererBackend                    renderer_backend_ =
        rerevved::gpu::RendererBackend::Xenos;
    rerevved::gpu::NativeRendererD3D12 native_renderer_;
};
