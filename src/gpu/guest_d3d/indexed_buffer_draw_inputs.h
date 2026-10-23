#pragma once

#include "gpu/d3d12/native_texture_upload.h"
#include "native_draw_dispatch.h"
#include "shader_resource_literals.h"
#include "solid_draw_adapter.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// The engine mesh path (sub_8250C498) calls IndexedBuffer (0x826A39F8) here.
inline constexpr std::uint32_t kSceneMeshDrawCaller = 0x8250C768;
// The civ select leader model's draws (sub_8250C840) call it here.
inline constexpr std::uint32_t kLeaderDrawCaller = 0x8250CB98;
// The FireGrafix fog of war (sub_823067F8) calls it here.
inline constexpr std::uint32_t kFogDrawCaller = 0x8230686C;
// Mesh batches call via 0x82511518 and paths
// 0x825118AC-0x825118D4/0x8250C73C-0x8250C764.
inline constexpr std::uint32_t kMeshBatchDrawCaller = 0x825118D8;
// The callers whose draws the Scene form reads.
inline constexpr std::array<std::uint32_t, 4> kSceneDrawCallers{ kSceneMeshDrawCaller, kLeaderDrawCaller, kFogDrawCaller, kMeshBatchDrawCaller };
// sub_825107A0 calls IndexedUpLowerer (0x826A3048) here for a particle batch,
// fills the 36-byte vertices (0x82508BC8) and 16-bit quad indices
// (0x824FF278) it reserved, then commits it (0x826A2FF0 at 0x82510CE0).
inline constexpr std::uint32_t kParticleDrawCaller = 0x82510BFC;

// One IndexedBuffer draw as it stood at entry, with the solid adapter's
// shader, metadata and state fields. `arguments.stride` holds stream 0's
// stride so the fetch patch consumes the base directly.
struct IndexedBufferDrawInputs : SolidDrawInputs
{
    bool          sceneCandidate = false;                                        // the pixel shader matched
    std::uint32_t primitive = 0, baseVertex = 0, startIndex = 0, indexCount = 0; // r4..r7
    // The G+0x3094 index buffer object: physical base of the read indices,
    // 16- or 32-bit indices (format 0 or 1) and the Xenos endian.
    std::uint32_t indexBuffer = 0, indexAddress = 0, indexFormat = 0, indexEndian = 0;
    // Per declared stream i, the vertex fetch 95 - i (G+0x480 + 8 * (95 - i))
    // and the bytes it addresses; `indexBytes` holds the index bytes read.
    GuestBytes                                indexBytes;
    std::vector<std::uint32_t>                streams;
    std::vector<std::array<std::uint32_t, 2>> streamFetches;
    std::vector<GuestBytes>                   streamBytes;
    // Menu form only: each stage's literal records as ReadShaderResourceLiterals
    // returns them, the vertex records' register values, and texture fetch 0
    // (G+0x480) with the base-level bytes it addresses.
    std::vector<ShaderLiteralRange>                      vertexLiteralRanges, pixelLiteralRanges;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> vertexLiteralRegisters;
    std::array<std::uint32_t, 6>                         textureFetch{};
    std::uint32_t                                        textureAddress = 0;
    GuestBytes                                           textureBytes;
    // Leader form only: the matched pixel shader's index, the pixel records'
    // register values, and texture fetch slots 0..n-1 (G+0x480 + 24 * slot)
    // with the base and mip storage bytes each addresses.
    std::size_t                                          pixelShader = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pixelLiteralRegisters;
    std::vector<std::array<std::uint32_t, 6>>            textureFetches;
    std::vector<NativeTextureMemoryRanges>               textureRanges;
    std::vector<GuestBytes>                              textureBaseBytes, textureMipBytes;
    // Per fetch slot, the resolved texture the reader's query returned; a
    // resolved slot reads no bytes and keeps both byte entries empty.
    std::vector<std::optional<NativeResolvedTexture>> textureResolved;
    // Scene form: the texture vectors end with fetch 16 (G+0x480 + 24 * 16),
    // whose bytes stay empty when it names no readable storage.
    bool vertexTextureFetch = false;
};

inline std::span<const std::uint8_t> indexBytes(const IndexedBufferDrawInputs& inputs) noexcept
{
    return inputs.indexBytes.empty() ? std::span<const std::uint8_t>(inputs.indices) : std::span<const std::uint8_t>(inputs.indexBytes);
}

// Gives a finished draw's inputs back to this thread, emptied, so the next
// ReadIndexedBufferDrawInputs here reuses their storage.
void ReturnDrawInputs(IndexedBufferDrawInputs&& inputs) noexcept;

// Prepass preserves raw literal bytes in vertexLiterals and pixelLiterals.
enum class IndexedBufferForm
{
    Prepass,
    Menu,
    Leader,
    SceneLeader,
    Scene,
    // Particle: r4 primitive, r5 base vertex, r7 index count, r9 stride; r8 bit
    // 2=index format, bits 0..1=endian(0x826A3048). Shadow is entry snapshot,
    // geometry is commit, not caller memory after reuse.
    Particle,
};

// Leader admitted pixel shader records number of texture slots sampled from 0.
// Scene snapshots vertex fetch 16 only with vertexTexture, for a paired VS that
// may sample it.
struct IndexedBufferPixelShader
{
    std::string_view wordDigest;
    std::uint32_t    textureFetches = 0;
    bool             vertexTexture  = false;
};

// Shader cache keys PS by object and VS/interface by both objects,
// declaration/form. Reuse only when view still equals every guest read, so hit
// matches reads. Code views never wait for resolves:none writes shader code.
class GuestShaderReadCache
{
public:
    GuestShaderReadCache();
    ~GuestShaderReadCache();
    GuestShaderReadCache(const GuestShaderReadCache&)            = delete;
    GuestShaderReadCache& operator=(const GuestShaderReadCache&) = delete;

    struct Counts
    {
        std::uint64_t hits = 0, misses = 0, stale = 0; // stale: an entry's bytes changed
    };

    Counts TakeCounts() noexcept;

    // Defined by the reader alone.
    struct Impl;

    Impl& State() noexcept
    {
        return *impl;
    }

private:
    std::unique_ptr<Impl> impl;
};

// Catalog word digest of guest microcode bytes in guest (big-endian) order.
std::string IndexedBufferMicrocodeWordDigest(std::span<const std::uint8_t> bytes);

// The word digest of a depth-only draw's VS after the guest's fetch patch,
// borrowing the inputs with an explicit stream-zero byte stride override,
// or an empty string when the patch cannot run on these inputs.
using PatchedVertexDigest = std::function<std::string(const IndexedBufferDrawInputs&, std::uint32_t)>;

// Menu and Leader refusals distinguish literal, unreadable range, and short
// texture extent; matches retain raw inputs.
bool ReadIndexedBufferDrawInputs(const NativeDrawSubmission& submission, const ReadMemory& read, std::string_view pixelWordDigest, IndexedBufferDrawInputs& inputs, std::string& error, IndexedBufferForm form = IndexedBufferForm::Prepass);
// Match any pixelShaders entry; inputs names match. Depth-only Scene uses
// patched VS digest, not stale guest words; pixel fields/literals/textures
// remain empty. Particle uses entry shadow and committed geometry, stride r9.
bool ReadIndexedBufferDrawInputs(const NativeDrawSubmission& submission, const ReadMemory& read, std::span<const IndexedBufferPixelShader> pixelShaders, IndexedBufferDrawInputs& inputs, std::string& error, IndexedBufferForm form, std::span<const std::string_view> depthOnlyVertexShaders = {}, const PatchedVertexDigest& patchedVertexDigest = {});

// Reads device-shadow shaders without pixel-shader or caller filtering.
bool ReadDrawShaderCode(std::uint32_t device, const ReadMemory& read, IndexedBufferDrawInputs& inputs, std::string& error);
bool ReadDrawShaderInputs(std::uint32_t device, const ReadMemory& read, IndexedBufferDrawInputs& inputs, std::string& error);

// Indexed bindings read declaration G+0x2E2C plus VS/PS metadata, fetch
// records, and patched stream-0 stride; PS code/full shadow are not read.
bool ReadHostPatchInputs(std::uint32_t device, std::uint32_t stride, const ReadMemory& read, IndexedBufferDrawInputs& inputs, bool& patches, std::string& error, bool diagnostic = false);

} // namespace rerevved::gpu::guest_d3d
