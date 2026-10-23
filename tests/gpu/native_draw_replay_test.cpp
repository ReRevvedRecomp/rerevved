#include "gpu/d3d12/native_draw_replay.h"
#include "gpu/d3d12/native_sample_coverage.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <type_traits>

namespace
{

using namespace rerevved::gpu;

NativeDrawReplayRecipe makeRecipe()
{
    NativeDrawReplayRecipe recipe;
    recipe.schemaVersion = 2;
    recipe.width = recipe.height = 4;
    recipe.sampleCount           = 4;
    recipe.sampleMask            = 9;
    recipe.vertexShaderHash      = 1;
    recipe.pixelShaderHash       = 2;
    // These tests exercise recipe admission only. D3D12 validates executable
    // shader containers when the replay creates its pipeline state.
    recipe.vertexShaderDxil.resize(4);
    recipe.pixelShaderDxil.resize(4);
    recipe.vertexAttributeCount = 3;
    recipe.vertexStrideBytes    = 48;
    recipe.vertexData.resize(3 * recipe.vertexStrideBytes);
    recipe.indices    = { 0, 1, 2 };
    recipe.indexCount = 3;
    recipe.vertexConstants.resize(256 * 16);
    recipe.pixelConstants.resize(224 * 16);
    recipe.sharedConstants.resize(336);
    recipe.initialSample0.resize(64);
    recipe.initialSample1.resize(64);
    recipe.viewport          = { 0.0F, 0.0F, 4.0F, 4.0F, 0.0F, 1.0F };
    recipe.scissor           = { 0, 0, 4, 4 };
    recipe.textureMask       = 1;
    recipe.textures[0].width = recipe.textures[0].height = 4;
    recipe.textures[0].bytes.resize(64);
    return recipe;
}

void bindingStorage()
{
    std::string error;
    auto        recipe = makeRecipe();
    UNSCOPED_INFO("compact binding storage source version");
    REQUIRE(CopyNativeDrawReplayTextureVersion(recipe.textures[0], error));
    const auto texture = recipe.textures[0];
    for (const std::size_t count : { std::size_t{ 0 }, std::size_t{ 1 }, std::size_t{ 2 }, kNativeDrawReplayTextureSlots })
    {
        recipe.textures    = {};
        recipe.textureMask = count == kNativeDrawReplayTextureSlots ? std::numeric_limits<std::uint32_t>::max() : (1U << count) - 1U;
        for (std::size_t slot = 0; slot < count; ++slot)
            recipe.textures[slot] = texture;
        std::shared_ptr<const NativeDrawReplayAdmission> admission;
        UNSCOPED_INFO("supported compact binding count is admitted");
        REQUIRE(AdmitNativeDrawReplayRecipe(recipe, admission, error));
        UNSCOPED_INFO("compact binding span contains only active slots");
        REQUIRE(admission->TextureBindings().size() == count);
        const auto storageBytes = admission->MetadataBytes() - sizeof(NativeDrawReplayAdmission);
        UNSCOPED_INFO("admission metadata includes retained binding record storage");
        REQUIRE((storageBytes >= count * sizeof(NativeDrawReplayTextureBinding) && storageBytes % sizeof(NativeDrawReplayTextureBinding) == 0));
        UNSCOPED_INFO("zero and sparse texture draws avoid full-slot binding storage");
        REQUIRE((count ? (count == kNativeDrawReplayTextureSlots || storageBytes < kNativeDrawReplayTextureSlots * sizeof(NativeDrawReplayTextureBinding)) : storageBytes == 0));
        for (const auto& binding : admission->TextureBindings())
        {
            UNSCOPED_INFO("every compact binding strongly retains the immutable source layout");
            REQUIRE((binding.textureVersion == texture.textureVersion && &binding.ResourceLayout() == &texture.textureVersion->Layout()));
        }
    }
}

} // namespace

TEST_CASE("native_draw_replay_recipe", "[ctest][native-draw-replay]")
{
    UNSCOPED_INFO("guest planes retain standard host sample positions 0 and 3");
    REQUIRE((kNativeStandardSamplePositions4x[0] == std::array<std::int8_t, 2>{ -2, -6 } &&
             kNativeStandardSamplePositions4x[3] == std::array<std::int8_t, 2>{ 2, 6 }));
    UNSCOPED_INFO("centroid selection includes standard host samples 1 and 2");
    REQUIRE((kNativeStandardSamplePositions4x[1] == std::array<std::int8_t, 2>{ 6, -2 } &&
             kNativeStandardSamplePositions4x[2] == std::array<std::int8_t, 2>{ -6, 2 }));
    UNSCOPED_INFO("guest 2x centroid rasterizes all four samples");
    REQUIRE(NativeRasterSampleMask(2, 4, 9) == 15);
    UNSCOPED_INFO("explicit four-sample coverage is preserved");
    REQUIRE(NativeRasterSampleMask(2, 4, 15) == 15);
    UNSCOPED_INFO("legacy shader sample mask is preserved");
    REQUIRE(NativeRasterSampleMask(1, 4, 9) == 9);
    UNSCOPED_INFO("single-sample rasterization is preserved");
    REQUIRE(NativeRasterSampleMask(2, 1, 1) == 1);
    bindingStorage();
    static_assert(!std::is_default_constructible_v<NativeDrawReplayAdmission>);
    static_assert(!std::is_copy_constructible_v<NativeDrawReplayAdmission>);
    static_assert(!std::is_constructible_v<NativeDrawReplayAdmission, NativeDrawReplayRecipe>);
    static_assert(!std::is_default_constructible_v<NativeDrawReplayTextureVersion>);
    static_assert(!std::is_copy_constructible_v<NativeDrawReplayTextureVersion>);
    static_assert(!std::is_constructible_v<NativeDrawReplayTextureVersion, std::shared_ptr<const std::vector<std::uint8_t>>, NativeDrawReplayTextureLayout, std::vector<NativeDrawReplayTextureSubresource>>);
    static_assert(std::is_same_v<decltype(std::declval<const NativeDrawReplayAdmission&>().Recipe()), const NativeDrawReplayRecipe&>);
    std::string error;
    auto        recipe = makeRecipe();
    UNSCOPED_INFO("textured float4 input admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    {
        auto source    = std::make_shared<std::vector<std::uint8_t>>(64, 0x7F);
        auto versioned = recipe;
        versioned.textures[0].bytes.clear();
        versioned.textures[0].sharedBytes = source;
        UNSCOPED_INFO("generic version creation detaches caller-owned bytes");
        REQUIRE(CopyNativeDrawReplayTextureVersion(versioned.textures[0], error));
        const auto version = versioned.textures[0].textureVersion;
        UNSCOPED_INFO("private texture version retains its exact owner and tight layout");
        REQUIRE((version && version->Payload() == versioned.textures[0].sharedBytes && version->Payload().get() != source.get() &&
                 version->MatchesOwner(version) && version->Subresources().size() == 1 &&
                 version->Subresources()[0].sourceOffset == 0 && version->Subresources()[0].rowBytes == 16 && version->Subresources()[0].rowCount == 4));
        UNSCOPED_INFO("version charge includes the wrapper and retained subresource storage");
        REQUIRE(version->MetadataBytes() >= sizeof(NativeDrawReplayTextureVersion) + sizeof(NativeDrawReplayTextureSubresource));
        {
            // Only the version's own handle matches: not one on its control
            // block that points elsewhere, nor one on another block.
            auto other = recipe;
            UNSCOPED_INFO("a second version");
            REQUIRE(CopyNativeDrawReplayTextureVersion(other.textures[0], error));
            const std::shared_ptr<const NativeDrawReplayTextureVersion> elsewhere(version, other.textures[0].textureVersion.get());
            const std::shared_ptr<const NativeDrawReplayTextureVersion> foreign(other.textures[0].textureVersion, version.get());
            const std::shared_ptr<const NativeDrawReplayTextureVersion> unowned(std::shared_ptr<const NativeDrawReplayTextureVersion>{}, version.get());
            UNSCOPED_INFO("a texture version matches only its own handle");
            REQUIRE((!version->MatchesOwner(elsewhere) && !other.textures[0].textureVersion->MatchesOwner(elsewhere) && !version->MatchesOwner(foreign) && !version->MatchesOwner(unowned) &&
                     !version->MatchesOwner(nullptr) && other.textures[0].textureVersion->MatchesOwner(other.textures[0].textureVersion)));
            auto swapped                       = versioned;
            swapped.textures[0].textureVersion = foreign;
            UNSCOPED_INFO("a recipe with another version's handle is refused");
            REQUIRE(!ValidateNativeDrawReplayRecipe(swapped, error));
        }
        (*source)[0] = 1;
        source->clear();
        source.reset();
        UNSCOPED_INFO("source mutation and release cannot alter a detached immutable texture");
        REQUIRE((version->Payload()->size() == 64 && (*version->Payload())[0] == 0x7F));
        std::shared_ptr<const NativeDrawReplayAdmission> admission;
        UNSCOPED_INFO("immutable texture leaf admission");
        REQUIRE(AdmitNativeDrawReplayRecipe(versioned, admission, error));
        const auto& binding = admission->TextureBindings()[0];
        UNSCOPED_INFO("admitted binding references one resource layout without copying mip records");
        REQUIRE((admission->TextureBindings().size() == 1 && binding.source == NativeDrawReplayTextureSource::CpuPayload &&
                 binding.textureVersion == version && &binding.ResourceLayout() == &version->Layout()));
        auto invalid                    = versioned;
        invalid.textures[0].sharedBytes = std::make_shared<const std::vector<std::uint8_t>>(64, 0x7F);
        UNSCOPED_INFO("same-size bytes cannot inherit another texture version");
        REQUIRE(!ValidateNativeDrawReplayRecipe(invalid, error));
        invalid                         = versioned;
        invalid.textures[0].sharedBytes = std::shared_ptr<const std::vector<std::uint8_t>>(version->Payload().get(), [](auto*)
                                                                                           {
                                                                                           });
        UNSCOPED_INFO("same payload address with another control block is rejected");
        REQUIRE(!ValidateNativeDrawReplayRecipe(invalid, error));
        invalid                            = versioned;
        invalid.textures[0].textureVersion = std::shared_ptr<const NativeDrawReplayTextureVersion>(version.get(), [](auto*)
                                                                                                   {
                                                                                                   });
        UNSCOPED_INFO("non-owning version wrapper cannot inherit canonical ownership");
        REQUIRE(!ValidateNativeDrawReplayRecipe(invalid, error));
        for (unsigned field = 0; field < 5; ++field)
        {
            invalid       = versioned;
            auto& texture = invalid.textures[0];
            if (field == 0)
                texture.width = 8;
            else if (field == 1)
                texture.height = 8;
            else if (field == 2)
                texture.format = NativeDrawReplayTextureFormat::R32Float;
            else if (field == 3)
                texture.mipLevels = 2;
            else
                texture.dimension = NativeDrawReplayTextureDimension::TextureCube;
            UNSCOPED_INFO("every storage layout field must match the immutable version");
            REQUIRE(!ValidateNativeDrawReplayRecipe(invalid, error));
        }
        invalid = versioned;
        invalid.textures[0].sharedBytes.reset();
        UNSCOPED_INFO("immutable version requires its effective shared payload");
        REQUIRE(!ValidateNativeDrawReplayRecipe(invalid, error));
        invalid                   = versioned;
        invalid.textures[0].bytes = { 0 };
        UNSCOPED_INFO("immutable version excludes legacy CPU mixture");
        REQUIRE(!ValidateNativeDrawReplayRecipe(invalid, error));
        invalid                            = versioned;
        invalid.textures[0].frameTextureId = 7;
        UNSCOPED_INFO("immutable CPU version excludes frame resource pairing");
        REQUIRE(!ValidateNativeDrawReplayRecipe(invalid, error));
        invalid             = versioned;
        invalid.textureMask = 0;
        UNSCOPED_INFO("unbound immutable version is rejected");
        REQUIRE(!ValidateNativeDrawReplayRecipe(invalid, error));
        invalid.textures[0].sharedBytes.reset();
        UNSCOPED_INFO("unbound version alone is rejected");
        REQUIRE(!ValidateNativeDrawReplayRecipe(invalid, error));
        auto changed                   = versioned;
        changed.textures[0].swizzle    = { 2, 1, 0, 3 };
        changed.samplers               = { changed.sampler };
        changed.samplers[0].minLinear  = true;
        changed.samplers[0].address[0] = 1;
        std::shared_ptr<const NativeDrawReplayAdmission> changedAdmission;
        UNSCOPED_INFO("independent draw bindings retain different swizzles and effective samplers for one version");
        REQUIRE((AdmitNativeDrawReplayRecipe(changed, changedAdmission, error) &&
                 changedAdmission->TextureBindings()[0].textureVersion == version &&
                 changedAdmission->TextureBindings()[0].swizzle == changed.textures[0].swizzle &&
                 changedAdmission->TextureBindings()[0].sampler.minLinear && changedAdmission->TextureBindings()[0].sampler.address[0] == 1 &&
                 admission->TextureBindings()[0].swizzle == recipe.textures[0].swizzle && !admission->TextureBindings()[0].sampler.minLinear));
        changed.textureMask         = 3;
        changed.textures[1]         = changed.textures[0];
        changed.textures[1].swizzle = { 0, 0, 0, 5 };
        changed.samplers.push_back(recipe.sampler);
        UNSCOPED_INFO("prepared bindings preserve packed order and per-slot sampler selection");
        REQUIRE((AdmitNativeDrawReplayRecipe(changed, changedAdmission, error) && changedAdmission->TextureBindings().size() == 2 &&
                 changedAdmission->TextureBindings()[1].textureVersion == version &&
                 changedAdmission->TextureBindings()[1].swizzle == changed.textures[1].swizzle &&
                 !changedAdmission->TextureBindings()[1].sampler.minLinear));
        UNSCOPED_INFO("producer replacement creates a fresh owned version");
        REQUIRE((CopyNativeDrawReplayTextureVersion(versioned.textures[0], error) && versioned.textures[0].textureVersion != version));
        versioned = {};
        UNSCOPED_INFO("earlier admission survives producer replacement and reset");
        REQUIRE((admission->Recipe().textures[0].textureVersion == version && (*admission->Recipe().textures[0].payloadBytes().data()) == 0x7F));
        auto native = recipe;
        native.textures[0].bytes.clear();
        native.textures[0].frameTextureId = 7;
        native.sampler.magLinear          = true;
        UNSCOPED_INFO("prepared frame bindings retain their native ID and shared effective sampler");
        REQUIRE((AdmitNativeDrawReplayRecipe(native, changedAdmission, error) &&
                 changedAdmission->TextureBindings()[0].source == NativeDrawReplayTextureSource::FrameTexture &&
                 changedAdmission->TextureBindings()[0].frameTextureId == 7 && changedAdmission->TextureBindings()[0].sampler.magLinear));
    }
    {
        std::shared_ptr<const NativeDrawReplayAdmission> admission;
        auto                                             owned     = recipe;
        const auto*                                      indices   = owned.indices.data();
        const auto*                                      constants = owned.vertexConstants.data();
        UNSCOPED_INFO("admission transfers owned vectors into one const recipe");
        REQUIRE((AdmitNativeDrawReplayRecipe(std::move(owned), admission, error) &&
                 admission->Recipe().indices.data() == indices && admission->Recipe().vertexConstants.data() == constants));
        const auto borrowed = recipe;
        UNSCOPED_INFO("const raw input keeps independent owned vectors");
        REQUIRE((AdmitNativeDrawReplayRecipe(borrowed, admission, error) &&
                 admission->Recipe().indices == borrowed.indices && admission->Recipe().indices.data() != borrowed.indices.data()));
        const auto pipelineState = admission->PipelineStateKey();
        owned                    = recipe;
        owned.viewport.x         = 0.25F;
        owned.scissor.right      = 3;
        owned.stencil.reference  = 7;
        owned.vertexConstants[0] ^= 1;
        UNSCOPED_INFO("ordered dynamic bindings leave the prepared pipeline state unchanged");
        REQUIRE((AdmitNativeDrawReplayRecipe(owned, admission, error) && admission->PipelineStateKey() == pipelineState));
        owned.blend.enabled     = true;
        owned.blend.sourceColor = NativeDrawReplayBlendFactor::SourceAlpha;
        UNSCOPED_INFO("admission prepares changed immutable blend state before recording");
        REQUIRE((AdmitNativeDrawReplayRecipe(std::move(owned), admission, error) && admission->PipelineStateKey() != pipelineState));
        owned                      = recipe;
        owned.rasterizer.depthClip = false;
        UNSCOPED_INFO("depth clipping is immutable pipeline state");
        REQUIRE((AdmitNativeDrawReplayRecipe(std::move(owned), admission, error) && admission->PipelineStateKey() != pipelineState));
        owned            = recipe;
        owned.indices[0] = 100;
        UNSCOPED_INFO("the validating factory rejects malformed leaves and clears its output");
        REQUIRE((!AdmitNativeDrawReplayRecipe(std::move(owned), admission, error) && !admission));
        // The single-validation factory keeps a refused recipe and admits
        // what the validating factory does.
        owned            = recipe;
        owned.indices[0] = 100;
        UNSCOPED_INFO("a refused recipe stays with the caller");
        REQUIRE((!TryAdmitNativeDrawReplayRecipe(owned, admission, error) && !admission && !error.empty() && owned.indices[0] == 100 &&
                 owned.indices.size() == recipe.indices.size()));
        std::shared_ptr<const NativeDrawReplayAdmission> validated, tried;
        auto                                             first = recipe, second = recipe;
        const auto*                                      moved = second.indices.data();
        UNSCOPED_INFO("an admitted recipe moves into an admission equal to the validating factory's");
        REQUIRE((AdmitNativeDrawReplayRecipe(std::move(first), validated, error) && TryAdmitNativeDrawReplayRecipe(second, tried, error) &&
                 tried->Recipe().indices.data() == moved && tried->PipelineStateKey() == validated->PipelineStateKey() &&
                 tried->TextureBindings().size() == validated->TextureBindings().size() && tried->MetadataBytes() == validated->MetadataBytes()));
        for (unsigned payload = 0; payload < 4; ++payload)
        {
            owned = recipe;
            std::shared_ptr<std::vector<std::uint8_t>> shared;
            if (payload == 0)
            {
                shared                       = std::make_shared<std::vector<std::uint8_t>>(std::move(owned.vertexShaderDxil));
                owned.sharedVertexShaderDxil = shared;
            }
            else if (payload == 1)
            {
                shared                      = std::make_shared<std::vector<std::uint8_t>>(std::move(owned.pixelShaderDxil));
                owned.sharedPixelShaderDxil = shared;
            }
            else if (payload == 2)
            {
                shared                 = std::make_shared<std::vector<std::uint8_t>>(std::move(owned.vertexData));
                owned.sharedVertexData = shared;
            }
            else
            {
                shared                        = std::make_shared<std::vector<std::uint8_t>>(std::move(owned.textures[0].bytes));
                owned.textures[0].sharedBytes = shared;
            }
            UNSCOPED_INFO("shared payload extents are admitted with their strong owners");
            REQUIRE((AdmitNativeDrawReplayRecipe(std::move(owned), admission, error) && admission->ValidateSharedPayloads(error)));
            (*shared)[0] ^= 1;
            UNSCOPED_INFO("extent receipts leave same-size identity to current byte hashing");
            REQUIRE(admission->ValidateSharedPayloads(error));
            shared->pop_back();
            UNSCOPED_INFO("a mutable shared alias cannot change an admitted extent");
            REQUIRE(!admission->ValidateSharedPayloads(error));
        }
    }
    auto sharedShaders                   = recipe;
    sharedShaders.sharedVertexShaderDxil = std::make_shared<const std::vector<std::uint8_t>>(std::move(sharedShaders.vertexShaderDxil));
    sharedShaders.sharedPixelShaderDxil  = std::make_shared<const std::vector<std::uint8_t>>(std::move(sharedShaders.pixelShaderDxil));
    UNSCOPED_INFO("immutable owned shader admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(sharedShaders, error));
    sharedShaders.vertexShaderDxil = { 1 };
    UNSCOPED_INFO("ambiguous shared and mutable vertex bytecode rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(sharedShaders, error));
    sharedShaders.vertexShaderDxil.clear();
    sharedShaders.pixelShaderDxil = { 1 };
    UNSCOPED_INFO("ambiguous shared and mutable pixel bytecode rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(sharedShaders, error));
    sharedShaders.pixelShaderDxil.clear();
    sharedShaders.sharedPixelShaderDxil = std::make_shared<const std::vector<std::uint8_t>>(4U * 1024U * 1024U + 4U);
    UNSCOPED_INFO("shared bytecode retains the per-shader input bound");
    REQUIRE(!ValidateNativeDrawReplayRecipe(sharedShaders, error));
    auto sharedGeometry             = recipe;
    sharedGeometry.sharedVertexData = std::make_shared<const std::vector<std::uint8_t>>(std::move(sharedGeometry.vertexData));
    UNSCOPED_INFO("owned immutable shared geometry admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(sharedGeometry, error));
    sharedGeometry.vertexData = { 1 };
    UNSCOPED_INFO("ambiguous shared and mutable geometry rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(sharedGeometry, error));
    sharedGeometry.vertexData.clear();
    sharedGeometry.indices[0] = static_cast<std::uint32_t>(sharedGeometry.vertexBytes().size() / sharedGeometry.vertexStrideBytes);
    UNSCOPED_INFO("indices remain bounded by the shared geometry extent");
    REQUIRE(!ValidateNativeDrawReplayRecipe(sharedGeometry, error));
    // Shared indices are bounded by the largest index their owner found.
    auto sharedIndices          = recipe;
    sharedIndices.sharedIndices = std::make_shared<const NativeDrawReplayIndices>(std::move(sharedIndices.indices));
    sharedIndices.indices.clear();
    UNSCOPED_INFO("owned immutable shared indices admission");
    REQUIRE((ValidateNativeDrawReplayRecipe(sharedIndices, error) && sharedIndices.sharedIndices->Maximum() == 2));
    sharedIndices.indices = { 0, 1, 2 };
    UNSCOPED_INFO("ambiguous shared and mutable indices rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(sharedIndices, error));
    sharedIndices.indices.clear();
    const std::array<const char*, 3> pastVertices{ "a first shared index past the vertices is rejected", "a middle shared index past the vertices is rejected", "a last shared index past the vertices is rejected" };
    for (std::size_t at = 0; at < pastVertices.size(); ++at)
    {
        std::vector<std::uint32_t> values{ 0, 1, 2 };
        values[at]                  = 3;
        sharedIndices.sharedIndices = std::make_shared<const NativeDrawReplayIndices>(std::move(values));
        UNSCOPED_INFO(pastVertices[at]);
        REQUIRE(!ValidateNativeDrawReplayRecipe(sharedIndices, error));
    }
    sharedIndices.sharedIndices = std::make_shared<const NativeDrawReplayIndices>(std::vector<std::uint32_t>{ 0, 1, 2, 2, 1, 0 });
    UNSCOPED_INFO("shared indices must match the index count");
    REQUIRE(!ValidateNativeDrawReplayRecipe(sharedIndices, error));
    recipe.initialSample0.clear();
    recipe.initialSample1.clear();
    UNSCOPED_INFO("schema two native clear admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.initialSample0.resize(64);
    UNSCOPED_INFO("partial captured color initialization rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe            = makeRecipe();
    recipe.sampleMask = 15;
    UNSCOPED_INFO("schema two full raster coverage admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.sampleMask = 3;
    UNSCOPED_INFO("unsupported sample layout rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe             = makeRecipe();
    recipe.sampleCount = recipe.sampleMask = 1;
    UNSCOPED_INFO("single-sample target admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.sampleMask = 9;
    UNSCOPED_INFO("multisample mask rejected for single-sample target");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.sampleMask        = 1;
    recipe.initialSample1[0] = 1;
    UNSCOPED_INFO("conflicting single-sample color planes rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.initialSample1[0] = 0;
    recipe.depth.enabled     = true;
    recipe.depth.initialSamples.resize(128);
    UNSCOPED_INFO("single-sample depth initialization admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.depth.initialSamples[65] = 1;
    UNSCOPED_INFO("conflicting single-sample depth planes rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe         = makeRecipe();
    recipe.scissor = { 2, 2, 2, 2 };
    UNSCOPED_INFO("empty HUD clipping rectangle admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.scissor.right = 1;
    UNSCOPED_INFO("inverted clipping rectangle rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe              = makeRecipe();
    recipe.targetFormat = NativeDrawReplayTargetFormat::Rgb10a2;
    UNSCOPED_INFO("RGB10 output admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.depth.enabled = true;
    UNSCOPED_INFO("mixed packed depth and RGB10 output rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe              = makeRecipe();
    recipe.targetFormat = NativeDrawReplayTargetFormat::Rg16Snorm;
    recipe.sampleCount = recipe.sampleMask = 1;
    UNSCOPED_INFO("signed two-channel target admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.depth.enabled = true;
    UNSCOPED_INFO("signed target with unsupported packed depth initialization rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe               = makeRecipe();
    recipe.depth.enabled = recipe.depth.writeEnabled = true;
    recipe.rasterizer.cull                           = 2;
    recipe.depth.initialSamples.resize(128);
    UNSCOPED_INFO("packed D24 depth input admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.depth.initialSamples[0] = 1;
    UNSCOPED_INFO("uncaptured stencil semantics rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.depth.initialSamples[0] = 0;
    recipe.depth.initialSamples.pop_back();
    UNSCOPED_INFO("incomplete depth plane rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.depth.initialSamples.clear();
    recipe.depth.initialClear = std::numeric_limits<float>::quiet_NaN();
    UNSCOPED_INFO("non-finite depth clear rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.depth.initialClear = 1.0F;
    recipe.depth.compare      = 9;
    UNSCOPED_INFO("invalid depth comparison rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.depth.compare = 4;
    recipe.depth.enabled = false;
    UNSCOPED_INFO("depth writes without testing rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    for (auto format : { NativeDrawReplayTextureFormat::Rgba8,
                         NativeDrawReplayTextureFormat::R8,
                         NativeDrawReplayTextureFormat::Bc1,
                         NativeDrawReplayTextureFormat::Bc2,
                         NativeDrawReplayTextureFormat::Bc3,
                         NativeDrawReplayTextureFormat::Rgba16Float,
                         NativeDrawReplayTextureFormat::Rg16Float,
                         NativeDrawReplayTextureFormat::R32Float })
    {
        recipe                    = makeRecipe();
        recipe.textures[0].format = format;
        const auto payloadBytes   = format == NativeDrawReplayTextureFormat::Rgba8 ? 64 : format == NativeDrawReplayTextureFormat::R8                                                ? 16
                                                                                      : format == NativeDrawReplayTextureFormat::Bc1                                                 ? 8
                                                                                      : format == NativeDrawReplayTextureFormat::Bc2 || format == NativeDrawReplayTextureFormat::Bc3 ? 16
                                                                                      : format == NativeDrawReplayTextureFormat::Rgba16Float                                         ? 128
                                                                                                                                                                                     : 64;
        recipe.textures[0].bytes.resize(payloadBytes);
        UNSCOPED_INFO("texture format payload admission");
        REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
        recipe.textures[0].bytes.pop_back();
        UNSCOPED_INFO("truncated texture rejection");
        REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    }
    recipe                       = makeRecipe();
    recipe.textures[0].mipLevels = 3;
    recipe.textures[0].bytes.resize(64 + 16 + 4);
    UNSCOPED_INFO("complete 2D mip chain admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.textures[0].mipLevels = 4;
    UNSCOPED_INFO("mip count beyond the extent rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe                       = makeRecipe();
    recipe.textures[0].mipLevels = 0;
    UNSCOPED_INFO("zero mip count rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe                       = makeRecipe();
    recipe.textures[0].dimension = NativeDrawReplayTextureDimension::TextureCube;
    recipe.textures[0].mipLevels = 2;
    recipe.textures[0].bytes.resize((64 + 16) * 6);
    UNSCOPED_INFO("complete cube mip chain admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.textures[0].bytes.pop_back();
    UNSCOPED_INFO("cube subresource payload mismatch rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.textures[0].bytes.resize((64 + 16) * 6);
    recipe.textures[0].width = 8;
    UNSCOPED_INFO("non-square cube rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.textures[0].width     = 4;
    recipe.textures[0].dimension = static_cast<NativeDrawReplayTextureDimension>(99);
    UNSCOPED_INFO("unsupported texture dimension rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe = makeRecipe();
    recipe.textures[0].bytes.clear();
    recipe.textures[0].sharedBytes = std::make_shared<const std::vector<std::uint8_t>>(64, 0x7F);
    UNSCOPED_INFO("shared texture payload admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.textures[0].bytes.assign(64, 0x7F);
    UNSCOPED_INFO("legacy and shared texture payloads rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe                   = makeRecipe();
    recipe.textures[0].width = 4097;
    recipe.textures[0].bytes.resize(4097ULL * 4);
    UNSCOPED_INFO("texture extent above 4096 rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe                     = makeRecipe();
    recipe.textures[0].swizzle = { 5, 5, 5, 0 };
    UNSCOPED_INFO("constant component mapping admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.textures[0].swizzle[0] = 6;
    UNSCOPED_INFO("invalid component selector rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe                    = makeRecipe();
    recipe.sampler.address[0] = 0;
    UNSCOPED_INFO("foreign sampler enum rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe                    = makeRecipe();
    recipe.sampler.anisotropy = 1;
    UNSCOPED_INFO("anisotropic 1x sampler admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.sampler.anisotropy = 5;
    UNSCOPED_INFO("anisotropic 16x sampler admission");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.sampler.anisotropy = 6;
    UNSCOPED_INFO("unsupported sampler anisotropy rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe                   = makeRecipe();
    recipe.sampler.border[0] = std::numeric_limits<float>::quiet_NaN();
    UNSCOPED_INFO("non-finite sampler rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe                   = makeRecipe();
    recipe.vertexStrideBytes = 32;
    UNSCOPED_INFO("attribute stride mismatch rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe             = makeRecipe();
    recipe.textureMask = 2;
    UNSCOPED_INFO("unbound texture slot rejection");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe             = makeRecipe();
    recipe.textureMask = 7;
    recipe.textures[1] = recipe.textures[0];
    UNSCOPED_INFO("missing third movie plane rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.textures[2] = recipe.textures[0];
    UNSCOPED_INFO("three complete texture planes admitted");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.textures[2].bytes.pop_back();
    UNSCOPED_INFO("truncated third plane rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.textureMask = 1;
    UNSCOPED_INFO("unused plane payload rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe             = makeRecipe();
    recipe.textureMask = (1U << 13) - 1;
    for (std::size_t slot = 1; slot < 13; ++slot)
        recipe.textures[slot] = recipe.textures[0];
    recipe.samplers.resize(13);
    UNSCOPED_INFO("thirteen gameplay texture bindings admitted");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.samplers.back().address[0] = 0;
    UNSCOPED_INFO("last independent sampler validated");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.samplers.back().address[0] = 3;
    recipe.samplers.pop_back();
    UNSCOPED_INFO("missing independent sampler rejected");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.samplers.clear();
    recipe.textureMask = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t slot = 13; slot < recipe.textures.size(); ++slot)
        recipe.textures[slot] = recipe.textures[0];
    UNSCOPED_INFO("all guest texture slots admitted");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.textures.back().bytes.clear();
    UNSCOPED_INFO("last texture slot payload validated");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe               = makeRecipe();
    recipe.schemaVersion = 1;
    UNSCOPED_INFO("schema one subset remains restricted");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.textureMask = 0;
    recipe.textures[0].bytes.clear();
    recipe.vertexShaderHash     = 0x11213E38D7154104ULL;
    recipe.pixelShaderHash      = 0x3A92D78FE55C7B83ULL;
    recipe.vertexAttributeCount = 2;
    recipe.vertexStrideBytes    = 32;
    recipe.vertexData.resize(96);
    UNSCOPED_INFO("schema one panel remains admitted");
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.rasterizer.depthClip = false;
    UNSCOPED_INFO("schema one keeps depth clipping");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
    recipe.rasterizer.depthClip = true;
    recipe.sampleMask           = 15;
    UNSCOPED_INFO("schema one sample mask remains restricted");
    REQUIRE(!ValidateNativeDrawReplayRecipe(recipe, error));
}
