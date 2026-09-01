#include "renderer_backend.h"

namespace rerevved::gpu
{

RendererBackend ParseRendererBackend(std::string_view value) noexcept
{
    if (value == "xenos")
    {
        return RendererBackend::Xenos;
    }
    if (value == "native")
    {
        return RendererBackend::Native;
    }
    return RendererBackend::Invalid;
}

const char* RendererBackendName(RendererBackend backend) noexcept
{
    switch (backend)
    {
        case RendererBackend::Xenos:
            return "xenos";
        case RendererBackend::Native:
            return "native";
        case RendererBackend::Invalid:
            return "invalid";
    }
    return "invalid";
}

} // namespace rerevved::gpu
