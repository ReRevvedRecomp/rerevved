#pragma once

#include <cstdint>
#include <memory>

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
    // Latches the latest non-zero extent and returns acceptance immediately.
    bool Resize(std::uint32_t width, std::uint32_t height);
    void Shutdown();

    bool initialized() const noexcept;

private:
    void RendererThreadMain(std::uintptr_t native_window,
                            std::uint32_t  width,
                            std::uint32_t  height);
    void HandleRendererFailure();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rerevved::gpu
