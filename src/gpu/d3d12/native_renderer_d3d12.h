#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "native_draw_replay.h"
#include "native_frame_replay.h"

namespace rex::ui
{

class Window;

}

namespace rerevved::gpu
{

struct NativeDrawStreamOutput
{
    // The caller supplies a fresh path and a copied direct guest 256-entry
    // B10G10R10X2 table. The stream owns both until native completion.
    std::filesystem::path          outputPath;
    std::array<std::uint32_t, 256> gammaTable{};
};

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
    NativeDrawReplayResult ReplayDrawFrames(const NativeDrawFramesRecipe& recipe);
    // Starts a persistent headless 1280x720 stream; lifecycle operations stay owner-serialized.
    bool StartDrawStream(std::string& error);
    // Takes owned draws, blocks through native completion, and optionally writes two RGBA8 sample planes
    // plus a packed R10G10B10A2 output selected by the caller's copied gamma table;
    // SubmitDrawFrame and Shutdown may run concurrently, with shutdown draining or cancelling the waiter.
    NativeDrawReplayResult SubmitDrawFrame(
        std::vector<NativeDrawReplayRecipe>   draws,
        const std::filesystem::path&          sampleOutputPath = {},
        std::optional<NativeDrawStreamOutput> output           = std::nullopt);
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
    void drawStreamThreadMain();
    void frameReplayThreadMain(NativeFrameReplayRecipe              recipe,
                               std::promise<NativeDrawReplayResult> result);
    void handleRendererFailure();
    void drawFramesThreadMain(NativeDrawFramesRecipe               recipe,
                              std::promise<NativeDrawReplayResult> result);

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace rerevved::gpu
