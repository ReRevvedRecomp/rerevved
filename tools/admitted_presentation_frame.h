// Builds the admitted rgba8-32-tiled Prepared fixture as one native render
// frame. It reads the matrix fixture and its control decode without editing
// them, takes the vertex registers from the fixture's own register image, and
// leaves the Original runner out entirely.
#pragma once

#include "rgba8_matrix_fixture.h"

#include "gpu/d3d12/native_gameplay_draw.h"
#include "gpu/d3d12/native_gameplay_shaders.h"
#include "gpu/d3d12/native_render_frame.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/crypto/sha256.h>

namespace presentation_fixture
{

using namespace rerevved::gpu;
using namespace rex::graphics::testing;

inline constexpr std::string_view kFormName = "rgba8-32-tiled";
inline constexpr std::uint32_t    kWidth    = kNativePreparedRgba8MatrixOutputWidth;
inline constexpr std::uint32_t    kHeight   = kNativePreparedRgba8MatrixOutputHeight;

struct AdmittedFrame
{
    NativeRenderFrame frame;
    // The 1280x720 RGBA8 canvas the fixture expects before the gamma pass.
    std::vector<std::uint8_t> expectedRgba8;
};

inline std::string WordDigest(std::span<const std::uint32_t> words)
{
    std::vector<std::uint8_t> bytes(words.size() * 4);
    for (std::size_t index = 0; index < words.size(); ++index)
    {
        for (std::size_t byte = 0; byte < 4; ++byte)
        {
            bytes[index * 4 + byte] = static_cast<std::uint8_t>(words[index] >> (byte * 8));
        }
    }
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

inline void SetBigEndianFloat(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    const auto bits      = std::bit_cast<std::uint32_t>(value);
    bytes.at(offset + 3) = static_cast<std::uint8_t>(bits);
    bytes[offset]        = static_cast<std::uint8_t>(bits >> 24);
    bytes[offset + 1]    = static_cast<std::uint8_t>(bits >> 16);
    bytes[offset + 2]    = static_cast<std::uint8_t>(bits >> 8);
}

// Independent k8in32 reversal and 0x60A component selection, checked against
// the fixture's pixel oracle for every texel.
inline bool DecodeControlTexture(const NativePreparedRgba8MatrixFixture& fixture, NativeDrawReplayTexture& texture, std::string& error)
{
    if (Rgba8MatrixR8() || fixture.texture.mips.size() != kNativePreparedRgba8MatrixMipCount)
    {
        error = "fixture is not a complete RGBA8 form";
        return false;
    }
    std::vector<std::vector<std::uint8_t>> levels;
    for (std::uint32_t level = 0; level < kNativePreparedRgba8MatrixMipCount; ++level)
    {
        const auto& mip    = fixture.texture.mips[level];
        const auto& source = level ? fixture.texture.mip_guest_bytes : fixture.texture.base_guest_bytes;
        levels.emplace_back(static_cast<std::size_t>(mip.width) * mip.height * 4U);
        for (std::uint32_t y = 0; y < mip.height; ++y)
        {
            for (std::uint32_t x = 0; x < mip.width; ++x)
            {
                const auto at  = Rgba8MatrixAddress(level, x, y);
                const auto out = (static_cast<std::size_t>(y) * mip.width + x) * 4U;
                if (at + 4U > source.size())
                {
                    error = "fixture texel address exceeds the guest allocation";
                    return false;
                }
                for (unsigned component = 0; component < 4; ++component)
                {
                    levels.back()[out + component] = source[at + 3 - component];
                }
                const std::array<std::uint8_t, 4> interpreted = { levels.back()[out + 2], levels.back()[out + 1], levels.back()[out], levels.back()[out + 3] };
                if (!std::equal(interpreted.begin(), interpreted.end(), mip.linear_rgba8.begin() + static_cast<std::ptrdiff_t>(out)))
                {
                    error = "independent raw decode differs from the fixture pixel oracle";
                    return false;
                }
            }
        }
    }
    std::vector<std::span<const std::uint8_t>> spans;
    for (const auto& level : levels)
    {
        spans.emplace_back(level);
    }
    NativeDrawReplayTextureLayout layout;
    layout.width     = kNativePreparedRgba8MatrixWidth;
    layout.height    = kNativePreparedRgba8MatrixHeight;
    layout.mipLevels = kNativePreparedRgba8MatrixMipCount;
    layout.format    = NativeDrawReplayTextureFormat::Rgba8;
    if (!PackNativeDrawReplayTextureVersion(layout, spans, texture, error))
    {
        return false;
    }
    texture.swizzle = { 2, 1, 0, 3 };
    return true;
}

inline bool LoadControlShader(const std::filesystem::path& catalog, const NativePreparedRgba8MatrixFixture& fixture, NativeGameplayShaderInterface& shader, std::string& error)
{
    std::vector<NativeGameplayShaderInterface> shaders;
    if (!LoadNativeGameplayShaders(catalog, shaders, error))
    {
        error = "shader catalog: " + error;
        return false;
    }
    if (shaders.size() != 1)
    {
        error = "shader catalog must contain one shader pair";
        return false;
    }
    shader = std::move(shaders.front());
    if (shader.vertexGuestWordDigest != WordDigest(fixture.vertex_shader_words) || shader.pixelGuestWordDigest != WordDigest(fixture.pixel_shader_words))
    {
        error = "shader catalog is not bound to the exact fixture words";
        return false;
    }
    if (shader.textureFetchSlots.size() != 1 || shader.textureFetchSlots[0] != 0 || shader.textureDimensions[0] != NativeDrawReplayTextureDimension::Texture2D || shader.vertexAttributes.empty())
    {
        error = "shader catalog must bind one guest slot-0 Texture2D and vertex attributes";
        return false;
    }
    return true;
}

// Builds the frame: explicit black initial contents, the fixture draw, and a
// full-canvas resolve. The caller supplies the gamma ramp the producer applies.
inline bool BuildAdmittedFrame(const std::filesystem::path& catalog, const std::array<std::uint32_t, 256>& gammaTable, AdmittedFrame& out, std::string& error)
{
    const auto form = std::find_if(kRgba8MatrixForms.begin(), kRgba8MatrixForms.end(), [](const auto& candidate)
                                   {
                                       return std::string_view(candidate.name) == kFormName;
                                   });
    if (form == kRgba8MatrixForms.end())
    {
        error = "the admitted form is not in the fixture table";
        return false;
    }
    SelectRgba8MatrixForm(*form);
    auto fixture                     = MakeNativePreparedRgba8MatrixFixture();
    fixture.selected_lods            = { 0U, 0U };
    fixture.expected_selected_pixels = ExpectedNativePreparedRgba8MatrixPixels(fixture, 0U, 0U);
    if (fixture.expected_selected_pixels.size() != static_cast<std::size_t>(kWidth) * kHeight * 4U)
    {
        error = "fixture did not provide a complete expected canvas";
        return false;
    }
    // The selected LOD pair is encoded in the vertex stream.
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
    {
        const auto base = vertex * fixture.vertex_stride;
        SetBigEndianFloat(fixture.vertex_bytes, base + 24, 0.0F);
        SetBigEndianFloat(fixture.vertex_bytes, base + 28, 0.0F);
    }
    NativeDrawReplayTexture texture;
    if (!DecodeControlTexture(fixture, texture, error))
    {
        return false;
    }
    NativeGameplayShaderInterface shader;
    if (!LoadControlShader(catalog, fixture, shader, error))
    {
        return false;
    }
    if (fixture.register_image.size() != kNativePreparedRgba8MatrixRegisterCount)
    {
        error = "fixture register bank has the wrong extent";
        return false;
    }

    const auto              vertexFetchRegister = 0x4800U + 95U * 2U;
    NativeDrawReplaySampler sampler;
    sampler.minLinear = false;
    sampler.magLinear = false;
    sampler.mipLinear = false;
    sampler.address   = { 3, 3, 3 };
    sampler.minLod    = 0.0F;
    sampler.maxLod    = 0.0F;
    const NativeGameplayVertexFetchSpan fetch{ 95, fixture.register_image[vertexFetchRegister] & ~3U, fixture.vertex_bytes };
    const NativeGameplayTextureBinding  binding{ 0, &texture, &sampler };
    NativeGameplayDrawView              view;
    view.registers       = fixture.register_image;
    view.vertexMicrocode = fixture.vertex_shader_words;
    view.pixelMicrocode  = fixture.pixel_shader_words;
    view.vertexFetches   = std::span<const NativeGameplayVertexFetchSpan>(&fetch, 1);
    view.indexBytes      = fixture.index_bytes;
    view.textureBindings = std::span<const NativeGameplayTextureBinding>(&binding, 1);
    view.primitive       = 4;
    view.indexCount      = 3;
    view.indexFormat     = 0;
    view.indexEndian     = 1;
    view.indexed         = true;

    NativeGameplayDrawTarget target;
    target.width       = kWidth;
    target.height      = kHeight;
    target.sampleCount = 1;
    target.sampleMask  = 1;
    target.format      = NativeDrawReplayTargetFormat::Rgba8;
    const auto zScale  = std::bit_cast<float>(fixture.register_image[0x2113U]);
    const auto zOffset = std::bit_cast<float>(fixture.register_image[0x2114U]);
    target.viewport    = { 0.0F, 0.0F, static_cast<float>(Rgba8MatrixViewportWidth()), static_cast<float>(Rgba8MatrixViewportHeight()), zOffset, zOffset + zScale };
    target.scissor     = { 0, 0, static_cast<std::int32_t>(Rgba8MatrixViewportWidth()), static_cast<std::int32_t>(Rgba8MatrixViewportHeight()) };
    NativeDrawReplayRecipe recipe;
    if (!BuildNativeGameplayDrawRecipe(view, shader, target, recipe, error))
    {
        return false;
    }
    // Presenter capture normalizes the untouched canvas to opaque alpha.
    recipe.clearColor = { 0, 0, 0, 255 };

    NativeRenderFrame frame;
    frame.textures             = { { 1, kWidth, kHeight, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.returnGpuOutput      = true;
    frame.returnResolvedOutput = true;
    frame.gammaTable           = gammaTable;
    NativeRenderFramePass pass;
    pass.target = { kWidth, kHeight, 1, NativeDrawReplayTargetFormat::Rgba8 };
    NativeRenderFramePass::Clear clear;
    clear.rectangle  = { 0, 0, static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight) };
    clear.color      = true;
    clear.colorValue = { 0.0F, 0.0F, 0.0F, 1.0F };
    pass.clears.push_back(clear);
    pass.draws.push_back(std::move(recipe));
    NativeRenderFrameResolve resolve;
    resolve.textureId = 1;
    resolve.width     = kWidth;
    resolve.height    = kHeight;
    resolve.samples   = { 0, 0 };
    pass.resolves.push_back(resolve);
    frame.passes.push_back(std::move(pass));
    if (!ValidateNativeRenderFrame(frame, error))
    {
        return false;
    }
    out.frame         = std::move(frame);
    out.expectedRgba8 = fixture.expected_selected_pixels;
    return true;
}

} // namespace presentation_fixture
