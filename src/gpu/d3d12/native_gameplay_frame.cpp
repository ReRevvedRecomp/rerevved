#include "native_gameplay_frame.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

#include "native_menu_draw.h"

namespace rerevved::gpu
{
namespace
{

using namespace rex::graphics::diagnostic;

const NativeGameplayShaderInterface* findShader(const DrawCaptureFrameDraw&                    draw,
                                                std::span<const NativeGameplayShaderInterface> shaders)
{
    const auto& r = draw.draw.registers;
    for (const auto& shader : shaders)
        if (shader.vertexShaderHash == draw.guest_vertex_shader_hash && shader.pixelShaderHash == draw.guest_pixel_shader_hash &&
            shader.shaderProgram == r[0x2180] && shader.shaderContext == r[0x2181] && shader.shaderInterpolators == r[0x2182] &&
            shader.alphaTestEnabled == bool(r[0x2202] & 8) && shader.alphaTestCompare == (r[0x2202] & 7))
            return &shader;
    return nullptr;
}

constexpr std::size_t kMaxGameplayTextureBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaxGameplayFrameBytes   = 512U * 1024U * 1024U;

struct TextureDecodeInfo
{
    NativeDrawReplayTextureFormat format;
    unsigned                      blockSize  = 1;
    unsigned                      blockBytes = 0;
};

TextureDecodeInfo textureDecodeInfo(const DrawCaptureTexture& source, const DrawCaptureTextureUse& use)
{
    if (!use.data_available || source.availability != DrawCaptureAvailability::kComplete || source.scaled_resolve ||
        source.mip_level != 0 || !source.host_mip_levels || !source.host_width || !source.host_height ||
        source.host_width > 4096 || source.host_height > 4096 ||
        (use.dimension != 1 && use.dimension != 3) ||
        source.host_depth_or_array_size != (use.dimension == 3 ? 6U : 1U))
        throw std::runtime_error("incomplete or unsupported gameplay texture extent");
    TextureDecodeInfo info;
    switch (source.host_format)
    {
        case 27:
        case 28:
            info.format     = NativeDrawReplayTextureFormat::Rgba8;
            info.blockBytes = 4;
            break;
        case 60:
        case 61:
            info.format     = NativeDrawReplayTextureFormat::R8;
            info.blockBytes = 1;
            break;
        case 70:
        case 71:
            info.format     = NativeDrawReplayTextureFormat::Bc1;
            info.blockBytes = 8;
            info.blockSize  = 4;
            break;
        case 73:
        case 74:
            info.format     = NativeDrawReplayTextureFormat::Bc2;
            info.blockBytes = 16;
            info.blockSize  = 4;
            break;
        case 76:
        case 77:
            info.format     = NativeDrawReplayTextureFormat::Bc3;
            info.blockBytes = 16;
            info.blockSize  = 4;
            break;
        case 9:
        case 10:
            info.format     = NativeDrawReplayTextureFormat::Rgba16Float;
            info.blockBytes = 8;
            break;
        case 33:
        case 34:
            info.format     = NativeDrawReplayTextureFormat::Rg16Float;
            info.blockBytes = 4;
            break;
        case 39:
        case 41:
            info.format     = NativeDrawReplayTextureFormat::R32Float;
            info.blockBytes = 4;
            break;
        default:
            throw std::runtime_error("unsupported gameplay host texture format");
    }
    if (use.is_signed && source.host_format != 9 && source.host_format != 10 &&
        source.host_format != 33 && source.host_format != 34 && source.host_format != 39 && source.host_format != 41)
        throw std::runtime_error("unsupported signed gameplay texture interpretation");
    if ((use.component_mapping & ~0x1FFFU) || !(use.component_mapping & 0x1000))
        throw std::runtime_error("invalid gameplay texture component mapping");
    for (std::size_t c = 0; c < 4; ++c)
    {
        if (((use.swizzled_signs >> (c * 2)) & 3) > 1)
            throw std::runtime_error("unsupported gameplay texture bias or gamma interpretation");
        if (((use.component_mapping >> (c * 3)) & 7) > 5)
            throw std::runtime_error("unsupported gameplay texture component selector");
    }
    return info;
}

std::span<const std::uint8_t> textureSubresourcePayload(const DrawCaptureTexture& source,
                                                        unsigned                  slice,
                                                        unsigned                  mip,
                                                        std::size_t               rowSize,
                                                        std::size_t               rows)
{
    const auto slices = source.host_depth_or_array_size;
    if (source.subresources.empty())
    {
        if (slices != 1 || source.host_mip_levels != 1 || source.row_size != rowSize || source.row_count != rows)
            throw std::runtime_error("gameplay texture has no complete mip chain");
        return source.bytes;
    }
    const auto& subresource = source.subresources[slice * source.host_mip_levels + mip];
    const auto  width       = std::max(1U, source.host_width >> mip);
    const auto  height      = std::max(1U, source.host_height >> mip);
    if (subresource.mip_level != mip || subresource.array_slice != slice ||
        subresource.host_width != width || subresource.host_height != height ||
        subresource.row_size != rowSize || subresource.row_count != rows)
        throw std::runtime_error("gameplay texture subresource layout mismatch");
    if (subresource.bytes.empty() && !mip && !slice)
        return source.bytes;
    return subresource.bytes;
}

std::size_t texturePayloadSize(const DrawCaptureTexture& source, const TextureDecodeInfo& info)
{
    const auto slices = source.host_depth_or_array_size;
    if (source.host_mip_levels > 13 ||
        (!source.subresources.empty() && source.subresources.size() != std::size_t(slices) * source.host_mip_levels))
        throw std::runtime_error("gameplay texture subresource count does not match its extent");
    std::size_t total = 0;
    for (unsigned slice = 0; slice < slices; ++slice)
        for (unsigned mip = 0; mip < source.host_mip_levels; ++mip)
        {
            const auto width   = std::max(1U, source.host_width >> mip);
            const auto height  = std::max(1U, source.host_height >> mip);
            const auto rowSize = std::size_t((width + info.blockSize - 1) / info.blockSize) * info.blockBytes;
            const auto rows    = (height + info.blockSize - 1) / info.blockSize;
            const auto size    = rowSize * rows;
            const auto payload = textureSubresourcePayload(source, slice, mip, rowSize, rows);
            if (payload.size() != size || size > kMaxGameplayTextureBytes - total)
                throw std::runtime_error("gameplay texture payload is incomplete or oversized");
            total += size;
        }
    return total;
}

std::shared_ptr<const std::vector<std::uint8_t>> decodeTexturePayload(const DrawCaptureTexture& source,
                                                                      const TextureDecodeInfo&  info,
                                                                      std::size_t               expectedSize)
{
    auto bytes = std::make_shared<std::vector<std::uint8_t>>();
    bytes->reserve(expectedSize);
    const auto slices = source.host_depth_or_array_size;
    for (unsigned slice = 0; slice < slices; ++slice)
        for (unsigned mip = 0; mip < source.host_mip_levels; ++mip)
        {
            const auto width   = std::max(1U, source.host_width >> mip);
            const auto height  = std::max(1U, source.host_height >> mip);
            const auto rowSize = std::size_t((width + info.blockSize - 1) / info.blockSize) * info.blockBytes;
            const auto rows    = (height + info.blockSize - 1) / info.blockSize;
            const auto payload = textureSubresourcePayload(source, slice, mip, rowSize, rows);
            bytes->insert(bytes->end(), payload.begin(), payload.end());
        }
    return bytes;
}

NativeDrawReplayTexture decodeTexture(const DrawCaptureTexture&                        source,
                                      const DrawCaptureTextureUse&                     use,
                                      const TextureDecodeInfo&                         info,
                                      std::shared_ptr<const std::vector<std::uint8_t>> payload)
{
    NativeDrawReplayTexture texture;
    texture.width     = source.host_width;
    texture.height    = source.host_height;
    texture.mipLevels = source.host_mip_levels;
    texture.dimension = use.dimension == 3 ? NativeDrawReplayTextureDimension::TextureCube : NativeDrawReplayTextureDimension::Texture2D;
    texture.format    = info.format;
    for (std::size_t c = 0; c < 4; ++c)
        texture.swizzle[c] = (use.component_mapping >> (c * 3)) & 7;
    texture.sharedBytes = std::move(payload);
    return texture;
}

NativeDrawReplaySampler decodeSampler(std::uint32_t value)
{
    NativeDrawReplaySampler sampler;
    std::string             error;
    if (!DecodeNativeGameplaySampler(value, sampler, error))
        throw std::runtime_error(error);
    return sampler;
}

} // namespace

bool DecodeNativeGameplaySampler(std::uint32_t            value,
                                 NativeDrawReplaySampler& sampler,
                                 std::string&             error)
{
    error.clear();
    sampler               = {};
    const auto anisotropy = (value >> 14) & 7;
    if ((value >> 22) || anisotropy > 5)
    {
        error = "unsupported gameplay sampler anisotropy";
        return false;
    }
    constexpr std::uint32_t modes[] = { 1, 2, 3, 5, 3, 5, 4, 5 };
    for (std::size_t axis = 0; axis < 3; ++axis)
        sampler.address[axis] = modes[(value >> (axis * 3)) & 7];
    sampler.magLinear  = (value >> 11) & 1;
    sampler.minLinear  = (value >> 12) & 1;
    sampler.mipLinear  = (value >> 13) & 1;
    sampler.anisotropy = static_cast<std::uint8_t>(anisotropy);
    if (anisotropy != 0)
        sampler.magLinear = sampler.minLinear = sampler.mipLinear = true;
    switch ((value >> 9) & 3)
    {
        case 1:
            sampler.border = { 1, 1, 1, 1 };
            break;
        case 2:
            sampler.border = { 0.5F, 0, 0.5F, 0 };
            break;
        case 3:
            sampler.border = { 0, 0.5F, 0.5F, 0 };
            break;
    }
    sampler.minLod = float((value >> 17) & 15);
    sampler.maxLod = value & (1U << 21)
                         ? sampler.minLod + (anisotropy == 0 ? 0.25F : 0.0F)
                         : std::numeric_limits<float>::max();
    return true;
}

namespace
{

void validateNativeClear(const DrawCaptureFrameEvent& event)
{
    NativeMenuDrawView view;
    const auto&        source = event.draw.draw;
    view.registers            = source.registers;
    view.vertexMicrocode      = source.vertex_ucode;
    view.pixelMicrocode       = source.pixel_ucode;
    view.vertexShaderHash     = event.draw.guest_vertex_shader_hash;
    view.pixelShaderHash      = event.draw.guest_pixel_shader_hash;
    view.indexed              = source.index.present;
    view.indexCount           = source.requested_index_count;
    for (const auto& fetch : source.vertex_fetches)
        if (fetch.fetch_constant == 0)
        {
            if (!ValidateNativeGameplayVertexFetch(fetch))
                throw std::runtime_error("main scene clear vertex fetch payload is incomplete");
            view.vertexGuestBase = fetch.base;
            view.vertexBytes     = fetch.Payload();
        }
    std::uint8_t alpha = 0;
    std::string  error;
    if (!event.success || !event.host_issued || !ValidateNativeMenuInitialClear(view, alpha, error) || alpha != 0)
        throw std::runtime_error("main scene does not begin with a validated black/far clear: " + error);
}

void validateColorResolve(const DrawCaptureFrameEvent& copy, const DrawCaptureFrameEvent& trigger, std::size_t half)
{
    const auto& draw = copy.draw.draw;
    const auto& r    = draw.registers;
    for (const auto [index, value] : {
             std::pair<unsigned, unsigned>{ 0x200E, 0 }, { 0x200F, 0x20002000 }, { 0x2205, 0x10000 }, { 0x2208, 6 }, { 0x2302, 4 }, { 0x231C, 0xFFF }, { 0x231D, 0xFFFFFF00 }, { 0x231E, 0 }, { 0x231F, 0 } })
        if (r[index] != value)
            throw std::runtime_error("unsupported gameplay resolve clear or sample state");
    const DrawCaptureVertexFetch* rectangle = nullptr;
    for (const auto& fetch : draw.vertex_fetches)
        if (fetch.fetch_constant == 0)
        {
            if (rectangle)
                throw std::runtime_error("duplicate gameplay resolve rectangle");
            if (!ValidateNativeGameplayVertexFetch(fetch))
                throw std::runtime_error("gameplay resolve rectangle payload is incomplete");
            rectangle = &fetch;
        }
    if (!rectangle || rectangle->Payload().size() != 24 || trigger.draw.draw.index.present || trigger.draw.draw.requested_index_count != 3 ||
        (r[0x4800] & 3) != 3 || (r[0x4801] & 3) != 2 ||
        (r[0x4800] & ~3U) != rectangle->base || ((r[0x4801] >> 2) & 0xFFFFFF) * 4 != 24)
        throw std::runtime_error("gameplay resolve has no complete source rectangle");
    const float left          = float(half * 640) - 0.5F;
    const float coordinates[] = { left, -0.5F, left + 640, -0.5F, left + 640, 719.5F };
    for (std::size_t i = 0; i < 6; ++i)
    {
        const auto* b    = rectangle->Payload().data() + i * 4;
        const auto  word = (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) |
                           (std::uint32_t(b[2]) << 8) | b[3];
        if (word != std::bit_cast<std::uint32_t>(coordinates[i]))
            throw std::runtime_error("gameplay resolve rectangle does not cover its native half");
    }
}

} // namespace

bool ValidateNativeGameplayVertexFetch(const DrawCaptureVertexFetch& fetch)
{
    return fetch.availability == DrawCaptureAvailability::kComplete && !fetch.Payload().empty();
}

bool ValidateNativeGameplayDrawCaptureInputs(const DrawCaptureFrameDraw&    draw,
                                             std::span<const std::uint32_t> textureFetchSlots,
                                             std::string&                   error)
{
    error.clear();
    if (draw.acquisition != DrawCaptureAvailability::kComplete || !draw.texture_bindings_complete)
    {
        error = "gameplay draw capture acquisition is incomplete";
        return false;
    }
    if (!draw.draw.index.present || draw.draw.index.availability != DrawCaptureAvailability::kComplete ||
        draw.draw.index.bytes.empty())
    {
        error = "gameplay draw index payload is incomplete";
        return false;
    }
    for (const auto& fetch : draw.draw.vertex_fetches)
        if (!ValidateNativeGameplayVertexFetch(fetch))
        {
            error = "gameplay draw vertex fetch payload is incomplete";
            return false;
        }
    for (const auto slot : textureFetchSlots)
    {
        bool selected = false;
        for (const auto& use : draw.texture_uses)
        {
            if (use.fetch_constant != slot || !use.required)
                continue;
            if (!use.data_available || use.availability != DrawCaptureAvailability::kComplete)
            {
                error = "gameplay draw texture use is incomplete";
                return false;
            }
            selected = true;
        }
        if (!selected)
        {
            error = "gameplay draw has no complete required texture use";
            return false;
        }
    }
    return true;
}

bool BuildNativeGameplayFrameRecipe(const DrawCaptureFrameSnapshot&                snapshot,
                                    std::span<const NativeGameplayShaderInterface> shaders,
                                    NativeFrameReplayRecipe&                       recipe,
                                    NativeGameplayFrameCoverage&                   coverage,
                                    std::string&                                   error)
{
    recipe   = {};
    coverage = {};
    error.clear();
    try
    {
        const auto& swap = snapshot.final_swap;
        if (snapshot.schema_version != kDrawCaptureFrameSchemaVersion ||
            snapshot.events.empty() || snapshot.events.size() > 4096 || snapshot.start_frame >= snapshot.end_frame ||
            snapshot.start_submission > snapshot.end_submission || swap.gamma.gamma_256.size() != 256 ||
            swap.gamma.use_pwl_gamma_ramp || swap.gamma.use_fxaa || !swap.gamma.is_8bpc ||
            swap.output_format != 24 || swap.output_width != 1280 || swap.output_height != 720 ||
            snapshot.events.back().kind != DrawCaptureFrameEvent::Kind::kSwap ||
            snapshot.events.back().frontbuffer_ptr != swap.frontbuffer_ptr)
            throw std::runtime_error("incomplete gameplay frame boundary or unsupported presentation state");
        std::copy(swap.gamma.gamma_256.begin(), swap.gamma.gamma_256.end(), recipe.gammaTable.begin());
        const DrawCaptureFrameEvent* previous       = nullptr;
        const DrawCaptureFrameEvent* resolveTrigger = nullptr;
        bool                         started        = false;
        std::size_t                  half           = 0;
        std::uint64_t                previousId     = 0;
        using TextureKey                            = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, bool>;
        std::map<TextureKey, NativeDrawReplayTexture>                 textures;
        std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> sourcePayloads(snapshot.textures.size());
        std::size_t                                                   sourcePayloadBytes = 0;
        for (const auto& event : snapshot.events)
        {
            const auto& capture = event.draw;
            const auto& draw    = capture.draw;
            const auto& r       = draw.registers;
            if (event.id <= previousId || !event.success ||
                (event.execution_status != DrawCaptureExecutionStatus::kIssued &&
                 event.execution_status != DrawCaptureExecutionStatus::kNoEffect) ||
                r.size() != 0x5003 ||
                event.frame < snapshot.start_frame || event.frame >= snapshot.end_frame ||
                event.submission < snapshot.start_submission || event.submission > snapshot.end_submission)
                throw std::runtime_error("invalid gameplay event order, completion or ownership boundary");
            previousId = event.id;
            if (half == 2)
                continue;
            if (!started)
            {
                if (event.kind == DrawCaptureFrameEvent::Kind::kDraw && event.host_issued &&
                    r[0x2000] == 0x0A010280 && r[0x2001] == 0 && findShader(capture, shaders))
                {
                    if (!previous)
                        throw std::runtime_error("main scene has no preceding native clear");
                    validateNativeClear(*previous);
                    started             = true;
                    coverage.firstEvent = event.id;
                }
                else
                {
                    previous = &event;
                    continue;
                }
            }
            if (event.kind == DrawCaptureFrameEvent::Kind::kCopy)
            {
                if (!resolveTrigger || event.copy_mode != "resolve" ||
                    resolveTrigger->draw.draw.registers != r || r[0x2000] != 0x0A010280 ||
                    r[0x2001] != 0 || r[0x2002] != 0x530)
                    throw std::runtime_error("unpaired or unsupported gameplay scene resolve");
                if ((r[0x2318] & 7) == 4)
                {
                    // Depth is not consumed by later draws in this half. Only
                    // a copy without a depth clear can be omitted here.
                    if (r[0x2318] != 4)
                        throw std::runtime_error("unsupported gameplay depth resolve side effect");
                    resolveTrigger = nullptr;
                    continue;
                }
                validateColorResolve(event, *resolveTrigger, half);
                resolveTrigger = nullptr;
                if (recipe.halves[half].empty() || r[0x2318] != (half ? 0x100040U : 0x100340U) ||
                    r[0x231A] != 0x02D00500 || r[0x231B] != 0x01000300 ||
                    r[0x2080] != (half ? 0x7D80U : 0U) || r[0x2081] != (half ? 640U : 0U) ||
                    r[0x2082] != (half ? 0x02D00500U : 0x02D00280U))
                    throw std::runtime_error("unsupported gameplay color resolve state");
                if (!half)
                {
                    coverage.sceneResolveBase      = r[0x2319];
                    constexpr auto tiledFrameBytes = 1280U * ((720U + 31U) & ~31U) * 4U;
                    if (!coverage.sceneResolveBase || coverage.sceneResolveBase > 0x20000000U - tiledFrameBytes ||
                        (coverage.sceneResolveBase & 0xFFF) ||
                        coverage.sceneResolveBase == swap.frontbuffer_ptr)
                        throw std::runtime_error("main scene resolve must precede final composition");
                }
                else if (r[0x2319] != coverage.sceneResolveBase + 0x14000U)
                    throw std::runtime_error("gameplay scene halves resolve to different images");
                coverage.lastEvent = event.id;
                ++half;
                continue;
            }
            if (resolveTrigger)
                throw std::runtime_error("gameplay resolve trigger has no copy event");
            if (event.kind != DrawCaptureFrameEvent::Kind::kDraw)
                throw std::runtime_error("gameplay scene ended before both color resolves");
            if (r[0x2208] == 6 && !event.host_issued && draw.primitive_type == 8)
            {
                resolveTrigger = &event;
                continue;
            }
            const auto* shader = findShader(capture, shaders);
            if (!shader || !event.host_issued || !draw.half_pixel_offset || draw.native_instance_count != 1 ||
                draw.native_start_vertex || draw.native_start_index || draw.native_base_vertex || draw.native_start_instance ||
                draw.host_vertex_shader_type || draw.tessellation_mode || draw.host_primitive_reset_enabled ||
                draw.guest_primitive_type != draw.primitive_type || draw.host_primitive_type != draw.primitive_type ||
                draw.guest_draw_vertex_count != draw.requested_index_count || draw.host_draw_vertex_count != draw.requested_index_count ||
                draw.native_topology != (draw.primitive_type == 6 ? 5U : 4U) ||
                !draw.index.present || draw.native_index_count != draw.requested_index_count ||
                draw.index.count != draw.requested_index_count || draw.guest_index_base != draw.index.guest_base)
                throw std::runtime_error("gameplay event " + std::to_string(event.id) + " has an unsupported or unavailable main draw");
            if (std::string inputError; !ValidateNativeGameplayDrawCaptureInputs(capture, shader->textureFetchSlots, inputError))
                throw std::runtime_error("gameplay event " + std::to_string(event.id) + ": " + inputError);
            std::vector<NativeGameplayVertexFetchSpan> fetches;
            for (const auto& fetch : draw.vertex_fetches)
                fetches.push_back({ fetch.fetch_constant, fetch.base, fetch.Payload() });
            std::vector<NativeGameplayTextureBinding> bindings;
            std::vector<NativeDrawReplaySampler>      samplers;
            samplers.reserve(shader->textureFetchSlots.size());
            for (const auto slot : shader->textureFetchSlots)
            {
                // Translation uses ordinary normalized sampling. Xenos applies
                // this fetch-constant bias even to a vertex fetch at LOD zero.
                if (slot >= 32 || (r[0x4800 + slot * 6 + 4] >> 12) ||
                    ((r[0x4800 + slot * 6 + 3] >> 13) & 63))
                    throw std::runtime_error("unsupported gameplay texture LOD or exponent adjustment");
                const DrawCaptureTextureUse* selected = nullptr;
                for (const auto& use : capture.texture_uses)
                {
                    if (use.fetch_constant != slot || !use.required)
                        continue;
                    if (!use.data_available || use.availability != DrawCaptureAvailability::kComplete)
                        throw std::runtime_error("required gameplay texture use is incomplete");
                    if (selected && (selected->texture_index != use.texture_index ||
                                     selected->component_mapping != use.component_mapping || selected->sampler_value != use.sampler_value ||
                                     selected->dimension != use.dimension || selected->is_signed != use.is_signed))
                        throw std::runtime_error("ambiguous signed or stage-specific gameplay texture use");
                    selected = &use;
                }
                if (!selected || selected->texture_index >= snapshot.textures.size())
                    throw std::runtime_error("required gameplay texture is unavailable");
                const auto       info = textureDecodeInfo(snapshot.textures[selected->texture_index], *selected);
                const TextureKey key{ selected->texture_index, selected->component_mapping, selected->dimension, selected->is_signed };
                auto             found = textures.find(key);
                if (found == textures.end())
                {
                    auto& payload = sourcePayloads[selected->texture_index];
                    if (!payload)
                    {
                        const auto expectedSize = texturePayloadSize(snapshot.textures[selected->texture_index], info);
                        if (expectedSize > kMaxGameplayFrameBytes - sourcePayloadBytes)
                            throw std::runtime_error("gameplay frame texture payload exceeds its aggregate byte bound");
                        sourcePayloadBytes += expectedSize;
                        payload = decodeTexturePayload(snapshot.textures[selected->texture_index], info, expectedSize);
                    }
                    found = textures.emplace(key, decodeTexture(snapshot.textures[selected->texture_index], *selected, info, payload)).first;
                }
                samplers.push_back(decodeSampler(selected->sampler_value));
                bindings.push_back({ slot, &found->second, &samplers.back() });
            }
            NativeGameplayDrawView view;
            view.registers       = r;
            view.vertexMicrocode = draw.vertex_ucode;
            view.pixelMicrocode  = draw.pixel_ucode;
            view.vertexFetches   = fetches;
            view.indexBytes      = draw.index.bytes;
            view.textureBindings = bindings;
            view.primitive       = draw.primitive_type;
            view.indexCount      = draw.requested_index_count;
            view.indexFormat     = draw.index.format;
            view.indexEndian     = draw.index.endianness;
            view.indexed         = draw.index.present;
            NativeDrawReplayRecipe native;
            if (!BuildNativeGameplayDrawRecipe(view, *shader, native, error))
                throw std::runtime_error("gameplay event " + std::to_string(event.id) + ": " + error);
            if (native.viewport.x != (half ? -640.0F : 0.0F))
                throw std::runtime_error("gameplay draw crosses its color resolve boundary");
            recipe.halves[half].push_back(std::move(native));
            ++coverage.draws;
        }
        coverage.textures = textures.size();
        if (half != 2 || resolveTrigger || !ValidateNativeFrameReplayRecipe(recipe, error))
            throw std::runtime_error("incomplete native gameplay scene: " + error);
        return true;
    }
    catch (const std::exception& exception)
    {
        recipe = {};
        error  = exception.what();
        return false;
    }
}

} // namespace rerevved::gpu
