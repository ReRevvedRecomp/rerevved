#include "gfx_draw_adapter.h"

#include "gpu/d3d12/native_texture_upload.h"
#include "leader_draw_adapter.h"

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
        throw std::runtime_error("gfx_adapter_input_extent");
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
    require(raw <= UINT32_MAX, "gfx_vertex_address_overflow");
    return (raw & 0x1FFFFFFFULL) + (((raw >> 20) + 512) & 0x1000);
}

std::vector<std::uint32_t> microcode(std::span<const std::uint8_t> bytes)
{
    require(!bytes.empty() && bytes.size() % 4 == 0, "gfx_microcode_extent");
    std::vector<std::uint32_t> words;
    for (std::size_t i = 0; i < bytes.size(); i += 4)
        words.push_back(word(bytes, i));
    return words;
}

std::string unpinned(std::span<const std::uint8_t> pixelCode, std::span<const std::uint8_t> vertexCode, std::uint32_t stride)
{
    const auto digest = [](std::span<const std::uint8_t> code)
    {
        return code.empty() || code.size() % 4 ? std::string("unreadable") : GfxMicrocodeWordDigest(code);
    };
    return std::format("gfx_shader_unpinned ps={} vs={} stride={}", digest(pixelCode), digest(vertexCode), stride);
}

template <std::size_t N>
bool admitted(const std::array<std::uint32_t, N>& values, std::uint32_t value)
{
    return std::ranges::find(values, value) != values.end();
}

} // namespace

bool LoadGfxDrawCatalog(const std::filesystem::path& directory, GfxDrawCatalog& catalog, std::string& error)
{
    catalog = {};
    error.clear();
    try
    {
        std::vector<NativeGameplayShaderInterface> shaders;
        if (!LoadNativeGameplayShaders(directory, shaders, error))
            return false;
        std::optional<NativeGameplayShaderInterface> offscreen;
        for (auto& shader : shaders)
        {
            const auto form = std::ranges::find_if(kGfxPairs, [&](const GfxPairForm& pair)
                                                   {
                                                       return pair.vertexHash == shader.vertexShaderHash && pair.pixelHash == shader.pixelShaderHash;
                                                   });
            if (form == kGfxPairs.end())
                continue;
            require(shader.vertexGuestWordDigest == form->vertexWords && shader.pixelGuestWordDigest == form->pixelWords, "gfx_catalog_word_digest");
            std::vector<std::uint32_t> slots(form->textures);
            for (std::uint32_t i = 0; i < form->textures; ++i)
                slots[i] = i;
            // Only the loading pair has an off-screen form, at one sample.
            const bool single = form == kGfxPairs.begin() && shader.sampleCount == 1 && shader.sampleMask == 1;
            require(shader.textureFetchSlots == slots &&
                        std::ranges::all_of(shader.textureDimensions, [](auto dimension)
                                            {
                                                return dimension == NativeDrawReplayTextureDimension::Texture2D;
                                            }) &&
                        !shader.alphaTestEnabled && !shader.screenSpacePosition && !shader.generatedPixelParameters && shader.colorTargetCount == 1 && shader.colorTargetFormat == 0 && (single || (shader.sampleCount == 4 && shader.sampleMask == 9)),
                    "gfx_catalog_interface");
            // XY16i position (k_16_16 integer), then normalized k_8_8_8_8 color
            // words up to the stride: GFx formats XY16i, XY16iC32 and XY16iCF32
            // (declarations 0x82F6E7AC, 0x82F6E7F4 and 0x82F6E818).
            const auto stride = form->stride;
            require(shader.vertexAttributes.size() == stride / 4, "gfx_catalog_layout");
            GfxDrawShader entry;
            for (std::uint32_t i = 0; i < stride / 4; ++i)
            {
                const auto& attribute = shader.vertexAttributes[i];
                const bool  position  = i == 0;
                require(attribute.fetchConstant == 95 && attribute.strideBytes == stride && attribute.offsetBytes == std::int32_t(i * 4) &&
                            attribute.format == (position ? 25U : 6U) && attribute.normalized == !position && attribute.isSigned == position,
                        "gfx_catalog_layout");
                entry.attributeOffsets.push_back(i * 4);
            }
            if (single)
            {
                require(!offscreen, "gfx_catalog_duplicate_pixel");
                offscreen = std::move(shader);
                continue;
            }
            require(!catalog.Find(shader.pixelGuestWordDigest), "gfx_catalog_duplicate_pixel");
            entry.stride   = stride;
            entry.textures = form->textures;
            entry.shader   = std::move(shader);
            catalog.entries.push_back(std::move(entry));
        }
        require(!catalog.entries.empty(), "gfx_catalog_entry");
        if (offscreen)
        {
            const auto loading = std::ranges::find_if(catalog.entries, [](const GfxDrawShader& entry)
                                                      {
                                                          return entry.shader.pixelGuestWordDigest == kGfxPixelWordDigest;
                                                      });
            require(loading != catalog.entries.end(), "gfx_catalog_entry");
            loading->offscreen = std::move(offscreen);
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        catalog = {};
        error   = exception.what();
        return false;
    }
}

bool DecodeGfxSampler(const std::array<std::uint32_t, 6>& words, NativeDrawReplaySampler& sampler, std::string& error)
{
    error.clear();
    // The label sampler normalization admits exactly one 2D level.
    if (!DecodeLabelSampler(words, sampler, error))
    {
        error = error == "label_texture_mip_levels" ? "gfx_texture_mipmapped" : "gfx_sampler_unsupported";
        return false;
    }
    namespace xenos = rex::graphics::xenos;
    xenos::xe_gpu_texture_fetch_t fetch{};
    static_assert(sizeof(fetch) == sizeof(words));
    std::memcpy(&fetch, words.data(), sizeof(fetch));
    const auto refuse = [&](const char* code)
    {
        sampler = {};
        error   = code;
        return false;
    };
    if (fetch.border_size)
        return refuse("gfx_sampler_border_size");
    if (fetch.nearest_clamp_policy)
        return refuse("gfx_sampler_nearest_clamp_policy");
    // Only repeat, mirror, clamp-to-edge and mirror-clamp-to-edge map exactly:
    // the label mapping approximates the halfway modes by their edge forms,
    // and the border modes need a border alpha the guest does not define.
    for (const auto clamp : { fetch.clamp_x, fetch.clamp_y })
        if (static_cast<unsigned>(clamp) > static_cast<unsigned>(xenos::ClampMode::kMirrorClampToEdge))
            return refuse("gfx_sampler_clamp_mode");
    // With one level and equal filters the LOD never changes the result. With
    // unequal filters it selects the magnification or minification filter.
    if (fetch.mag_filter != fetch.min_filter)
    {
        if (fetch.lod_bias)
            return refuse("gfx_sampler_lod_bias");
        if (fetch.grad_exp_adjust_h || fetch.grad_exp_adjust_v)
            return refuse("gfx_sampler_gradient_adjust");
    }
    return true;
}

bool DecodeGfxTexture(const std::array<std::uint32_t, 6>& fetch, std::span<const std::uint8_t> base, std::span<const std::uint8_t> mips, NativeDrawReplayTexture& texture, NativeDrawReplaySampler& sampler, std::string& error, LabelTextureCache* cache)
{
    texture = {};
    sampler = {};
    error.clear();
    const auto refuse = [&](std::string code)
    {
        texture = {};
        sampler = {};
        error   = std::move(code);
        return false;
    };
    using Format = NativeDrawReplayTextureFormat;
    std::string textureError;
    if (mips.empty())
    {
        if (!DecodeGfxSampler(fetch, sampler, error))
            return false;
        if (!(cache ? cache->Decode(fetch, base, texture, textureError) : DecodeNativeTextureVersioned(fetch, base, {}, texture, textureError)))
            return refuse("gfx_texture_decode: " + textureError);
        // The accepted M2 forms, one 2D level: the sampler refuses a mip chain.
        const auto format = texture.format;
        if (!(format == Format::Rgba8 || format == Format::Bc1 || format == Format::Bc2 || format == Format::Bc3) || texture.mipLevels != 1 ||
            texture.dimension != NativeDrawReplayTextureDimension::Texture2D)
            return refuse(std::format("gfx_texture_form {}", static_cast<unsigned>(format)));
        return true;
    }

    // The loading backgrounds: a linear DXT1 with packed mips, sampled with
    // the computed LOD, so every level the fetch names is uploaded.
    namespace xenos = rex::graphics::xenos;
    xenos::xe_gpu_texture_fetch_t words{};
    static_assert(sizeof(words) == sizeof(fetch));
    std::memcpy(&words, fetch.data(), sizeof(words));
    if (words.format != xenos::TextureFormat::k_DXT1 || words.tiled || !words.packed_mips || words.mag_filter != words.min_filter)
        return refuse("gfx_texture_mipmapped");
    // The SDK scales the computed gradients by exp2(fetch LOD bias / 32)
    // before sample_d; with equal filters a host mip bias is the same LOD.
    auto sampled = fetch;
    sampled[4] &= ~(0x3FFU << 12);
    if (!DecodeLeaderSampler(sampled, NativeDrawReplayTextureDimension::Texture2D, sampler, error, kLeaderAnisotropicOverride))
        return refuse(error.starts_with("leader_") ? "gfx_" + error.substr(7) : error);
    sampler.mipBias = float(words.lod_bias) / 32.0F;
    if (!(cache ? cache->Decode(fetch, base, texture, textureError, mips) : DecodeNativeTextureVersioned(fetch, base, mips, texture, textureError)))
        return refuse("gfx_texture_decode: " + textureError);
    if (texture.format != Format::Bc1 || texture.mipLevels != words.mip_max_level + 1U || texture.dimension != NativeDrawReplayTextureDimension::Texture2D)
        return refuse(std::format("gfx_texture_form {}", static_cast<unsigned>(texture.format)));
    return true;
}

bool BuildGfxDrawRecipe(const GfxDrawInputs& inputs, const GfxDrawCatalog& catalog, GfxDrawRecipe& recipe, std::string& error, LabelTextureCache* cache)
{
    recipe = {};
    error.clear();
    try
    {
        const auto* entry = inputs.pixelCode.empty() || inputs.pixelCode.size() % 4 ? nullptr : catalog.Find(GfxMicrocodeWordDigest(inputs.pixelCode));
        if (!entry)
        {
            error = unpinned(inputs.pixelCode, inputs.vertexCode, inputs.arguments.stride);
            return false;
        }
        require(inputs.inputsComplete, "gfx_inputs_incomplete");
        require(inputs.state.size() == 0x3500 && !(inputs.state[0x2ABC] & 0x40) && !(inputs.state[0x2ABE] & 0x40),
                "gfx_conditional_or_recorded_shader");
        require(word(inputs.vertexMetadata, 28) == entry->attributeOffsets.size() && inputs.declaration.size() == entry->attributeOffsets.size() * 12,
                "gfx_fetch_count");
        for (const auto* literals : { &inputs.vertexLiterals, &inputs.pixelLiterals })
            require(literals->size() <= 20 || (word(*literals, 20) & 0xFFFF) == 0, "gfx_shader_resource_literals");

        // 0x826ADA38 applies linkage before 0x826AD150's fetch patch.
        std::vector<std::uint8_t> code = inputs.vertexCode, state, patched;
        if (!FoldSolidShaderMetadata(inputs, code, state, error))
            return false;
        SolidDrawInputs linked = inputs;
        linked.vertexCode      = code;
        if (!PatchVertexFetch(linked, entry->attributeOffsets, patched, error))
            return false;
        return BuildGfxEffectiveDrawRecipe(inputs, *entry, std::move(patched), std::move(state), recipe, error, cache);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool BuildGfxEffectiveDrawRecipe(const GfxDrawInputs& inputs, const GfxDrawShader& entry, std::vector<std::uint8_t> vertexCode, std::vector<std::uint8_t> effectiveState, GfxDrawRecipe& recipe, std::string& error, LabelTextureCache* cache)
{
    recipe = {};
    error.clear();
    try
    {
        const auto& a = inputs.arguments;
        require(a.caller == kGfxDrawCaller && a.primitive == 4 && a.stride == entry.stride && !a.minimumVertex && a.indexFormat == 1 &&
                    a.vertexCount && a.indexCount && a.indexCount % 3 == 0 && !(a.vertices & 3U) &&
                    inputs.vertices.size() == std::uint64_t(a.vertexCount) * entry.stride &&
                    inputs.indices.size() == std::uint64_t(a.indexCount) * 2,
                "gfx_unsupported_indexed_up_tuple");
        require(GfxMicrocodeWordDigest(inputs.pixelCode) == entry.shader.pixelGuestWordDigest, "gfx_shader_unpinned");
        recipe.patchedVertexCode = std::move(vertexCode);
        recipe.effectiveState    = std::move(effectiveState);
        require(recipe.effectiveState.size() == 0x3500, "gfx_adapter_input_extent");
        require(GfxMicrocodeWordDigest(recipe.patchedVertexCode) == entry.shader.vertexGuestWordDigest, "gfx_patched_vs_identity");

        auto       registers = GuestDrawRegisters(recipe.effectiveState);
        const auto refuse    = [&](std::string_view field, std::uint32_t index)
        {
            error = std::format("gfx_{} {:#x}: {:#x}", field, index, registers[index]);
            return false;
        };
        // The loading pair also draws the off-screen panels; any other
        // surface or depth keeps the presented form's refusal.
        const auto& form = entry.shader.pixelGuestWordDigest == kGfxPixelWordDigest && registers[0x2000] == kGfxOffscreenForm.surfaceInfo ? kGfxOffscreenForm
                                                                                                                                          : kGfxPresentedForm;
        for (const auto [index, value] : kGfxFixedState)
            if (registers[index] != (index == 0x2000 ? form.surfaceInfo : index == 0x2002 ? form.depthInfo
                                                                                          : value))
                return refuse("fixed_state", index);
        // The catalog has no 1X loading entry yet; name that before the
        // panel's own textures and viewport are decoded.
        if (&form == &kGfxOffscreenForm && !entry.offscreen)
        {
            error = "gfx_offscreen_unpinned";
            return false;
        }
        // The folded program words name the catalog entry's specialization.
        const auto& shader = &form == &kGfxOffscreenForm ? *entry.offscreen : entry.shader;
        if (registers[0x2180] != shader.shaderProgram)
            return refuse("program_state", 0x2180);
        if (registers[0x2181] != shader.shaderContext)
            return refuse("program_state", 0x2181);
        if (registers[0x2182] != shader.shaderInterpolators)
            return refuse("program_state", 0x2182);
        if (admitted(kGfxAlphaCompositeBlendControls, registers[0x2201]))
            return refuse("blend_alpha_composite", 0x2201);
        if (!admitted(kGfxBlendControls, registers[0x2201]))
            return refuse("blend_control", 0x2201);
        if (!admitted(kGfxColorMasks, registers[0x2104]))
            return refuse("color_mask", 0x2104);
        if (!admitted(kGfxDepthControls, registers[0x2200]))
            return refuse("depth_control", 0x2200);
        if (!admitted(kGfxColorControls, registers[0x2202]))
            return refuse("color_control", 0x2202);
        if ((registers[0x2200] & 1U) && (registers[0x210D] & ~0xFFU) != kGfxStencilMasks)
            return refuse("stencil_mask", 0x210D);

        std::vector<NativeGameplayTextureBinding> bindings;
        const auto                                bind = [&](std::uint32_t slot, const std::array<std::uint32_t, 6>& fetch, const GuestBytes& bytes, const GuestBytes& mips, NativeDrawReplayTexture& texture, NativeDrawReplaySampler& sampler)
        {
            // Texture slot n is G+0x480 + 24n, the fetch the reader sized the bytes by.
            for (std::size_t i = 0; i < fetch.size(); ++i)
                require(word(recipe.effectiveState, 0x480 + (slot * 6 + i) * 4) == fetch[i], "gfx_texture_fetch_mismatch");
            if (!DecodeGfxTexture(fetch, bytes, mips, texture, sampler, error, cache))
                return false;
            bindings.push_back({ slot, &texture, &sampler });
            return true;
        };
        if (entry.textures > 0 && !bind(0, inputs.textureFetch, inputs.textureBytes, inputs.textureMipBytes, recipe.texture, recipe.sampler))
            return false;
        if (entry.textures > 1 &&
            !bind(1, inputs.secondTextureFetch, inputs.secondTextureBytes, inputs.secondTextureMipBytes, recipe.secondTexture, recipe.secondSampler))
            return false;

        // The fetch descriptor describes the CPU source span, not the ring copy.
        const auto base   = static_cast<std::uint32_t>(physical(a.vertices));
        registers[0x48BE] = base | 3U;
        registers[0x48BF] = static_cast<std::uint32_t>(inputs.vertices.size()) | 0x10000002U;
        const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, base, inputs.vertices } } };
        const auto                                         vs = microcode(recipe.patchedVertexCode), ps = microcode(inputs.pixelCode);
        NativeGameplayDrawView                             view;
        view.registers       = registers;
        view.vertexMicrocode = vs;
        view.pixelMicrocode  = ps;
        view.vertexFetches   = fetches;
        view.textureBindings = bindings;
        view.indexBytes      = inputs.indices;
        view.primitive       = a.primitive;
        view.indexCount      = a.indexCount;
        view.indexFormat     = 0;
        // The movie's display rectangle on the presented form; the off-screen
        // form's viewport and window scissor are its 900x675 panel at 1X.
        NativeGameplayDrawTarget target;
        if (&form == &kGfxPresentedForm)
        {
            target.fullViewportTarget = true;
            if (!DecodeNativeOverlayDisplayRectangle(registers, "gfx", target, error))
                return false;
        }
        else
        {
            target.width       = form.width;
            target.height      = form.height;
            target.sampleCount = 1;
            target.sampleMask  = 1;
            if (!DecodeNativeGameplayViewportAndScissor(registers, target.viewport, target.scissor, error))
                return false;
            const auto& v = target.viewport;
            if (registers[0x2081] || registers[0x2082] != form.scissor || v.x != 0 || v.y != 0 || v.width != float(form.viewportWidth) ||
                v.height != float(form.viewportHeight))
            {
                error = std::format("gfx_offscreen_viewport: scissor {:#x} {:#x} viewport {},{} {}x{}", registers[0x2081], registers[0x2082], v.x, v.y, v.width, v.height);
                return false;
            }
        }
        const std::array<float, 6> viewport{ target.viewport.x, target.viewport.y, target.viewport.width, target.viewport.height, target.viewport.minDepth, target.viewport.maxDepth };
        for (unsigned i = 0; i < viewport.size(); ++i)
            require(word(recipe.effectiveState, 0x3168 + i * 4) == std::bit_cast<std::uint32_t>(viewport[i]), "gfx_viewport_shadow_scale_mismatch");
        if (!BuildNativeGameplayDrawRecipe(view, shader, target, recipe.native, error))
        {
            error = "gfx_recipe_rejected: " + error;
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

bool BuildGfxDraw(const GfxDrawInputs& inputs, const GfxDrawCatalog& catalog, NativeDrawReplayRecipe& draw, std::string& detail, LabelTextureCache* cache)
{
    detail.clear();
    if (!inputs.gfxCandidate)
        return false;
    GfxDrawRecipe recipe;
    if (!BuildGfxDrawRecipe(inputs, catalog, recipe, detail, cache))
    {
        if (detail.empty())
            detail = "gfx_recipe_rejected";
        return false;
    }
    draw = std::move(recipe.native);
    return true;
}

NativeDrawFamily MakeGfxDrawFamily(ReadMemory read, std::shared_ptr<const GfxDrawCatalog> catalog)
{
    return MakeReaderDrawFamily<GfxDrawInputs>([read = std::move(read), catalog](const NativeDrawSubmission& submission, GfxDrawInputs& inputs, std::string& detail)
                                               {
                                                   return ReadGfxDrawInputs(submission, read, *catalog, inputs, detail);
                                               },
                                               [catalog, cache = std::make_shared<LabelTextureCache>()](const GfxDrawInputs& inputs, NativeDrawReplayRecipe& draw, std::string& detail)
                                               {
                                                   return BuildGfxDraw(inputs, *catalog, draw, detail, cache.get());
                                               });
}

NativeDrawFamilyRegistration GfxDrawFamily()
{
    return { "GFx", "native_draw_gfx_shaders", "GFx shader catalog", [](const std::filesystem::path& source, ReadMemory read, NativeDrawFamily& family, std::string& summary, std::string& error)
             {
                 auto catalog = std::make_shared<GfxDrawCatalog>();
                 if (!LoadGfxDrawCatalog(source, *catalog, error))
                     return false;
                 summary = std::format("({} pairs)", catalog->entries.size());
                 family  = MakeGfxDrawFamily(std::move(read), std::move(catalog));
                 return true;
             } };
}

} // namespace rerevved::gpu::guest_d3d
