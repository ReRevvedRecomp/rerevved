#include "movie_draw_adapter.h"

#include "gfx_draw_adapter.h"
#include "gpu/d3d12/native_texture_upload.h"

#include <algorithm>
#include <bit>
#include <format>
#include <stdexcept>

namespace rerevved::gpu::guest_d3d
{
namespace
{

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("movie_adapter_input_extent");
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

void require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

std::uint64_t physical(std::uint64_t raw)
{
    require(raw <= UINT32_MAX, "movie_vertex_address_overflow");
    return (raw & 0x1FFFFFFFULL) + (((raw >> 20) + 512) & 0x1000);
}

std::vector<std::uint32_t> microcode(std::span<const std::uint8_t> bytes)
{
    require(!bytes.empty() && bytes.size() % 4 == 0, "movie_microcode_extent");
    std::vector<std::uint32_t> words;
    for (std::size_t i = 0; i < bytes.size(); i += 4)
        words.push_back(word(bytes, i));
    return words;
}

bool isMovieEntry(const NativeGameplayShaderInterface& entry)
{
    return entry.vertexShaderHash == kMovieVertexShaderHash && entry.pixelShaderHash == kMoviePixelShaderHash;
}

const std::vector<std::uint32_t> kMovieSlots{ 0, 1, 2 };

} // namespace

bool LoadMovieDrawShader(const std::filesystem::path& directory, NativeGameplayShaderInterface& shader, std::string& error)
{
    shader = {};
    error.clear();
    try
    {
        std::vector<NativeGameplayShaderInterface> catalog;
        if (!LoadNativeGameplayShaders(directory, catalog, error))
            return false;
        require(std::ranges::count_if(catalog, isMovieEntry) == 1, "movie_catalog_entry");
        shader = *std::ranges::find_if(catalog, isMovieEntry);
        require(shader.vertexGuestWordDigest == kMovieVertexWordDigest && shader.pixelGuestWordDigest == kMoviePixelWordDigest,
                "movie_catalog_word_digest");
        require(shader.textureFetchSlots == kMovieSlots &&
                    shader.textureDimensions == std::vector<NativeDrawReplayTextureDimension>(3, NativeDrawReplayTextureDimension::Texture2D) &&
                    !shader.alphaTestEnabled && shader.screenSpacePosition &&
                    shader.shaderProgram == 0x10010101 && shader.shaderContext == 4 && shader.shaderInterpolators == 0x10001 &&
                    shader.vertexAttributes.size() == kMovieAttributeOffsets.size(),
                "movie_catalog_interface");
        // Position is k_32_32_32_FLOAT and UV k_32_32_FLOAT, neither normalized.
        constexpr std::array<std::uint32_t, 2> formats{ 57, 37 };
        for (std::size_t i = 0; i < formats.size(); ++i)
        {
            const auto& attribute = shader.vertexAttributes[i];
            require(attribute.fetchConstant == 95 && attribute.strideBytes == kMovieStride &&
                        attribute.offsetBytes == std::int32_t(kMovieAttributeOffsets[i]) && attribute.format == formats[i] &&
                        !attribute.normalized && attribute.isSigned,
                    "movie_catalog_layout");
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        shader = {};
        error  = exception.what();
        return false;
    }
}

bool DecodeMovieSampler(const std::array<std::uint32_t, 6>& fetch, NativeDrawReplaySampler& sampler, std::string& error)
{
    // The GFx exactness rules apply unchanged: with one level and equal
    // filters, a LOD bias or gradient adjust cannot change the result.
    if (DecodeGfxSampler(fetch, sampler, error))
        return true;
    if (error.starts_with("gfx_"))
        error = "movie_" + error.substr(4);
    return false;
}

bool BuildMovieDrawRecipe(const MovieDrawInputs& inputs, const NativeGameplayShaderInterface& shader, MovieDrawRecipe& recipe, std::string& error, MovieTextureCache* cache)
{
    recipe = {};
    error.clear();
    if (!IsMoviePixelShader(inputs.pixelCode))
        return false;
    try
    {
        require(inputs.inputsComplete, "movie_inputs_incomplete");
        const auto& a = inputs.arguments;
        require(a.caller == kMovieDrawCaller && a.primitive == kMoviePrimitive && a.stride == kMovieStride && a.vertexCount == kMovieVertices &&
                    !a.minimumVertex && !a.indexCount && !(a.vertices & 3U) &&
                    inputs.vertices.size() == std::uint64_t(kMovieVertices) * kMovieStride,
                "movie_unsupported_nonindexed_up_tuple");
        // Guest clipping is off (0x2204 = 0x90000) but the host pipeline clips
        // depth, so only the captured Z of +0.0 is known to rasterize the same.
        for (std::uint32_t i = 0; i < kMovieVertices; ++i)
            require(word(inputs.vertices, i * kMovieStride + 8) == 0, "movie_vertex_depth");
        require(inputs.state.size() == 0x3500 && !(inputs.state[0x2ABC] & 0x40) && !(inputs.state[0x2ABE] & 0x40),
                "movie_conditional_or_recorded_shader");
        require(word(inputs.vertexMetadata, 28) == 2 && inputs.declaration.size() == 24, "movie_fetch_count");
        // The c252..c255 record is the only literal input either stage loads.
        bool literalShape = inputs.literalRanges.size() == 1 && inputs.literalRanges[0].firstFloat4 == kMovieLiteralFirst &&
                            inputs.literalRanges[0].dwordCount == kMovieLiteralCount && inputs.literals.size() == kMovieLiteralCount &&
                            inputs.vertexLiterals.empty() && inputs.pixelLiterals.empty();
        for (std::size_t i = 0; literalShape && i < inputs.literals.size(); ++i)
            literalShape = inputs.literals[i].first == 0x47F0 + i;
        require(literalShape, "movie_literal_shape");
        require(shader.textureFetchSlots == kMovieSlots && shader.screenSpacePosition, "movie_shader_interface");

        // 0x826ADA38 applies linkage before 0x826AD150's fetch patch.
        recipe.patchedVertexCode = inputs.vertexCode;
        if (!FoldSolidShaderMetadata(inputs, recipe.patchedVertexCode, recipe.effectiveState, error))
            return false;
        SolidDrawInputs linked = inputs;
        linked.vertexCode      = recipe.patchedVertexCode;
        if (!PatchVertexFetch(linked, kMovieAttributeOffsets, recipe.patchedVertexCode, error))
            return false;
        require(MovieMicrocodeWordDigest(recipe.patchedVertexCode) == kMovieVertexWordDigest, "movie_patched_vs_identity");

        auto registers = GuestDrawRegisters(recipe.effectiveState);
        for (const auto [index, value] : kMovieFixedState)
            if (index == 0x2202 ? ((registers[index] ^ value) & ~7U) != 0 : registers[index] != value)
            {
                error = std::format("movie_state {:#x}: {:#x}", index, registers[index]);
                return false;
            }
        if (registers[0x2200] != 0x24F00770 && registers[0x2200] != 0x24F00270)
        {
            error = std::format("movie_state {:#x}: {:#x}", 0x2200, registers[0x2200]);
            return false;
        }
        // GuestDrawRegisters covers pixel constants c0..c223 from the shadow;
        // the literal record supplies c252..c255.
        for (const auto [index, value] : inputs.literals)
            registers[index] = value;

        for (std::size_t slot = 0; slot < inputs.planes.size(); ++slot)
        {
            const auto& plane = inputs.planes[slot];
            for (std::size_t i = 0; i < plane.fetch.size(); ++i)
                require(word(inputs.state, kMovieFetchShadows[slot] + i * 4) == plane.fetch[i], "movie_texture_fetch_mismatch");
            if (!DecodeMovieSampler(plane.fetch, recipe.samplers[slot], error))
                return false;
            auto&       texture = recipe.textures[slot];
            std::string textureError;
            if (!(cache ? cache->planes[slot].Decode(plane.fetch, plane.bytes, texture, textureError)
                        : DecodeNativeTextureVersioned(plane.fetch, plane.bytes, {}, texture, textureError)))
            {
                error = "movie_texture_decode: " + textureError;
                return false;
            }
            require(texture.format == NativeDrawReplayTextureFormat::R8 && texture.mipLevels == 1 && texture.width == (slot ? 640U : 1280U) &&
                        texture.height == (slot ? 360U : 720U) && texture.swizzle == std::array<std::uint8_t, 4>{ 4, 4, 4, 0 },
                    "movie_texture_form");
        }

        // The fetch descriptor describes the CPU source span, not the ring copy.
        const auto base   = static_cast<std::uint32_t>(physical(a.vertices));
        registers[0x48BE] = base | 3U;
        registers[0x48BF] = static_cast<std::uint32_t>(inputs.vertices.size()) | 0x10000002U;
        const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, base, inputs.vertices } } };
        const auto                                         vs = microcode(recipe.patchedVertexCode), ps = microcode(inputs.pixelCode);
        // Texture i binds fetch constant i.
        const std::array<NativeGameplayTextureBinding, 3> bindings{ {
            { 0, &recipe.textures[0], &recipe.samplers[0] },
            { 1, &recipe.textures[1], &recipe.samplers[1] },
            { 2, &recipe.textures[2], &recipe.samplers[2] },
        } };
        NativeGameplayDrawView                            view;
        view.registers       = registers;
        view.vertexMicrocode = vs;
        view.pixelMicrocode  = ps;
        view.vertexFetches   = fetches;
        view.textureBindings = bindings;
        view.primitive       = a.primitive;
        view.indexCount      = a.vertexCount;
        view.indexed         = false;
        // The VS maps pixels to NDC itself; the host viewport is the full target.
        NativeGameplayDrawTarget target;
        target.fullViewportTarget = true;
        target.width              = 1280;
        target.height             = 720;
        if (!DecodeNativeGameplayViewportAndScissor(registers, target.viewport, target.scissor, error))
            return false;
        const std::array<float, 6> viewport{ target.viewport.x, target.viewport.y, target.viewport.width, target.viewport.height, target.viewport.minDepth, target.viewport.maxDepth };
        for (unsigned i = 0; i < viewport.size(); ++i)
            require(word(recipe.effectiveState, 0x3168 + i * 4) == std::bit_cast<std::uint32_t>(viewport[i]), "movie_viewport_shadow_scale_mismatch");
        if (!BuildNativeGameplayDrawRecipe(view, shader, target, recipe.native, error))
        {
            error = "movie_recipe_rejected: " + error;
            return false;
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool BuildMovieDraw(const MovieDrawInputs& inputs, const NativeGameplayShaderInterface& shader, NativeDrawReplayRecipe& draw, std::string& detail, MovieTextureCache* cache)
{
    detail.clear();
    if (!inputs.movieCandidate)
        return false;
    MovieDrawRecipe recipe;
    if (!BuildMovieDrawRecipe(inputs, shader, recipe, detail, cache))
    {
        if (detail.empty())
            detail = "movie_recipe_rejected";
        return false;
    }
    draw = std::move(recipe.native);
    return true;
}

NativeDrawFamily MakeMovieDrawFamily(ReadMemory read, NativeDrawFamilyShader shader)
{
    return MakeReaderDrawFamily<MovieDrawInputs>([read = std::move(read)](const NativeDrawSubmission& submission, MovieDrawInputs& inputs, std::string& detail)
                                                 {
                                                     return ReadMovieDrawInputs(submission, read, inputs, detail);
                                                 },
                                                 [shader = std::move(shader), cache = std::make_shared<MovieTextureCache>()](const MovieDrawInputs& inputs, NativeDrawReplayRecipe& draw, std::string& detail)
                                                 {
                                                     return BuildMovieDraw(inputs, *shader, draw, detail, cache.get());
                                                 });
}

NativeDrawFamilyRegistration MovieDrawFamily()
{
    return { "intro movie", "native_draw_movie_shaders", "movie shader catalog", AdmitShaderDrawFamily<LoadMovieDrawShader, MakeMovieDrawFamily> };
}

} // namespace rerevved::gpu::guest_d3d
