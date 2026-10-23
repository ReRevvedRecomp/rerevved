#include "label_draw_adapter.h"

#include "gpu/d3d12/native_texture_upload.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <initializer_list>
#include <limits>
#include <stdexcept>

#include <rex/graphics/xenos.h>
#include <toml++/toml.hpp>

namespace rerevved::gpu::guest_d3d
{
namespace
{

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("label_adapter_input_extent");
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
    require(raw <= UINT32_MAX, "label_vertex_address_overflow");
    return (raw & 0x1FFFFFFFULL) + (((raw >> 20) + 512) & 0x1000);
}

std::vector<std::uint32_t> microcode(std::span<const std::uint8_t> bytes)
{
    require(!bytes.empty() && bytes.size() % 4 == 0, "label_microcode_extent");
    std::vector<std::uint32_t> words;
    for (std::size_t i = 0; i < bytes.size(); i += 4)
        words.push_back(word(bytes, i));
    return words;
}

} // namespace

bool LabelTextureCache::Decode(const std::array<std::uint32_t, 6>& words, std::span<const std::uint8_t> bytes, NativeDrawReplayTexture& texture, std::string& error, std::span<const std::uint8_t> mips)
{
    std::lock_guard lock(mutex);
    if (!valid || words != fetch || !std::ranges::equal(bytes, source) || !std::ranges::equal(mips, mipSource))
    {
        valid = false;
        if (!DecodeNativeTextureVersioned(words, bytes, mips, decoded, error))
            return false;
        fetch = words;
        source.assign(bytes.begin(), bytes.end());
        mipSource.assign(mips.begin(), mips.end());
        valid = true;
    }
    texture = decoded;
    return true;
}

bool LoadLabelDrawShaders(const std::filesystem::path& directory, LabelDrawShaders& shaders, std::string& error)
{
    shaders = {};
    error.clear();
    try
    {
        std::vector<NativeGameplayShaderInterface> catalog;
        if (!LoadNativeGameplayShaders(directory, catalog, error))
            return false;
        std::vector<const NativeGameplayShaderInterface*> presented, offscreen;
        for (const auto& shader : catalog)
        {
            if (shader.vertexShaderHash != kLabelVertexShaderHash || shader.pixelShaderHash != kLabelPixelShaderHash)
                continue;
            require(shader.vertexGuestWordDigest == kLabelVertexWordDigest && shader.pixelGuestWordDigest == kLabelPixelWordDigest,
                    "label_catalog_word_digest");
            require(shader.textureFetchSlots == std::vector<std::uint32_t>{ 0 } &&
                        shader.textureDimensions == std::vector<NativeDrawReplayTextureDimension>{ NativeDrawReplayTextureDimension::Texture2D } &&
                        !shader.alphaTestEnabled && shader.shaderProgram == 0x10110201 && shader.shaderContext == 8 &&
                        shader.shaderInterpolators == 0x10003 && shader.vertexAttributes.size() == kLabelAttributeOffsets.size(),
                    "label_catalog_interface");
            constexpr std::array<std::uint32_t, 3> formats{ 38, 37, 6 };
            for (std::size_t i = 0; i < formats.size(); ++i)
            {
                const auto& attribute = shader.vertexAttributes[i];
                require(attribute.fetchConstant == 95 && attribute.strideBytes == kLabelStride &&
                            attribute.offsetBytes == std::int32_t(kLabelAttributeOffsets[i]) && attribute.format == formats[i] &&
                            attribute.normalized == (formats[i] == 6) && attribute.isSigned == (formats[i] != 6),
                        "label_catalog_layout");
            }
            (shader.sampleCount == 4 ? presented : offscreen).push_back(&shader);
        }
        require(presented.size() == 1 && offscreen.size() <= 1, "label_catalog_entry");
        shaders.presented = *presented.front();
        if (!offscreen.empty())
            shaders.offscreen = *offscreen.front();
        return true;
    }
    catch (const std::exception& exception)
    {
        shaders = {};
        error   = exception.what();
        return false;
    }
}

bool DecodeLabelSampler(const std::array<std::uint32_t, 6>& words, NativeDrawReplaySampler& sampler, std::string& error)
{
    sampler         = {};
    namespace xenos = rex::graphics::xenos;
    xenos::xe_gpu_texture_fetch_t fetch{};
    static_assert(sizeof(fetch) == sizeof(words));
    std::memcpy(&fetch, words.data(), sizeof(fetch));
    if (fetch.mip_max_level)
    {
        error = "label_texture_mip_levels";
        return false;
    }
    // Ported from native_guest_menu_draw.cpp: base level only, mip filter
    // point, linear or base map (all equal with one level), and
    // texture_util's 2D axis normalization.
    if (fetch.type != xenos::FetchConstantType::kTexture || fetch.dimension != xenos::DataDimension::k2DOrStacked ||
        fetch.stacked || fetch.mip_min_level || fetch.mip_address || fetch.packed_mips ||
        static_cast<unsigned>(fetch.mag_filter) > 1 || static_cast<unsigned>(fetch.min_filter) > 1 ||
        static_cast<unsigned>(fetch.mip_filter) > static_cast<unsigned>(xenos::TextureFilter::kBaseMap) ||
        static_cast<unsigned>(fetch.aniso_filter) ||
        static_cast<unsigned>(fetch.border_color))
    {
        error = "label_sampler_unsupported";
        return false;
    }
    constexpr std::array<std::uint32_t, 8> addressModes{ 1, 2, 3, 5, 3, 5, 4, 5 };
    sampler.address   = { addressModes[static_cast<unsigned>(fetch.clamp_x)], addressModes[static_cast<unsigned>(fetch.clamp_y)], 3 };
    sampler.magLinear = fetch.mag_filter == xenos::TextureFilter::kLinear;
    sampler.minLinear = fetch.min_filter == xenos::TextureFilter::kLinear;
    sampler.mipLinear = fetch.mip_filter == xenos::TextureFilter::kLinear;
    sampler.maxLod    = std::numeric_limits<float>::max();
    return true;
}

bool LoadLabelDrawShader(const std::filesystem::path& directory, NativeGameplayShaderInterface& shader, std::string& error)
{
    LabelDrawShaders shaders;
    if (!LoadLabelDrawShaders(directory, shaders, error))
    {
        shader = {};
        return false;
    }
    shader = std::move(shaders.presented);
    return true;
}

bool BuildLabelDrawRecipe(const LabelDrawInputs& inputs, const LabelDrawShaders& shaders, LabelDrawRecipe& recipe, std::string& error, LabelTextureCache* cache)
{
    recipe = {};
    error.clear();
    if (!IsLabelPixelShader(inputs.pixelCode))
        return false;
    try
    {
        require(inputs.inputsComplete, "label_inputs_incomplete");
        const auto& a = inputs.arguments;
        require(a.caller == kLabelDrawCaller && a.primitive == 4 && a.stride == kLabelStride && a.vertexCount && a.vertexCount % 3 == 0 &&
                    !a.minimumVertex && !a.indexCount && !(a.vertices & 3U) &&
                    inputs.vertices.size() == std::uint64_t(a.vertexCount) * kLabelStride,
                "label_unsupported_nonindexed_up_tuple");
        require(inputs.state.size() == 0x3500 && !(inputs.state[0x2ABC] & 0x40) && !(inputs.state[0x2ABE] & 0x40),
                "label_conditional_or_recorded_shader");
        require(word(inputs.vertexMetadata, 28) == 3 && inputs.declaration.size() == 36, "label_fetch_count");
        for (const auto* literals : { &inputs.vertexLiterals, &inputs.pixelLiterals })
            require(literals->size() <= 20 || (word(*literals, 20) & 0xFFFF) == 0, "label_shader_resource_literals");

        // 0x826ADA38 applies linkage before 0x826AD150's fetch patch.
        recipe.patchedVertexCode = inputs.vertexCode;
        if (!FoldSolidShaderMetadata(inputs, recipe.patchedVertexCode, recipe.effectiveState, error))
            return false;
        SolidDrawInputs linked = inputs;
        linked.vertexCode      = recipe.patchedVertexCode;
        if (!PatchVertexFetch(linked, kLabelAttributeOffsets, recipe.patchedVertexCode, error))
            return false;
        require(LabelMicrocodeWordDigest(recipe.patchedVertexCode) == kLabelVertexWordDigest, "label_patched_vs_identity");

        auto        registers = GuestDrawRegisters(recipe.effectiveState);
        const auto* form      = registers[0x2000] == kLabelPresentedForm.surfaceInfo   ? &kLabelPresentedForm
                                : registers[0x2000] == kLabelOffscreenForm.surfaceInfo ? &kLabelOffscreenForm
                                                                                       : nullptr;
        for (const auto [index, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{
                 { 0x2000, form ? form->surfaceInfo : kLabelPresentedForm.surfaceInfo }, { 0x2002, form ? form->depthInfo : kLabelPresentedForm.depthInfo } })
            if (registers[index] != value)
            {
                error = std::format("label_fixed_state {:#x}: {:#x}", index, registers[index]);
                return false;
            }
        for (const auto [index, value] : kLabelFixedState)
            if (registers[index] != value)
            {
                error = std::format("label_fixed_state {:#x}: {:#x}", index, registers[index]);
                return false;
            }
        // A label under a Scaleform mask tests stencil equal to the mask
        // depth, on the presented form only.
        const bool masked = registers[0x2200] == kLabelMaskedDepthControl && form == &kLabelPresentedForm;
        if ((registers[0x2200] != 0x24F00770 && registers[0x2200] != 0x24F00270 && !masked) ||
            (registers[0x2202] != 0x87000004 && registers[0x2202] != 0x87000007))
        {
            error = std::format("label_fixed_state depth {:#x} color {:#x}", registers[0x2200], registers[0x2202]);
            return false;
        }
        if (masked && (registers[0x210D] & ~0xFFU) != kLabelStencilMasks)
        {
            error = std::format("label_stencil_mask {:#x}: {:#x}", 0x210D, registers[0x210D]);
            return false;
        }

        // Texture slot 0 is G+0x480, the fetch the reader sized the bytes by.
        for (std::size_t i = 0; i < inputs.textureFetch.size(); ++i)
            require(word(inputs.state, 0x480 + i * 4) == inputs.textureFetch[i], "label_texture_fetch_mismatch");
        if (!DecodeLabelSampler(inputs.textureFetch, recipe.sampler, error))
            return false;
        std::string textureError;
        if (!(cache ? cache->Decode(inputs.textureFetch, inputs.textureBytes, recipe.texture, textureError)
                    : DecodeNativeTextureVersioned(inputs.textureFetch, inputs.textureBytes, {}, recipe.texture, textureError)))
        {
            error = "label_texture_decode: " + textureError;
            return false;
        }
        require(recipe.texture.format == NativeDrawReplayTextureFormat::R8 && recipe.texture.mipLevels == 1 &&
                    recipe.texture.swizzle == std::array<std::uint8_t, 4>{ 4, 4, 4, 0 },
                "label_texture_form");

        // The fetch descriptor describes the CPU source span, not the ring copy.
        const auto base   = static_cast<std::uint32_t>(physical(a.vertices));
        registers[0x48BE] = base | 3U;
        registers[0x48BF] = static_cast<std::uint32_t>(inputs.vertices.size()) | 0x10000002U;
        const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, base, inputs.vertices } } };
        const auto                                         vs = microcode(recipe.patchedVertexCode), ps = microcode(inputs.pixelCode);
        const std::array<NativeGameplayTextureBinding, 1>  bindings{ { { 0, &recipe.texture, &recipe.sampler } } };
        NativeGameplayDrawView                             view;
        view.registers       = registers;
        view.vertexMicrocode = vs;
        view.pixelMicrocode  = ps;
        view.vertexFetches   = fetches;
        view.textureBindings = bindings;
        view.primitive       = a.primitive;
        view.indexCount      = a.vertexCount;
        view.indexed         = false;
        // The presented form's display rectangle, as for GFx; the off-screen
        // form draws its whole target at one sample.
        NativeGameplayDrawTarget             target;
        const NativeGameplayShaderInterface* shader = &shaders.presented;
        if (form == &kLabelPresentedForm)
        {
            target.fullViewportTarget = true;
            if (!DecodeNativeOverlayDisplayRectangle(registers, "label", target, error))
                return false;
        }
        else
        {
            target.width       = form->width;
            target.height      = form->height;
            target.sampleCount = 1;
            target.sampleMask  = 1;
            if (!DecodeNativeGameplayViewportAndScissor(registers, target.viewport, target.scissor, error))
                return false;
            const auto& v = target.viewport;
            if (registers[0x2081] || registers[0x2082] != form->scissor || v.x != 0 || v.y != 0 || v.width != float(form->width) ||
                v.height != float(form->height))
            {
                error = std::format("label_offscreen_viewport: scissor {:#x} {:#x} viewport {},{} {}x{}", registers[0x2081], registers[0x2082], v.x, v.y, v.width, v.height);
                return false;
            }
            require(shaders.offscreen.has_value(), "label_offscreen_unpinned");
            shader = &*shaders.offscreen;
        }
        const std::array<float, 6> viewport{ target.viewport.x, target.viewport.y, target.viewport.width, target.viewport.height, target.viewport.minDepth, target.viewport.maxDepth };
        for (unsigned i = 0; i < viewport.size(); ++i)
            require(word(recipe.effectiveState, 0x3168 + i * 4) == std::bit_cast<std::uint32_t>(viewport[i]), "label_viewport_shadow_scale_mismatch");
        if (!BuildNativeGameplayDrawRecipe(view, *shader, target, recipe.native, error))
        {
            error = "label_recipe_rejected: " + error;
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

bool BuildLabelDraw(const LabelDrawInputs& inputs, const LabelDrawShaders& shaders, NativeDrawReplayRecipe& draw, std::string& detail, LabelTextureCache* cache)
{
    detail.clear();
    if (!inputs.labelCandidate)
        return false;
    LabelDrawRecipe recipe;
    if (!BuildLabelDrawRecipe(inputs, shaders, recipe, detail, cache))
    {
        if (detail.empty())
            detail = "label_recipe_rejected";
        return false;
    }
    draw = std::move(recipe.native);
    return true;
}

NativeDrawFamily MakeLabelDrawFamily(ReadMemory read, std::shared_ptr<const LabelDrawShaders> shaders)
{
    return MakeReaderDrawFamily<LabelDrawInputs>([read = std::move(read)](const NativeDrawSubmission& submission, LabelDrawInputs& inputs, std::string& detail)
                                                 {
                                                     return ReadLabelDrawInputs(submission, read, inputs, detail);
                                                 },
                                                 [shaders = std::move(shaders), cache = std::make_shared<LabelTextureCache>()](const LabelDrawInputs& inputs, NativeDrawReplayRecipe& draw, std::string& detail)
                                                 {
                                                     return BuildLabelDraw(inputs, *shaders, draw, detail, cache.get());
                                                 });
}

NativeDrawFamilyRegistration LabelDrawFamily()
{
    return { "label", "native_draw_label_shaders", "label shader catalog", [](const std::filesystem::path& source, ReadMemory read, NativeDrawFamily& family, std::string& summary, std::string& error)
             {
                 auto shaders = std::make_shared<LabelDrawShaders>();
                 if (!LoadLabelDrawShaders(source, *shaders, error))
                     return false;
                 if (shaders->offscreen)
                     summary = "(with the 256x32 off-screen form)";
                 family = MakeLabelDrawFamily(std::move(read), std::move(shaders));
                 return true;
             } };
}

} // namespace rerevved::gpu::guest_d3d
