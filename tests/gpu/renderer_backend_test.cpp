#include "gpu/renderer_backend.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "renderer_backend_test: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using rerevved::gpu::ParseRendererBackend;
    using rerevved::gpu::RendererBackend;
    using rerevved::gpu::RendererBackendName;

    require(ParseRendererBackend("xenos") == RendererBackend::Xenos,
            "xenos selection");
    require(ParseRendererBackend("native") == RendererBackend::Native,
            "native selection");
    require(ParseRendererBackend("") == RendererBackend::Invalid,
            "empty selection rejection");
    require(ParseRendererBackend("Native") == RendererBackend::Invalid,
            "case drift rejection");
    require(ParseRendererBackend("vulkan") == RendererBackend::Invalid,
            "unknown selection rejection");
    require(std::string_view(RendererBackendName(RendererBackend::Xenos)) == "xenos",
            "xenos name");
    require(std::string_view(RendererBackendName(RendererBackend::Native)) == "native",
            "native name");
    require(std::string_view(RendererBackendName(RendererBackend::Invalid)) == "invalid",
            "invalid name");

    std::cout << "renderer_backend_test: PASS\n";
    return 0;
}
