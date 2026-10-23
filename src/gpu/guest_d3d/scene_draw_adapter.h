#pragma once

#include "indexed_buffer_draw_inputs.h"
#include "leader_draw_adapter.h"
#include "native_draw_families.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// One specialization of a scene pair from a shader catalog: its interface,
// stream-0 stride and attribute byte offsets. Several entries may share a
// pixel shader, and one pair has an entry per specialization.
struct SceneDrawShader
{
    NativeGameplayShaderInterface shader;
    std::uint32_t                 stride = 0;
    std::vector<std::uint32_t>    attributeOffsets;
    // Pixel texture fetch slots 0..n-1, then optionally the vertex-stage
    // fetch (slot 16), bound after them.
    std::uint32_t pixelTextures = 0;
    bool          vertexTexture = false;
};

// Every entry of a scene catalog, keyed per draw by (patched VS word digest,
// PS word digest, specialization).
struct SceneDrawCatalog
{
    std::vector<SceneDrawShader> entries;
    // The vertex-only entries of the directory's depth-only list; their
    // stride and attributes come from each draw's live VS.
    std::vector<SceneDrawShader> depthOnly;

    // Entries whose pixel word digest is `pixelWordDigest`, in catalog order.
    std::vector<const SceneDrawShader*> WithPixel(std::string_view pixelWordDigest) const;
};

// Scene target forms include surface/color/depth registers, window scissor,
// logical extent, sample count, and tile-rounded pitch.
struct SceneTargetForm
{
    std::uint32_t surfaceInfo = 0, colorInfo = 0, depthInfo = 0, scissor = 0;
    std::uint32_t width = 0, height = 0, sampleCount = 0;
};

// Scene target fact table:
// Presented target: 2X 1280x720, with either the full-window scissor or the civ
//   select leader band's 1280x351 scissor.
// Single-sample RGBA8 off-screen targets: 512x512 -> depth 0x720; 1024x1024 ->
//   depth 0x4C0; 1280x720 -> depth 0x530.
// World-frame 2X RGBA8 blur targets: K15 -> 640x368 surface, 640x360 scissor,
//   depth 0x690; K16 -> 320x192 surface, 320x180 scissor, depth 0x7A0.
// Surface rule: a 2X off-screen surface's pitch equals its width.
// Fog pass: 0x8230686C draws a single-sample 1280x720 k_16_16 surface at EDRAM
//   base 0 or 0x2D0 (COLOR_INFO format 4).
// Post pass: 0x8250C768 draws 2X k_2_10_10_10 (format 2) at 1280x720 over depth
//   0x530 and at 640x368 with scissor 640x360 over depth 0x690.
inline constexpr std::array<SceneTargetForm, 11> kSceneTargetForms{ {
    { 0x0A010280, 0, 0x530, 0x02D00500, 1280, 720, 4 },
    { 0x0A010280, 0, 0x530, 0x015F0500, 1280, 720, 4 },
    { 0x08000230, 0, 0x720, 0x02000200, 512, 512, 1 },
    { 0x10000410, 0, 0x4C0, 0x04000400, 1024, 1024, 1 },
    { 0x14000500, 0, 0x530, 0x02D00500, 1280, 720, 1 },
    { 0x0A010280, 0, 0x690, 0x01680280, 640, 368, 4 },
    { 0x05010140, 0, 0x7A0, 0x00B40140, 320, 192, 4 },
    { 0x14000500, 0x00040000, 0x530, 0x02D00500, 1280, 720, 1 },
    { 0x14000500, 0x000402D0, 0x530, 0x02D00500, 1280, 720, 1 },
    { 0x0A010280, 0x00020000, 0x530, 0x02D00500, 1280, 720, 4 },
    { 0x0A010280, 0x00020000, 0x690, 0x01680280, 640, 368, 4 },
} };

// RB_COLOR_INFO format (bits 16..19) of a form: 0 k_8_8_8_8, 2 k_2_10_10_10
// or 4 k_16_16. A packed form's draws must specialize for it.
constexpr std::uint32_t SceneTargetColorFormat(const SceneTargetForm& form)
{
    return (form.colorInfo >> 16) & 0xF;
}

// Scene draws share RT0 mask 0xF and fixed
// index/depth/clip/viewport/sample/hi-Z state. Pixel-shader draws may instead
// use kSceneNoColorMask, retaining depth/stencil/kill effects.
inline constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 16> kSceneFixedState{ {
    { 0x200E, 0 },
    { 0x200F, 0x20002000 },
    { 0x2080, 0 },
    { 0x2081, 0 },
    { 0x2100, 0xFFFFFF },
    { 0x2101, 0 },
    { 0x2102, 0 },
    { 0x2104, 15 },
    { 0x2113, 0x3F800000 },
    { 0x2114, 0 },
    { 0x2204, 0x80000 },
    { 0x2206, 0x43F },
    { 0x2208, 4 },
    { 0x2302, 4 },
    { 0x2307, 0xFF000 },
    { 0x2308, 0xFF100 },
} };

// Depth:LessEqual test/write, test-only/off; Always write/off. Blend/raster
// tables below cover color/alpha variants, fog reverse-subtract/minimum, and
// CCW/CW back-face cull or none.
inline constexpr std::array<std::uint32_t, 5> kSceneDepthControls{ 0x24F00736, 0x24F00732, 0x24F00730, 0x24F00770, 0x24F00776 };
// Blend forms:source-alpha color/source alpha, ONE/ZERO, SRC_ALPHA/ONE color,
// source-alpha both, ONE/ONE, SRC_ALPHA/ONE both; fog also ONE/ONE
// reverse-subtract/minimum.
inline constexpr std::array<std::uint32_t, 8> kSceneBlendControls{ 0x00010706, 0x00010001, 0x00010106, 0x07060706, 0x01010101, 0x01060106, 0x01810181, 0x01410141 };
// RB_COLOR_MASK on a k_16_16 form may also write red or green alone: the fog
// passes of 0x8230686C accumulate into one channel at a time.
inline constexpr std::array<std::uint32_t, 2> kScenePackedColorMasks{ 1, 2 };
// Raster forms are back-face culling with CCW or CW fronts, or none.
inline constexpr std::array<std::uint32_t, 3> kSceneRasterControls{ 0x18002, 0x18000, 0x18006 };
// PA_CL_CLIP_CNTL with UCP_ENA_0 beside the fixed 0x80000, for an entry
// marked user_clip_plane.
inline constexpr std::uint32_t kSceneUserClipControl = 0x80001;
// A depth-only draw shares the fixed state but RB_COLOR_MASK 0 and
// RB_MODECONTROL 5 (depth only), as its commit leaves them: { 0x2104, 0x2208 }.
inline constexpr std::array<std::uint32_t, 2> kSceneDepthOnlyState{ 0, 5 };
// RB_COLOR_MASK of a draw that runs its pixel shader in RB_MODECONTROL 4 but
// writes no color: the host draw keeps depth, stencil and kill.
inline constexpr std::uint32_t kSceneNoColorMask = 0;

// Scene textures include k_8, RGBA8, DXT, k_24_8, float fog, k_32_FLOAT, and
// packed post-resolve formats.
inline constexpr std::array<std::uint32_t, 10> kSceneTextureFormats{ 2, 6, 18, 19, 20, 22, 31, 32, 36, 54 };

struct SceneDrawRecipe
{
    NativeDrawReplayRecipe               native;
    std::vector<std::uint8_t>            patchedVertexCode, effectiveState;
    std::vector<NativeDrawReplayTexture> textures;
    std::vector<NativeDrawReplaySampler> samplers;
    const SceneDrawShader*               entry = nullptr;
    const SceneTargetForm*               form  = nullptr;
    // The entries of the pair the patched vertex words select, kept when a
    // later rule refuses the draw.
    std::vector<const SceneDrawShader*> pair;
};

struct SceneBuildCacheState;

// Reuse content work only with equal draw bytes; geometry needs retained
// stream, equal attributes/index controls/bytes. Same recipe shares payloads.
// Catalog stays unchanged; other catalogs uncached, builds serialize.
class SceneBuildCache
{
public:
    // Decoded vertices, indices and the streams they hold.
    static constexpr std::size_t kGeometryBudgetBytes = std::size_t(256) << 20;
    // Builds between sweeps of the geometry no build used since the previous sweep.
    static constexpr std::size_t kSweepBuilds = 4096;

    explicit SceneBuildCache(std::shared_ptr<const SceneDrawCatalog> catalog, std::size_t geometryBudgetBytes = kGeometryBudgetBytes);
    ~SceneBuildCache();
    SceneBuildCache(const SceneBuildCache&)            = delete;
    SceneBuildCache& operator=(const SceneBuildCache&) = delete;

    // Bytes the kept geometry counts against its budget.
    std::size_t GeometryBytes();

private:
    friend struct SceneBuildCacheAccess;
    std::mutex                            mutex;
    std::unique_ptr<SceneBuildCacheState> state;
};

// Scene catalog includes every entry as a specialization and its optional
// depth-only list.
bool LoadSceneDrawCatalog(const std::filesystem::path& directory, SceneDrawCatalog& catalog, std::string& error);
// The same rules for interfaces already loaded.
bool AdmitSceneDrawShaders(std::vector<NativeGameplayShaderInterface> shaders, SceneDrawCatalog& catalog, std::string& error);
// The reader's pixel shader list: each distinct pixel word digest once, with
// its pixel texture count; the digests are views of the catalog's.
std::vector<IndexedBufferPixelShader> ScenePixelShaders(const SceneDrawCatalog& catalog);
// The reader's depth-only list: each distinct VS word digest once, as views
// of the catalog's.
std::vector<std::string_view> SceneDepthOnlyVertexShaders(const SceneDrawCatalog& catalog);
// The target form the registers name, or null.
const SceneTargetForm* FindSceneTargetForm(std::span<const std::uint32_t> registers);

// Scene admission checks target forms 0x2000/0x2001/0x2002/0x2082 and zpass
// 0x2200/0x2201 plus catalog, literal, sampler, and recipe state.
bool BuildSceneDrawRecipe(const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, SceneDrawRecipe& recipe, std::string& error, LeaderTextureCache* cache = nullptr, SceneBuildCache* build = nullptr);
// 0x826AD150 patches depth-only declaration offsets for arguments.stride;
// 0x826ADD88 applies before GPU. Entry words are previous patch or initial
// templates, not this draw's executed words.
bool PatchDepthOnlyVertexFetch(const IndexedBufferDrawInputs& inputs, std::vector<std::uint8_t>& code, std::string& error);
// Depth-only draws need a listed VS after this draw's patch. If as-read words
// are listed but patched words are not, refuse; otherwise an unlisted draw is
// foreign(false, empty error).
bool BuildSceneDepthOnlyRecipe(const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, SceneDrawRecipe& recipe, std::string& error, SceneBuildCache* build = nullptr);
// With G+20 dirty 0x1E0000, 0x826ADD88 sets mode 5; on mode change, G+0x2ABF
// bit 3 clears color format. Mode change OR dirty 0x180000 updates VS SQ
// program/context; exports needing linkage fail scene_depth_only_linkage.
bool CommitSceneDepthOnlyState(std::vector<std::uint8_t>& state, std::span<const std::uint8_t> vertexMetadata, std::string& error);
// The register rules of a committed depth-only draw, from scene_target_form
// through the depth, blend and raster tables: on success `form` and the
// `pair` entry whose SQ words the registers hold.
bool CheckSceneDepthOnlyState(std::span<std::uint32_t> registers, std::span<const SceneDrawShader* const> pair, const SceneTargetForm*& form, const SceneDrawShader*& entry, std::string& error);
// The native draw of registers that passed CheckSceneDepthOnlyState, with the
// patched VS's decoded attributes: the form's target, the viewport the shadow
// scale must match, and BuildNativeGameplayDrawRecipe's rules.
bool BuildSceneDepthOnlyNative(const NativeGameplayDrawView& view, std::span<const std::uint8_t> effectiveState, const SceneDrawShader& entry, const SceneTargetForm& form, std::vector<NativeGameplayVertexAttribute> attributes, NativeDrawReplayRecipe& native, std::string& error);
bool BuildSceneDraw(const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, NativeDrawReplayRecipe& draw, std::string& detail, LeaderTextureCache* cache = nullptr, SceneBuildCache* build = nullptr);

// Scene family claims unclaimed kSceneDrawCallers and committed
// kParticleDrawCaller IndexedUpLowerer draws.
NativeDrawFamily MakeSceneDrawFamily(ReadMemory read, std::shared_ptr<const SceneDrawCatalog> catalog);
// The scene catalog, a native_draw_scene_shaders family.
NativeDrawFamilyRegistration SceneDrawFamily();

} // namespace rerevved::gpu::guest_d3d
