#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include <rex/rex_app.h>
#include <rex/system.h>
#include <rex/system/interfaces/video_delegate.h>
#include <rex/ui/frame_stats.h>

#include "gpu/d3d12/native_completion_source.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/guest_d3d/native_draw_dispatch.h"
#include "gpu/guest_d3d/native_frame_assembler.h"
#include "gpu/present/native_app_drain.h"
#include "gpu/present/native_app_route.h"
#include "gpu/present/native_presentation.h"
#include "gpu/renderer_backend.h"

namespace rerevved::gpu
{

class GuestVideoServices;
class NativeAppSession;
class NativeOverlayMailbox;

} // namespace rerevved::gpu

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
        // Before any SDK startup code can show a message box.
        rex::SetMessageBoxTitle("ReRevved");
        return std::unique_ptr<App>(new App(ctx, "rerevved", PPCImageConfig));
    }

protected:
    void OnPreSetup(rex::RuntimeConfig& config) override;
    void OnPreSystemLinkInit() override;

    std::filesystem::path GetDefaultUserDataRoot() const override;

    void OnConfigurePaths(rex::PathConfig& paths) override;

    std::optional<rex::system::ProfileCopySpecification> GetProfileCopySpecification() const override;

    bool SetupEnvironment() override;

    bool                                      SetupPresentation() override;
    std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override;
    bool                                      ConstructRuntime(const rex::PathConfig& paths) override;

    std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig&               defaults,
                                                   std::function<void(rex::PathConfig)> resume) override;

    void OnPostLoadXexImage() override;
    void OnPostSetup() override;
    void OnConfigureFonts(ImFontAtlas* atlas) override;
    void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override;
    void OnConfigureSettings(rex::ui::SettingsLayout& layout) override;

    void OnPostLaunchModule(rex::system::XThread* thread) override;
    void OnGuestThreadExit(rex::system::XThread* thread) override;

    void OnShutdown() override;
    void OnBeforeHardExit() override;
    void OnWindowMinimized() override;
    void OnWindowRestored() override;
    void OnDpiScaleChanged(float scale) override;

    bool OnWindowCloseRequested() override;

    void OnWindowFocusChanged(bool focused) override;

    void OnWindowPixelSizeChanged(uint32_t pixelWidth, uint32_t pixelHeight) override;

    // The base class keeps this private; the native script calls it directly.
    void OnKeyDown(rex::ui::KeyEvent& event) override;

private:
    bool                       setupNativePresentation();
    bool                       selectGameData();
    bool                       startGuestVideoServices();
    bool                       createNativeFrames();
    bool                       installNativeDraws();
    bool                       removeNativeDraws();
    void                       releaseNativeFrames(bool destroy);
    void                       closeNativeCompletion();
    std::optional<std::string> assembleNativeSwap(const rex::system::DetachedVideoSwap& swap);
    void                       stopGuestVideoProducers();
    void                       nativePresentationTick(gpu::PresentResult result);
    void                       requestNativeExtent();

    [[noreturn]] void hardExitAfterMissedVblankJoin();

#if REREVVED_DEVELOPER
    struct NativeFrameTiming;
    void reportNativeFrameTiming(std::chrono::steady_clock::duration assembly);
#endif

    std::shared_ptr<gpu::NativeAppSession>                nativePresentation;
    std::shared_ptr<gpu::GuestVideoServices>              guestVideo;
    std::unique_ptr<gpu::guest_d3d::NativeDrawDispatcher> nativeDraws;
    // The dispatcher's hooks and the ledger reach both; they are destroyed
    // only after the dispatcher was withdrawn and the producers stopped.
    std::shared_ptr<gpu::D3D12CompletionSource>           nativeCompletion;
    std::shared_ptr<gpu::guest_d3d::NativeFrameAssembler> nativeFrames;
    std::shared_ptr<void>                                 nativeWriteWatch; // the write notice reaching nativeFrames; reset unregisters it
    std::atomic<std::uint64_t>                            nativeSwapSequence{ 0 };
    bool                                                  nativeCompletionFailed = false; // UI thread
    bool                                                  nativeFixture          = false;
    std::shared_ptr<gpu::NativeOverlayMailbox>            nativeOverlays;
    std::shared_ptr<std::mutex>                           nativeDispatchMutex = std::make_shared<std::mutex>();
    std::shared_ptr<rex::ui::FrameRateMeter>              nativeFrameRate     = std::make_shared<rex::ui::FrameRateMeter>(); // presentation worker records, overlay reads
#if REREVVED_DEVELOPER
    std::shared_ptr<NativeFrameTiming> nativeFrameTiming; // set by native_frame_timing
#endif
    gpu::NativeAppClose                            nativeClose;
    std::optional<gpu::NativeAppDrain::Completion> nativeDrain;
    bool                                           nativeClosing = false;
#if REREVVED_DEVELOPER
    unsigned nativeScriptStage       = 0;
    unsigned nativeScriptFrames      = 0;
    unsigned nativeScriptClearFrames = 0;
#endif
    rerevved::gpu::RendererBackend rendererBackend =
        rerevved::gpu::RendererBackend::Xenos;
    rerevved::gpu::NativeRendererD3D12 nativeRenderer;
    std::shared_ptr<std::atomic<bool>> gameplayUiAlive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace rerevved
