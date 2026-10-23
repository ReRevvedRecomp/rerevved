#pragma once

#include "indexed_buffer_draw_inputs.h"
#include "label_draw_adapter.h"
#include "leader_draw_adapter.h"
#include "native_draw_families.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Translator identity of the post-ESRB MRT prepass pair. The word digests
// serialize numeric microcode words in little endian; the vertex digest is
// of the words after the guest's fetch patch.
inline constexpr std::uint64_t    kSceneMeshVertexShaderHash = 0x3648CFB8645444C7ULL;
inline constexpr std::uint64_t    kSceneMeshPixelShaderHash  = 0x0B8225BDBB2FEE74ULL;
inline constexpr std::string_view kSceneMeshVertexWordDigest = "6d7daa9f91b0fc5c6ee7e0bd7aed22962adfd7d7d62b28ca4efc9829a49b1b52";
inline constexpr std::string_view kSceneMeshPixelWordDigest  = "20396b53bb7500c92ac340b4bd4ad4bd3748cbb3802aaf84eb5b68afa898579c";

// Stream 0 holds a float3 position and a float2 UV, 20 bytes per vertex.
inline constexpr std::uint32_t                kSceneMeshStride = 20;
inline constexpr std::array<std::uint32_t, 2> kSceneMeshAttributeOffsets{ 0, 12 };

// Capture event 1895 prepass: tiled RT0 k_8_8_8_8/RT1 k_2_10_10_10, 2X with
// depth, logical 1280x720; ONE/ZERO, color mask 0xFF both targets,
// depth/stencil off, back-face culling.
inline constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 35> kSceneMeshFixedState{ {
    { 0x2000, 0x070101E0 },
    { 0x2001, 0 },
    { 0x2002, 0x5E4 },
    { 0x2003, 0x0002021C },
    { 0x2004, 0 },
    { 0x2005, 0 },
    { 0x200E, 0 },
    { 0x200F, 0x20002000 },
    { 0x2080, 0 },
    { 0x2081, 0 },
    { 0x2082, 0x02D00500 },
    { 0x2100, 0xFFFFFF },
    { 0x2101, 0 },
    { 0x2102, 0 },
    { 0x2104, 0xFF },
    { 0x210F, 0x44200000 },
    { 0x2110, 0x44200000 },
    { 0x2111, 0xC3B40000 },
    { 0x2112, 0x43B40000 },
    { 0x2113, 0x3F800000 },
    { 0x2114, 0 },
    { 0x2180, 0x40010001 },
    { 0x2181, 4 },
    { 0x2182, 0x10001 },
    { 0x2200, 0x24F00770 },
    { 0x2201, 0x00010001 },
    { 0x2202, 0x87000007 },
    { 0x2204, 0x80000 },
    { 0x2205, 0x18002 },
    { 0x2206, 0x43F },
    { 0x2208, 4 },
    { 0x2209, 0x00010001 },
    { 0x2302, 4 },
    { 0x2307, 0xFF000 },
    { 0x2308, 0xFF100 },
} };

struct SceneMeshDrawRecipe
{
    NativeDrawReplayRecipe    native;
    std::vector<std::uint8_t> patchedVertexCode, effectiveState;
};

bool IsSceneMeshPixelShader(std::span<const std::uint8_t> pixelCode);
// The prepass pair's catalog entry from a shader directory. Its guest word
// digests, input layout and two color targets must be the pinned pair's.
bool LoadSceneMeshDrawShader(const std::filesystem::path& directory, NativeGameplayShaderInterface& shader, std::string& error);
// False with an empty error means the inputs are not a prepass candidate.
bool BuildSceneMeshDrawRecipe(const IndexedBufferDrawInputs& inputs, const NativeGameplayShaderInterface& shader, SceneMeshDrawRecipe& recipe, std::string& error);
bool BuildSceneMeshDraw(const IndexedBufferDrawInputs& inputs, const NativeGameplayShaderInterface& shader, NativeDrawReplayRecipe& draw, std::string& detail);

// The post-ESRB MRT prepass mesh, a native_draw_mesh_shaders family.
NativeDrawFamily             MakeSceneMeshDrawFamily(ReadMemory read, NativeDrawFamilyShader shader);
NativeDrawFamilyRegistration SceneMeshDrawFamily();
// Menu backdrop same call site: native_menu_draw.cpp:42,
// scene/full-viewport:92-176.
// Title-screen scene_mesh_draw_adapter_test menu capture confirms values.
inline constexpr std::uint64_t    kSceneMenuVertexShaderHash = 0xC3BAB94E67553E12ULL;
inline constexpr std::uint64_t    kSceneMenuPixelShaderHash  = 0x64D8E6A475FFCB2DULL;
inline constexpr std::string_view kSceneMenuVertexWordDigest = "ca4917e94fd85d07331bfcb1949fe134234c59f1229953de8227af39dd236b91";
inline constexpr std::string_view kSceneMenuPixelWordDigest  = "2cac681fefe9253f28a78bd8e5e0b5e523a62f2ff768b2ed343e163c56de3757";

// Stream 0 holds a float3 position, a float2 UV and a float3, 32 bytes per
// vertex (contract dwords 0, 3 and 5).
inline constexpr std::uint32_t                kSceneMenuStride = 32;
inline constexpr std::array<std::uint32_t, 3> kSceneMenuAttributeOffsets{ 0, 12, 20 };

// The one VS literal record 0x826ADB98 loads: c252..c255, 16 dwords, which
// land in registers 0x43F0..0x43FF over the shadow's c252..c255.
inline constexpr std::uint16_t kSceneMenuLiteralFirst = 252;
inline constexpr std::uint16_t kSceneMenuLiteralCount = 16;

// The two texture forms the hybrid menu frames sampled through fetch 0, one
// per scene draw: BC1 1024x256 and BC2 2048x512, each one 2D level.
struct SceneMenuTextureForm
{
    NativeDrawReplayTextureFormat format;
    std::uint32_t                 width, height;
};

inline constexpr std::array<SceneMenuTextureForm, 2> kSceneMenuTextureForms{ {
    { NativeDrawReplayTextureFormat::Bc1, 1024, 256 },
    { NativeDrawReplayTextureFormat::Bc2, 2048, 512 },
} };

// Menu form is 2X depth 0x530, RT0 mask 0xF, alpha ref 0x210E.
inline constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 31> kSceneMenuFixedState{ {
    { 0x2000, 0x0A010280 },
    { 0x2001, 0 },
    { 0x2002, 0x530 },
    { 0x200E, 0 },
    { 0x200F, 0x20002000 },
    { 0x2080, 0 },
    { 0x2081, 0 },
    { 0x2082, 0x02D00500 },
    { 0x2100, 0xFFFFFF },
    { 0x2101, 0 },
    { 0x2102, 0 },
    { 0x2104, 15 },
    { 0x210F, 0x44200000 },
    { 0x2110, 0x44200000 },
    { 0x2111, 0xC3B40000 },
    { 0x2112, 0x43B40000 },
    { 0x2113, 0x3F800000 },
    { 0x2114, 0 },
    { 0x2180, 0x10210204 },
    { 0x2181, 8 },
    { 0x2182, 0x10007 },
    { 0x2200, 0x24F00736 },
    { 0x2201, 0x00010706 },
    { 0x2202, 0x8700000C },
    { 0x2204, 0x80000 },
    { 0x2205, 0x18002 },
    { 0x2206, 0x43F },
    { 0x2208, 4 },
    { 0x2302, 4 },
    { 0x2307, 0xFF000 },
    { 0x2308, 0xFF100 },
} };

// Guest word digests that select a form and pin its patched VS. The runtime
// uses kSceneMenuIdentity; synthetic fixtures substitute their own words.
struct SceneMeshIdentity
{
    std::string_view vertexWordDigest, pixelWordDigest;
};

inline constexpr SceneMeshIdentity kSceneMenuIdentity{ kSceneMenuVertexWordDigest, kSceneMenuPixelWordDigest };

struct SceneMenuDrawRecipe
{
    NativeDrawReplayRecipe    native;
    std::vector<std::uint8_t> patchedVertexCode, effectiveState;
    NativeDrawReplayTexture   texture;
    NativeDrawReplaySampler   sampler;
};

// Menu catalog entry pins the pair's digest, interface, and layout; refusal
// names catalog, digest, interface, targets, or layout.
bool LoadSceneMenuDrawShader(const std::filesystem::path& directory, NativeGameplayShaderInterface& shader, std::string& error);

// A menu shader directory: the backdrop pair and any leader-band pairs, which
// the scene caller draws with leader state into the leader viewport (the civ
// select props).
struct SceneMenuDrawCatalog
{
    std::shared_ptr<const NativeGameplayShaderInterface> backdrop;
    std::shared_ptr<const LeaderDrawCatalog>             band;
};

// Menu backdrop follows LoadSceneMenuDrawShader; other entries follow
// AdmitLeaderDrawShaders, and duplicate backdrop PS is refused.
bool LoadSceneMenuDrawCatalog(const std::filesystem::path& directory, SceneMenuDrawCatalog& catalog, std::string& error);
// The GFx sampler for texture fetch 0, with its refusal codes renamed
// scene_mesh_texture_mipmapped and scene_mesh_sampler_*.
bool DecodeSceneMenuSampler(const std::array<std::uint32_t, 6>& fetch, NativeDrawReplaySampler& sampler, std::string& error);
// Menu inputs are foreign when PS words differ from identity; empty error
// distinguishes that case.
bool BuildSceneMenuDrawRecipe(const IndexedBufferDrawInputs& inputs, const NativeGameplayShaderInterface& shader, SceneMenuDrawRecipe& recipe, std::string& error, LabelTextureCache* cache = nullptr, const SceneMeshIdentity& identity = kSceneMenuIdentity);
bool BuildSceneMenuDraw(const IndexedBufferDrawInputs& inputs, const NativeGameplayShaderInterface& shader, NativeDrawReplayRecipe& draw, std::string& detail, LabelTextureCache* cache = nullptr, const SceneMeshIdentity& identity = kSceneMenuIdentity);
// Menu family combines IndexedBuffer Menu input with BuildSceneMenuDraw;
// identity views outlive the family.
NativeDrawFamily MakeSceneMenuDrawFamilyFor(ReadMemory read, NativeDrawFamilyShader shader, SceneMeshIdentity identity);
// The same for the runtime's kSceneMenuIdentity.
NativeDrawFamily MakeSceneMenuDrawFamily(ReadMemory read, NativeDrawFamilyShader shader);
// The catalog as one family: the backdrop family for `identity`, then for a
// draw it finds NotMine the band's MakeLeaderDrawFamily.
NativeDrawFamily MakeSceneMenuCatalogFamily(ReadMemory read, SceneMenuDrawCatalog catalog, SceneMeshIdentity identity = kSceneMenuIdentity);
// The title/menu backdrop and civ select props, a native_draw_menu_shaders
// family.
NativeDrawFamilyRegistration SceneMenuDrawFamily();
// The family as a frame draw builder: false with an empty detail is "not
// mine"; a null shader returns an empty builder.
NativeFrameDrawBuilder MakeSceneMenuDrawBuilder(ReadMemory read, std::shared_ptr<const NativeGameplayShaderInterface> shader, SceneMeshIdentity identity = kSceneMenuIdentity);

} // namespace rerevved::gpu::guest_d3d
