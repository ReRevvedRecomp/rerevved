#include "gpu/renderer_backend.h"

#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>

// Mirrors the title's definition: the overlay edits it while the game runs.
REXCVAR_DEFINE_STRING(renderer, "xenos", "ReRevved", "Renderer backend: xenos or native")
    .allowed({ "xenos", "native" })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace
{

} // namespace

TEST_CASE("renderer_backend", "[ctest][renderer-backend]")
{
    using rerevved::gpu::LatchRendererBackend;
    using rerevved::gpu::ParseRendererBackend;
    using rerevved::gpu::RendererBackend;
    using rerevved::gpu::RendererBackendName;
    using rerevved::gpu::SelectedRendererBackend;

    UNSCOPED_INFO("xenos selection");
    REQUIRE(ParseRendererBackend("xenos") == RendererBackend::Xenos);
    UNSCOPED_INFO("native selection");
    REQUIRE(ParseRendererBackend("native") == RendererBackend::Native);
    UNSCOPED_INFO("empty selection rejection");
    REQUIRE(ParseRendererBackend("") == RendererBackend::Invalid);
    UNSCOPED_INFO("case drift rejection");
    REQUIRE(ParseRendererBackend("Native") == RendererBackend::Invalid);
    UNSCOPED_INFO("unknown selection rejection");
    REQUIRE(ParseRendererBackend("vulkan") == RendererBackend::Invalid);
    UNSCOPED_INFO("xenos name");
    REQUIRE(std::string_view(RendererBackendName(RendererBackend::Xenos)) == "xenos");
    UNSCOPED_INFO("native name");
    REQUIRE(std::string_view(RendererBackendName(RendererBackend::Native)) == "native");
    UNSCOPED_INFO("invalid name");
    REQUIRE(std::string_view(RendererBackendName(RendererBackend::Invalid)) == "invalid");

    UNSCOPED_INFO("unlatched default is xenos");
    REQUIRE(SelectedRendererBackend() == RendererBackend::Xenos);
    UNSCOPED_INFO("invalid value is reported");
    REQUIRE(LatchRendererBackend("vulkan") == RendererBackend::Invalid);
    UNSCOPED_INFO("invalid value does not latch");
    REQUIRE(SelectedRendererBackend() == RendererBackend::Xenos);

    UNSCOPED_INFO("set native");
    REQUIRE(rex::cvar::SetFlagByName("renderer", "native"));
    UNSCOPED_INFO("startup latches native");
    REQUIRE(LatchRendererBackend(REXCVAR_GET(renderer)) == RendererBackend::Native);
    UNSCOPED_INFO("latched native");
    REQUIRE(SelectedRendererBackend() == RendererBackend::Native);

    // Overlay edits must leave the backend latched for this run.
    UNSCOPED_INFO("set xenos");
    REQUIRE(rex::cvar::SetFlagByName("renderer", "xenos"));
    UNSCOPED_INFO("cvar changed after startup");
    REQUIRE(REXCVAR_GET(renderer) == "xenos");
    UNSCOPED_INFO("change awaits a restart");
    REQUIRE(!rex::cvar::GetPendingRestartFlags().empty());
    UNSCOPED_INFO("cvar change leaves the latch alone");
    REQUIRE(SelectedRendererBackend() == RendererBackend::Native);
    UNSCOPED_INFO("a later startup latches again");
    REQUIRE((LatchRendererBackend("xenos") == RendererBackend::Xenos &&
             SelectedRendererBackend() == RendererBackend::Xenos));
}
