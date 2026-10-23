#include "gpu/d3d12/native_guest_menu_draw.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "manual_cases.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <toml++/toml.hpp>

using namespace rerevved::gpu;

namespace
{

std::vector<std::uint8_t> read(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    UNSCOPED_INFO("fixture size bound");
    REQUIRE(size <= 16 * 1024 * 1024);
    std::vector<std::uint8_t> bytes(size);
    std::ifstream             file(path, std::ios::binary);
    UNSCOPED_INFO(path.string());
    REQUIRE(bool(file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())));
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

// With no capture root only the synthetic rejections run; with one, the saved draw
// replays too.
void checkGuestMenuDraw(const char* captureRoot, const char* shaderRoot)
{
    NativeMenuShaders      shaders;
    NativeDrawReplayRecipe recipe;
    NativeGuestMenuDraw    draw;
    std::string            error;
    UNSCOPED_INFO("incomplete CPU input rejected");
    REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    std::vector<std::uint8_t> emptyState(0x3500);
    draw.state       = emptyState;
    draw.primitive   = 4;
    draw.stride      = 8;
    draw.indexFormat = 1;
    UNSCOPED_INFO("unset viewport rejected");
    REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    if (!captureRoot)
        return;
    const std::filesystem::path root(captureRoot);
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeMenuShaders(shaderRoot, shaders, error));
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
            UNSCOPED_INFO(error);
            REQUIRE(DecodeNativeTexture(fetch, source, textures[slot], error));
            UNSCOPED_INFO("guest texture conversion stable");
            REQUIRE(textures[slot].bytes == read(root / (prefix + "-native.bin")));
            draw.textures[slot] = &textures[slot];
        }
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    UNSCOPED_INFO("guest full viewport must produce one full-width native target");
    REQUIRE((recipe.width == 1280 && recipe.height == 720 && recipe.viewport.x == 0 &&
             recipe.viewport.width == 1280 && recipe.scissor.right == 1280));
    UNSCOPED_INFO("guest draw must own its inputs and require no captured attachments");
    REQUIRE((recipe.initialSample0.empty() && recipe.initialSample1.empty() &&
             recipe.textureMask == (movie ? 7U : draw.stride == 8 ? 0U
                                                                  : 1U) &&
             recipe.indexCount == (movie ? 6U : draw.indexed ? draw.indexCount
                                                             : draw.vertexCount)));
    const bool firstInFrame = draw.firstInFrame;
    draw.firstInFrame       = true;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    UNSCOPED_INFO("first draw must preserve the captured black clear alpha");
    REQUIRE((recipe.clearColor == std::array<std::uint8_t, 4>{ 0, 0, 0, static_cast<std::uint8_t>(bigWord(state, 0x2A30) >> 24) }));
    state[0x2A33] ^= 1;
    UNSCOPED_INFO("nonblack guest clear rejected");
    REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    state[0x2A33] ^= 1;
    state[0x2A34] ^= 1;
    UNSCOPED_INFO("mismatched guest clear words rejected");
    REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    state[0x2A34] ^= 1;
    draw.firstInFrame = false;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    UNSCOPED_INFO("later draw has no initial clear alpha");
    REQUIRE((recipe.clearColor == std::array<std::uint8_t, 4>{}));
    draw.firstInFrame = firstInFrame;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    if (draw.indexed && draw.stride == 32)
    {
        UNSCOPED_INFO("scene fixture must use one saved CPU geometry shape");
        REQUIRE(((draw.vertexCount == 8 && draw.indexCount == 12) ||
                 (draw.vertexCount == 289 && draw.indexCount == 1536)));
        UNSCOPED_INFO("scene fixture input buffers must match the bounded draw tuple");
        REQUIRE((vertices.size() == std::size_t(draw.vertexCount) * 32 && indices.size() == std::size_t(draw.indexCount) * 2));
        UNSCOPED_INFO("scene draw must produce the pinned three-attribute buffer shape");
        REQUIRE((recipe.vertexStrideBytes == 48 && recipe.vertexAttributeCount == 3 &&
                 recipe.vertexData.size() == std::size_t(draw.vertexCount) * 48 &&
                 recipe.indices.size() == draw.indexCount));
        UNSCOPED_INFO("scene draw must select the saved shader pair");
        REQUIRE((recipe.vertexShaderHash == 0xC3BAB94E67553E12ULL &&
                 recipe.pixelShaderHash == 0x64D8E6A475FFCB2DULL));
        UNSCOPED_INFO("scene draw must enable depth over the full guest range");
        REQUIRE((recipe.depth.enabled && recipe.depth.writeEnabled && recipe.sampleMask == 9 &&
                 recipe.viewport.minDepth == 0.0F &&
                 recipe.viewport.maxDepth == 1.0F));
        UNSCOPED_INFO("scene alpha test constant must follow the guest state");
        REQUIRE(littleWord(recipe.sharedConstants, 300) == bigWord(state, 0x2904));
        for (std::size_t i = 0; i < 16; ++i)
        {
            UNSCOPED_INFO("scene shader literals must override the device shadow");
            REQUIRE(littleWord(recipe.vertexConstants, (1008 + i) * 4) == bigWord(literals, i * 4));
        }
        draw.vertexLiterals = {};
        UNSCOPED_INFO("scene rejects missing shader literals");
        REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
        draw.vertexLiterals = literals;
        UNSCOPED_INFO(error);
        REQUIRE(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
        UNSCOPED_INFO("saved scene pixel shader must retain its exact texture words");
        REQUIRE((bigWord(ps, 12) == 0x10081001U && bigWord(ps, 16) == 0x1F1FF688U && bigWord(ps, 20) == 0x00004000U));
        draw.indexFormat = 0;
        UNSCOPED_INFO("scene rejects non-GFx index format");
        REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
        draw.indexFormat = 1;
        UNSCOPED_INFO(error);
        REQUIRE(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    }
    if (!draw.indexed && !movie)
    {
        UNSCOPED_INFO("label position, UV and color must use the pinned shader interface");
        REQUIRE((recipe.vertexStrideBytes == 48 && recipe.vertexAttributeCount == 3));
        for (std::uint32_t i = 0; i < draw.vertexCount; ++i)
        {
            UNSCOPED_INFO("nonindexed label must retain vertex order");
            REQUIRE(recipe.indices[i] == i);
        }
        draw.indexed = true;
        UNSCOPED_INFO("label rejects indexed submission");
        REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
        draw.indexed = false;
        UNSCOPED_INFO(error);
        REQUIRE(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    }
    if (draw.stride != 8 && !movie)
    {
        const bool notice          = bigWord(state, 0x2934) == 0x24F00770U && bigWord(state, 0x293C) == 0x87000007U;
        const auto expectedAddress = draw.stride == 32 || notice ? std::array<std::uint32_t, 3>{ 1, 1, 3 }
                                                                 : std::array<std::uint32_t, 3>{ 3, 3, 3 };
        UNSCOPED_INFO("base-level guest sampler must match captured menu binding");
        REQUIRE((recipe.sampler.address == expectedAddress &&
                 recipe.sampler.minLinear && recipe.sampler.magLinear && recipe.sampler.mipLinear &&
                 recipe.sampler.mipBias == 0 && recipe.textures[0].bytes == textures[0].bytes));
        state[0x490 + 3] ^= 4;
        UNSCOPED_INFO("mipmapped guest texture rejected");
        REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
        state[0x490 + 3] ^= 4;
    }
    if (movie)
    {
        UNSCOPED_INFO("movie strip winding");
        REQUIRE(recipe.indices == std::vector<std::uint32_t>{ 0, 1, 2, 2, 1, 3 });
        UNSCOPED_INFO("movie shader constants, planes and base-map sampler");
        REQUIRE((recipe.pixelConstants.size() == 4096 && !recipe.blend.enabled &&
                 recipe.textureMask == 7 && recipe.sampler.maxLod == 0.25F));
        for (std::size_t i = 0; i < 16; ++i)
        {
            UNSCOPED_INFO("movie shader resource literals override the device shadow");
            REQUIRE(littleWord(recipe.pixelConstants, (1008 + i) * 4) == bigWord(pixelLiterals, i * 4));
        }
        draw.pixelLiterals = {};
        UNSCOPED_INFO("missing movie literals rejected");
        REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
        draw.pixelLiterals = std::span(pixelLiterals).first(48);
        UNSCOPED_INFO("partial movie literal block rejected");
        REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
        draw.pixelLiterals = pixelLiterals;
        draw.textures[2]   = nullptr;
        UNSCOPED_INFO("missing movie plane rejected");
        REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
        draw.textures[2] = &textures[2];
        UNSCOPED_INFO(error);
        REQUIRE(BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    }
    vs[0] ^= 1;
    UNSCOPED_INFO("changed guest shader rejected");
    REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    vs[0] ^= 1;
    state[0x3170] ^= 1;
    UNSCOPED_INFO("changed viewport rejected");
    REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    state[0x3170] ^= 1;
    state[0x28C8] ^= 1;
    UNSCOPED_INFO("changed guest scissor rejected");
    REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
    state[0x28C8] ^= 1;
    draw.minimumVertex = 1;
    UNSCOPED_INFO("unsupported index bias rejected");
    REQUIRE(!BuildNativeGuestMenuDrawRecipe(draw, shaders, recipe, error));
}

} // namespace

TEST_CASE("native_guest_menu_draw", "[ctest][native-guest-menu-draw]")
{
    checkGuestMenuDraw(nullptr, nullptr);
}

// Replays a saved guest menu draw: native-guest-menu-draw.capture <draw root> <shader root>.
TEST_CASE("native_guest_menu_draw_capture", "[.][manual][native-guest-menu-draw]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass the draw and shader roots through --helper native-guest-menu-draw.capture");
    }
    UNSCOPED_INFO("pass exactly the draw root and the shader root");
    REQUIRE(args.argc() == 2);
    checkGuestMenuDraw(args.argv()[1], args.argv()[2]);
}

REREVVED_MANUAL_HELPER(nativeGuestMenuDrawCapture, "native-guest-menu-draw.capture", "native_guest_menu_draw_capture")
