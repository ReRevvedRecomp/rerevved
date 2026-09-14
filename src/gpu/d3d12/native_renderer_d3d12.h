#pragma once

#include <cstdint>
#include <future>
#include <memory>

#include "native_draw_replay.h"
#include "native_frame_replay.h"

namespace rex::ui
{

class Window;

}

namespace rerevved::gpu
{

class NativeRendererD3D12
{
public:
    NativeRendererD3D12();
    ~NativeRendererD3D12();

    NativeRendererD3D12(const NativeRendererD3D12&)            = delete;
    NativeRendererD3D12& operator=(const NativeRendererD3D12&) = delete;

    bool Initialize(rex::ui::Window& window);
    // Process startup only: enabling the debug layer after device creation
    // removes existing D3D12 devices. Live callers inherit the host's settings.
    static void            ConfigureReplayDiagnostics();
    NativeDrawReplayResult ReplayOffscreen(const NativeDrawReplayRecipe& recipe);
    NativeDrawReplayResult ReplayFrame(const NativeFrameReplayRecipe& recipe);
    // Latches the latest non-zero extent and returns acceptance immediately.
    bool Resize(std::uint32_t width, std::uint32_t height);
    void Shutdown();

    bool Initialized() const noexcept;

private:
    void rendererThreadMain(std::uintptr_t nativeWindow,
                            std::uint32_t  width,
                            std::uint32_t  height);
    void headlessReplayThreadMain(NativeDrawReplayRecipe               recipe,
                                  std::promise<NativeDrawReplayResult> result);
    void frameReplayThreadMain(NativeFrameReplayRecipe              recipe,
                               std::promise<NativeDrawReplayResult> result);
    void handleRendererFailure();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace rerevved::gpu
