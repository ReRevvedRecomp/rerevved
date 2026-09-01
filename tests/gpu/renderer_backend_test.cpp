#include "gpu/renderer_backend.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{

void Require(bool condition, const char* message)
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

    Require(ParseRendererBackend("xenos") == RendererBackend::Xenos,
            "xenos selection");
    Require(ParseRendererBackend("native") == RendererBackend::Native,
            "native selection");
    Require(ParseRendererBackend("") == RendererBackend::Invalid,
            "empty selection rejection");
    Require(ParseRendererBackend("Native") == RendererBackend::Invalid,
            "case drift rejection");
    Require(ParseRendererBackend("vulkan") == RendererBackend::Invalid,
            "unknown selection rejection");
    Require(std::string_view(RendererBackendName(RendererBackend::Xenos)) == "xenos",
            "xenos name");
    Require(std::string_view(RendererBackendName(RendererBackend::Native)) == "native",
            "native name");
    Require(std::string_view(RendererBackendName(RendererBackend::Invalid)) == "invalid",
            "invalid name");

    std::cout << "renderer_backend_test: PASS\n";
    return 0;
}
