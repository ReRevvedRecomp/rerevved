#pragma once

#include "label_draw_adapter.h"
#include "movie_draw_inputs.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Position (k_32_32_32_FLOAT) and UV (k_32_32_FLOAT) byte offsets of the
// 20-byte movie vertex.
inline constexpr std::array<std::uint32_t, 2> kMovieAttributeOffsets{ 0, 12 };

// Movie contract native_menu_draw.cpp:43 plus GFx state: screen-space, clipping
// off, no blend, full viewport.0x2200 stencil compare irrelevant when disabled;
// 0x2202 ALPHA_FUNC0..2 varies only with enable bit 3 clear(boot 7, start 4).
inline constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 30> kMovieFixedState{ {
    { 0x2000, 0x0A010280 },
    { 0x2001, 0 },
    { 0x2002, 0x530 },
    { 0x200E, 0 },
    { 0x200F, 0x20002000 },
    { 0x2080, 0 },
    { 0x2081, 0 },
    { 0x2082, 0x02D00500 },
    { 0x2100, 0xFFFFFF },
    { 0x2101, 0 },
    { 0x2102, 0 },
    { 0x2104, 15 },
    { 0x210F, 0x44200000 },
    { 0x2110, 0x44200000 },
    { 0x2111, 0xC3B40000 },
    { 0x2112, 0x43B40000 },
    { 0x2113, 0x3F800000 },
    { 0x2114, 0 },
    { 0x2180, 0x10010101 },
    { 0x2181, 4 },
    { 0x2182, 0x10001 },
    { 0x2201, 0x00010001 },
    { 0x2202, 0x87000007 },
    { 0x2204, 0x90000 },
    { 0x2205, 0x18000 },
    { 0x2206, 0x400 },
    { 0x2208, 4 },
    { 0x2302, 4 },
    { 0x2307, 0xFF000 },
    { 0x2308, 0xFF100 },
} };

// One decoded-texture cache per plane slot. A held frame (the same fetch and
// bytes) reuses its immutable texture versions; a new frame misses.
struct MovieTextureCache
{
    std::array<LabelTextureCache, 3> planes;
};

struct MovieDrawRecipe
{
    NativeDrawReplayRecipe                 native;
    std::vector<std::uint8_t>              patchedVertexCode, effectiveState;
    std::array<NativeDrawReplayTexture, 3> textures;
    std::array<NativeDrawReplaySampler, 3> samplers;
};

// Movie catalog entry pins guest digests, screen specialization, and input
// interface; refusals name catalog, digest, interface, or layout.
bool LoadMovieDrawShader(const std::filesystem::path& directory, NativeGameplayShaderInterface& shader, std::string& error);
// Host sampler for one movie plane: the GFx sampler, with its refusal codes
// renamed movie_texture_mipmapped and movie_sampler_*.
bool DecodeMovieSampler(const std::array<std::uint32_t, 6>& fetch, NativeDrawReplaySampler& sampler, std::string& error);
// False with an empty error means the inputs are not a movie candidate.
bool BuildMovieDrawRecipe(const MovieDrawInputs& inputs, const NativeGameplayShaderInterface& shader, MovieDrawRecipe& recipe, std::string& error, MovieTextureCache* cache = nullptr);
bool BuildMovieDraw(const MovieDrawInputs& inputs, const NativeGameplayShaderInterface& shader, NativeDrawReplayRecipe& draw, std::string& detail, MovieTextureCache* cache = nullptr);

// The intro movie quad, a native_draw_movie_shaders family.
NativeDrawFamily             MakeMovieDrawFamily(ReadMemory read, NativeDrawFamilyShader shader);
NativeDrawFamilyRegistration MovieDrawFamily();

} // namespace rerevved::gpu::guest_d3d
