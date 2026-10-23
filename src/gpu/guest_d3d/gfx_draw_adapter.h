#pragma once

#include "gfx_draw_inputs.h"
#include "label_draw_adapter.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Position (k_16_16) and two color byte offsets of the 12-byte GFx vertex.
inline constexpr std::array<std::uint32_t, 3> kGfxAttributeOffsets{ 0, 4, 8 };

// GFx non-scene state follows native_menu_draw.cpp:92-176. Program words come
// from catalog; RB_SURFACE_INFO 0x2000/RB_DEPTH_INFO 0x2002 identify target
// forms, using presented-form values here.
inline constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 18> kGfxFixedState{ {
    { 0x2000, 0x0A010280 },
    { 0x2001, 0 },
    { 0x2002, 0x530 },
    { 0x200E, 0 },
    { 0x200F, 0x20002000 },
    { 0x2080, 0 },
    { 0x2100, 0xFFFFFF },
    { 0x2101, 0 },
    { 0x2102, 0 },
    { 0x2113, 0 },
    { 0x2114, 0 },
    { 0x2204, 0x80000 },
    { 0x2205, 0x18000 },
    { 0x2206, 0x43F },
    { 0x2208, 4 },
    { 0x2302, 4 },
    { 0x2307, 0xFF000 },
    { 0x2308, 0xFF100 },
} };

// A GFx target: RB_SURFACE_INFO, RB_DEPTH_INFO, and for an off-screen form
// the window scissor's bottom right, the surface extent and the viewport.
struct GfxTargetForm
{
    std::uint32_t surfaceInfo = 0, depthInfo = 0, scissor = 0, width = 0, height = 0, viewportWidth = 0, viewportHeight = 0;
};

// The presented 1280x720 surface at 2X, whose viewport and window scissor are
// the movie's display rectangle (DecodeNativeOverlayDisplayRectangle).
inline constexpr GfxTargetForm kGfxPresentedForm{ 0x0A010280, 0x530 };
// Hall panels 14AEC15D use 900x675 in a 928x688 RGBA8 surface, pitch 960 and
// depth 0x5FC.
inline constexpr GfxTargetForm kGfxOffscreenForm{ 0x0E8003C0, 0x5FC, 0x02A30384, 928, 688, 900, 675 };

// GFx blend setup 0x823036D0 reads path-B table 0x82F7C1A8; captures use Normal
// and disabled one/zero.
inline constexpr std::array<std::uint32_t, 8> kGfxBlendControls{ 0x07060706, 0x00080008, 0x01660166, 0x01460146, 0x01060106, 0x01860186, 0x01000100, 0x00010001 };
// Path A, the separate-alpha table at 0x82F7C260 that renderer+20 bit 1
// selects ("alpha composite"), has no capture. Its words outside path B are
// refused as gfx_blend_alpha_composite.
inline constexpr std::array<std::uint32_t, 3> kGfxAlphaCompositeBlendControls{ 0x07010701, 0x01000101, 0x01800181 };
// RB_COLOR_MASK: all channels, or none for a stencil mask.
inline constexpr std::array<std::uint32_t, 2> kGfxColorMasks{ 15, 0 };
// RB_DEPTHCONTROL: depth off with the stencil off (two retained compare
// functions), or one front-face stencil form of the Scaleform mask stack:
// test equal, write the reference, and equal with increment or decrement wrap.
inline constexpr std::array<std::uint32_t, 6> kGfxDepthControls{ 0x24F00770, 0x24F00270, 0x24F00271, 0x24F08771, 0x24F18271, 0x24F1C271 };
// RB_COLORCONTROL: alpha test off with two retained compare functions.
inline constexpr std::array<std::uint32_t, 2> kGfxColorControls{ 0x87000004, 0x87000007 };
// RB_STENCILREFMASK with the stencil on: full read and write masks; the
// reference byte is the mask depth.
inline constexpr std::uint32_t kGfxStencilMasks = 0x00FFFF00;

struct GfxDrawRecipe
{
    NativeDrawReplayRecipe    native;
    std::vector<std::uint8_t> patchedVertexCode, effectiveState;
    NativeDrawReplayTexture   texture, secondTexture; // fetch slots 0 and 1
    NativeDrawReplaySampler   sampler, secondSampler;
};

// GFx one-level 2D uses label sampler minus unreproducible fields. With unequal
// mag/min, LOD bias or gradient adjustment can change guest filter selection
// the host omits; refuse those, plus unsupported mip/border/clamp policies.
bool DecodeGfxSampler(const std::array<std::uint32_t, 6>& fetch, NativeDrawReplaySampler& sampler, std::string& error);
// Without mips: one-level k_8_8_8_8, DXT1, DXT2/3, DXT4/5 via DecodeGfxSampler.
// With packed mips:only linear DXT1, equal mag/min, all named levels; leader
// sampler/SDK anisotropic override and fetch LOD host bias.
bool DecodeGfxTexture(const std::array<std::uint32_t, 6>& fetch, std::span<const std::uint8_t> base, std::span<const std::uint8_t> mips, NativeDrawReplayTexture& texture, NativeDrawReplaySampler& sampler, std::string& error, LabelTextureCache* cache = nullptr);
// Gfx catalog pairs carry word digests, stride layout, and interface; each pair
// has one 2X entry and loading may add one single-sample entry.
bool LoadGfxDrawCatalog(const std::filesystem::path& directory, GfxDrawCatalog& catalog, std::string& error);
// A pixel shader outside `catalog` is gfx_shader_unpinned.
bool BuildGfxDrawRecipe(const GfxDrawInputs& inputs, const GfxDrawCatalog& catalog, GfxDrawRecipe& recipe, std::string& error, LabelTextureCache* cache = nullptr);
// Folded recipe exposes linked patched vertexCode/effectiveState; readers use
// only `inputs` arguments plus pixel code, geometry, and texture.
bool BuildGfxEffectiveDrawRecipe(const GfxDrawInputs& inputs, const GfxDrawShader& entry, std::vector<std::uint8_t> vertexCode, std::vector<std::uint8_t> effectiveState, GfxDrawRecipe& recipe, std::string& error, LabelTextureCache* cache = nullptr);
bool BuildGfxDraw(const GfxDrawInputs& inputs, const GfxDrawCatalog& catalog, NativeDrawReplayRecipe& draw, std::string& detail, LabelTextureCache* cache = nullptr);

// The Scaleform quads of the catalog's GFx pairs, a native_draw_gfx_shaders
// family.
NativeDrawFamily             MakeGfxDrawFamily(ReadMemory read, std::shared_ptr<const GfxDrawCatalog> catalog);
NativeDrawFamilyRegistration GfxDrawFamily();

} // namespace rerevved::gpu::guest_d3d
