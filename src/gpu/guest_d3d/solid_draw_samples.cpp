#include "solid_draw_samples.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rerevved::gpu::guest_d3d
{
namespace
{

constexpr std::size_t kTilePlaneBytes = 640 * 720 * 4;
constexpr std::size_t kFullPlaneBytes = 1280 * 720 * 4;

void checkX(std::uint32_t x)
{
    if (x != 0 && x != 640)
        throw std::runtime_error("adapter_comparison_tile_window");
}

} // namespace

std::vector<std::uint8_t> ExtractSolidDrawReferenceTile(std::span<const std::uint8_t> full, std::uint32_t x)
{
    checkX(x);
    if (full.size() != 2 * kFullPlaneBytes)
        throw std::runtime_error("adapter_full_sample_extent");
    std::vector<std::uint8_t> bytes(2 * kTilePlaneBytes);
    for (unsigned plane = 0; plane < 2; ++plane)
        for (unsigned y = 0; y < 720; ++y)
            std::copy_n(full.data() + plane * kFullPlaneBytes + (y * 1280 + x) * 4,
                        640 * 4,
                        bytes.data() + plane * kTilePlaneBytes + y * 640 * 4);
    return bytes;
}

void SeedSolidDrawReferenceDestination(NativeDrawReplayRecipe& recipe, std::span<const std::uint8_t> before, std::uint32_t x)
{
    checkX(x);
    if (recipe.width != 1280 || recipe.height != 720 || recipe.sampleCount != 4 ||
        recipe.targetFormat != NativeDrawReplayTargetFormat::Rgba8 ||
        recipe.clearColor != std::array<std::uint8_t, 4>{} || recipe.depth.enabled || recipe.stencil.enabled)
        throw std::runtime_error("adapter_destination_target_unsupported");
    if (before.size() != 2 * kTilePlaneBytes)
        throw std::runtime_error("adapter_reference_sample_extent");
    // Allocate complete zero-filled planes so the other half stays transparent.
    std::vector<std::uint8_t>                    sample0(kFullPlaneBytes), sample1(kFullPlaneBytes);
    const std::array<std::span<std::uint8_t>, 2> planes{ sample0, sample1 };
    for (unsigned plane = 0; plane < 2; ++plane)
        for (unsigned y = 0; y < 720; ++y)
            std::copy_n(before.data() + plane * kTilePlaneBytes + y * 640 * 4, 640 * 4, planes[plane].data() + (y * 1280 + x) * 4);
    recipe.initialSample0 = std::move(sample0);
    recipe.initialSample1 = std::move(sample1);
}

} // namespace rerevved::gpu::guest_d3d
