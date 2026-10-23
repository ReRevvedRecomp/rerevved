#include "scene_mesh_draw_adapter.h"

#include "gfx_draw_adapter.h"
#include "gpu/d3d12/native_texture_upload.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <stdexcept>

#include <rex/graphics/xenos.h>

namespace rerevved::gpu::guest_d3d
{
namespace
{

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("scene_mesh_input_extent");
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

void require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

std::vector<std::uint32_t> microcode(std::span<const std::uint8_t> bytes)
{
    require(!bytes.empty() && bytes.size() % 4 == 0, "scene_mesh_microcode_extent");
    std::vector<std::uint32_t> words;
    for (std::size_t i = 0; i < bytes.size(); i += 4)
        words.push_back(word(bytes, i));
    return words;
}

bool isSceneMeshEntry(const NativeGameplayShaderInterface& entry)
{
    return entry.vertexShaderHash == kSceneMeshVertexShaderHash && entry.pixelShaderHash == kSceneMeshPixelShaderHash;
}

bool isSceneMenuEntry(const NativeGameplayShaderInterface& entry)
{
    return entry.vertexShaderHash == kSceneMenuVertexShaderHash && entry.pixelShaderHash == kSceneMenuPixelShaderHash;
}

} // namespace

bool IsSceneMeshPixelShader(std::span<const std::uint8_t> pixelCode)
{
    return !pixelCode.empty() && pixelCode.size() % 4 == 0 && IndexedBufferMicrocodeWordDigest(pixelCode) == kSceneMeshPixelWordDigest;
}

bool LoadSceneMeshDrawShader(const std::filesystem::path& directory, NativeGameplayShaderInterface& shader, std::string& error)
{
    shader = {};
    error.clear();
    try
    {
        std::vector<NativeGameplayShaderInterface> catalog;
        if (!LoadNativeGameplayShaders(directory, catalog, error))
            return false;
        require(std::ranges::count_if(catalog, isSceneMeshEntry) == 1, "scene_mesh_catalog_entry");
        shader = *std::ranges::find_if(catalog, isSceneMeshEntry);
        require(shader.vertexGuestWordDigest == kSceneMeshVertexWordDigest && shader.pixelGuestWordDigest == kSceneMeshPixelWordDigest,
                "scene_mesh_catalog_word_digest");
        require(shader.textureFetchSlots.empty() && !shader.alphaTestEnabled && shader.shaderProgram == 0x40010001 && shader.shaderContext == 4 &&
                    shader.shaderInterpolators == 0x10001 && shader.colorTargetFormat == 0 && shader.colorExpBias == 0 &&
                    shader.vertexAttributes.size() == kSceneMeshAttributeOffsets.size(),
                "scene_mesh_catalog_interface");
        // RT1 is RB_COLOR1_INFO's k_2_10_10_10 target.
        require(shader.colorTargetCount == 2 && shader.color1Format == 2, "scene_mesh_catalog_color_targets");
        // k_32_32_32_FLOAT position and k_32_32_FLOAT UV, both signed floats.
        constexpr std::array<std::uint32_t, 2> formats{ 57, 37 };
        for (std::size_t i = 0; i < formats.size(); ++i)
        {
            const auto& attribute = shader.vertexAttributes[i];
            require(attribute.fetchConstant == 95 && attribute.strideBytes == kSceneMeshStride &&
                        attribute.offsetBytes == std::int32_t(kSceneMeshAttributeOffsets[i]) && attribute.format == formats[i] && attribute.isSigned &&
                        !attribute.normalized,
                    "scene_mesh_catalog_layout");
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

bool BuildSceneMeshDrawRecipe(const IndexedBufferDrawInputs& inputs, const NativeGameplayShaderInterface& shader, SceneMeshDrawRecipe& recipe, std::string& error)
{
    recipe = {};
    error.clear();
    if (!IsSceneMeshPixelShader(inputs.pixelCode))
        return false;
    try
    {
        require(inputs.inputsComplete, "scene_mesh_inputs_incomplete");
        // One indexed triangle list through 16-bit big-endian indices at base
        // vertex 0: VGT_INDX_OFFSET (0x2102) is pinned to zero below.
        require(inputs.arguments.caller == kSceneMeshDrawCaller && inputs.primitive == 4 && !inputs.baseVertex && inputs.indexCount &&
                    inputs.indexCount % 3 == 0 && inputs.indexFormat == 0 && inputs.indexEndian == 1 &&
                    indexBytes(inputs).size() == std::uint64_t(inputs.indexCount) * 2,
                "scene_mesh_unsupported_draw_tuple");
        require(inputs.state.size() == 0x3500 && !(inputs.state[0x2ABC] & 0x40) && !(inputs.state[0x2ABE] & 0x40),
                "scene_mesh_conditional_or_recorded_shader");
        require(word(inputs.vertexMetadata, 28) == 2 && inputs.declaration.size() == 24, "scene_mesh_fetch_count");
        require(inputs.streams == std::vector<std::uint32_t>{ 0 } && inputs.streamFetches.size() == 1 && inputs.streamBytes.size() == 1 &&
                    inputs.arguments.stride == kSceneMeshStride,
                "scene_mesh_stream_form");
        for (const auto* literals : { &inputs.vertexLiterals, &inputs.pixelLiterals })
            require(literals->size() <= 20 || (word(*literals, 20) & 0xFFFF) == 0, "scene_mesh_shader_resource_literals");

        // 0x826ADD88 applies linkage before the fetch patch, as the UP routes do.
        recipe.patchedVertexCode = inputs.vertexCode;
        if (!FoldSolidShaderMetadata(inputs, recipe.patchedVertexCode, recipe.effectiveState, error))
            return false;
        SolidDrawInputs linked = inputs;
        linked.vertexCode      = recipe.patchedVertexCode;
        if (!PatchVertexFetch(linked, kSceneMeshAttributeOffsets, recipe.patchedVertexCode, error))
            return false;
        require(IndexedBufferMicrocodeWordDigest(recipe.patchedVertexCode) == kSceneMeshVertexWordDigest, "scene_mesh_patched_vs_identity");

        auto registers = GuestDrawRegisters(recipe.effectiveState);
        for (const auto [index, value] : kSceneMeshFixedState)
            if (registers[index] != value)
            {
                error = std::format("scene_mesh_fixed_state {:#x}: {:#x}", index, registers[index]);
                return false;
            }
        require(shader.colorTargetCount == 2 && ((registers[0x2003] >> 16) & 0xF) == shader.color1Format && shader.color1Format == 2,
                "scene_mesh_color_targets");

        // Vertex fetch 95 is stream 0's guest descriptor and span.
        const auto& fetch = inputs.streamFetches[0];
        registers[0x48BE] = fetch[0];
        registers[0x48BF] = fetch[1];
        // Scene mesh binds RT0 and checks the two-tile pitch; the assembler
        // owns the full 1280x720 three-tile pitch.
        const auto colorMask = registers[0x2104];
        registers[0x2000]    = (registers[0x2000] & ~0x3FFFU) | 640;
        registers[0x2104]    = colorMask & 0xF;
        const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, fetch[0] & ~3U, inputs.streamBytes[0] } } };
        const auto                                         vs = microcode(recipe.patchedVertexCode), ps = microcode(inputs.pixelCode);
        NativeGameplayDrawView                             view;
        view.registers       = registers;
        view.vertexMicrocode = vs;
        view.pixelMicrocode  = ps;
        view.vertexFetches   = fetches;
        view.indexBytes      = indexBytes(inputs);
        view.primitive       = inputs.primitive;
        view.indexCount      = inputs.indexCount;
        view.indexFormat     = 0;
        NativeGameplayDrawTarget target;
        target.fullViewportTarget = true;
        target.width              = 1280;
        target.height             = 720;
        if (!DecodeNativeGameplayViewportAndScissor(registers, target.viewport, target.scissor, error))
            return false;
        const std::array<float, 6> viewport{ target.viewport.x, target.viewport.y, target.viewport.width, target.viewport.height, target.viewport.minDepth, target.viewport.maxDepth };
        for (unsigned i = 0; i < viewport.size(); ++i)
            require(word(recipe.effectiveState, 0x3168 + i * 4) == std::bit_cast<std::uint32_t>(viewport[i]), "scene_mesh_viewport_shadow_scale_mismatch");
        if (!BuildNativeGameplayDrawRecipe(view, shader, target, recipe.native, error))
        {
            error = "scene_mesh_recipe_rejected: " + error;
            return false;
        }
        // RB_BLENDCONTROL1 (0x2209) is pinned equal to RB_BLENDCONTROL0, so
        // RT1 blends as RT0 under its own RB_COLOR_MASK nibble.
        recipe.native.extraTargetCount                = 1;
        recipe.native.extraTargets[0].format          = NativeDrawReplayTargetFormat::Rgb10a2;
        recipe.native.extraTargets[0].blend           = recipe.native.blend;
        recipe.native.extraTargets[0].blend.writeMask = static_cast<std::uint8_t>((colorMask >> 4) & 0xF);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool BuildSceneMeshDraw(const IndexedBufferDrawInputs& inputs, const NativeGameplayShaderInterface& shader, NativeDrawReplayRecipe& draw, std::string& detail)
{
    detail.clear();
    if (!inputs.sceneCandidate)
        return false;
    SceneMeshDrawRecipe recipe;
    if (!BuildSceneMeshDrawRecipe(inputs, shader, recipe, detail))
    {
        if (detail.empty())
            detail = "scene_mesh_recipe_rejected";
        return false;
    }
    draw = std::move(recipe.native);
    return true;
}

NativeDrawFamily MakeSceneMeshDrawFamily(ReadMemory read, NativeDrawFamilyShader shader)
{
    return MakeReaderDrawFamily<IndexedBufferDrawInputs>([read = std::move(read)](const NativeDrawSubmission& submission, IndexedBufferDrawInputs& inputs, std::string& detail)
                                                         {
                                                             return ReadIndexedBufferDrawInputs(submission, read, kSceneMeshPixelWordDigest, inputs, detail);
                                                         },
                                                         [shader = std::move(shader)](const IndexedBufferDrawInputs& inputs, NativeDrawReplayRecipe& draw, std::string& detail)
                                                         {
                                                             return BuildSceneMeshDraw(inputs, *shader, draw, detail);
                                                         });
}

NativeDrawFamilyRegistration SceneMeshDrawFamily()
{
    return { "MRT prepass mesh", "native_draw_mesh_shaders", "MRT prepass shader catalog", AdmitShaderDrawFamily<LoadSceneMeshDrawShader, MakeSceneMeshDrawFamily> };
}

namespace
{

void admitSceneMenuShader(const std::vector<NativeGameplayShaderInterface>& catalog, NativeGameplayShaderInterface& shader)
{
    require(std::ranges::count_if(catalog, isSceneMenuEntry) == 1, "scene_mesh_menu_catalog_entry");
    shader = *std::ranges::find_if(catalog, isSceneMenuEntry);
    require(shader.vertexGuestWordDigest == kSceneMenuVertexWordDigest && shader.pixelGuestWordDigest == kSceneMenuPixelWordDigest,
            "scene_mesh_menu_catalog_word_digest");
    // RB_COLORCONTROL 0x8700000C enables the alpha test with Greater (4).
    require(shader.textureFetchSlots == std::vector<std::uint32_t>{ 0 } &&
                shader.textureDimensions == std::vector<NativeDrawReplayTextureDimension>{ NativeDrawReplayTextureDimension::Texture2D } &&
                shader.alphaTestEnabled && shader.alphaTestCompare == 4 && !shader.screenSpacePosition && shader.shaderProgram == 0x10210204 &&
                shader.shaderContext == 8 && shader.shaderInterpolators == 0x10007 && shader.colorTargetFormat == 0 && shader.colorExpBias == 0 &&
                shader.vertexAttributes.size() == kSceneMenuAttributeOffsets.size(),
            "scene_mesh_menu_catalog_interface");
    require(shader.colorTargetCount == 1, "scene_mesh_menu_catalog_color_targets");
    // k_32_32_32_FLOAT, k_32_32_FLOAT and k_32_32_32_FLOAT, all signed floats.
    constexpr std::array<std::uint32_t, 3> formats{ 57, 37, 57 };
    for (std::size_t i = 0; i < formats.size(); ++i)
    {
        const auto& attribute = shader.vertexAttributes[i];
        require(attribute.fetchConstant == 95 && attribute.strideBytes == kSceneMenuStride &&
                    attribute.offsetBytes == std::int32_t(kSceneMenuAttributeOffsets[i]) && attribute.format == formats[i] && attribute.isSigned &&
                    !attribute.normalized,
                "scene_mesh_menu_catalog_layout");
    }
}

} // namespace

bool LoadSceneMenuDrawShader(const std::filesystem::path& directory, NativeGameplayShaderInterface& shader, std::string& error)
{
    shader = {};
    error.clear();
    try
    {
        std::vector<NativeGameplayShaderInterface> catalog;
        if (!LoadNativeGameplayShaders(directory, catalog, error))
            return false;
        admitSceneMenuShader(catalog, shader);
        return true;
    }
    catch (const std::exception& exception)
    {
        shader = {};
        error  = exception.what();
        return false;
    }
}

bool LoadSceneMenuDrawCatalog(const std::filesystem::path& directory, SceneMenuDrawCatalog& catalog, std::string& error)
{
    catalog = {};
    error.clear();
    try
    {
        std::vector<NativeGameplayShaderInterface> shaders;
        if (!LoadNativeGameplayShaders(directory, shaders, error))
            return false;
        auto backdrop = std::make_shared<NativeGameplayShaderInterface>();
        admitSceneMenuShader(shaders, *backdrop);
        std::erase_if(shaders, isSceneMenuEntry);
        // The backdrop reader claims its PS words before the band is tried.
        require(std::ranges::none_of(shaders, [&](const NativeGameplayShaderInterface& shader)
                                     {
                                         return shader.pixelGuestWordDigest == backdrop->pixelGuestWordDigest;
                                     }),
                "scene_mesh_menu_catalog_duplicate_pixel");
        auto band = std::make_shared<LeaderDrawCatalog>();
        if (!AdmitLeaderDrawShaders(std::move(shaders), kSceneMeshDrawCaller, *band, error))
            return false;
        catalog = { std::move(backdrop), std::move(band) };
        return true;
    }
    catch (const std::exception& exception)
    {
        catalog = {};
        error   = exception.what();
        return false;
    }
}

bool DecodeSceneMenuSampler(const std::array<std::uint32_t, 6>& fetch, NativeDrawReplaySampler& sampler, std::string& error)
{
    // With one level and equal filters, a LOD bias or gradient adjust cannot
    // change the result; the GFx rules apply unchanged.
    if (DecodeGfxSampler(fetch, sampler, error))
        return true;
    if (error.starts_with("gfx_"))
        error = "scene_mesh_" + error.substr(4);
    return false;
}

bool BuildSceneMenuDrawRecipe(const IndexedBufferDrawInputs& inputs, const NativeGameplayShaderInterface& shader, SceneMenuDrawRecipe& recipe, std::string& error, LabelTextureCache* cache, const SceneMeshIdentity& identity)
{
    recipe = {};
    error.clear();
    if (inputs.pixelCode.empty() || inputs.pixelCode.size() % 4 || IndexedBufferMicrocodeWordDigest(inputs.pixelCode) != identity.pixelWordDigest)
        return false;
    try
    {
        require(inputs.inputsComplete, "scene_mesh_inputs_incomplete");
        // The capture contract's form: an indexed triangle list through 16-bit
        // big-endian indices at base vertex 0 (VGT_INDX_OFFSET is pinned below).
        require(inputs.arguments.caller == kSceneMeshDrawCaller && inputs.primitive == 4 && !inputs.baseVertex && inputs.indexCount &&
                    inputs.indexCount % 3 == 0 && inputs.indexFormat == 0 && inputs.indexEndian == 1 &&
                    indexBytes(inputs).size() == std::uint64_t(inputs.indexCount) * 2,
                "scene_mesh_unsupported_draw_tuple");
        require(inputs.state.size() == 0x3500 && !(inputs.state[0x2ABC] & 0x40) && !(inputs.state[0x2ABE] & 0x40),
                "scene_mesh_conditional_or_recorded_shader");
        require(word(inputs.vertexMetadata, 28) == 3 && inputs.declaration.size() == 36, "scene_mesh_fetch_count");
        require(inputs.streams == std::vector<std::uint32_t>{ 0 } && inputs.streamFetches.size() == 1 && inputs.streamBytes.size() == 1 &&
                    inputs.arguments.stride == kSceneMenuStride,
                "scene_mesh_stream_form");
        bool literalShape = inputs.vertexLiteralRanges.size() == 1 && inputs.vertexLiteralRanges[0].firstFloat4 == kSceneMenuLiteralFirst &&
                            inputs.vertexLiteralRanges[0].dwordCount == kSceneMenuLiteralCount &&
                            inputs.vertexLiteralRegisters.size() == kSceneMenuLiteralCount && inputs.pixelLiteralRanges.empty();
        for (std::size_t i = 0; literalShape && i < inputs.vertexLiteralRegisters.size(); ++i)
            literalShape = inputs.vertexLiteralRegisters[i].first == 0x4000 + kSceneMenuLiteralFirst * 4 + i;
        require(literalShape, "scene_mesh_literal_shape");
        require(shader.colorTargetCount == 1, "scene_mesh_color_targets");

        // 0x826ADD88 applies linkage before the fetch patch, as the UP routes do.
        recipe.patchedVertexCode = inputs.vertexCode;
        if (!FoldSolidShaderMetadata(inputs, recipe.patchedVertexCode, recipe.effectiveState, error))
            return false;
        SolidDrawInputs linked = inputs;
        linked.vertexCode      = recipe.patchedVertexCode;
        if (!PatchVertexFetch(linked, kSceneMenuAttributeOffsets, recipe.patchedVertexCode, error))
            return false;
        require(IndexedBufferMicrocodeWordDigest(recipe.patchedVertexCode) == identity.vertexWordDigest, "scene_mesh_patched_vs_identity");
        // The live microcode decides the layout and binding; the entry must agree.
        const auto layout = MatchShaderFetches(recipe.patchedVertexCode, inputs.pixelCode, shader);
        require(layout != ShaderFetchMatch::VertexLayout, "scene_mesh_vertex_fetch_layout");
        require(layout != ShaderFetchMatch::TextureLayout, "scene_mesh_texture_fetch_layout");

        auto registers = GuestDrawRegisters(recipe.effectiveState);
        for (const auto [index, value] : kSceneMenuFixedState)
            if (registers[index] != value)
            {
                error = std::format("scene_mesh_fixed_state {:#x}: {:#x}", index, registers[index]);
                return false;
            }
        // The shadow lacks c252..c255; the literal record supplies them.
        for (const auto [index, value] : inputs.vertexLiteralRegisters)
            registers[index] = value;

        for (std::size_t i = 0; i < inputs.textureFetch.size(); ++i)
            require(word(inputs.state, 0x480 + i * 4) == inputs.textureFetch[i], "scene_mesh_texture_fetch_mismatch");
        if (!DecodeSceneMenuSampler(inputs.textureFetch, recipe.sampler, error))
            return false;
        std::string textureError;
        if (!(cache ? cache->Decode(inputs.textureFetch, inputs.textureBytes, recipe.texture, textureError)
                    : DecodeNativeTextureVersioned(inputs.textureFetch, inputs.textureBytes, {}, recipe.texture, textureError)))
        {
            error = "scene_mesh_texture_decode: " + textureError;
            return false;
        }
        // The sampler already admits only one unpacked 2D level.
        require(std::ranges::any_of(kSceneMenuTextureForms, [&](const SceneMenuTextureForm& form)
                                    {
                                        return recipe.texture.format == form.format && recipe.texture.width == form.width && recipe.texture.height == form.height;
                                    }),
                "scene_mesh_texture_form");

        // Vertex fetch 95 is stream 0's guest descriptor and span.
        const auto& fetch = inputs.streamFetches[0];
        registers[0x48BE] = fetch[0];
        registers[0x48BF] = fetch[1];
        const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, fetch[0] & ~3U, inputs.streamBytes[0] } } };
        const auto                                         vs = microcode(recipe.patchedVertexCode), ps = microcode(inputs.pixelCode);
        const std::array<NativeGameplayTextureBinding, 1>  bindings{ { { 0, &recipe.texture, &recipe.sampler } } };
        NativeGameplayDrawView                             view;
        view.registers       = registers;
        view.vertexMicrocode = vs;
        view.pixelMicrocode  = ps;
        view.vertexFetches   = fetches;
        view.textureBindings = bindings;
        view.indexBytes      = indexBytes(inputs);
        view.primitive       = inputs.primitive;
        view.indexCount      = inputs.indexCount;
        view.indexFormat     = 0;
        NativeGameplayDrawTarget target;
        target.fullViewportTarget = true;
        target.width              = 1280;
        target.height             = 720;
        if (!DecodeNativeGameplayViewportAndScissor(registers, target.viewport, target.scissor, error))
            return false;
        const std::array<float, 6> viewport{ target.viewport.x, target.viewport.y, target.viewport.width, target.viewport.height, target.viewport.minDepth, target.viewport.maxDepth };
        for (unsigned i = 0; i < viewport.size(); ++i)
            require(word(recipe.effectiveState, 0x3168 + i * 4) == std::bit_cast<std::uint32_t>(viewport[i]), "scene_mesh_viewport_shadow_scale_mismatch");
        if (!BuildNativeGameplayDrawRecipe(view, shader, target, recipe.native, error))
        {
            error = "scene_mesh_recipe_rejected: " + error;
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

bool BuildSceneMenuDraw(const IndexedBufferDrawInputs& inputs, const NativeGameplayShaderInterface& shader, NativeDrawReplayRecipe& draw, std::string& detail, LabelTextureCache* cache, const SceneMeshIdentity& identity)
{
    detail.clear();
    if (!inputs.sceneCandidate)
        return false;
    SceneMenuDrawRecipe recipe;
    if (!BuildSceneMenuDrawRecipe(inputs, shader, recipe, detail, cache, identity))
    {
        if (detail.empty())
            detail = "scene_mesh_recipe_rejected";
        return false;
    }
    draw = std::move(recipe.native);
    return true;
}

NativeDrawFamily MakeSceneMenuDrawFamilyFor(ReadMemory read, NativeDrawFamilyShader shader, SceneMeshIdentity identity)
{
    // The two scene draws alternate between the BC1 and BC2 textures.
    auto caches = std::make_shared<std::array<LabelTextureCache, 2>>();
    return MakeReaderDrawFamily<IndexedBufferDrawInputs>([read = std::move(read), identity](const NativeDrawSubmission& submission, IndexedBufferDrawInputs& inputs, std::string& detail)
                                                         {
                                                             return ReadIndexedBufferDrawInputs(submission, read, identity.pixelWordDigest, inputs, detail, IndexedBufferForm::Menu);
                                                         },
                                                         [shader = std::move(shader), caches, identity](const IndexedBufferDrawInputs& inputs, NativeDrawReplayRecipe& draw, std::string& detail)
                                                         {
                                                             namespace xenos = rex::graphics::xenos;
                                                             xenos::xe_gpu_texture_fetch_t fetch{};
                                                             static_assert(sizeof(fetch) == sizeof(inputs.textureFetch));
                                                             std::memcpy(&fetch, inputs.textureFetch.data(), sizeof(fetch));
                                                             auto& cache = (*caches)[fetch.format == xenos::TextureFormat::k_DXT1 ? 0 : 1];
                                                             return BuildSceneMenuDraw(inputs, *shader, draw, detail, &cache, identity);
                                                         });
}

NativeDrawFamily MakeSceneMenuDrawFamily(ReadMemory read, NativeDrawFamilyShader shader)
{
    return MakeSceneMenuDrawFamilyFor(std::move(read), std::move(shader), kSceneMenuIdentity);
}

NativeDrawFamily MakeSceneMenuCatalogFamily(ReadMemory read, SceneMenuDrawCatalog catalog, SceneMeshIdentity identity)
{
    auto backdrop = MakeSceneMenuDrawFamilyFor(read, std::move(catalog.backdrop), identity);
    if (!catalog.band || catalog.band->entries.empty())
        return backdrop;
    return [backdrop = std::move(backdrop), band = MakeLeaderDrawFamily(std::move(read), std::move(catalog.band))](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
    {
        const auto result = backdrop(submission, draw, detail);
        return result == NativeDrawFamilyResult::NotMine ? band(submission, draw, detail) : result;
    };
}

NativeDrawFamilyRegistration SceneMenuDrawFamily()
{
    return { "title/menu backdrop", "native_draw_menu_shaders", "menu shader catalog", [](const std::filesystem::path& source, ReadMemory read, NativeDrawFamily& family, std::string& summary, std::string& error)
             {
                 SceneMenuDrawCatalog catalog;
                 if (!LoadSceneMenuDrawCatalog(source, catalog, error))
                     return false;
                 summary = std::format("(backdrop and {} leader-band pairs)", catalog.band->entries.size());
                 family  = MakeSceneMenuCatalogFamily(std::move(read), std::move(catalog));
                 return true;
             } };
}

NativeFrameDrawBuilder MakeSceneMenuDrawBuilder(ReadMemory read, std::shared_ptr<const NativeGameplayShaderInterface> shader, SceneMeshIdentity identity)
{
    if (!shader)
        return {};
    return [family = MakeSceneMenuDrawFamilyFor(std::move(read), std::move(shader), identity)](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
    {
        detail.clear();
        return family(submission, draw, detail) == NativeDrawFamilyResult::Built;
    };
}

} // namespace rerevved::gpu::guest_d3d
