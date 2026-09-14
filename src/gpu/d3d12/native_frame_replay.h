#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "native_draw_replay.h"

namespace rerevved::gpu
{

struct NativeFrameReplayRecipe
{
    // Each half starts with native black color and far depth. Draw attachments
    // stay on the GPU until that half's resolve and gamma pass have completed.
    std::array<std::vector<NativeDrawReplayRecipe>, 2> halves;
    std::array<std::uint32_t, 256>                     gammaTable{};
    std::filesystem::path                              outputPath;
    // Optional half0 sample0/sample1, then half1 sample0/sample1 RGBA8 planes.
    std::filesystem::path sampleOutputPath;
};

bool ValidateNativeFrameReplayRecipe(const NativeFrameReplayRecipe& recipe,
                                     std::string&                   error);
bool LoadNativeFrameReplayRecipe(const std::filesystem::path& path,
                                 NativeFrameReplayRecipe&     recipe,
                                 std::string&                 error);

} // namespace rerevved::gpu
