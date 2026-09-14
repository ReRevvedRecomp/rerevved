#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "native_draw_replay.h"

namespace rerevved::gpu
{

struct NativeFrameReplayRecipe
{
    // Each half starts with native black color (the first draw's clear alpha)
    // and far depth. Attachments stay on the GPU through resolve and gamma.
    std::array<std::vector<NativeDrawReplayRecipe>, 2> halves;
    std::array<std::uint32_t, 256>                     gammaTable{};
    std::filesystem::path                              outputPath;
    // Optional half0 sample0/sample1, then half1 sample0/sample1 RGBA8 planes.
    std::filesystem::path sampleOutputPath;
};

struct NativeDrawFramesRecipe
{
    // Consecutive full-width frames start with native black/far clears and
    // the first draw's captured clear alpha.
    // The executor retains its device across frames and color/depth between draws.
    std::vector<std::vector<NativeDrawReplayRecipe>> frames;
    std::filesystem::path                            outputDirectory;
};

bool ValidateNativeDrawFramesRecipe(const NativeDrawFramesRecipe& recipe, std::string& error);

// Validates one owned full-width frame without copying its draw inputs.
bool ValidateNativeDrawFrame(std::span<const NativeDrawReplayRecipe> draws, std::string& error);

bool ValidateNativeFrameReplayRecipe(const NativeFrameReplayRecipe& recipe,
                                     std::string&                   error);
bool LoadNativeFrameReplayRecipe(const std::filesystem::path& path,
                                 NativeFrameReplayRecipe&     recipe,
                                 std::string&                 error);

} // namespace rerevved::gpu
