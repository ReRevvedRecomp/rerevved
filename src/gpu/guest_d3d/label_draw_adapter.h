#pragma once

#include "label_draw_inputs.h"
#include "native_draw_families.h"
#include "native_frame_assembler.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Position, UV and color byte offsets of the 28-byte label vertex.
inline constexpr std::array<std::uint32_t, 3> kLabelAttributeOffsets{ 0, 16, 24 };

// Label state follows native_menu_draw.cpp:96-135; 0x2200/0x2202 differ only in
// disabled compare tests.
inline constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 21> kLabelFixedState{ {
    { 0x2001, 0 },
    { 0x200E, 0 },
    { 0x200F, 0x20002000 },
    { 0x2080, 0 },
    { 0x2100, 0xFFFFFF },
    { 0x2101, 0 },
    { 0x2102, 0 },
    { 0x2104, 15 },
    { 0x2113, 0 },
    { 0x2114, 0 },
    { 0x2180, 0x10110201 },
    { 0x2181, 8 },
    { 0x2182, 0x10003 },
    { 0x2201, 0x07060706 },
    { 0x2204, 0x80000 },
    { 0x2205, 0x18000 },
    { 0x2206, 0x43F },
    { 0x2208, 4 },
    { 0x2302, 4 },
    { 0x2307, 0xFF000 },
    { 0x2308, 0xFF100 },
} };

// Masked label: depth off, front stencil equals reference and keeps
// stencil(kGfxDepthControls). World frame 007561C9EFB23973-0 draws key 9E3FD59C
// with refs 1/2.
inline constexpr std::uint32_t kLabelMaskedDepthControl = 0x24F00271;
// RB_STENCILREFMASK under that test: full read and write masks, with the
// mask depth as the reference byte.
inline constexpr std::uint32_t kLabelStencilMasks = 0x00FFFF00;

// A label target: RB_SURFACE_INFO, RB_DEPTH_INFO, and for an off-screen form
// the window scissor's bottom right and its extent at one sample.
struct LabelTargetForm
{
    std::uint32_t surfaceInfo = 0, depthInfo = 0, scissor = 0, width = 0, height = 0;
};

// The presented 1280x720 surface at 2X, whose viewport and window scissor are
// the label's display rectangle (DecodeNativeOverlayDisplayRectangle).
inline constexpr LabelTargetForm kLabelPresentedForm{ 0x0A010280, 0x530 };
// The 256x32 single-sample RGBA8 off-screen target (pitch 320, depth 0x7F8)
// the label pair also draws into, with the whole target as viewport and
// window scissor (world harvest key 735031E1).
inline constexpr LabelTargetForm kLabelOffscreenForm{ 0x04000140, 0x7F8, 0x00200100, 256, 32 };

// The label pair's catalog entries: the presented form's 2X specialization
// and, optionally, the off-screen form's single-sample one.
struct LabelDrawShaders
{
    NativeGameplayShaderInterface                presented;
    std::optional<NativeGameplayShaderInterface> offscreen;
};

// The last decoded texture. A draw with the same fetch, base bytes and mip
// bytes reuses its immutable texture version.
class LabelTextureCache
{
public:
    bool Decode(const std::array<std::uint32_t, 6>& fetch, std::span<const std::uint8_t> bytes, NativeDrawReplayTexture& texture, std::string& error, std::span<const std::uint8_t> mips = {});

private:
    std::mutex                   mutex;
    std::array<std::uint32_t, 6> fetch{};
    std::vector<std::uint8_t>    source, mipSource;
    NativeDrawReplayTexture      decoded;
    bool                         valid = false;
};

struct LabelDrawRecipe
{
    NativeDrawReplayRecipe    native;
    std::vector<std::uint8_t> patchedVertexCode, effectiveState;
    NativeDrawReplayTexture   texture;
    NativeDrawReplaySampler   sampler;
};

// Exactly one 2X label catalog entry(sample mask 9), at most one 1X; each must
// match pinned word digests/interface. Other entries fail label_catalog_entry.
bool LoadLabelDrawShaders(const std::filesystem::path& directory, LabelDrawShaders& shaders, std::string& error);
// The presented form's entry alone, as MakeBootDrawBuilder takes it.
bool LoadLabelDrawShader(const std::filesystem::path& directory, NativeGameplayShaderInterface& shader, std::string& error);
// Host sampler for a one-level 2D guest fetch, normalized as the guest menu
// route does. The fetch's LOD bias has no effect on a single level, and
// mip filter base map (kBaseMap) samples that level like point or linear.
bool DecodeLabelSampler(const std::array<std::uint32_t, 6>& fetch, NativeDrawReplaySampler& sampler, std::string& error);
// Label refusal uses 0x2000/0x2002 fixed-state words or 0x210d stencil mask.
bool BuildLabelDrawRecipe(const LabelDrawInputs& inputs, const LabelDrawShaders& shaders, LabelDrawRecipe& recipe, std::string& error, LabelTextureCache* cache = nullptr);
bool BuildLabelDraw(const LabelDrawInputs& inputs, const LabelDrawShaders& shaders, NativeDrawReplayRecipe& draw, std::string& detail, LabelTextureCache* cache = nullptr);
// The label batches, a native_draw_label_shaders family. MakeBootDrawBuilder's
// single shader is the presented form's alone.
NativeDrawFamily             MakeLabelDrawFamily(ReadMemory read, std::shared_ptr<const LabelDrawShaders> shaders);
NativeDrawFamilyRegistration LabelDrawFamily();

} // namespace rerevved::gpu::guest_d3d
