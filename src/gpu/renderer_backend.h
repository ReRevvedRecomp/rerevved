#pragma once

#include <atomic>
#include <string_view>

namespace rerevved::gpu
{

enum class RendererBackend
{
    Xenos,
    Native,
    Invalid,
};

RendererBackend ParseRendererBackend(std::string_view value) noexcept;
const char*     RendererBackendName(RendererBackend backend) noexcept;

// Backend choice latches at startup, so later overlay cvar reads cannot change
// it.
RendererBackend LatchRendererBackend(std::string_view value) noexcept;

namespace detail
{

// Inline so every target that compiles a reader needs no extra source.
inline std::atomic<RendererBackend> selectedBackend{ RendererBackend::Xenos };

} // namespace detail

inline RendererBackend SelectedRendererBackend() noexcept
{
    return detail::selectedBackend.load(std::memory_order_acquire);
}

} // namespace rerevved::gpu
