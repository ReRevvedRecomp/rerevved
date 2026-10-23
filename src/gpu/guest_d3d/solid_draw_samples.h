#pragma once

#include "gpu/d3d12/native_draw_replay.h"

namespace rerevved::gpu::guest_d3d
{

// DecodeNativeMenuPanelColor emits guest sample 0, then guest sample 1,
// each row-major RGBA8. Replay uses host samples 0 and 3 for these planes.
std::vector<std::uint8_t> ExtractSolidDrawReferenceTile(std::span<const std::uint8_t> full, std::uint32_t x);
void                      SeedSolidDrawReferenceDestination(NativeDrawReplayRecipe& recipe, std::span<const std::uint8_t> before, std::uint32_t x);

} // namespace rerevved::gpu::guest_d3d
