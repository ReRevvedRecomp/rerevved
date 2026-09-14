#pragma once

#include "native_draw_replay.h"

#include <span>

namespace rerevved::gpu
{

struct NativeMenuDrawView
{
    std::span<const std::uint32_t> registers;
    std::span<const std::uint32_t> vertexMicrocode;
    std::span<const std::uint32_t> pixelMicrocode;
    std::span<const std::uint8_t>  vertexBytes;
    std::span<const std::uint8_t>  indexBytes;
    std::uint64_t                  vertexShaderHash = 0;
    std::uint64_t                  pixelShaderHash  = 0;
    std::uint32_t                  vertexGuestBase  = 0;
    std::uint32_t                  indexCount       = 0;
    bool                           indexed          = true;
    std::uint32_t                  indexFormat      = 0;
    std::uint32_t                  indexEndian      = 1;
    bool                           halfPixelOffset  = true;
    // Render the guest's complete viewport into one native attachment.
    bool fullViewportTarget = false;
    // Direct UP input bytes have a CPU lifetime, not a GPU fetch allocation.
    bool guestSourceVertices = false;
    // These are owned by the caller and describe the actual sampled resource.
    const NativeDrawReplayTexture* texture = nullptr;
    const NativeDrawReplaySampler* sampler = nullptr;
};

struct NativeMenuShaderPair
{
    std::uint64_t             vertexHash = 0;
    std::uint64_t             pixelHash  = 0;
    std::vector<std::uint8_t> vertexDxil;
    std::vector<std::uint8_t> pixelDxil;
};

using NativeMenuShaders = std::array<NativeMenuShaderPair, 5>;

bool LoadNativeMenuShaders(const std::filesystem::path& directory,
                           NativeMenuShaders&           shaders,
                           std::string&                 error);
bool DecodeNativeMenuDrawGeometry(const NativeMenuDrawView& view,
                                  NativeDrawReplayRecipe&   recipe,
                                  std::string&              error);
bool ValidateNativeMenuInitialClear(const NativeMenuDrawView& view, std::uint8_t& alpha, std::string& error);
bool BuildNativeMenuDrawRecipe(const NativeMenuDrawView& view,
                               const NativeMenuShaders&  shaders,
                               NativeDrawReplayRecipe&   recipe,
                               std::string&              error);

} // namespace rerevved::gpu
