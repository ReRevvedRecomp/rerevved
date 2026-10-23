#include "leader_draw_adapter.h"

#include "gpu/d3d12/native_texture_upload.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>
#include <tuple>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>

namespace rerevved::gpu::guest_d3d
{
namespace
{

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("leader_input_extent");
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
    require(!bytes.empty() && bytes.size() % 4 == 0, "leader_microcode_extent");
    std::vector<std::uint32_t> words;
    for (std::size_t i = 0; i < bytes.size(); i += 4)
        words.push_back(word(bytes, i));
    return words;
}

// Every literal register inside the stage's 256 float4 constants.
bool inStage(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& registers, bool pixel)
{
    const auto first = kShaderLiteralRegisterBase + (pixel ? kShaderLiteralPixelFirst * 4 : 0);
    return std::ranges::all_of(registers, [&](const auto& entry)
                               {
                                   return entry.first >= first && entry.first < first + 1024;
                               });
}

} // namespace

bool LeaderTextureCache::Decode(const std::array<std::uint32_t, 6>& words, const GuestBytes& base, const GuestBytes& mips, NativeDrawReplayTexture& texture, std::string& error)
{
    std::lock_guard lock(mutex);
    ++clock;
    const auto same = [](const GuestBytes& kept, const GuestBytes& read)
    {
        return kept.SharesBuffer(read) || kept == read;
    };
    const auto key           = XXH3_64bits(words.data(), sizeof(words));
    const auto [first, last] = index.equal_range(key);
    for (auto found = first; found != last; ++found)
        if (auto& entry = entries[found->second]; entry.fetch == words && same(entry.base, base) && same(entry.mips, mips))
        {
            entry.used = clock;
            // Equal bytes in another retained buffer, as after the read mirror
            // refreshed the range: keep the newer one so its next read shares.
            // Move-assigned, so a copied read's buffer is freed, not kept beside it.
            if (base.IsShared() && !entry.base.SharesBuffer(base))
                entry.base = GuestBytes(base);
            if (mips.IsShared() && !entry.mips.SharesBuffer(mips))
                entry.mips = GuestBytes(mips);
            texture = entry.decoded;
            return true;
        }
    Entry entry;
    if (!DecodeNativeTextureVersioned(words, base, mips, entry.decoded, error))
        return false;
    entry.fetch = words;
    entry.base  = base;
    entry.mips  = mips;
    entry.used  = clock;
    texture     = entry.decoded;
    // The version shares the decoded payload.
    entry.bytes = base.size() + mips.size() + entry.decoded.payloadBytes().size();
    if (entry.bytes > budget)
        return true;
    bool evicted = false;
    while (retained + entry.bytes > budget)
    {
        const auto oldest = std::ranges::min_element(entries, {}, &Entry::used);
        retained -= oldest->bytes;
        entries.erase(oldest);
        evicted = true;
    }
    // An eviction moves later entries down, so their positions are rebuilt.
    if (evicted)
    {
        index.clear();
        for (std::size_t position = 0; position < entries.size(); ++position)
            index.emplace(XXH3_64bits(entries[position].fetch.data(), sizeof(entries[position].fetch)), position);
    }
    index.emplace(key, entries.size());
    retained += entry.bytes;
    entries.push_back(std::move(entry));
    return true;
}

std::pair<std::size_t, std::size_t> LeaderTextureCache::Retained()
{
    std::lock_guard lock(mutex);
    return { retained, entries.size() };
}

const LeaderDrawShader* LeaderDrawCatalog::Find(std::string_view pixelWordDigest) const
{
    const auto found = std::ranges::find_if(entries, [&](const LeaderDrawShader& entry)
                                            {
                                                return entry.shader.pixelGuestWordDigest == pixelWordDigest;
                                            });
    return found == entries.end() ? nullptr : &*found;
}

std::vector<IndexedBufferPixelShader> LeaderPixelShaders(const LeaderDrawCatalog& catalog)
{
    std::vector<IndexedBufferPixelShader> shaders;
    for (const auto& entry : catalog.entries)
        shaders.push_back({ entry.shader.pixelGuestWordDigest, static_cast<std::uint32_t>(entry.shader.textureFetchSlots.size()) });
    return shaders;
}

bool LoadLeaderDrawCatalog(const std::filesystem::path& directory, LeaderDrawCatalog& catalog, std::string& error)
{
    catalog = {};
    error.clear();
    std::vector<NativeGameplayShaderInterface> shaders;
    if (!LoadNativeGameplayShaders(directory, shaders, error))
        return false;
    return AdmitLeaderDrawShaders(std::move(shaders), kLeaderDrawCaller, catalog, error);
}

bool AdmitLeaderDrawShaders(std::vector<NativeGameplayShaderInterface> shaders, std::uint32_t caller, LeaderDrawCatalog& catalog, std::string& error)
{
    catalog        = {};
    catalog.caller = caller;
    error.clear();
    try
    {
        for (auto& shader : shaders)
        {
            // The shared loader bounds the slots and admits 2D and cube dimensions.
            for (std::uint32_t i = 0; i < shader.textureFetchSlots.size(); ++i)
                require(shader.textureFetchSlots[i] == i, "leader_catalog_textures");
            // RB_COLORCONTROL 0x8700000C enables the alpha test with Greater (4).
            require(shader.alphaTestEnabled && shader.alphaTestCompare == 4 && !shader.screenSpacePosition && shader.colorTargetFormat == 0 && shader.colorExpBias == 0,
                    "leader_catalog_interface");
            require(shader.colorTargetCount == 1, "leader_catalog_color_targets");
            const auto& attributes = shader.vertexAttributes;
            require(!attributes.empty(), "leader_catalog_layout");
            LeaderDrawShader entry;
            entry.stride = attributes[0].strideBytes;
            require(entry.stride && entry.stride % 4 == 0 && entry.stride / 4 <= 0xFF, "leader_catalog_layout");
            for (const auto& attribute : attributes)
            {
                // k_32_32_FLOAT (37) and k_32_32_32_FLOAT (57) as signed floats,
                // or k_8_8_8_8 (6) as unsigned normalized bytes.
                const bool          bytes = attribute.format == 6;
                const std::uint32_t size  = attribute.format == 57 ? 12U : (attribute.format == 37 ? 8U : (bytes ? 4U : 0U));
                require(size && attribute.fetchConstant == 95 && attribute.strideBytes == entry.stride && attribute.offsetBytes >= 0 && attribute.offsetBytes % 4 == 0 &&
                            std::uint32_t(attribute.offsetBytes) + size <= entry.stride && attribute.isSigned == !bytes && attribute.normalized == bytes,
                        "leader_catalog_layout");
                entry.attributeOffsets.push_back(std::uint32_t(attribute.offsetBytes));
            }
            require(!catalog.Find(shader.pixelGuestWordDigest), "leader_catalog_duplicate_pixel");
            entry.shader = std::move(shader);
            catalog.entries.push_back(std::move(entry));
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

bool DecodeLeaderSampler(const std::array<std::uint32_t, 6>& words, NativeDrawReplayTextureDimension dimension, NativeDrawReplaySampler& sampler, std::string& error, std::int32_t anisotropicOverride)
{
    sampler         = {};
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
    const auto expected = dimension == NativeDrawReplayTextureDimension::TextureCube ? xenos::DataDimension::kCube : xenos::DataDimension::k2DOrStacked;
    if (fetch.type != xenos::FetchConstantType::kTexture || fetch.dimension != expected || fetch.stacked)
        return refuse("leader_sampler_dimension");
    if (static_cast<unsigned>(fetch.mag_filter) > 1 || static_cast<unsigned>(fetch.min_filter) > 1)
        return refuse("leader_sampler_filter");
    if (static_cast<unsigned>(fetch.mip_filter) > static_cast<unsigned>(xenos::TextureFilter::kBaseMap))
        return refuse("leader_sampler_mip_filter");
    if (static_cast<unsigned>(fetch.aniso_filter))
        return refuse("leader_sampler_anisotropy");
    if (fetch.lod_bias)
        return refuse("leader_sampler_lod_bias");
    if (fetch.grad_exp_adjust_h || fetch.grad_exp_adjust_v)
        return refuse("leader_sampler_gradient_adjust");
    if (fetch.border_size)
        return refuse("leader_sampler_border_size");
    if (fetch.nearest_clamp_policy)
        return refuse("leader_sampler_nearest_clamp_policy");
    // As for GFx: only the modes without a halfway point or border map exactly.
    // Cube sampling ignores the address modes.
    for (const auto clamp : { fetch.clamp_x, fetch.clamp_y })
        if (static_cast<unsigned>(clamp) > static_cast<unsigned>(xenos::ClampMode::kMirrorClampToEdge))
            return refuse("leader_sampler_clamp_mode");
    if (fetch.mip_min_level)
        return refuse("leader_sampler_min_level");
    constexpr std::array<std::uint32_t, 4> addressModes{ 1, 2, 3, 5 };
    sampler.address   = { addressModes[static_cast<unsigned>(fetch.clamp_x)], addressModes[static_cast<unsigned>(fetch.clamp_y)], 3 };
    sampler.magLinear = fetch.mag_filter == xenos::TextureFilter::kLinear;
    sampler.minLinear = fetch.min_filter == xenos::TextureFilter::kLinear;
    sampler.mipLinear = fetch.mip_filter == xenos::TextureFilter::kLinear;
    // The decoded texture holds levels 0..mip_max_level; base map keeps level 0.
    // TriJuice (tri_clamp) narrows Xenos trilinear blending; the host blends
    // across each whole level interval.
    sampler.maxLod = fetch.mip_filter == xenos::TextureFilter::kBaseMap ? 0.0F : std::numeric_limits<float>::max();
    // The SDK texture cache forces anisotropic_override on mipped fetches with
    // linear min and mag filters and a point or linear mip filter.
    std::uint32_t minLevel = 0, maxLevel = 0;
    rex::graphics::texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr, &minLevel, &maxLevel);
    if (anisotropicOverride > 0 && anisotropicOverride <= 5 && maxLevel > minLevel && sampler.magLinear && sampler.minLinear &&
        fetch.mip_filter != xenos::TextureFilter::kBaseMap)
    {
        sampler.anisotropy = static_cast<std::uint8_t>(anisotropicOverride);
        sampler.mipLinear  = true;
    }
    return true;
}

bool BuildLeaderDrawRecipe(const IndexedBufferDrawInputs& inputs, const LeaderDrawCatalog& catalog, LeaderDrawRecipe& recipe, std::string& error, LeaderTextureCache* cache)
{
    recipe = {};
    error.clear();
    if (inputs.pixelCode.empty() || inputs.pixelCode.size() % 4)
        return false;
    const auto* entry = catalog.Find(IndexedBufferMicrocodeWordDigest(inputs.pixelCode));
    if (!entry)
        return false;
    const auto& shader   = entry->shader;
    const auto& textures = shader.textureDimensions;
    try
    {
        require(inputs.inputsComplete && inputs.textureFetches.size() == textures.size() && inputs.textureBaseBytes.size() == textures.size() &&
                    inputs.textureMipBytes.size() == textures.size(),
                "leader_inputs_incomplete");
        // An indexed triangle list through 16-bit big-endian indices at base
        // vertex 0 (VGT_INDX_OFFSET is pinned below).
        require(inputs.arguments.caller == catalog.caller && inputs.primitive == 4 && !inputs.baseVertex && inputs.indexCount && inputs.indexCount % 3 == 0 &&
                    inputs.indexFormat == 0 && inputs.indexEndian == 1 && indexBytes(inputs).size() == std::uint64_t(inputs.indexCount) * 2,
                "leader_unsupported_draw_tuple");
        require(inputs.state.size() == 0x3500 && !(inputs.state[0x2ABC] & 0x40) && !(inputs.state[0x2ABE] & 0x40), "leader_conditional_or_recorded_shader");
        // One fetch record per translated attribute; the declaration may hold
        // elements the VS does not fetch.
        require(word(inputs.vertexMetadata, 28) == entry->attributeOffsets.size(), "leader_fetch_count");
        require(inputs.streams == std::vector<std::uint32_t>{ 0 } && inputs.streamFetches.size() == 1 && inputs.streamBytes.size() == 1 &&
                    inputs.arguments.stride == entry->stride,
                "leader_stream_form");
        require(inStage(inputs.vertexLiteralRegisters, false) && inStage(inputs.pixelLiteralRegisters, true), "leader_literal_stage");

        // 0x826ADD88 applies linkage before the fetch patch.
        recipe.patchedVertexCode = inputs.vertexCode;
        if (!FoldSolidShaderMetadata(inputs, recipe.patchedVertexCode, recipe.effectiveState, error))
            return false;
        SolidDrawInputs linked = inputs;
        linked.vertexCode      = recipe.patchedVertexCode;
        if (!PatchVertexFetch(linked, entry->attributeOffsets, recipe.patchedVertexCode, error))
            return false;
        require(IndexedBufferMicrocodeWordDigest(recipe.patchedVertexCode) == shader.vertexGuestWordDigest, "leader_patched_vs_identity");
        // The live microcode decides the layout and bindings; the entry must agree.
        const auto layout = MatchShaderFetches(recipe.patchedVertexCode, inputs.pixelCode, shader);
        require(layout != ShaderFetchMatch::VertexLayout, "leader_vertex_fetch_layout");
        require(layout != ShaderFetchMatch::TextureLayout, "leader_texture_fetch_layout");

        auto                                                         registers = GuestDrawRegisters(recipe.effectiveState);
        const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> program{ { { 0x2180, shader.shaderProgram }, { 0x2181, shader.shaderContext }, { 0x2182, shader.shaderInterpolators } } };
        for (const auto& pinned : { std::span<const std::pair<std::uint32_t, std::uint32_t>>(kLeaderFixedState), std::span<const std::pair<std::uint32_t, std::uint32_t>>(program) })
            for (const auto [index, value] : pinned)
                if (registers[index] != value)
                {
                    error = std::format("leader_fixed_state {:#x}: {:#x}", index, registers[index]);
                    return false;
                }
        if (std::ranges::none_of(kLeaderTargetForms,
                                 [&](const LeaderTargetForm& form)
                                 {
                                     return registers[0x2082] == form.scissorBottomRight && registers[0x2111] == form.viewportYScale && registers[0x2112] == form.viewportYOffset;
                                 }))
        {
            error = std::format("leader_target_form: {:#x} {:#x} {:#x}", registers[0x2082], registers[0x2111], registers[0x2112]);
            return false;
        }
        for (const auto& [index, forms, code] : { std::tuple{ 0x2200U, std::span<const std::uint32_t>(kLeaderDepthControls), "leader_depth_control" },
                                                  std::tuple{ 0x2201U, std::span<const std::uint32_t>(kLeaderBlendControls), "leader_blend_control" },
                                                  std::tuple{ 0x2205U, std::span<const std::uint32_t>(kLeaderRasterControls), "leader_raster_control" } })
            if (std::ranges::find(forms, registers[index]) == forms.end())
            {
                error = std::format("{}: {:#x}", code, registers[index]);
                return false;
            }
        // The shadow lacks the literal records; 0x826ADB98 loads them in order.
        for (const auto* literals : { &inputs.vertexLiteralRegisters, &inputs.pixelLiteralRegisters })
            for (const auto [index, value] : *literals)
                registers[index] = value;

        recipe.textures.resize(textures.size());
        recipe.samplers.resize(textures.size());
        std::vector<NativeGameplayTextureBinding> bindings;
        for (std::uint32_t slot = 0; slot < textures.size(); ++slot)
        {
            const auto& fetch = inputs.textureFetches[slot];
            for (std::size_t i = 0; i < fetch.size(); ++i)
                require(word(inputs.state, 0x480 + slot * 24 + i * 4) == fetch[i], "leader_texture_fetch_mismatch");
            if (!DecodeLeaderSampler(fetch, textures[slot], recipe.samplers[slot], error, kLeaderAnisotropicOverride))
                return false;
            const auto& base = inputs.textureBaseBytes[slot];
            const auto& mips = inputs.textureMipBytes[slot];
            std::string textureError;
            if (slot < inputs.textureResolved.size() && inputs.textureResolved[slot])
                recipe.textures[slot] = BindNativeResolvedTexture(*inputs.textureResolved[slot]);
            else if (!(cache ? cache->Decode(fetch, base, mips, recipe.textures[slot], textureError)
                             : DecodeNativeTextureVersioned(fetch, base, mips, recipe.textures[slot], textureError)))
            {
                error = "leader_texture_decode: " + textureError;
                return false;
            }
            require(recipe.textures[slot].dimension == textures[slot], "leader_texture_dimension");
            bindings.push_back({ slot, &recipe.textures[slot], &recipe.samplers[slot] });
        }

        // Vertex fetch 95 is stream 0's guest descriptor and span.
        const auto& stream = inputs.streamFetches[0];
        registers[0x48BE]  = stream[0];
        registers[0x48BF]  = stream[1];
        const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, stream[0] & ~3U, inputs.streamBytes[0] } } };
        const auto                                         vs = microcode(recipe.patchedVertexCode), ps = microcode(inputs.pixelCode);
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
            require(word(recipe.effectiveState, 0x3168 + i * 4) == std::bit_cast<std::uint32_t>(viewport[i]), "leader_viewport_shadow_scale_mismatch");
        if (!BuildNativeGameplayDrawRecipe(view, shader, target, recipe.native, error))
        {
            error = "leader_recipe_rejected: " + error;
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

bool BuildLeaderDraw(const IndexedBufferDrawInputs& inputs, const LeaderDrawCatalog& catalog, NativeDrawReplayRecipe& draw, std::string& detail, LeaderTextureCache* cache)
{
    detail.clear();
    if (!inputs.sceneCandidate)
        return false;
    LeaderDrawRecipe recipe;
    if (!BuildLeaderDrawRecipe(inputs, catalog, recipe, detail, cache))
    {
        if (detail.empty())
            detail = "leader_recipe_rejected";
        return false;
    }
    draw = std::move(recipe.native);
    return true;
}

NativeDrawFamily MakeLeaderDrawFamily(ReadMemory read, std::shared_ptr<const LeaderDrawCatalog> catalog)
{
    auto cache = std::make_shared<LeaderTextureCache>();
    // The reader's digest views borrow from the catalog it holds.
    const auto form = catalog->caller == kSceneMeshDrawCaller ? IndexedBufferForm::SceneLeader : IndexedBufferForm::Leader;
    return MakeReaderDrawFamily<IndexedBufferDrawInputs>([read = std::move(read), catalog, form, shaders = LeaderPixelShaders(*catalog)](const NativeDrawSubmission& submission, IndexedBufferDrawInputs& inputs, std::string& detail)
                                                         {
                                                             return ReadIndexedBufferDrawInputs(submission, read, shaders, inputs, detail, form);
                                                         },
                                                         [catalog, cache](const IndexedBufferDrawInputs& inputs, NativeDrawReplayRecipe& draw, std::string& detail)
                                                         {
                                                             return BuildLeaderDraw(inputs, *catalog, draw, detail, cache.get());
                                                         });
}

NativeDrawFamilyRegistration LeaderDrawFamily()
{
    return { "leader model", "native_draw_leader_shaders", "leader model shader catalog", [](const std::filesystem::path& source, ReadMemory read, NativeDrawFamily& family, std::string& summary, std::string& error)
             {
                 auto catalog = std::make_shared<LeaderDrawCatalog>();
                 if (!LoadLeaderDrawCatalog(source, *catalog, error))
                     return false;
                 summary = std::format("({} pairs)", catalog->entries.size());
                 family  = MakeLeaderDrawFamily(std::move(read), std::move(catalog));
                 return true;
             } };
}

} // namespace rerevved::gpu::guest_d3d
