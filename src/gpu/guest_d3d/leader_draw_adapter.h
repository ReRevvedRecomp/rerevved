#pragma once

#include "indexed_buffer_draw_inputs.h"
#include "native_draw_families.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Leader catalog entries supply digests, fetch dimensions, vertex layout, and
// program words.
struct LeaderDrawShader
{
    NativeGameplayShaderInterface shader;
    std::uint32_t                 stride = 0;
    std::vector<std::uint32_t>    attributeOffsets;
};

// Leader pairs, at most one per pixel word digest, and the IndexedBuffer call
// site their draws come from.
struct LeaderDrawCatalog
{
    std::vector<LeaderDrawShader> entries;
    std::uint32_t                 caller = kLeaderDrawCaller;

    const LeaderDrawShader* Find(std::string_view pixelWordDigest) const;
};

// Leader pairs share 0x530 depth, 0xF color mask, and 0x210E alpha reference on
// 1280x720 2X.
inline constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 22> kLeaderFixedState{ {
    { 0x2000, 0x0A010280 },
    { 0x2001, 0 },
    { 0x2002, 0x530 },
    { 0x200E, 0 },
    { 0x200F, 0x20002000 },
    { 0x2080, 0 },
    { 0x2081, 0 },
    { 0x2100, 0xFFFFFF },
    { 0x2101, 0 },
    { 0x2102, 0 },
    { 0x2104, 15 },
    { 0x210F, 0x44200000 },
    { 0x2110, 0x44200000 },
    { 0x2113, 0x3F800000 },
    { 0x2114, 0 },
    { 0x2202, 0x8700000C },
    { 0x2204, 0x80000 },
    { 0x2206, 0x43F },
    { 0x2208, 4 },
    { 0x2302, 4 },
    { 0x2307, 0xFF000 },
    { 0x2308, 0xFF100 },
} };

// Harvested forms pair scissor BR with viewport Y scale/offset whole: Civ
// select 1280x351 band or world 1280x720. World harvests contain no other form.
struct LeaderTargetForm
{
    std::uint32_t scissorBottomRight = 0, viewportYScale = 0, viewportYOffset = 0;
};

inline constexpr std::array<LeaderTargetForm, 2> kLeaderTargetForms{ { { 0x015F0500, 0xC32F8000, 0x432F8000 }, { 0x02D00500, 0xC3B40000, 0x43B40000 } } };

// Leader state forms cover LessEqual depth test/write or test-only,
// source-alpha blends, and CCW-front culling.
inline constexpr std::array<std::uint32_t, 2> kLeaderDepthControls{ 0x24F00736, 0x24F00732 };
inline constexpr std::array<std::uint32_t, 3> kLeaderBlendControls{ 0x00010706, 0x01060106, 0x07060706 };
inline constexpr std::array<std::uint32_t, 2> kLeaderRasterControls{ 0x18002, 0x18000 };

// The renderer=xenos default of the SDK's anisotropic_override (4x), which
// its texture cache applies to the leader's mipped fetches. The native route
// loads no GPU plugin, so the leader samplers carry the value themselves.
inline constexpr std::int32_t kLeaderAnisotropicOverride = 3;

// Decoded textures by fetch words and source bytes. A world frame samples
// about a hundred, so the cache holds entries up to a byte budget, counting
// each entry's source and decoded payload, and evicts the least recently used.
class LeaderTextureCache
{
public:
    // World harvests: 192-229 textures, 109-119 MiB decoded, ~240 MiB with source; ~2x, never exceeded.
    static constexpr std::size_t kBudgetBytes = std::size_t(512) << 20;

    explicit LeaderTextureCache(std::size_t budgetBytes = kBudgetBytes) noexcept
    : budget(budgetBytes)
    {
    }

    // A texture larger than the budget decodes but is not kept.
    // A read sharing an entry's retained buffer matches without a compare.
    bool Decode(const std::array<std::uint32_t, 6>& fetch, const GuestBytes& base, const GuestBytes& mips, NativeDrawReplayTexture& texture, std::string& error);
    // Bytes the kept entries count against the budget, and their number.
    std::pair<std::size_t, std::size_t> Retained();

private:
    struct Entry
    {
        std::array<std::uint32_t, 6> fetch{};
        GuestBytes                   base, mips;
        NativeDrawReplayTexture      decoded;
        std::uint64_t                used  = 0;
        std::size_t                  bytes = 0;
    };

    std::size_t        budget;
    std::mutex         mutex;
    std::vector<Entry> entries;
    // Entries by the XXH3 of their fetch words, as positions in `entries`.
    std::unordered_multimap<std::uint64_t, std::size_t> index;
    std::size_t                                         retained = 0;
    std::uint64_t                                       clock    = 0;
};

struct LeaderDrawRecipe
{
    NativeDrawReplayRecipe               native;
    std::vector<std::uint8_t>            patchedVertexCode, effectiveState;
    std::vector<NativeDrawReplayTexture> textures;
    std::vector<NativeDrawReplaySampler> samplers;
};

// Entries require slots 0..n-1, Greater alpha, world-space position, RT0
// format/bias and one target. Fetch 95 attributes fit one stride: signed
// float2/3 or normalized unsigned k_8_8_8_8; duplicate pixels refuse.
bool LoadLeaderDrawCatalog(const std::filesystem::path& directory, LeaderDrawCatalog& catalog, std::string& error);
// The same rules for interfaces already loaded, as pairs drawn from `caller`.
bool AdmitLeaderDrawShaders(std::vector<NativeGameplayShaderInterface> shaders, std::uint32_t caller, LeaderDrawCatalog& catalog, std::string& error);
// Leader point/linear sampler covers every named mip. anisotropicOverride1..5
// applies only to mipped linear fetches as SDK anisotropic_override does;
// builder passes kLeaderAnisotropicOverride.
bool DecodeLeaderSampler(const std::array<std::uint32_t, 6>& fetch, NativeDrawReplayTextureDimension dimension, NativeDrawReplaySampler& sampler, std::string& error, std::int32_t anisotropicOverride = -1);
// Leader admission checks catalog identity, literals, sampler/fetch patch,
// layout, fixed state, textures, viewport, and recipe.
bool BuildLeaderDrawRecipe(const IndexedBufferDrawInputs& inputs, const LeaderDrawCatalog& catalog, LeaderDrawRecipe& recipe, std::string& error, LeaderTextureCache* cache = nullptr);
bool BuildLeaderDraw(const IndexedBufferDrawInputs& inputs, const LeaderDrawCatalog& catalog, NativeDrawReplayRecipe& draw, std::string& detail, LeaderTextureCache* cache = nullptr);
// The reader's pixel shader list for `catalog`, in entry order; the digests
// are views of the catalog's.
std::vector<IndexedBufferPixelShader> LeaderPixelShaders(const LeaderDrawCatalog& catalog);

// The leader model of civ select and the world: the IndexedBuffer reader in
// the form for catalog.caller, then BuildLeaderDraw with one shared texture
// cache.
NativeDrawFamily MakeLeaderDrawFamily(ReadMemory read, std::shared_ptr<const LeaderDrawCatalog> catalog);
// The leader model, a native_draw_leader_shaders family.
NativeDrawFamilyRegistration LeaderDrawFamily();

} // namespace rerevved::gpu::guest_d3d
