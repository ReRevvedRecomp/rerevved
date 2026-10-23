#include "native_render_frame.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace rerevved::gpu
{
namespace
{

constexpr std::size_t kMaxFrameBytes = 512U * 1024U * 1024U;

struct TextureState
{
    NativeRenderFrameTexture                             description;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> written;
    // The guest-word packing every resolve into it used; mixed packings
    // leave no typed view.
    std::optional<NativeGuestWordPacking> packing;
    bool                                  mixedPacking = false;

    // Castable packed words may bind as R32Uint or another producer-approved
    // 32-bit view.
    bool typedView(NativeDrawReplayTextureFormat format) const
    {
        return !mixedPacking && packing && *packing != NativeGuestWordPacking::None &&
               (format == NativeDrawReplayTextureFormat::Rgba8 || format == NativeDrawReplayTextureFormat::Rgb10a2 ||
                format == NativeDrawReplayTextureFormat::Rg16Float);
    }

    bool complete() const
    {
        std::uint32_t end = 0;
        for (const auto& strip : written)
        {
            if (strip.first > end)
                return false;
            end = std::max(end, strip.second);
        }
        return end == description.width;
    }
};

std::uint64_t drawLocalBytes(const NativeDrawReplayRecipe& draw)
{
    auto bytes = detail::OwnedAdd(detail::OwnedMultiply(draw.indices.capacity(), sizeof(std::uint32_t)), detail::OwnedMultiply(draw.samplers.capacity(), sizeof(NativeDrawReplaySampler)));
    for (const auto* value : { &draw.vertexConstants, &draw.pixelConstants, &draw.sharedConstants, &draw.initialSample0, &draw.initialSample1, &draw.depth.initialSamples })
        bytes = detail::OwnedAdd(bytes, value->capacity());
    bytes = detail::OwnedAdd(bytes, detail::OwnedAdd(draw.vertexData.capacity(), detail::OwnedAdd(draw.vertexShaderDxil.capacity(), draw.pixelShaderDxil.capacity())));
    for (const auto& texture : draw.textures)
        bytes = detail::OwnedAdd(bytes, texture.bytes.capacity());
    return bytes;
}

} // namespace

void detail::VisitNativeDrawOwnedBytes(const NativeDrawReplayRecipe& draw, OwnedVisitor visitor, bool local)
{
    if (local)
        visitor.Add(nullptr, drawLocalBytes(draw));
    const auto payload = [&](const auto& shared)
    {
        if (shared)
            visitor.Add(shared.get(), OwnedAdd(sizeof(*shared), shared->capacity()));
    };
    payload(draw.sharedVertexData);
    if (draw.sharedIndices)
        visitor.Add(draw.sharedIndices.get(), OwnedAdd(sizeof(*draw.sharedIndices), OwnedMultiply(draw.sharedIndices->Capacity(), sizeof(std::uint32_t))));
    payload(draw.sharedVertexShaderDxil);
    payload(draw.sharedPixelShaderDxil);
    if (draw.shaderVersion)
        visitor.Add(draw.shaderVersion.get(), sizeof(NativeDrawReplayShaderVersion));
    if (draw.shaderVersion)
        for (const auto& bytecode : { draw.shaderVersion->VertexBytecode(), draw.shaderVersion->PixelBytecode() })
            visitor.Add(bytecode.get(), OwnedAdd(sizeof(*bytecode), bytecode->capacity()));
    for (const auto& texture : draw.textures)
    {
        payload(texture.sharedBytes);
        if (texture.textureVersion)
        {
            const auto& versionPayload = texture.textureVersion->Payload();
            visitor.Add(texture.textureVersion.get(), texture.textureVersion->MetadataBytes() - (versionPayload->capacity() - versionPayload->size()));
            visitor.Add(versionPayload.get(), OwnedAdd(sizeof(*versionPayload), versionPayload->capacity()));
        }
    }
}

void detail::VisitNativeRenderFrameOwnedBytes(const NativeRenderFrame& frame, OwnedVisitor visitor)
{
    auto bytes = OwnedAdd(sizeof(frame), OwnedAdd(OwnedMultiply(frame.outputPath.native().capacity(), sizeof(std::filesystem::path::value_type)), OwnedMultiply(frame.textures.capacity(), sizeof(NativeRenderFrameTexture))));
    bytes      = OwnedAdd(bytes, OwnedAdd(OwnedMultiply(frame.surfaces.capacity(), sizeof(NativeRenderFrameSurface)), OwnedMultiply(frame.releasedSurfaces.capacity(), sizeof(std::uint64_t))));
    visitor.Add(nullptr, bytes);
    VisitNativeRenderFramePassesOwnedBytes(frame.passes, visitor);
}

void detail::VisitNativeRenderFramePassesOwnedBytes(const std::vector<NativeRenderFramePass>& passes, OwnedVisitor visitor)
{
    auto bytes = OwnedMultiply(passes.capacity(), sizeof(NativeRenderFramePass));
    for (const auto& pass : passes)
    {
        bytes = OwnedAdd(bytes, OwnedAdd(OwnedMultiply(pass.draws.capacity(), sizeof(NativeDrawReplayRecipe)), OwnedAdd(OwnedMultiply(pass.admittedDraws.capacity(), sizeof(std::shared_ptr<const NativeDrawReplayAdmission>)), OwnedAdd(OwnedMultiply(pass.clears.capacity(), sizeof(NativeRenderFramePass::Clear)), OwnedMultiply(pass.resolves.capacity(), sizeof(NativeRenderFrameResolve))))));
        for (const auto& draw : pass.draws)
            VisitNativeDrawOwnedBytes(draw, visitor);
        for (const auto& admission : pass.admittedDraws)
            if (admission)
            {
                visitor.Add(admission.get(), OwnedAdd(admission->MetadataBytes(), drawLocalBytes(admission->Recipe())));
                VisitNativeDrawOwnedBytes(admission->Recipe(), visitor, false);
            }
    }
    visitor.Add(nullptr, bytes);
}

bool NativeRenderFramePass::ValidateDrawStorage(std::string& error) const
{
    if (!draws.empty() && !admittedDraws.empty())
    {
        error = "render frame pass mixes raw and admitted draw storage";
        return false;
    }
    for (const auto& admission : admittedDraws)
        if (!admission)
        {
            error = "render frame pass contains an empty admitted draw owner";
            return false;
        }
        else if (!admission->ValidateSharedPayloads(error))
            return false;
    return true;
}

std::size_t NativeRenderFramePass::DrawCount() const
{
    if (!draws.empty() && !admittedDraws.empty())
        throw std::logic_error("render frame pass mixes raw and admitted draw storage");
    return admittedDraws.empty() ? draws.size() : admittedDraws.size();
}

const NativeDrawReplayRecipe& NativeRenderFramePass::Draw(std::size_t index) const
{
    if (!draws.empty() && !admittedDraws.empty())
        throw std::logic_error("render frame pass mixes raw and admitted draw storage");
    if (admittedDraws.empty())
        return draws.at(index);
    const auto& admission = admittedDraws.at(index);
    if (!admission)
        throw std::logic_error("render frame pass contains an empty admitted draw owner");
    return admission->Recipe();
}

bool BatchNativeRenderFrame(NativeRenderFrame& frame, std::string& error)
{
    error.clear();
    std::size_t                                                               metadataBytes = sizeof(NativeRenderFrame);
    std::unordered_set<std::shared_ptr<const NativeDrawReplayAdmission>>      owners;
    std::unordered_set<std::shared_ptr<const NativeDrawReplayShaderVersion>>  shaderVersions;
    std::unordered_set<std::shared_ptr<const NativeDrawReplayTextureVersion>> textureVersions;
    std::unordered_set<std::shared_ptr<const std::vector<std::uint8_t>>>      payloads;
    std::unordered_set<std::shared_ptr<const NativeDrawReplayIndices>>        indexPayloads;
    const auto                                                                addBytes = [&](std::size_t bytes)
    {
        if (bytes > kMaxFrameBytes - metadataBytes)
            return false;
        metadataBytes += bytes;
        return true;
    };
    if (frame.textures.size() > (kMaxFrameBytes - metadataBytes) / sizeof(NativeRenderFrameTexture) ||
        !addBytes(frame.textures.size() * sizeof(NativeRenderFrameTexture)) ||
        frame.surfaces.size() > (kMaxFrameBytes - metadataBytes) / sizeof(NativeRenderFrameSurface) ||
        !addBytes(frame.surfaces.size() * sizeof(NativeRenderFrameSurface)) ||
        frame.releasedSurfaces.size() > (kMaxFrameBytes - metadataBytes) / sizeof(std::uint64_t) ||
        !addBytes(frame.releasedSurfaces.size() * sizeof(std::uint64_t)))
    {
        error = "render frame command metadata exceeds the owned byte bound";
        return false;
    }
    for (const auto& pass : frame.passes)
    {
        if (!pass.ValidateDrawStorage(error))
            return false;
        const auto add = [&](std::size_t count, std::size_t size)
        {
            if (count > (kMaxFrameBytes - metadataBytes) / size)
            {
                return false;
            }
            metadataBytes += count * size;
            return true;
        };
        if (!add(1, sizeof(NativeRenderFramePass)) ||
            !add(pass.draws.size(), sizeof(NativeDrawReplayRecipe)) ||
            !add(pass.admittedDraws.size(), sizeof(std::shared_ptr<const NativeDrawReplayAdmission>)) ||
            !add(pass.clears.size(), sizeof(NativeRenderFramePass::Clear)) ||
            !add(pass.resolves.size(), sizeof(NativeRenderFrameResolve)))
        {
            error = "render frame command metadata exceeds the owned byte bound";
            return false;
        }
        const auto commands = pass.DrawCount() + pass.clears.size() + pass.resolves.size();
        if (commands && !add((commands - 1) / NativeRenderBatchCommands, sizeof(NativeRenderFramePass)))
        {
            error = "render frame command metadata exceeds the owned byte bound";
            return false;
        }
        for (std::size_t index = 0; index < pass.DrawCount(); ++index)
        {
            if (!pass.admittedDraws.empty())
            {
                if (!owners.insert(pass.admittedDraws[index]).second)
                    continue;
                if (!addBytes(pass.admittedDraws[index]->MetadataBytes()))
                {
                    error = "render frame draw inputs exceed the owned byte bound";
                    return false;
                }
            }
            const auto& draw       = pass.Draw(index);
            const auto  addPayload = [&](const auto& owner, std::size_t bytes)
            {
                return owner && !payloads.insert(owner).second ? true : addBytes(bytes);
            };
            const auto addIndices = [&]
            {
                return draw.sharedIndices && !indexPayloads.insert(draw.sharedIndices).second ? true : addBytes(draw.indexSpan().size() * sizeof(std::uint32_t));
            };
            if ((draw.shaderVersion && shaderVersions.insert(draw.shaderVersion).second && !addBytes(sizeof(NativeDrawReplayShaderVersion))) ||
                !addPayload(draw.sharedVertexShaderDxil, draw.vertexShaderBytes().size()) ||
                !addPayload(draw.sharedPixelShaderDxil, draw.pixelShaderBytes().size()) ||
                !addPayload(draw.sharedVertexData, draw.vertexBytes().size()) ||
                !addIndices() ||
                !addBytes(draw.samplers.size() * sizeof(NativeDrawReplaySampler)))
            {
                error = "render frame draw inputs exceed the owned byte bound";
                return false;
            }
            for (const auto bytes : { draw.vertexConstants.size(), draw.pixelConstants.size(), draw.sharedConstants.size(), draw.initialSample0.size(), draw.initialSample1.size(), draw.depth.initialSamples.size() })
                if (!addBytes(bytes))
                {
                    error = "render frame draw inputs exceed the owned byte bound";
                    return false;
                }
            for (const auto& texture : draw.textures)
                if ((texture.textureVersion && textureVersions.insert(texture.textureVersion).second && !addBytes(texture.textureVersion->MetadataBytes())) ||
                    !addPayload(texture.sharedBytes, texture.payloadBytes().size()))
                {
                    error = "render frame texture inputs exceed the owned byte bound";
                    return false;
                }
        }
    }
    std::vector<NativeRenderFramePass> batches;
    std::vector<std::uint32_t>         finalSourcePass(frame.passes.size());
    for (std::size_t index = 0; index < frame.passes.size(); ++index)
    {
        auto& source = frame.passes[index];
        if (source.colorSourcePass > index)
        {
            error = "render frame color alias requires an earlier source";
            return false;
        }
        const auto target          = GetNativeRenderFrameTarget(source);
        const auto colorSourcePass = source.colorSourcePass ? finalSourcePass[source.colorSourcePass - 1] : 0;
        if (source.clears.size() + source.DrawCount() + source.resolves.size() <= NativeRenderBatchCommands)
        {
            source.target          = target;
            source.colorSourcePass = colorSourcePass;
            batches.push_back(std::move(source));
            finalSourcePass[index] = static_cast<std::uint32_t>(batches.size());
            continue;
        }
        NativeRenderFramePass batch;
        batch.target                      = target;
        batch.preservePreviousAttachments = source.preservePreviousAttachments;
        batch.resumeAttachments           = source.resumeAttachments;
        batch.colorSourcePass             = colorSourcePass;
        batch.presented                   = source.presented;
        std::size_t commands              = 0;
        const auto  flush                 = [&]()
        {
            batches.push_back(std::move(batch));
            batch                             = {};
            batch.target                      = target;
            batch.preservePreviousAttachments = true;
            batch.presented                   = source.presented;
            commands                          = 0;
        };
        const auto next = [&]()
        {
            if (commands == NativeRenderBatchCommands)
            {
                flush();
            }
            ++commands;
        };
        for (auto& clear : source.clears)
        {
            next();
            batch.clears.push_back(std::move(clear));
        }
        for (auto& draw : source.draws)
        {
            next();
            batch.draws.push_back(std::move(draw));
        }
        for (auto& draw : source.admittedDraws)
        {
            next();
            batch.admittedDraws.push_back(std::move(draw));
        }
        for (auto& resolve : source.resolves)
        {
            next();
            batch.resolves.push_back(std::move(resolve));
        }
        // Leave an empty invalid pass visible to the normal validator.
        flush();
        finalSourcePass[index] = static_cast<std::uint32_t>(batches.size());
    }
    frame.passes = std::move(batches);
    return true;
}

NativeRenderFramePass::Target GetNativeRenderFrameTarget(const NativeRenderFramePass& pass)
{
    const auto count = pass.DrawCount();
    if (pass.target.width || !count)
        return pass.target;
    return NativeRenderFrameDrawTarget(pass.Draw(0));
}

NativeRenderFramePass::Target NativeRenderFrameDrawTarget(const NativeDrawReplayRecipe& draw)
{
    NativeRenderFramePass::Target target{ draw.width, draw.height, draw.sampleCount, draw.targetFormat };
    target.extraColorTargets = draw.extraTargetCount;
    for (std::size_t index = 0; index < std::min<std::size_t>(draw.extraTargetCount, target.extraFormats.size()); ++index)
        target.extraFormats[index] = draw.extraTargets[index].format;
    return target;
}

NativeDrawReplayTargetFormat NativeRenderFrameSlotFormat(const NativeRenderFramePass::Target& target, std::uint32_t slot)
{
    return slot >= 1 && slot <= target.extraColorTargets && slot <= target.extraFormats.size() ? target.extraFormats[slot - 1] : target.format;
}

struct NativeRenderFramePrefixState::Impl
{
    std::map<std::uint64_t, TextureState>                                   textures;
    detail::OwnedSet<std::uint64_t>                                         retainedKeys;
    detail::OwnedSet<std::shared_ptr<const std::vector<std::uint8_t>>>      sharedPayloads;
    detail::OwnedSet<std::shared_ptr<const NativeDrawReplayIndices>>        sharedIndices;
    detail::OwnedSet<std::shared_ptr<const NativeDrawReplayAdmission>>      admittedOwners;
    detail::OwnedSet<std::shared_ptr<const NativeDrawReplayShaderVersion>>  shaderVersions;
    detail::OwnedSet<std::shared_ptr<const NativeDrawReplayTextureVersion>> textureVersions;
    detail::OwnedSet<std::uint32_t>                                         savedColorPasses;
    NativeRenderFramePass::Target                                           previous{};
    std::size_t                                                             textureBytes = 0, inputBytes = 0, passCount = 0;
    bool                                                                    depthInitialized = false, failed = false, ended = false;
    // The chain's identityAttachments setting, and its latest presented pass.
    std::optional<bool>                          identity;
    std::optional<NativeRenderFramePass::Target> lastPresented;
    bool                                         lastPresentedDepth = false;
    bool                                         Validate(const NativeRenderFrame& frame, bool terminal, std::string& error);

    explicit Impl(detail::OwnedHeapBudget* budget = nullptr)
    : retainedKeys(detail::OwnedAllocator<std::uint64_t>{ budget })
    , sharedPayloads(detail::OwnedAllocator<std::shared_ptr<const std::vector<std::uint8_t>>>{ budget })
    , sharedIndices(detail::OwnedAllocator<std::shared_ptr<const NativeDrawReplayIndices>>{ budget })
    , admittedOwners(detail::OwnedAllocator<std::shared_ptr<const NativeDrawReplayAdmission>>{ budget })
    , shaderVersions(detail::OwnedAllocator<std::shared_ptr<const NativeDrawReplayShaderVersion>>{ budget })
    , textureVersions(detail::OwnedAllocator<std::shared_ptr<const NativeDrawReplayTextureVersion>>{ budget })
    , savedColorPasses(detail::OwnedAllocator<std::uint32_t>{ budget })
    {
    }
};

void NativeRenderFramePrefixState::VisitOwnedBytes(detail::OwnedVisitor visitor) const
{
    if (!impl)
        return;
    auto       bytes     = detail::OwnedAdd(sizeof(Impl), detail::OwnedMultiply(impl->textures.size(), sizeof(std::pair<const std::uint64_t, TextureState>) + 4 * sizeof(void*)));
    const auto container = [&](const auto& values)
    {
        using Value              = typename std::decay_t<decltype(values)>::value_type;
        constexpr auto alignment = std::max(alignof(Value), alignof(void*));
        constexpr auto unpadded  = sizeof(Value) + 2 * sizeof(void*);
        constexpr auto nodeBytes = ((unpadded + alignment - 1) / alignment) * alignment;
        return detail::OwnerSetBytes(values.size(), values.bucket_count(), nodeBytes);
    };
    if (!impl->retainedKeys.get_allocator().budget)
        for (const auto value : { container(impl->retainedKeys), container(impl->sharedPayloads), container(impl->sharedIndices), container(impl->admittedOwners), container(impl->shaderVersions), container(impl->textureVersions), container(impl->savedColorPasses) })
            bytes = detail::OwnedAdd(bytes, value);
    for (const auto& [id, texture] : impl->textures)
        bytes = detail::OwnedAdd(bytes, detail::OwnedMultiply(texture.written.capacity(), sizeof(std::pair<std::uint32_t, std::uint32_t>)));
    for (const auto& owner : impl->admittedOwners)
    {
        visitor.Add(owner.get(), detail::OwnedAdd(owner->MetadataBytes(), drawLocalBytes(owner->Recipe())));
        detail::VisitNativeDrawOwnedBytes(owner->Recipe(), visitor, false);
    }
    for (const auto& owner : impl->sharedPayloads)
        visitor.Add(owner.get(), detail::OwnedAdd(sizeof(*owner), owner->capacity()));
    for (const auto& owner : impl->sharedIndices)
        visitor.Add(owner.get(), detail::OwnedAdd(sizeof(*owner), detail::OwnedMultiply(owner->Capacity(), sizeof(std::uint32_t))));
    for (const auto& version : impl->shaderVersions)
    {
        visitor.Add(version.get(), sizeof(NativeDrawReplayShaderVersion));
        for (const auto& bytecode : { version->VertexBytecode(), version->PixelBytecode() })
            visitor.Add(bytecode.get(), detail::OwnedAdd(sizeof(*bytecode), bytecode->capacity()));
    }
    for (const auto& version : impl->textureVersions)
    {
        const auto& payload = version->Payload();
        visitor.Add(version.get(), version->MetadataBytes() - (payload->capacity() - payload->size()));
        visitor.Add(payload.get(), detail::OwnedAdd(sizeof(*payload), payload->capacity()));
    }
    visitor.Add(nullptr, bytes);
}

void NativeRenderFramePrefixState::ForecastOwnedHeap(const NativeRenderFrame& frame, std::uint64_t& growth, std::uint64_t& peak) const
{
    std::uint64_t draws = 0, textures = 0;
    for (const auto& pass : frame.passes)
    {
        const auto addDraw = [&](const NativeDrawReplayRecipe& draw)
        {
            draws    = detail::OwnedAdd(draws, 1);
            textures = detail::OwnedAdd(textures, draw.textures.size());
        };
        for (const auto& draw : pass.draws)
            addDraw(draw);
        for (const auto& admission : pass.admittedDraws)
            if (admission)
                addDraw(admission->Recipe());
    }
    const auto add = [&](const auto& set, std::uint64_t additions, std::uint64_t node)
    {
        const auto current  = detail::OwnerSetBytes(set.size(), set.bucket_count(), node);
        const auto forecast = detail::ForecastOwnerSet(set.size(), set.bucket_count(), additions, node);
        growth              = detail::OwnedAdd(growth, forecast.bytes - current);
        peak                = detail::OwnedAdd(peak, forecast.peak - current);
    };
    add(impl->retainedKeys, frame.textures.size(), 24);
    add(impl->sharedPayloads, detail::OwnedAdd(detail::OwnedMultiply(3, draws), textures), 32);
    add(impl->sharedIndices, draws, 32);
    add(impl->admittedOwners, draws, 32);
    add(impl->shaderVersions, draws, 32);
    add(impl->textureVersions, textures, 32);
    add(impl->savedColorPasses, frame.passes.size(), 24);
}

std::uint64_t NativeRenderFramePrefixState::OwnedMetadataBytes()
{
    return sizeof(Impl);
}

bool NativeRenderFramePrefixState::Impl::Validate(const NativeRenderFrame& frame, bool terminal, std::string& error)
{
    error.clear();
    const auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    for (const auto& pass : frame.passes)
        if (!pass.ValidateDrawStorage(error))
            return false;
    // A tail whose chain recorded every pass carries only the output.
    if ((frame.passes.empty() && !(terminal && frame.continuedPrefixPasses)) || frame.passes.size() > kMaxFrameBytes / sizeof(NativeRenderFramePass) - passCount ||
        (terminal && frame.textures.empty()) || frame.textures.size() > NativeRenderBatchCommands ||
        (terminal && !frame.retainOnly && !frame.returnResolvedOutput && !frame.returnGpuOutput && frame.outputPath.empty()))
        return fail("render frame requires bounded passes, textures and an output destination");
    if (std::any_of(frame.outputSwizzle.begin(), frame.outputSwizzle.end(), [](auto component)
                    {
                        return component > 5;
                    }))
        return fail("render frame output component mapping is invalid");
    if (identity && *identity != frame.identityAttachments)
        return fail("render frame chain changed its attachment identity");
    identity = frame.identityAttachments;

    std::unordered_set<std::uint64_t> declared;
    for (const auto& texture : frame.textures)
    {
        if (!texture.id || !texture.width || !texture.height || texture.width > 4096 || texture.height > 4096 ||
            (texture.format != NativeDrawReplayTextureFormat::Rgba8 &&
             texture.format != NativeDrawReplayTextureFormat::Rgb10a2 &&
             texture.format != NativeDrawReplayTextureFormat::Rg16Float &&
             texture.format != NativeDrawReplayTextureFormat::R32Float &&
             texture.format != NativeDrawReplayTextureFormat::R32Uint))
            return fail("render frame texture descriptor is unsupported");
        if (!declared.insert(texture.id).second)
            return fail("render frame texture identities must be unique");
        if (const auto found = textures.find(texture.id); found != textures.end())
        {
            // Prefix packets declare a generated texture without the key its
            // terminal publishes, so a terminal seals exactly that zero key.
            auto       recorded = found->second.description;
            const bool seal     = terminal && !texture.imported && !recorded.persistentKey && texture.persistentKey;
            if (seal)
                recorded.persistentKey = texture.persistentKey;
            if (recorded != texture)
                return fail("native append changed a recorded resource descriptor");
            if (seal)
            {
                if (!retainedKeys.insert(texture.persistentKey).second)
                    return fail("retained texture versions must be nonzero and unique");
                found->second.description.persistentKey = texture.persistentKey;
            }
            continue;
        }
        if (textures.size() == NativeRenderBatchCommands)
            return fail("native append resource identities exceed the frame bound");
        const auto bytes = std::uint64_t(texture.width) * texture.height * 4;
        if (bytes > kMaxFrameBytes - textureBytes)
            return fail("render frame textures exceed the resource byte bound");
        textureBytes += static_cast<std::size_t>(bytes);
        if (!textures.emplace(texture.id, TextureState{ texture, {} }).second)
            return fail("render frame texture identities must be unique");
        if ((!texture.imported && texture.producerVersion) || (texture.imported && !texture.persistentKey) ||
            (texture.persistentKey && !retainedKeys.insert(texture.persistentKey).second))
            return fail("retained texture versions must be nonzero and unique");
        if (texture.imported)
        {
            auto& state = textures.at(texture.id);
            state.written.emplace_back(0, texture.width);
            if (texture.importedWords != NativeGuestWordPacking::None)
            {
                if (texture.format != NativeDrawReplayTextureFormat::R32Uint)
                    return fail("render frame imported guest words need an R32Uint texture");
                state.packing = texture.importedWords;
            }
        }
        else if (texture.importedWords != NativeGuestWordPacking::None)
            return fail("render frame guest-word packing is declared only by an import");
    }

    const auto addBytes = [&](std::size_t bytes)
    {
        if (bytes > kMaxFrameBytes - inputBytes)
            return false;
        inputBytes += bytes;
        return true;
    };
    if (frame.surfaces.size() > kMaxFrameBytes / sizeof(NativeRenderFrameSurface) || frame.releasedSurfaces.size() > kMaxFrameBytes / sizeof(std::uint64_t) ||
        !addBytes(sizeof(NativeRenderFrame) + frame.textures.size() * sizeof(NativeRenderFrameTexture)) || !addBytes(frame.surfaces.size() * sizeof(NativeRenderFrameSurface)) ||
        !addBytes(frame.releasedSurfaces.size() * sizeof(std::uint64_t)))
        return fail("render frame command metadata exceeds the owned byte bound");
    // A surface names a texture the frame produces; an assembled one fits
    // its destination. A depth conversion reads only Depth24Stencil8 words
    // into its own key.
    for (const auto& surface : frame.surfaces)
    {
        const auto texture = std::find_if(frame.textures.begin(), frame.textures.end(), [&](const NativeRenderFrameTexture& declared)
                                          {
                                              return declared.id == surface.textureId;
                                          });
        if (texture == frame.textures.end() || texture->imported ||
            (surface.width && (surface.x > surface.width || texture->width > surface.width - surface.x || surface.y > surface.height || texture->height > surface.height - surface.y)))
            return fail("render frame surface names no produced texture or does not fit its destination");
        if (surface.depthKey && (surface.depthKey == surface.key || texture->format != NativeDrawReplayTextureFormat::R32Uint ||
                                 std::any_of(frame.passes.begin(), frame.passes.end(), [&](const NativeRenderFramePass& pass)
                                             {
                                                 return std::any_of(pass.resolves.begin(), pass.resolves.end(), [&](const NativeRenderFrameResolve& resolve)
                                                                    {
                                                                        return resolve.textureId == surface.textureId && resolve.guestWords != NativeGuestWordPacking::Depth24Stencil8;
                                                                    });
                                             })))
            return fail("render frame depth conversion needs Depth24Stencil8 words and its own key");
    }
    for (std::size_t passIndex = 0; passIndex < frame.passes.size(); ++passIndex)
    {
        const auto& pass     = frame.passes[passIndex];
        std::size_t commands = 0;
        if ((!pass.DrawCount() && pass.clears.empty() && pass.resolves.empty()) ||
            pass.DrawCount() > NativeRenderBatchCommands - commands)
            return fail("render frame pass is empty or exceeds the command bound");
        commands += pass.DrawCount();
        if (pass.clears.size() > NativeRenderBatchCommands - commands)
            return fail("render frame clears exceed the command bound");
        commands += pass.clears.size();
        if (pass.resolves.size() > NativeRenderBatchCommands - commands)
            return fail("render frame resolves exceed the command bound");
        commands += pass.resolves.size();
        if (!addBytes(sizeof(NativeRenderFramePass) + pass.draws.size() * sizeof(NativeDrawReplayRecipe) +
                      pass.admittedDraws.size() * sizeof(std::shared_ptr<const NativeDrawReplayAdmission>) +
                      pass.clears.size() * sizeof(NativeRenderFramePass::Clear) +
                      pass.resolves.size() * sizeof(NativeRenderFrameResolve)))
        {
            return fail("render frame command metadata exceeds the owned byte bound");
        }
        const auto target = GetNativeRenderFrameTarget(pass);
        if (!target.width || !target.height || target.width > 4096 || target.height > 4096 ||
            (target.sampleCount != 1 && target.sampleCount != 4) ||
            (target.format != NativeDrawReplayTargetFormat::Rgba8 &&
             target.format != NativeDrawReplayTargetFormat::Rgb10a2 &&
             target.format != NativeDrawReplayTargetFormat::Rg16Snorm))
            return fail("render frame pass target is unsupported");
        const bool mrt             = target.extraColorTargets != 0;
        bool       supportedExtras = target.extraColorTargets <= target.extraFormats.size();
        for (std::size_t index = 0; index < target.extraFormats.size(); ++index)
            supportedExtras &= index < target.extraColorTargets
                                   ? target.extraFormats[index] == NativeDrawReplayTargetFormat::Rgba8 || target.extraFormats[index] == NativeDrawReplayTargetFormat::Rgb10a2
                                   : target.extraFormats[index] == NativeDrawReplayTargetFormat{} && !target.extraEdramBases[index];
        if (!supportedExtras)
            return fail("render frame MRT attachments are unsupported");
        if (mrt && (pass.presented || pass.colorSourcePass))
            return fail("render frame MRT pass cannot be presented or aliased");
        // Without extras a clear may name any of the four same-format slots.
        const std::uint32_t boundSlots = mrt ? (1U << (target.extraColorTargets + 1)) - 1 : 0xFU;
        if (pass.resumeAttachments)
        {
            // An adjacent pass preserves instead; a resume skips off-screen work.
            if (!frame.identityAttachments || !pass.presented || pass.preservePreviousAttachments || pass.colorSourcePass ||
                !lastPresented || *lastPresented != target || previous == target)
                return fail("render frame cannot resume incompatible or absent presented attachments");
            depthInitialized = lastPresentedDepth;
        }
        else if (pass.preservePreviousAttachments)
        {
            if (!previous.width || previous != target)
                return fail("render frame cannot preserve incompatible or absent attachments");
        }
        else
            depthInitialized = false;
        if (pass.colorSourcePass)
        {
            if (pass.colorSourcePass > passIndex || pass.preservePreviousAttachments || target.sampleCount != 1)
                return fail("render frame color alias requires an earlier single-sample source");
            const auto source = GetNativeRenderFrameTarget(frame.passes[pass.colorSourcePass - 1]);
            if (source.sampleCount != 1 || source.width != target.width || source.height != target.height)
                return fail("render frame color alias dimensions or sample layout differ");
            const auto rowPitch = (std::size_t(source.width) * 4U + 255U) & ~std::size_t(255U);
            if (savedColorPasses.insert(pass.colorSourcePass).second && !addBytes(rowPitch * source.height))
                return fail("render frame color aliases exceed the owned byte bound");
        }
        bool usesDepth = false;
        for (const auto& clear : pass.clears)
        {
            const auto& rect = clear.rectangle;
            if ((!clear.color && !clear.depth && !clear.stencil) ||
                (clear.color && (!clear.colorTargets || (clear.colorTargets & ~boundSlots))) || rect.left < 0 || rect.top < 0 ||
                rect.right <= rect.left || rect.bottom <= rect.top ||
                rect.right > static_cast<std::int32_t>(target.width) ||
                rect.bottom > static_cast<std::int32_t>(target.height) ||
                !std::isfinite(clear.depthValue) || clear.depthValue < 0 || clear.depthValue > 1 ||
                std::any_of(clear.colorValue.begin(), clear.colorValue.end(), [](float value)
                            {
                                return !std::isfinite(value);
                            }))
                return fail("render frame clear rectangle or value is invalid");
            depthInitialized |= clear.depth || clear.stencil;
            usesDepth |= clear.depth || clear.stencil;
        }
        for (std::size_t index = 0; index < pass.DrawCount(); ++index)
        {
            const auto& draw       = pass.Draw(index);
            bool        sameExtras = draw.extraTargetCount == target.extraColorTargets;
            for (std::size_t slot = 0; sameExtras && slot < std::min<std::size_t>(draw.extraTargetCount, draw.extraTargets.size()); ++slot)
                sameExtras = draw.extraTargets[slot].format == target.extraFormats[slot];
            if (draw.width != target.width || draw.height != target.height ||
                draw.sampleCount != target.sampleCount || draw.targetFormat != target.format || !sameExtras || draw.schemaVersion != 2 ||
                !draw.initialSample0.empty() || !draw.initialSample1.empty() || !draw.depth.initialSamples.empty())
                return fail("render frame pass requires compatible targets and native attachment initialization");
            for (const auto& texture : draw.textures)
            {
                const auto found = texture.frameTextureId ? textures.find(texture.frameTextureId) : textures.end();
                if (texture.format == NativeDrawReplayTextureFormat::R32Uint ||
                    (found != textures.end() && found->second.description.format == NativeDrawReplayTextureFormat::R32Uint &&
                     (!frame.typedGuestWords || !found->second.typedView(texture.format))))
                    return fail("render frame draw samples a guest-word texture");
            }
            if (pass.admittedDraws.empty() && !ValidateNativeDrawReplayRecipe(draw, error))
                return false;
            depthInitialized |= draw.depth.enabled || draw.stencil.enabled;
            usesDepth |= draw.depth.enabled || draw.stencil.enabled;
            const bool newOwner   = pass.admittedDraws.empty() || admittedOwners.insert(pass.admittedDraws[index]).second;
            const auto addPayload = [&](const auto& owner, std::size_t bytes)
            {
                return owner && !sharedPayloads.insert(owner).second ? true : addBytes(bytes);
            };
            const auto addIndices = [&]
            {
                return draw.sharedIndices && !sharedIndices.insert(draw.sharedIndices).second ? true : addBytes(draw.indexSpan().size() * sizeof(std::uint32_t));
            };
            if (newOwner)
            {
                const bool admitted = !pass.admittedDraws.empty();
                if ((admitted && !addBytes(pass.admittedDraws[index]->MetadataBytes())) ||
                    (draw.shaderVersion && shaderVersions.insert(draw.shaderVersion).second && !addBytes(sizeof(NativeDrawReplayShaderVersion))) ||
                    !(admitted ? addPayload(draw.sharedVertexShaderDxil, draw.vertexShaderBytes().size()) : addBytes(draw.vertexShaderBytes().size())) ||
                    !(admitted ? addPayload(draw.sharedPixelShaderDxil, draw.pixelShaderBytes().size()) : addBytes(draw.pixelShaderBytes().size())) ||
                    !(admitted ? addPayload(draw.sharedVertexData, draw.vertexBytes().size()) : addBytes(draw.vertexBytes().size())) ||
                    !addIndices() ||
                    !addBytes(draw.samplers.size() * sizeof(NativeDrawReplaySampler)))
                    return fail("render frame draw inputs exceed the owned byte bound");
                for (const auto bytes : { draw.vertexConstants.size(), draw.pixelConstants.size(), draw.sharedConstants.size() })
                    if (!addBytes(bytes))
                        return fail("render frame draw inputs exceed the owned byte bound");
            }
            for (const auto& texture : draw.textures)
            {
                if (texture.frameTextureId)
                {
                    const auto found = textures.find(texture.frameTextureId);
                    if (found == textures.end() || !found->second.complete())
                        return fail("render frame draw samples a missing or incompletely resolved texture");
                    const auto& description = found->second.description;
                    if (texture.width != description.width || texture.height != description.height ||
                        (texture.format != description.format && !found->second.typedView(texture.format)))
                        return fail("render frame texture binding does not match its resolved resource");
                }
                else if (newOwner && ((texture.textureVersion && textureVersions.insert(texture.textureVersion).second && !addBytes(texture.textureVersion->MetadataBytes())) ||
                                      !addPayload(texture.sharedBytes, texture.payloadBytes().size())))
                    return fail("render frame texture inputs exceed the owned byte bound");
            }
        }
        // Presented passes come from guest EDRAM, so their depth work names the
        // depth surface it hits. Offline recipes predate the identity.
        if (pass.presented && usesDepth && target.depthEdramBase == kNoDepthEdramBase)
            return fail("render frame presented depth work requires a depth surface identity");
        for (const auto& resolve : pass.resolves)
        {
            const auto found = textures.find(resolve.textureId);
            if (found == textures.end())
                return fail("render frame resolve has no declared destination");
            auto&       state       = found->second;
            const auto& destination = state.description;
            if (destination.imported)
                return fail("an imported native texture version is immutable");
            if (!resolve.width || !resolve.height || resolve.height != destination.height ||
                resolve.sourceX > target.width || resolve.width > target.width - resolve.sourceX ||
                resolve.sourceY > target.height || resolve.height > target.height - resolve.sourceY ||
                resolve.destinationX > destination.width || resolve.width > destination.width - resolve.destinationX ||
                resolve.colorTarget >= 4 || (mrt && resolve.colorTarget > target.extraColorTargets) ||
                (resolve.depth && (!depthInitialized || resolve.colorTarget != 0)) ||
                !std::isfinite(resolve.valueScale) || resolve.valueScale <= 0)
                return fail("render frame resolve rectangle, source or scale is invalid");
            for (const auto sample : resolve.samples)
                if (sample >= target.sampleCount ||
                    (target.sampleCount == 4 && sample != 0 && sample != 3))
                    return fail("render frame resolve selects an unsupported sample");
            // Conversions follow the resolved slot's format, not RT0's.
            const auto slotFormat = NativeRenderFrameSlotFormat(target, resolve.colorTarget);
            if (resolve.guestWords != NativeGuestWordPacking::None)
            {
                const bool packing = resolve.depth ? resolve.guestWords == NativeGuestWordPacking::Depth24Stencil8 && resolve.samples[0] == resolve.samples[1]
                                                   : (resolve.guestWords == NativeGuestWordPacking::Color8888 && slotFormat == NativeDrawReplayTargetFormat::Rgba8) ||
                                                         (resolve.guestWords == NativeGuestWordPacking::Color2101010 && slotFormat == NativeDrawReplayTargetFormat::Rgb10a2) ||
                                                         (resolve.guestWords == NativeGuestWordPacking::Float16x2 && slotFormat == NativeDrawReplayTargetFormat::Rg16Snorm);
                if (!packing || destination.format != NativeDrawReplayTextureFormat::R32Uint || resolve.swapRedBlue || resolve.valueScale != 1.0F)
                    return fail("render frame guest-word resolve is invalid");
            }
            else if (resolve.depth && (destination.format != NativeDrawReplayTextureFormat::R32Float ||
                                       resolve.swapRedBlue || resolve.valueScale != 1.0F))
                return fail("render frame depth resolve requires an unscaled float destination");
            else if (!resolve.depth &&
                     !((slotFormat == NativeDrawReplayTargetFormat::Rgba8 &&
                        destination.format == NativeDrawReplayTextureFormat::Rgba8 && resolve.valueScale == 1.0F) ||
                       (slotFormat == NativeDrawReplayTargetFormat::Rgb10a2 &&
                        destination.format == NativeDrawReplayTextureFormat::Rgb10a2 && resolve.valueScale == 1.0F) ||
                       (slotFormat == NativeDrawReplayTargetFormat::Rg16Snorm &&
                        destination.format == NativeDrawReplayTextureFormat::Rg16Float && resolve.valueScale == 32.0F &&
                        !resolve.swapRedBlue)))
                return fail("render frame color resolve conversion is unsupported");
            if (resolve.readback && (destination.persistentKey || resolve.destinationX || resolve.width != destination.width))
                return fail("render frame resolve readback requires a whole unretained destination");
            if (state.packing && *state.packing != resolve.guestWords)
                state.mixedPacking = true;
            state.packing = resolve.guestWords;
            state.written.emplace_back(resolve.destinationX, resolve.destinationX + resolve.width);
            std::sort(state.written.begin(), state.written.end());
        }
        previous = target;
        if (pass.presented)
        {
            lastPresented      = target;
            lastPresentedDepth = depthInitialized;
        }
    }
    passCount += frame.passes.size();
    if (!terminal)
        return true;
    // A terminal's depth conversion reads its texture's words from every
    // resolve of the chain, as the joined graph's check above would.
    for (const auto& surface : frame.surfaces)
        if (const auto& state = textures.at(surface.textureId);
            surface.depthKey && state.packing && (*state.packing != NativeGuestWordPacking::Depth24Stencil8 || state.mixedPacking))
            return fail("render frame depth conversion needs Depth24Stencil8 words and its own key");
    for (const auto& [id, state] : textures)
        if (state.description.persistentKey && !state.complete())
            return fail("retained native texture must be completely resolved");
    // Packed guest words leave only through a retained producer's raw readback.
    const auto output = textures.find(frame.outputTextureId);
    if (output == textures.end() || !output->second.complete() ||
        (!frame.retainOnly && (output->second.description.width != 1280 || output->second.description.height != 720)) ||
        (output->second.description.format != NativeDrawReplayTextureFormat::Rgba8 &&
         (output->second.description.format != NativeDrawReplayTextureFormat::R32Uint || !frame.retainOnly || !frame.returnRawOutput)))
        return fail("render frame output requires a complete 1280x720 RGBA8 texture");
    if (frame.returnRawOutput && !frame.retainOnly)
        return fail("raw readback requires a retained producer frame");
    if (frame.retainOnly && (!output->second.description.persistentKey ||
                             frame.returnResolvedOutput || frame.returnGpuOutput || !frame.outputPath.empty()))
        return fail("producer frame requires a retained output without presentation or gamma readback");
    return true;
}

NativeRenderFramePrefixState::NativeRenderFramePrefixState()
: impl(std::make_unique<Impl>())
{
}

NativeRenderFramePrefixState::NativeRenderFramePrefixState(detail::OwnedHeapBudget* budget)
: impl(std::make_unique<Impl>(budget))
{
}

NativeRenderFramePrefixState::~NativeRenderFramePrefixState()                                                  = default;
NativeRenderFramePrefixState::NativeRenderFramePrefixState(NativeRenderFramePrefixState&&) noexcept            = default;
NativeRenderFramePrefixState& NativeRenderFramePrefixState::operator=(NativeRenderFramePrefixState&&) noexcept = default;

bool NativeRenderFramePrefixState::Append(const NativeRenderFrame& packet, std::string& error)
{
    error.clear();
    const auto fail = [&](const char* message)
    {
        impl->failed = true;
        error        = message;
        return false;
    };
    if (impl->ended)
        return fail("native prefix chain already ended");
    if (impl->failed)
        return fail("native append validation already failed");
    if (packet.continuedPrefixPasses)
        return fail("native prefix packet cannot continue a chain");
    if (packet.passes.empty() || packet.retainOnly || packet.returnResolvedOutput || packet.returnRawOutput ||
        !packet.outputPath.empty() || packet.outputTextureId)
        return fail("native prefix requires passes without output or color aliasing");
    if (!impl->passCount && packet.passes.front().preservePreviousAttachments)
        return fail("native prefix requires a first sealed pass without output or color aliasing");
    for (const auto& pass : packet.passes)
        if (!pass.ValidateDrawStorage(error))
        {
            impl->failed = true;
            return false;
        }
        else if (pass.colorSourcePass)
            return fail("native append requires passes without color aliasing");
    if (!impl->Validate(packet, false, error))
    {
        impl->failed = true;
        return false;
    }
    return true;
}

bool NativeRenderFramePrefixState::ValidateTerminal(const NativeRenderFrame& frame, std::string& error) const
{
    if (impl->ended || impl->failed)
    {
        error = impl->ended ? "native prefix chain already ended" : "native append validation already failed";
        return false;
    }
    auto state = *impl;
    return state.Validate(frame, true, error);
}

bool NativeRenderFramePrefixState::FinishTerminal(const NativeRenderFrame& tail, std::string& error)
{
    if (impl->ended || impl->failed)
    {
        error = impl->ended ? "native prefix chain already ended" : "native append validation already failed";
        return false;
    }
    impl->ended = true;
    if (!impl->passCount || tail.continuedPrefixPasses != impl->passCount)
    {
        error = "native terminal does not continue this prefix chain";
        return false;
    }
    return impl->Validate(tail, true, error);
}

bool JoinNativeRenderFrameTail(NativeRenderFrame& tail, std::vector<NativeRenderFramePass>&& prefixPasses, std::vector<NativeRenderFrameTexture> prefixTextures, bool prefixTypedGuestWords, std::string& error)
{
    // Moved passes keep their draw storage, whose addresses key inline payloads.
    static_assert(std::is_nothrow_move_constructible_v<NativeRenderFramePass>);
    if (!tail.continuedPrefixPasses || tail.continuedPrefixPasses != prefixPasses.size())
    {
        error = "native terminal does not continue this prefix chain";
        return false;
    }
    const auto prefixCount = static_cast<std::uint32_t>(prefixPasses.size());
    // Only these reservations allocate, so a throw leaves the tail unchanged.
    prefixPasses.reserve(prefixPasses.size() + tail.passes.size());
    tail.textures.reserve(tail.textures.size() + prefixTextures.size());
    for (auto& pass : tail.passes)
    {
        if (pass.colorSourcePass)
            pass.colorSourcePass += prefixCount;
        prefixPasses.push_back(std::move(pass));
    }
    tail.passes.swap(prefixPasses);
    prefixPasses.clear();
    for (auto& texture : prefixTextures)
        if (std::none_of(tail.textures.begin(), tail.textures.end(), [&](const NativeRenderFrameTexture& declared)
                         {
                             return declared.id == texture.id;
                         }))
            tail.textures.push_back(std::move(texture));
    tail.typedGuestWords |= prefixTypedGuestWords;
    tail.continuedPrefixPasses = 0;
    return true;
}

bool SplitNativeRenderFrameTail(NativeRenderFrame& frame, std::size_t prefixPasses, std::string& error)
{
    if (!prefixPasses || frame.continuedPrefixPasses || prefixPasses > frame.passes.size())
    {
        error = "native graph cannot continue this prefix chain";
        return false;
    }
    for (std::size_t index = 0; index < frame.passes.size(); ++index)
        if (frame.passes[index].colorSourcePass && (index < prefixPasses || frame.passes[index].colorSourcePass <= prefixPasses))
        {
            error = "recorded native prefix cannot be a deferred color snapshot source";
            return false;
        }
    for (auto pass = frame.passes.begin() + prefixPasses; pass != frame.passes.end(); ++pass)
        if (pass->colorSourcePass)
            pass->colorSourcePass -= static_cast<std::uint32_t>(prefixPasses);
    frame.passes.erase(frame.passes.begin(), frame.passes.begin() + prefixPasses);
    frame.continuedPrefixPasses = prefixPasses;
    return true;
}

bool ValidateNativeRenderFrame(const NativeRenderFrame& frame, std::string& error)
{
    if (frame.continuedPrefixPasses)
    {
        error = "render frame continues a prefix chain outside its prefix state";
        return false;
    }
    NativeRenderFramePrefixState::Impl state;
    return state.Validate(frame, true, error);
}

bool ValidateNativeRenderFramePrefix(const NativeRenderFrame& frame, std::string& error)
{
    NativeRenderFramePrefixState state;
    if (!state.Append(frame, error))
        return false;
    const auto target = GetNativeRenderFrameTarget(frame.passes.front());
    for (std::size_t index = 1; index < frame.passes.size(); ++index)
        if (!frame.passes[index].preservePreviousAttachments || GetNativeRenderFrameTarget(frame.passes[index]) != target)
        {
            error = "native prefix batches must preserve one compatible attachment chain";
            return false;
        }
    return true;
}

} // namespace rerevved::gpu
