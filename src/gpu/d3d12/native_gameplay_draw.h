#pragma once

#include "native_draw_replay.h"
#include "native_gameplay_preparation.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rerevved::gpu
{

// A fetch span is the guest byte range addressed by one Xenos vertex fetch
// constant. Attribute offsets and strides are supplied by the translated
// shader interface below, rather than reconstructed from a shader filename.
struct NativeGameplayVertexFetchSpan
{
    std::uint32_t                 fetchConstant = 0;
    std::uint32_t                 guestBase     = 0;
    std::span<const std::uint8_t> bytes;
};

struct NativeGameplayVertexAttribute
{
    std::uint32_t fetchConstant = 0;
    std::uint32_t strideBytes   = 0;
    std::int32_t  offsetBytes   = 0;
    std::uint32_t format        = 0;
    bool          isSigned      = false;
    bool          normalized    = false;
};

struct NativeGameplayTextureBinding
{
    std::uint32_t                  guestTextureSlot = 0;
    const NativeDrawReplayTexture* texture          = nullptr;
    const NativeDrawReplaySampler* sampler          = nullptr;
};

// Shader digests cover caller-supplied guest words or translated DXIL/SPIR-V;
// ordered attributes and texture slots define the interface.
struct NativeGameplayShaderInterface
{
    std::uint64_t vertexShaderHash = 0;
    std::uint64_t pixelShaderHash  = 0;

    std::vector<std::uint8_t> vertexShaderDxil;
    std::vector<std::uint8_t> pixelShaderDxil;
    std::string               vertexGuestWordDigest;
    std::string               pixelGuestWordDigest;
    std::string               vertexDxilDigest;
    std::string               pixelDxilDigest;
    // Filled instead of the DXIL fields when a catalog is loaded for SPIR-V.
    std::vector<std::uint8_t> vertexShaderSpirv;
    std::vector<std::uint8_t> pixelShaderSpirv;
    std::string               vertexSpirvDigest;
    std::string               pixelSpirvDigest;

    std::vector<NativeGameplayVertexAttribute>    vertexAttributes;
    std::vector<std::uint32_t>                    textureFetchSlots;
    std::vector<NativeDrawReplayTextureDimension> textureDimensions;

    // The translator must specialize alpha testing and depth export from this
    // explicit state. A false value is meaningful and rejects an enabled guest
    // alpha test rather than silently dropping it.
    bool          alphaTestEnabled = false;
    std::uint32_t alphaTestCompare = 0;
    bool          rasterizedDepth  = false;
    // The runtime binds a distinct captured sampler for each packed texture slot.
    bool independentSamplers = false;
    // Confirms that SQ_PROGRAM_CNTL.param_gen replaces the selected input
    // register with rasterizer pixel coordinates, including the face sign.
    bool generatedPixelParameters = false;
    // The translated VS maps pretransformed pixel positions to NDC itself.
    // Admits only guest clipping off with the viewport transform disabled
    // (PA_CL_CLIP_CNTL 0x90000, PA_CL_VTE_CNTL 0x400), and requires that state.
    bool screenSpacePosition = false;
    // The translated VS writes SV_ClipDistance0 = dot(guest position, plane)
    // from shared constant c29. Admits and requires PA_CL_CLIP_CNTL 0x80001.
    bool userClipPlane = false;

    // Render-target specialization inputs from RB_COLOR_INFO and
    // RB_SURFACE_INFO. Legacy catalog entries use format 0, bias 0, and the
    // native four-sample coverage mask 0x9.
    std::uint32_t colorTargetFormat = 0;
    std::int32_t  colorExpBias      = 0;
    std::uint32_t sampleCount       = 4;
    std::uint32_t sampleMask        = 9;
    // Render targets the pixel shader writes and RB_COLOR1_INFO's format for
    // a second one. The draw conversion itself binds RT0 only.
    std::uint32_t colorTargetCount = 1;
    std::uint32_t color1Format     = 0;

    // Guest shader program identity is part of the pinned interface catalog.
    std::uint32_t shaderProgram       = 0;
    std::uint32_t shaderContext       = 0;
    std::uint32_t shaderInterpolators = 0;

    // A vertex-only entry for draws in RB_MODECONTROL depth-only mode, which
    // run no pixel shader: the pixel hash, digests and DXIL are empty, the
    // draw supplies no pixel microcode and writes no color.
    bool depthOnly = false;
};

// A draw's triangle-list indices and the decoded vertices, as an earlier
// build decoded them from the same index bytes, index controls, vertex
// attributes and fetch bytes. The recipe shares both.
struct NativeGameplayDecodedGeometry
{
    std::shared_ptr<const NativeDrawReplayIndices>   indices;
    std::shared_ptr<const std::vector<std::uint8_t>> vertices;
};

struct NativeGameplayDrawView
{
    std::span<const std::uint32_t> registers;
    std::span<const std::uint32_t> vertexMicrocode;
    std::span<const std::uint32_t> pixelMicrocode;

    std::span<const NativeGameplayVertexFetchSpan> vertexFetches;
    std::span<const std::uint8_t>                  indexBytes;
    std::span<const NativeGameplayTextureBinding>  textureBindings;

    std::uint32_t primitive   = 0; // Xenos triangle list (4) or strip (6).
    std::uint32_t indexCount  = 0;
    std::uint32_t indexFormat = 0; // Xenos 16-bit index format.
    std::uint32_t indexEndian = 1; // Xenos big-endian index stream.
    bool          indexed     = true;
    // The caller's earlier decode of these exact geometry inputs, which the
    // build takes instead of decoding; every other check still runs.
    const NativeGameplayDecodedGeometry* decodedGeometry = nullptr;
};

// One owned shader pair, bounded by the existing 4 MiB per-payload checks.
// Snapshot conversion keeps this cache only for the duration of one frame.
class NativeGameplayShaderValidationCache
{
public:
    // Digest hits trust immutable interfaces whose DXIL and guest-word bytes
    // match their digests.
    enum class Hit
    {
        Bytes,
        Digests,
    };

    NativeGameplayShaderValidationCache() noexcept = default;

    explicit NativeGameplayShaderValidationCache(Hit hit) noexcept
    : hit_(hit)
    {
    }

    bool Validate(const NativeGameplayDrawView&        view,
                  const NativeGameplayShaderInterface& shader,
                  std::string&                         error);

    const std::shared_ptr<const std::vector<std::uint8_t>>& VertexBytecode() const noexcept
    {
        return vertexShaderDxil_;
    }

    const std::shared_ptr<const std::vector<std::uint8_t>>& PixelBytecode() const noexcept
    {
        return pixelShaderDxil_;
    }

    const std::shared_ptr<const NativeDrawReplayShaderVersion>& Version() const noexcept
    {
        return version_;
    }

    std::size_t ManagedBytes() const noexcept;
    std::size_t ForecastChanges(std::size_t vertexWords, std::size_t pixelWords, const NativeGameplayShaderInterface& shader) const noexcept;

private:
    std::vector<std::uint32_t>                           vertexMicrocode_, pixelMicrocode_;
    std::shared_ptr<const std::vector<std::uint8_t>>     vertexShaderDxil_, pixelShaderDxil_;
    std::shared_ptr<const NativeDrawReplayShaderVersion> version_;
    std::string                                          vertexGuestWordDigest_, pixelGuestWordDigest_;
    std::string                                          vertexDxilDigest_, pixelDxilDigest_;
    Hit                                                  hit_ = Hit::Bytes;
};

struct NativeGameplayDrawTarget
{
    std::uint32_t                width       = 640;
    std::uint32_t                height      = 720;
    std::uint32_t                sampleCount = 4;
    std::uint32_t                sampleMask  = 9;
    NativeDrawReplayTargetFormat format      = NativeDrawReplayTargetFormat::Rgba8;
    NativeDrawReplayViewport     viewport    = { 0, 0, 1280, 720, 0, 1 };
    NativeDrawReplayScissor      scissor     = { 0, 0, 640, 720 };
    // Guest D3D snapshots describe the full logical menu viewport before the
    // command emitter splits it into EDRAM tiles.
    bool fullViewportTarget = false;
};

// Index reuse follows index-shape and register validation; vertex reuse
// follows fetch/attribute validation. Owned source bytes decide both hits.
class NativeGameplayGeometryCache
{
public:
    explicit NativeGameplayGeometryCache(std::size_t budget)
    : budget_(budget)
    {
    }

    void        BeginFrame();
    void        SetBudget(std::size_t budget);
    std::size_t ManagedBytes() const noexcept;

    std::size_t Supplements() const noexcept
    {
        return supplements_;
    }

    preparation::Forecast ForecastRemember(std::size_t attributes, std::size_t fetches, std::size_t decoded, std::size_t sourceBytes, std::size_t indexCount) const noexcept;
    bool                  LookupIndices(const NativeGameplayDrawView& view, std::vector<std::uint32_t>& indices, std::uint32_t& maximum);
    bool                  Lookup(std::span<const NativeGameplayVertexAttribute>    attributes,
                                 std::span<const NativeGameplayVertexFetchSpan>    fetches,
                                 std::uint64_t                                     vertexCount,
                                 std::shared_ptr<const std::vector<std::uint8_t>>& decoded,
                                 const NativeGameplayDrawView&                     view,
                                 std::span<const std::uint32_t>                    indices);
    void                  Remember(std::span<const NativeGameplayVertexAttribute> attributes,
                                   std::span<const NativeGameplayVertexFetchSpan> fetches,
                                   std::uint64_t                                  vertexCount,
                                   const std::vector<std::uint8_t>&               decoded,
                                   const NativeGameplayDrawView&                  view,
                                   std::span<const std::uint32_t>                 indices);

private:
    struct Source
    {
        std::uint32_t             slot, base;
        std::size_t               extent;
        std::vector<std::uint8_t> bytes;
    };

    struct Entry
    {
        std::uint64_t                                    hash, vertexCount;
        std::vector<NativeGameplayVertexAttribute>       attributes;
        std::vector<Source>                              sources;
        std::shared_ptr<const std::vector<std::uint8_t>> decoded;
        std::array<std::uint32_t, 10>                    indexControls{};
        std::vector<std::uint8_t>                        indexSource;
        std::vector<std::uint32_t>                       indices;
        bool                                             used = true;
    };

    static std::array<std::uint32_t, 10> IndexControls(const NativeGameplayDrawView& view);
    void                                 RememberIndices(Entry& entry, const NativeGameplayDrawView& view, std::span<const std::uint32_t> indices);
    static std::size_t                   EntryBytes(const Entry& entry);
    static std::size_t                   EntrySupplements(const Entry& entry) noexcept;
    std::vector<Entry>                   entries_;
    std::size_t                          budget_ = 0, ownedBytes_ = 0, supplements_ = 0;
};

std::size_t NativeGameplayVertexFormatBytes(std::uint32_t format) noexcept;

// Shared constant bytes a pair's draw binds: 336, or 464 when either stage's
// control flow has loops, whose constants i0..i31 occupy bytes 336..463, or
// 480 with user clip plane 0 at bytes 464..479 (c29).
inline constexpr std::size_t kNativeGameplayUserClipPlaneOffset = 464;
std::size_t                  NativeGameplaySharedConstantBytes(std::span<const std::uint32_t> vertexMicrocode,
                                                               std::span<const std::uint32_t> pixelMicrocode,
                                                               bool                           userClipPlane = false);

bool DecodeNativeGameplayViewportAndScissor(std::span<const std::uint32_t> registers,
                                            NativeDrawReplayViewport&      viewport,
                                            NativeDrawReplayScissor&       scissor,
                                            std::string&                   error);

// On 1280x720 with zero window offset, require integral viewport inside target
// and nonempty scissor inside viewport. Xenos guard-band/scissor and host
// viewport clipping agree only then.
bool DecodeNativeOverlayDisplayRectangle(std::span<const std::uint32_t> registers,
                                         std::string_view               prefix,
                                         NativeGameplayDrawTarget&      target,
                                         std::string&                   error);

bool NativeGameplayInterpolatorsMatch(const NativeGameplayShaderInterface& shader,
                                      std::uint32_t                        interpolators);

bool BuildNativeGameplayDrawRecipe(const NativeGameplayDrawView&        view,
                                   const NativeGameplayShaderInterface& shader,
                                   const NativeGameplayDrawTarget&      target,
                                   NativeDrawReplayRecipe&              recipe,
                                   std::string&                         error,
                                   NativeGameplayShaderValidationCache* shaderValidation = nullptr,
                                   NativeGameplayGeometryCache*         geometryCache    = nullptr);

bool BuildNativeGameplayDrawRecipe(const NativeGameplayDrawView&        view,
                                   const NativeGameplayShaderInterface& shader,
                                   NativeDrawReplayRecipe&              recipe,
                                   std::string&                         error);

bool BuildNativeGameplayDrawAdmission(const NativeGameplayDrawView&                     view,
                                      const NativeGameplayShaderInterface&              shader,
                                      const NativeGameplayDrawTarget&                   target,
                                      std::shared_ptr<const NativeDrawReplayAdmission>& admission,
                                      std::string&                                      error,
                                      NativeGameplayShaderValidationCache*              shaderValidation = nullptr,
                                      NativeGameplayGeometryCache*                      geometryCache    = nullptr);

} // namespace rerevved::gpu
