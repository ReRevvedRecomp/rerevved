#include "gpu/d3d12/native_guest_menu_draw.h"
#include "gpu/d3d12/native_texture_upload.h"

#include <array>
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

std::uint32_t bigWord(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

std::uint32_t littleWord(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
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
    const auto literals      = manifest["stride"].value_or(0U) == 32 ? read(root / "vertex-literals.be.bin") : std::vector<std::uint8_t>{};
    const bool movie         = manifest["stride"].value_or(0U) == 20;
    const auto pixelLiterals = movie ? read(root / "pixel-literals.be.bin") : std::vector<std::uint8_t>{};
    draw.pixelLiterals       = pixelLiterals;
    draw.primitive           = manifest["primitive"].value_or(4U);
    draw.vertexLiterals      = literals;
    draw.state               = state;
    draw.vertexMicrocode     = vs;
    draw.pixelMicrocode      = ps;
    draw.vertices            = vertices;
    draw.indices             = indices;
    draw.minimumVertex       = manifest["minimum_vertex"].value_or(0U);
    draw.vertexCount         = manifest["vertex_count"].value_or(0U);
    draw.indexCount          = manifest["index_count"].value_or(0U);
    draw.stride              = manifest["stride"].value_or(0U);
    draw.indexFormat         = manifest["index_format"].value_or(0U);
    draw.indexed             = manifest["indexed"].value_or(true);
    draw.firstInFrame        = manifest["first_in_frame"].value_or(false);
    std::array<NativeDrawReplayTexture, 3> textures;
    constexpr std::array<unsigned, 3>      movieFetches{ 2, 0, 1 };
    if (draw.stride != 8)
        for (unsigned slot = 0; slot < (movie ? 3U : 1U); ++slot)
        {
            NativeTextureFetch fetch;
            const auto         fetchIndex = movie ? movieFetches[slot] : 0;
            for (std::size_t i = 0; i < fetch.size(); ++i)
                fetch[i] = bigWord(state, 0x480 + fetchIndex * 24 + i * 4);
            const auto prefix = movie ? "texture-" + std::to_string(slot) : "texture";
            const auto source = read(root / (prefix + "-guest.bin"));
            require(DecodeNativeTexture(fetch, source, textures[slot], error), error);
            require(textures[slot].bytes == read(root / (prefix + "-native.bin")), "guest texture conversion stable");
            draw.textures[slot] = &textures[slot];
        }
    require(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), error);
    require(recipe.width == 1280 && recipe.height == 720 && recipe.viewport.x == 0 &&
                recipe.viewport.width == 1280 && recipe.scissor.right == 1280,
            "guest full viewport must produce one full-width native target");
    require(recipe.initialSample0.empty() && recipe.initialSample1.empty() &&
                recipe.textureMask == (movie ? 7U : draw.stride == 8 ? 0U
                                                                     : 1U) &&
                recipe.indexCount == (movie ? 6U : draw.indexed ? draw.indexCount
                                                                : draw.vertexCount),
            "guest draw must own its inputs and require no captured attachments");
    const bool firstInFrame = draw.firstInFrame;
    draw.firstInFrame       = true;
    require(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), error);
    require(recipe.clearColor == std::array<std::uint8_t, 4>{ 0, 0, 0, static_cast<std::uint8_t>(bigWord(state, 0x2A30) >> 24) },
            "first draw must preserve the captured black clear alpha");
    state[0x2A33] ^= 1;
    require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "nonblack guest clear rejected");
    state[0x2A33] ^= 1;
    state[0x2A34] ^= 1;
    require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "mismatched guest clear words rejected");
    state[0x2A34] ^= 1;
    draw.firstInFrame = false;
    require(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), error);
    require(recipe.clearColor == std::array<std::uint8_t, 4>{}, "later draw has no initial clear alpha");
    draw.firstInFrame = firstInFrame;
    require(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), error);
    if (draw.indexed && draw.stride == 32)
    {
        require((draw.vertexCount == 8 && draw.indexCount == 12) ||
                    (draw.vertexCount == 289 && draw.indexCount == 1536),
                "scene fixture must use one saved CPU geometry shape");
        require(vertices.size() == std::size_t(draw.vertexCount) * 32 && indices.size() == std::size_t(draw.indexCount) * 2,
                "scene fixture input buffers must match the bounded draw tuple");
        require(recipe.vertexStrideBytes == 48 && recipe.vertexAttributeCount == 3 &&
                    recipe.vertexData.size() == std::size_t(draw.vertexCount) * 48 &&
                    recipe.indices.size() == draw.indexCount,
                "scene draw must produce the pinned three-attribute buffer shape");
        require(recipe.vertexShaderHash == 0xC3BAB94E67553E12ULL &&
                    recipe.pixelShaderHash == 0x64D8E6A475FFCB2DULL,
                "scene draw must select the saved shader pair");
        require(recipe.depth.enabled && recipe.depth.writeEnabled && recipe.sampleMask == 9 &&
                    recipe.viewport.minDepth == 0.0F &&
                    recipe.viewport.maxDepth == 1.0F,
                "scene draw must enable depth over the full guest range");
        require(littleWord(recipe.sharedConstants, 300) == bigWord(state, 0x2904),
                "scene alpha test constant must follow the guest state");
        for (std::size_t i = 0; i < 16; ++i)
            require(littleWord(recipe.vertexConstants, (1008 + i) * 4) == bigWord(literals, i * 4),
                    "scene shader literals must override the device shadow");
        draw.vertexLiterals = {};
        require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "scene rejects missing shader literals");
        draw.vertexLiterals = literals;
        require(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), error);
        require(bigWord(ps, 12) == 0x10081001U && bigWord(ps, 16) == 0x1F1FF688U && bigWord(ps, 20) == 0x00004000U,
                "saved scene pixel shader must retain its exact texture words");
        draw.indexFormat = 0;
        require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "scene rejects non-GFx index format");
        draw.indexFormat = 1;
        require(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), error);
    }
    if (!draw.indexed && !movie)
    {
        require(recipe.vertexStrideBytes == 48 && recipe.vertexAttributeCount == 3,
                "label position, UV and color must use the pinned shader interface");
        for (std::uint32_t i = 0; i < draw.vertexCount; ++i)
            require(recipe.indices[i] == i, "nonindexed label must retain vertex order");
        draw.indexed = true;
        require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "label rejects indexed submission");
        draw.indexed = false;
        require(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), error);
    }
    if (draw.stride != 8 && !movie)
    {
        const bool notice          = bigWord(state, 0x2934) == 0x24F00770U && bigWord(state, 0x293C) == 0x87000007U;
        const auto expectedAddress = draw.stride == 32 || notice ? std::array<std::uint32_t, 3>{ 1, 1, 3 }
                                                                 : std::array<std::uint32_t, 3>{ 3, 3, 3 };
        require(recipe.sampler.address == expectedAddress &&
                    recipe.sampler.minLinear && recipe.sampler.magLinear && recipe.sampler.mipLinear &&
                    recipe.sampler.mipBias == 0 && recipe.textures[0].bytes == textures[0].bytes,
                "base-level guest sampler must match captured menu binding");
        state[0x490 + 3] ^= 4;
        require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "mipmapped guest texture rejected");
        state[0x490 + 3] ^= 4;
    }
    if (movie)
    {
        require(recipe.indices == std::vector<std::uint32_t>{ 0, 1, 2, 2, 1, 3 }, "movie strip winding");
        require(recipe.pixelConstants.size() == 4096 && !recipe.blend.enabled &&
                    recipe.textureMask == 7 && recipe.sampler.maxLod == 0.25F,
                "movie shader constants, planes and base-map sampler");
        for (std::size_t i = 0; i < 16; ++i)
            require(littleWord(recipe.pixelConstants, (1008 + i) * 4) == bigWord(pixelLiterals, i * 4),
                    "movie shader resource literals override the device shadow");
        draw.pixelLiterals = {};
        require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "missing movie literals rejected");
        draw.pixelLiterals = std::span(pixelLiterals).first(48);
        require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "partial movie literal block rejected");
        draw.pixelLiterals = pixelLiterals;
        draw.textures[2]   = nullptr;
        require(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), "missing movie plane rejected");
        draw.textures[2] = &textures[2];
        require(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error), error);
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
