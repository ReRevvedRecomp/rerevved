#include "gpu/d3d12/native_guest_menu_draw.h"
#include "gpu/d3d12/native_texture_upload.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <toml++/toml.hpp>

using namespace rerevved::gpu;

namespace
{

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::vector<std::uint8_t> read(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    require(size <= 16 * 1024 * 1024, "fixture size bound");
    std::vector<std::uint8_t> bytes(size);
    std::ifstream             file(path, std::ios::binary);
    require(bool(file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), path.string());
    return bytes;
}

} // namespace

int main(int argc, char** argv)
{
    NativeMenuShaders      shaders;
    NativeDrawReplayRecipe recipe;
    NativeGuestMenuDraw    draw;
    std::string            error;
    require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "incomplete CPU input rejected");
    std::vector<std::uint8_t> emptyState(0x3500);
    draw.state       = emptyState;
    draw.primitive   = 4;
    draw.stride      = 8;
    draw.indexFormat = 1;
    require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "unset viewport rejected");
    if (argc != 3)
        return argc == 1 ? 0 : 1;
    const std::filesystem::path root(argv[1]);
    require(LoadNativeMenuShaders(argv[2], shaders, error), error);
    const auto manifest = toml::parse_file((root / "manifest.toml").string());
    auto       state    = read(root / "state-after.be.bin");
    auto       vs       = read(root / "vertex-shader.be.bin");
    const auto ps       = read(root / "pixel-shader.be.bin");
    const auto vertices = read(root / "vertex.bin"), indices = read(root / "index.bin");
    draw.state           = state;
    draw.vertexMicrocode = vs;
    draw.pixelMicrocode  = ps;
    draw.vertices        = vertices;
    draw.indices         = indices;
    draw.minimumVertex   = manifest["minimum_vertex"].value_or(0U);
    draw.vertexCount     = manifest["vertex_count"].value_or(0U);
    draw.indexCount      = manifest["index_count"].value_or(0U);
    draw.stride          = manifest["stride"].value_or(0U);
    draw.indexFormat     = manifest["index_format"].value_or(0U);
    NativeDrawReplayTexture texture;
    if (draw.stride != 8)
    {
        NativeTextureFetch fetch;
        for (std::size_t i = 0; i < fetch.size(); ++i)
        {
            const auto* b = state.data() + 0x480 + i * 4;
            fetch[i]      = (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) | b[3];
        }
        const auto source = read(root / "texture-guest.bin");
        require(DecodeNativeTexture(fetch, source, texture, error), error);
        require(texture.bytes == read(root / "texture-native.bin"), "guest texture conversion stable");
        draw.texture = &texture;
    }
    require(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), error);
    require(recipe.width == 1280 && recipe.height == 720 && recipe.viewport.x == 0 &&
                recipe.viewport.width == 1280 && recipe.scissor.right == 1280,
            "guest full viewport must produce one full-width native target");
    require(recipe.initialSample0.empty() && recipe.initialSample1.empty() &&
                recipe.textureMask == (draw.stride == 8 ? 0U : 1U) && recipe.indexCount == draw.indexCount,
            "guest draw must own its inputs and require no captured attachments");
    if (draw.stride != 8)
    {
        require(recipe.sampler.address == std::array<std::uint32_t, 3>{ 3, 3, 3 } &&
                    recipe.sampler.minLinear && recipe.sampler.magLinear && recipe.sampler.mipLinear &&
                    recipe.sampler.mipBias == 0 && recipe.texture.bytes == texture.bytes,
                "base-level guest sampler must match captured menu binding");
        state[0x490 + 3] ^= 4;
        require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "mipmapped guest texture rejected");
        state[0x490 + 3] ^= 4;
    }
    vs[0] ^= 1;
    require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "changed guest shader rejected");
    vs[0] ^= 1;
    state[0x3170] ^= 1;
    require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "changed viewport rejected");
    state[0x3170] ^= 1;
    state[0x28C8] ^= 1;
    require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "changed guest scissor rejected");
    state[0x28C8] ^= 1;
    draw.minimumVertex = 1;
    require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "unsupported index bias rejected");
    std::cout << "Guest shader, viewport, geometry, and rejection checks passed\n";
    return 0;
}
