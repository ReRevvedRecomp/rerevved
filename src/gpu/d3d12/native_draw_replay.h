#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rex::ui::d3d12
{

class GpuGuestOutputTicket;

}

namespace rerevved::gpu
{

// Xenos texture fetch constants occupy 32 slots. Recipes pack used bindings
// into descriptor order; the shader translation owns the guest-slot mapping.
inline constexpr std::size_t kNativeDrawReplayTextureSlots = 32;

enum class NativeDrawReplayBlendFactor : std::uint8_t
{
    Zero,
    One,
    SourceColor,
    InverseSourceColor,
    SourceAlpha,
    InverseSourceAlpha,
    DestinationAlpha,
    InverseDestinationAlpha,
    DestinationColor,
    InverseDestinationColor,
    SourceAlphaSaturated,
    BlendFactor,
    InverseBlendFactor,
};

enum class NativeDrawReplayBlendOp : std::uint8_t
{
    Add,
    Subtract,
    ReverseSubtract,
    Minimum,
    Maximum,
};

struct NativeDrawReplayViewport
{
    float x        = 0.0F;
    float y        = 0.0F;
    float width    = 0.0F;
    float height   = 0.0F;
    float minDepth = 0.0F;
    float maxDepth = 1.0F;
};

struct NativeDrawReplayScissor
{
    std::int32_t left   = 0;
    std::int32_t top    = 0;
    std::int32_t right  = 0;
    std::int32_t bottom = 0;
};

struct NativeDrawReplayBlendState
{
    bool                        enabled          = false;
    bool                        alphaToCoverage  = false;
    bool                        logicOpEnabled   = false;
    NativeDrawReplayBlendFactor sourceColor      = NativeDrawReplayBlendFactor::One;
    NativeDrawReplayBlendFactor destinationColor = NativeDrawReplayBlendFactor::Zero;
    NativeDrawReplayBlendOp     colorOp          = NativeDrawReplayBlendOp::Add;
    NativeDrawReplayBlendFactor sourceAlpha      = NativeDrawReplayBlendFactor::One;
    NativeDrawReplayBlendFactor destinationAlpha = NativeDrawReplayBlendFactor::Zero;
    NativeDrawReplayBlendOp     alphaOp          = NativeDrawReplayBlendOp::Add;
    std::uint8_t                writeMask        = 0x0F;

    bool operator==(const NativeDrawReplayBlendState&) const = default;
};

enum class NativeDrawReplayTextureFormat : std::uint8_t
{
    Rgba8,
    R8,
    Bc1,
    Bc2,
    Bc3,
    Rgba16Float,
    Rg16Float,
    R32Float,
    Rgb10a2,
    // Guest-format words a resolve packed; never sampled.
    R32Uint,
};

enum class NativeDrawReplayTextureDimension : std::uint8_t
{
    Texture2D,
    TextureCube,
};

struct NativeDrawReplayTextureLayout
{
    std::uint32_t                    width = 0, height = 0;
    NativeDrawReplayTextureFormat    format                                                 = NativeDrawReplayTextureFormat::Rgba8;
    std::uint32_t                    mipLevels                                              = 1;
    NativeDrawReplayTextureDimension dimension                                              = NativeDrawReplayTextureDimension::Texture2D;
    bool                             operator==(const NativeDrawReplayTextureLayout&) const = default;
};

struct NativeDrawReplayTextureSubresource
{
    std::uint32_t arraySlice = 0, mipLevel = 0;
    std::size_t   sourceOffset = 0, rowBytes = 0;
    std::uint32_t rowCount = 0;
};

bool SizeNativeDrawReplayTextureLayout(const NativeDrawReplayTextureLayout& layout, std::size_t& bytes, std::size_t& subresources) noexcept;

struct NativeDrawReplayTexture;

class NativeDrawReplayTextureVersion
{
public:
    const std::shared_ptr<const std::vector<std::uint8_t>>& Payload() const noexcept
    {
        return payload_;
    }

    const NativeDrawReplayTextureLayout& Layout() const noexcept
    {
        return layout_;
    }

    std::span<const NativeDrawReplayTextureSubresource> Subresources() const noexcept
    {
        return subresources_;
    }

    std::size_t MetadataBytes() const noexcept
    {
        return sizeof(*this) + subresources_.capacity() * sizeof(NativeDrawReplayTextureSubresource) + payload_->capacity() - payload_->size();
    }

    std::size_t SubresourceCapacity() const noexcept
    {
        return subresources_.capacity();
    }

    // Whether `owner` is this version's own handle. A live handle on owner_'s
    // control block that points here is one, so no lock is needed.
    bool MatchesOwner(const std::shared_ptr<const NativeDrawReplayTextureVersion>& owner) const noexcept
    {
        return owner.get() == this && !owner_.owner_before(owner) && !owner.owner_before(owner_);
    }

private:
    NativeDrawReplayTextureVersion(const NativeDrawReplayTextureVersion&)            = delete;
    NativeDrawReplayTextureVersion& operator=(const NativeDrawReplayTextureVersion&) = delete;
    friend bool                     CopyNativeDrawReplayTextureVersion(NativeDrawReplayTexture&, std::string&);
    friend bool                     PackNativeDrawReplayTextureVersion(const NativeDrawReplayTextureLayout&, std::span<const std::span<const std::uint8_t>>, NativeDrawReplayTexture&, std::string&);
    friend bool                     DecodeNativeTextureVersioned(const std::array<std::uint32_t, 6>&, std::span<const std::uint8_t>, std::span<const std::uint8_t>, NativeDrawReplayTexture&, std::string&);
    static bool                     AdoptStaging(NativeDrawReplayTexture&, std::string&);
    NativeDrawReplayTextureVersion(std::shared_ptr<const std::vector<std::uint8_t>> payload,
                                   NativeDrawReplayTextureLayout                    layout,
                                   std::vector<NativeDrawReplayTextureSubresource>  subresources);
    const std::shared_ptr<const std::vector<std::uint8_t>> payload_;
    const NativeDrawReplayTextureLayout                    layout_;
    const std::vector<NativeDrawReplayTextureSubresource>  subresources_;
    std::weak_ptr<const NativeDrawReplayTextureVersion>    owner_;
};

struct NativeDrawReplayTexture
{
    std::vector<std::uint8_t>     bytes;
    std::uint32_t                 width  = 0;
    std::uint32_t                 height = 0;
    NativeDrawReplayTextureFormat format = NativeDrawReplayTextureFormat::Rgba8;
    // Component selectors: RGBA=0..3, constant zero=4, constant one=5.
    std::array<std::uint8_t, 4> swizzle = { 0, 1, 2, 3 };
    // Packed subresources are ordered by array slice, then mip level. The
    // default preserves the legacy single-level 2D recipe contract.
    std::uint32_t                    mipLevels = 1;
    NativeDrawReplayTextureDimension dimension = NativeDrawReplayTextureDimension::Texture2D;
    // Shared ownership alone does not prove immutable contents.
    std::shared_ptr<const std::vector<std::uint8_t>> sharedBytes;

    // Nonzero references a texture produced by resolves in NativeRenderFrame.
    // It is mutually exclusive with CPU payloads and scoped to that frame.
    std::uint64_t                                         frameTextureId = 0;
    std::shared_ptr<const NativeDrawReplayTextureVersion> textureVersion;

    std::span<const std::uint8_t> payloadBytes() const noexcept
    {
        const auto* payload = sharedBytes ? sharedBytes.get() : &bytes;
        return { payload->data(), payload->size() };
    }
};

// General caller-owned payloads are copied into a fresh const allocation.
bool CopyNativeDrawReplayTextureVersion(NativeDrawReplayTexture& texture, std::string& error);
bool PackNativeDrawReplayTextureVersion(const NativeDrawReplayTextureLayout&           layout,
                                        std::span<const std::span<const std::uint8_t>> subresources,
                                        NativeDrawReplayTexture&                       texture,
                                        std::string&                                   error);

struct NativeDrawReplaySampler
{
    bool minLinear = false;
    bool magLinear = false;
    bool mipLinear = false;
    // Xenos AnisoFilter enum: disabled=0, max1to1=1 through max16to1=5.
    // The value is retained even when it is max1to1: disabled and anisotropic
    // 1x use different host filter modes.
    std::uint8_t anisotropy = 0;
    // D3D texture address modes: wrap, mirror, clamp, border, mirror-once.
    std::array<std::uint32_t, 3> address = { 3, 3, 3 };
    std::array<float, 4>         border  = {};
    float                        minLod  = 0.0F;
    float                        maxLod  = 0.0F;
    float                        mipBias = 0.0F;
};

enum class NativeDrawReplayTextureSource : std::uint8_t
{
    CpuPayload,
    FrameTexture,
};

struct NativeDrawReplayTextureBinding
{
    NativeDrawReplayTextureSource                         source = NativeDrawReplayTextureSource::CpuPayload;
    NativeDrawReplayTextureLayout                         layout;
    std::shared_ptr<const NativeDrawReplayTextureVersion> textureVersion;
    std::uint64_t                                         frameTextureId = 0;
    std::array<std::uint8_t, 4>                           swizzle        = { 0, 1, 2, 3 };
    NativeDrawReplaySampler                               sampler;

    const NativeDrawReplayTextureLayout& ResourceLayout() const noexcept
    {
        return textureVersion ? textureVersion->Layout() : layout;
    }
};

struct NativeDrawReplayDepthState
{
    bool enabled      = false;
    bool writeEnabled = false;
    // D3D comparison values: never=1 through always=8.
    std::uint32_t compare      = 4;
    float         initialClear = 1.0F;
    // Optional two planes of little-endian (depth24 << 8), with zero stencil.
    std::vector<std::uint8_t> initialSamples;
};

struct NativeDrawReplayRasterizer
{
    // none=0, front=1, back=2.
    std::uint32_t cull                  = 0;
    bool          frontCounterClockwise = false;
    // D3D12 DepthClipEnable. False keeps geometry outside the depth planes and
    // clamps its depth, as the SDK does for PA_CL_CLIP_CNTL.clip_disable.
    bool depthClip = true;
};

struct NativeDrawReplayStencilState
{
    struct Face
    {
        // D3D values: comparison never=1..always=8, operations keep=1..decrement=8.
        std::uint8_t compare   = 8;
        std::uint8_t fail      = 1;
        std::uint8_t depthFail = 1;
        std::uint8_t pass      = 1;
    };

    bool         enabled   = false;
    std::uint8_t reference = 0;
    std::uint8_t readMask  = 0xFF;
    std::uint8_t writeMask = 0xFF;
    Face         front;
    Face         back;
};

enum class NativeDrawReplayTargetFormat : std::uint8_t
{
    Rgba8,
    Rgb10a2,
    // Host-normalized storage for Xenos fixed-point 16_16 targets. The
    // translated shader owns the guest output scale; this is signed [-1, 1].
    Rg16Snorm,
};

// One additional color target of a multiple-render-target draw.
struct NativeDrawReplayExtraTarget
{
    NativeDrawReplayTargetFormat format = NativeDrawReplayTargetFormat::Rgba8;
    // RB_BLENDCONTROLk and RB_COLOR_MASK nibble k of the guest draw.
    NativeDrawReplayBlendState blend;
    friend bool                operator==(const NativeDrawReplayExtraTarget&, const NativeDrawReplayExtraTarget&) = default;
};

class NativeGameplayShaderValidationCache;

// Only exact shader validation creates versions backed by const allocations.
class NativeDrawReplayShaderVersion
{
public:
    const std::shared_ptr<const std::vector<std::uint8_t>>& VertexBytecode() const noexcept
    {
        return vertexBytecode_;
    }

    const std::shared_ptr<const std::vector<std::uint8_t>>& PixelBytecode() const noexcept
    {
        return pixelBytecode_;
    }

private:
    friend class NativeGameplayShaderValidationCache;
    NativeDrawReplayShaderVersion(const NativeDrawReplayShaderVersion&)            = delete;
    NativeDrawReplayShaderVersion& operator=(const NativeDrawReplayShaderVersion&) = delete;

    NativeDrawReplayShaderVersion(std::shared_ptr<const std::vector<std::uint8_t>> vertex,
                                  std::shared_ptr<const std::vector<std::uint8_t>> pixel)
    : vertexBytecode_(std::move(vertex))
    , pixelBytecode_(std::move(pixel))
    {
    }

    const std::shared_ptr<const std::vector<std::uint8_t>> vertexBytecode_, pixelBytecode_;
};

struct NativeDrawReplayPipelineStateKey
{
    // Sample mask/count, input/root layout, target, cull and depth comparison.
    std::array<std::uint32_t, 7> scalars{};
    // Blend, rasterizer, depth and stencil fields exclude dynamic references;
    // the last marks a depth-only pipeline, which has no pixel shader.
    std::array<std::uint8_t, 26> flags{};
    // Extra target count, then per extra target its format and blend fields.
    // All zero for a single-target draw, so those keys are unchanged.
    std::array<std::uint8_t, 34> extraTargets{};
    bool                         operator==(const NativeDrawReplayPipelineStateKey&) const = default;
};

// Immutable triangle-list indices that recipes share, with their largest
// index found once at construction, so validation need not scan them again.
class NativeDrawReplayIndices
{
public:
    explicit NativeDrawReplayIndices(std::vector<std::uint32_t> values) noexcept
    : values_(std::move(values))
    , maximum_(values_.empty() ? 0 : *std::ranges::max_element(values_))
    {
    }

    std::span<const std::uint32_t> Values() const noexcept
    {
        return values_;
    }

    std::uint32_t Maximum() const noexcept
    {
        return maximum_;
    }

    std::size_t Capacity() const noexcept
    {
        return values_.capacity();
    }

private:
    const std::vector<std::uint32_t> values_;
    const std::uint32_t              maximum_;
};

// The recipe is deliberately made of host-independent bytes and scalar state.
// All byte files are little-endian and are bounded by LoadNativeDrawReplayRecipe.
struct NativeDrawReplayRecipe
{
    std::uint32_t         schemaVersion = 1;
    std::filesystem::path sourcePath;
    // The caller supplies a fresh output path before replay.
    std::filesystem::path outputPath;

    std::vector<std::uint8_t> vertexShaderDxil;
    std::vector<std::uint8_t> pixelShaderDxil;
    std::vector<std::uint8_t> vertexData;
    // Reused geometry owns immutable decoded bytes across frame recipe copies.
    // A recipe uses either this payload or vertexData, never both.
    std::shared_ptr<const std::vector<std::uint8_t>> sharedVertexData;

    std::span<const std::uint8_t> vertexBytes() const noexcept
    {
        const auto& bytes = sharedVertexData ? *sharedVertexData : vertexData;
        return { bytes.data(), bytes.size() };
    }

    std::vector<std::uint32_t> indices;
    // Reused geometry's indices; a recipe uses either these or indices.
    std::shared_ptr<const NativeDrawReplayIndices> sharedIndices;

    std::span<const std::uint32_t> indexSpan() const noexcept
    {
        return sharedIndices ? sharedIndices->Values() : std::span<const std::uint32_t>(indices);
    }

    std::vector<std::uint8_t> vertexConstants;
    std::vector<std::uint8_t> pixelConstants;
    std::vector<std::uint8_t> sharedConstants;
    std::vector<std::uint8_t> initialSample0;
    std::vector<std::uint8_t> initialSample1;

    std::array<std::uint8_t, 4>                                        clearColor = { 0, 0, 0, 255 };
    NativeDrawReplayViewport                                           viewport;
    NativeDrawReplayScissor                                            scissor;
    NativeDrawReplayBlendState                                         blend;
    std::array<NativeDrawReplayTexture, kNativeDrawReplayTextureSlots> textures;
    NativeDrawReplaySampler                                            sampler;
    // Empty retains the shared sampler contract of existing recipes.
    std::vector<NativeDrawReplaySampler> samplers;
    NativeDrawReplayDepthState           depth;
    NativeDrawReplayRasterizer           rasterizer;
    NativeDrawReplayTargetFormat         targetFormat = NativeDrawReplayTargetFormat::Rgba8;
    // RT1.. the pixel shader writes. Recipe files never set them.
    std::uint8_t                               extraTargetCount = 0;
    std::array<NativeDrawReplayExtraTarget, 3> extraTargets{};

    std::uint32_t                width                = 0;
    std::uint32_t                height               = 0;
    std::uint32_t                sampleCount          = 0;
    std::uint32_t                sampleMask           = 0;
    std::uint32_t                vertexStrideBytes    = 0;
    std::uint32_t                vertexAttributeCount = 0;
    std::uint32_t                indexCount           = 0;
    std::uint32_t                textureMask          = 0;
    std::uint64_t                vertexShaderHash     = 0;
    std::uint64_t                pixelShaderHash      = 0;
    NativeDrawReplayStencilState stencil;
    // RB_MODECONTROL depth-only mode runs no pixel shader: the pixel hash is
    // zero, the pixel DXIL is empty and no color target is written.
    bool depthOnly = false;

    // Validated draws may share immutable owned bytecode. Each shared payload
    // excludes its corresponding mutable vector and survives cache retirement.
    std::shared_ptr<const std::vector<std::uint8_t>>     sharedVertexShaderDxil;
    std::shared_ptr<const std::vector<std::uint8_t>>     sharedPixelShaderDxil;
    std::shared_ptr<const NativeDrawReplayShaderVersion> shaderVersion;

    std::span<const std::uint8_t> vertexShaderBytes() const noexcept
    {
        const auto* bytes = sharedVertexShaderDxil ? sharedVertexShaderDxil.get() : &vertexShaderDxil;
        return { bytes->data(), bytes->size() };
    }

    std::span<const std::uint8_t> pixelShaderBytes() const noexcept
    {
        const auto* bytes = sharedPixelShaderDxil ? sharedPixelShaderDxil.get() : &pixelShaderDxil;
        return { bytes->data(), bytes->size() };
    }
};

// A resolve destination a recorded prefix copied for the host. Read is valid
// once the stream fence reaches the prefix's completionFenceValue; the bytes
// stay owned until the last holder releases the handle.
class NativeRawReadback
{
public:
    virtual ~NativeRawReadback() = default;
    // Linear texels of the whole destination, rows packed at rowPitch bytes.
    virtual bool Read(std::vector<std::uint8_t>& texels, std::uint32_t& rowPitch, std::string& error) const = 0;
    // Host bytes the readback holds until released.
    virtual std::uint64_t Bytes() const noexcept = 0;
};

struct NativeDrawReplayResult
{
    bool                  success = false;
    std::filesystem::path outputPath;
    std::string           error;
    // Non-zero for a successfully completed frame replay or stream frame. A
    // recorded prefix reports the queue fence value that will complete it.
    std::uint64_t completionFenceValue = 0;
    // For a failed stream request: the last queue fence value signaled before
    // the failure was published, which covers every batch it already submitted.
    std::uint64_t submittedFenceValue = 0;
    // Admission lineage for an explicitly requested SDK GPU output copy.
    std::uint64_t presenterToken = 0;
    // Owned packed R10G10B10A2 pixels, when requested by a frame or stream caller.
    std::vector<std::uint8_t> resolvedOutput;
    // Linear host-format texels before swizzle, gamma, or guest byte encoding.
    std::vector<std::uint8_t> rawOutput;
    std::uint32_t             rawRowPitch = 0;
    // A recorded prefix's resolve readbacks, by destination texture id.
    std::vector<std::pair<std::uint64_t, std::shared_ptr<const NativeRawReadback>>> rawReadbacks;
    // Pins the exact export's consumer lineage independently of later frames.
    std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket> gpuOutputTicket;
};

class NativeDrawReplayAdmission
{
public:
    const NativeDrawReplayRecipe& Recipe() const noexcept
    {
        return recipe_;
    }

    const NativeDrawReplayPipelineStateKey& PipelineStateKey() const noexcept
    {
        return pipelineStateKey_;
    }

    bool ValidateSharedPayloads(std::string& error) const;

    std::span<const NativeDrawReplayTextureBinding> TextureBindings() const noexcept
    {
        return textureBindings_;
    }

    std::size_t MetadataBytes() const noexcept
    {
        return sizeof(*this) + textureBindings_.capacity() * sizeof(NativeDrawReplayTextureBinding);
    }

    std::size_t TextureBindingCapacity() const noexcept
    {
        return textureBindings_.capacity();
    }

private:
    NativeDrawReplayAdmission(const NativeDrawReplayAdmission&)            = delete;
    NativeDrawReplayAdmission& operator=(const NativeDrawReplayAdmission&) = delete;
    friend bool                AdmitNativeDrawReplayRecipe(NativeDrawReplayRecipe, std::shared_ptr<const NativeDrawReplayAdmission>&, std::string&);
    friend bool                TryAdmitNativeDrawReplayRecipe(NativeDrawReplayRecipe&, std::shared_ptr<const NativeDrawReplayAdmission>&, std::string&);
    explicit NativeDrawReplayAdmission(NativeDrawReplayRecipe&& recipe);
    const NativeDrawReplayRecipe                               recipe_;
    const NativeDrawReplayPipelineStateKey                     pipelineStateKey_;
    const std::vector<NativeDrawReplayTextureBinding>          textureBindings_;
    std::array<std::size_t, 3 + kNativeDrawReplayTextureSlots> sharedExtents_{};
};

bool AdmitNativeDrawReplayRecipe(NativeDrawReplayRecipe                            recipe,
                                 std::shared_ptr<const NativeDrawReplayAdmission>& admission,
                                 std::string&                                      error);
// The same with one validation and one move: the admission takes an
// admitted recipe, and a refused one stays with the caller unchanged.
bool TryAdmitNativeDrawReplayRecipe(NativeDrawReplayRecipe&                           recipe,
                                    std::shared_ptr<const NativeDrawReplayAdmission>& admission,
                                    std::string&                                      error);

bool LoadNativeDrawReplayRecipe(const std::filesystem::path& recipePath,
                                NativeDrawReplayRecipe&      recipe,
                                std::string&                 error);

bool ValidateNativeDrawReplayRecipe(const NativeDrawReplayRecipe& recipe,
                                    std::string&                  error);

} // namespace rerevved::gpu
