#pragma once

#include "native_frame_assembler.h"
#include "solid_draw_adapter.h"

#include <optional>

namespace rerevved::gpu::guest_d3d
{

// Pinned scene quads with oPos=c0..c3*(position, 1), alpha-test-only kills, and
// two diagonal rectangle triangles prove whole-square coverage after raster
// snap/viewport limit, for any sample pattern. Empty proves nothing.
std::optional<NativeDrawReplayScissor> SceneQuadCoverage(const NativeDrawReplayRecipe& draw);

// The vertex hash a scene recipe of the fog composite carries: the scene
// catalog's hash of guest VS F65866A40B248418 after the family links and
// fetch-patches it, which the catalog entry names `vertex_hash`.
inline constexpr std::uint64_t kSceneCompositeVertexHash = 0x96DCE6C1AB07E076ULL;

// Fog key 62663883, VS F65866A4/PS F6CF41BE: one sample, pass-through clip
// position w=1. Rect-splitting triangles cover strictly-inside centers after
// half-pixel offset and 1/256 snap within viewport; otherwise empty.
std::optional<NativeDrawReplayScissor> SceneCompositeCoverage(const NativeDrawReplayRecipe& draw);

// Scene coverage honors G+0x293C alpha-test and alpha-to-mask state; recipe
// omits those fields.
NativeFrameDrawCoverage MakeSceneDrawCoverage(ReadMemory read);

} // namespace rerevved::gpu::guest_d3d
