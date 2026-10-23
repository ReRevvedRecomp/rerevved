#pragma once

#include "gpu/d3d12/native_draw_replay.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rerevved::gpu
{

struct VulkanDrawOutput
{
    std::vector<std::uint8_t> sample0;
    std::vector<std::uint8_t> sample3;
    std::string               adapterReport;
};

struct VulkanDrawPassOptions
{
    bool                 resetBetweenDraws = false;
    std::array<float, 4> clearColor        = {};
    float                clearDepth        = 1.0F;
    std::uint32_t        clearStencil      = 0;
};

bool ValidateVulkanOffscreenPass(std::span<const NativeDrawReplayRecipe> recipes,
                                 std::string&                            error);

// Requires an already validated recipe and the three exact independently
// validated VS, PS, and sample helper modules admitted by the fixed-specialization
// CLI. Recipe validation does not validate SPIR-V. The helper uses entry "main".
bool ReplayVulkanOffscreen(const NativeDrawReplayRecipe&     recipe,
                           const std::vector<std::uint32_t>& vertexSpirv,
                           const std::vector<std::uint32_t>& pixelSpirv,
                           const std::vector<std::uint32_t>& sampleSpirv,
                           VulkanDrawOutput&                 output,
                           std::string&                      error);

// Requires one or two complete recipes sharing the Vulkan target and fixed
// pipeline state. The recipes are recorded in order within one render pass.
// The optional reset clears the same attachments between the two draws for a
// diagnostic control; all resources remain owned through the terminal fence.
bool ReplayVulkanOffscreenPass(std::span<const NativeDrawReplayRecipe> recipes,
                               const std::vector<std::uint32_t>&       vertexSpirv,
                               const std::vector<std::uint32_t>&       pixelSpirv,
                               const std::vector<std::uint32_t>&       sampleSpirv,
                               const VulkanDrawPassOptions&            options,
                               VulkanDrawOutput&                       output,
                               std::string&                            error);

} // namespace rerevved::gpu
