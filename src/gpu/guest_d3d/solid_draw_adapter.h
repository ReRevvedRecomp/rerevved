#pragma once

#include "gpu/d3d12/native_gameplay_shaders.h"
#include "gpu/guest_d3d/guest_bytes.h"
#include "gpu/guest_d3d/guest_resource_read.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Why a texture fetch has no resolved texture to bind. The first eight name
// the stage 2 alias refusals; Unmodeled means no resolve alias is modeled for
// the fetch: aliasing is off, or its resolve has no graph texture.
enum class NativeResolvedTextureMiss : std::uint8_t
{
    Form,
    Coverage,
    Written,
    Superseded,
    Retired,
    Packing,
    Reset,
    Offscreen,
    Unmodeled,
};

// A frame texture a resolve produced, bound in place of a fetch's guest
// bytes, with the layout and swizzle the fetch would have decoded to.
struct NativeResolvedTexture
{
    std::uint64_t                 frameTextureId = 0;
    NativeDrawReplayTextureLayout layout;
    std::array<std::uint8_t, 4>   swizzle = { 0, 1, 2, 3 };
};

// Query six-word fetch at operation mark; 0 follows all recorded operations for
// a build. Never wait or take a reader-held lock; may submit producer resolves
// as a bytes read would.
using ResolvedTextureQuery = std::function<std::optional<NativeResolvedTexture>(const std::array<std::uint32_t, 6>&, std::uint64_t, NativeResolvedTextureMiss&)>;

using GuestMemoryRead = std::function<GuestBytes(std::uint64_t, std::uint64_t, bool)>;
// Guest byte view(address, size, physical) lasts until next guest write; empty
// if unreadable/unviewable or paired read returns different bytes, such as
// under an unwritten resolve image.
using GuestMemoryView = std::function<std::span<const std::uint8_t>(std::uint64_t, std::uint64_t, bool)>;

class GuestShaderReadCache;

// Query resolvedTexture before texture bytes and mirroredResource before bound
// bytes. With view+shaderCache, retain shader objects only while view still
// matches every byte read.
struct ReadMemory : GuestMemoryRead
{
    using GuestMemoryRead::function;
    ResolvedTextureQuery                  resolvedTexture;
    MirroredResourceQuery                 mirroredResource;
    GuestMemoryView                       view;
    std::shared_ptr<GuestShaderReadCache> shaderCache;

    // A bound resource's bytes: the mirror's when it holds them, otherwise a
    // physical read of the same range.
    GuestBytes Resource(const GuestResourceRead& request) const
    {
        if (mirroredResource)
        {
            const GuestReadSiteScope site(GuestResourceReadSite(request.kind, true));
            if (auto bytes = mirroredResource(request))
                return std::move(*bytes);
        }
        const GuestReadSiteScope site(GuestResourceReadSite(request.kind, false));
        return (*this)(request.address, request.size, true);
    }
};

// The recipe texture for a resolved fetch: a frame texture, no CPU payload.
inline NativeDrawReplayTexture BindNativeResolvedTexture(const NativeResolvedTexture& resolved)
{
    NativeDrawReplayTexture texture;
    texture.width          = resolved.layout.width;
    texture.height         = resolved.layout.height;
    texture.format         = resolved.layout.format;
    texture.mipLevels      = resolved.layout.mipLevels;
    texture.dimension      = resolved.layout.dimension;
    texture.swizzle        = resolved.swizzle;
    texture.frameTextureId = resolved.frameTextureId;
    return texture;
}

struct IndexedUpArguments
{
    std::uint32_t graphics = 0, primitive = 0, minimumVertex = 0, vertexCount = 0;
    std::uint32_t indexCount = 0, indices = 0, indexFormat = 0, vertices = 0, stride = 0;
    std::uint64_t caller = 0;
};

struct SolidDrawInputs
{
    bool                      solidCandidate = false, inputsComplete = false;
    IndexedUpArguments        arguments;
    std::uint32_t             vertexObject = 0, pixelObject = 0, declarationAddress = 0, variant = 0;
    std::vector<std::uint8_t> state, vertexCode, pixelCode, vertexMetadata, pixelMetadata;
    std::vector<std::uint8_t> vertexHeader, pixelHeader, declarationHeader, vertexLiterals, pixelLiterals;
    std::vector<std::uint8_t> fetchTemplates, fetchRecords, declaration, swizzleTable, formatTable;
    std::vector<std::uint8_t> vertices, indices;
};

struct SolidDrawRecipe
{
    NativeDrawReplayRecipe    native;
    std::vector<std::uint8_t> patchedVertexCode, effectiveState;
    bool                      linked = false;
};

// Reads only CPU state at 0x826A3568 entry. No original or command input exists.
// False with an empty error means foreign; candidate read failures retain the
// available raw inputs and set solidCandidate without inputsComplete.
bool ReadSolidDrawInputs(const IndexedUpArguments& arguments, const ReadMemory& read, SolidDrawInputs& inputs, std::string& error);
// This thread's spare state shadow storage, so a reader's per-draw copy of
// the device shadow reuses one buffer: Take returns it emptied, Return keeps
// the largest buffer given back.
std::vector<std::uint8_t> TakeStateBuffer() noexcept;
void                      ReturnStateBuffer(std::vector<std::uint8_t>&& buffer) noexcept;

bool IsSolidDrawCandidate(const SolidDrawInputs& inputs);
// codeOnly folds linkage without reading or copying the per-draw state.
bool FoldSolidShaderMetadata(const SolidDrawInputs& inputs, std::vector<std::uint8_t>& code, std::vector<std::uint8_t>& state, std::string& error, bool codeOnly = false);
// 0x826AD150 patches stream-zero fetch records by semantic and admitted
// offsets.
bool PatchVertexFetch(const SolidDrawInputs& inputs, std::span<const std::uint32_t> admittedOffsets, std::vector<std::uint8_t>& code, std::string& error);
// The same patch of `vertexCode` in place of inputs.vertexCode; it must not
// alias `code`. An explicit byte stride also permits shader-only inputs with
// no device shadow; nonzero streams are still refused.
bool PatchVertexFetch(const SolidDrawInputs& inputs, std::span<const std::uint8_t> vertexCode, std::span<const std::uint32_t> admittedOffsets, std::vector<std::uint8_t>& code, std::string& error, std::optional<std::uint32_t> strideOverride = {});

struct VertexShaderIdentity
{
    std::vector<std::uint8_t> code;
    std::string               digest, error;
    bool                      ok = false;
};

// Borrows the patch inputs and caches only shader results. Every byte used by
// linkage or fetch patching, object headers, and admission offsets participate
// in equality; current constants and registers never come from this cache.
std::shared_ptr<const VertexShaderIdentity> MemoizedVertexShaderIdentity(const SolidDrawInputs& inputs, std::span<const std::uint32_t> admittedOffsets, bool linked, std::optional<std::uint32_t> strideOverride = {});

struct VertexShaderIdentityCounts
{
    std::uint64_t hits = 0, misses = 0, patches = 0, guestBytes = 0;
};

VertexShaderIdentityCounts TakeVertexShaderIdentityCounts() noexcept;

namespace detail
{

// Adapter-only targets count reads without linking the draw implementation.
inline std::atomic<std::uint64_t> shaderIdentityGuestBytes{ 0 };

} // namespace detail

inline void CountVertexShaderIdentityBytes(std::uint64_t bytes) noexcept
{
    detail::shaderIdentityGuestBytes.fetch_add(bytes, std::memory_order_relaxed);
}
// Follow build-native-shaders.py exec walk: ordered VS attributes(mini inherits
// full constant/stride), exact PS slots/dimensions plus normalized 2D VS slot
// 16. All other fetches mismatch.
enum class ShaderFetchMatch
{
    Matched,
    VertexLayout,
    TextureLayout,
};
ShaderFetchMatch MatchShaderFetches(std::span<const std::uint8_t> vertexCode, std::span<const std::uint8_t> pixelCode, const NativeGameplayShaderInterface& shader);
// The vertex fetches the exec clauses of a VS run, in order, as the
// attributes MatchShaderFetches compares; false for a fetch it refuses. With
// `textures`, texture fetches are collected there instead of refused.
bool DecodeShaderVertexFetches(std::span<const std::uint8_t> vertexCode, std::vector<NativeGameplayVertexAttribute>& attributes, std::vector<std::array<std::uint32_t, 3>>* textures = nullptr);
// Hardware registers the guest dirty-state emitter writes from a 0x3500-byte
// device shadow, indexed by register number. Fetch constants are not included.
inline constexpr std::size_t kGuestDrawRegisterCount = 0x5003;
std::vector<std::uint32_t>   GuestDrawRegisters(std::span<const std::uint8_t> state);
// GuestDrawRegisters into kGuestDrawRegisterCount words, writing only the
// registers it emits; the others keep their values.
void FillGuestDrawRegisters(std::span<const std::uint8_t> state, std::span<std::uint32_t> registers);
bool LoadSolidDrawShader(const std::filesystem::path& directory, NativeGameplayShaderInterface& shader, std::string& error);
// The panel interface from a solid adapter capture root: its adapter DXIL and
// folded state under the pinned identity. The saved patched VS and PS words
// must match the pinned word digests.
bool LoadSolidPanelShader(const std::filesystem::path& capture, NativeGameplayShaderInterface& shader, std::string& error);
// Catalog word digest of a pixel shader object's guest microcode, read as
// ReadSolidDrawInputs reads it. Throws when a field is unreadable.
std::string PixelShaderWordDigest(std::uint32_t pixelObject, const ReadMemory& read);
// The same for a vertex shader object's regular variant zero, the variant an
// active PS selects.
std::string VertexShaderWordDigest(std::uint32_t vertexObject, const ReadMemory& read);
bool        BuildSolidDrawRecipe(const SolidDrawInputs& inputs, const NativeGameplayShaderInterface& shader, SolidDrawRecipe& recipe, std::string& error);
bool        SaveSolidDrawInputs(const std::filesystem::path& root, const SolidDrawInputs& inputs, const SolidDrawRecipe& recipe, std::string& error);
bool        SaveRejectedSolidDrawInputs(const std::filesystem::path& root, const SolidDrawInputs& inputs, const std::string& reason, std::string& error);
bool        LoadSolidDrawInputs(const std::filesystem::path& root, SolidDrawInputs& inputs, std::string& error);

struct FixedWord
{
    std::uint32_t shadow, hardware;
};

inline constexpr std::array<std::uint32_t, 2> kSolidAttributeOffsets{ 0, 4 };

inline constexpr std::array<FixedWord, 28> kSolidFixedWords{ { { 0x2880, 0x2000 }, { 0x2884, 0x2001 }, { 0x2888, 0x2002 }, { 0x28B8, 0x200E }, { 0x28BC, 0x200F }, { 0x28CC, 0x2100 }, { 0x28D0, 0x2101 }, { 0x28D4, 0x2102 }, { 0x28DC, 0x2104 }, { 0x2908, 0x210F }, { 0x290C, 0x2110 }, { 0x2910, 0x2111 }, { 0x2914, 0x2112 }, { 0x2918, 0x2113 }, { 0x291C, 0x2114 }, { 0x2920, 0x2180 }, { 0x2924, 0x2181 }, { 0x2928, 0x2182 }, { 0x2934, 0x2200 }, { 0x2938, 0x2201 }, { 0x293C, 0x2202 }, { 0x2944, 0x2204 }, { 0x2948, 0x2205 }, { 0x294C, 0x2206 }, { 0x2954, 0x2208 }, { 0x29C0, 0x2302 }, { 0x29D4, 0x2307 }, { 0x29D8, 0x2308 } } };

// The caller supplies storage, not a PM4 reservation. Successful completion
// preserves the source-copy/pending-output/return contract of RVA-F-0033.
struct IndexedUpStorage
{
    std::uint32_t pendingCursor = 0, vertexOutput = 0, indexOutput = 0;
};

bool CompleteIndexedUpAbi(const IndexedUpArguments& arguments, const IndexedUpStorage& storage, std::span<std::uint8_t> memory, std::uint32_t memoryBase, std::uint32_t& returnRegister, std::string& error);

} // namespace rerevved::gpu::guest_d3d
