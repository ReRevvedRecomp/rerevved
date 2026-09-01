#pragma once

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

} // namespace rerevved::gpu
