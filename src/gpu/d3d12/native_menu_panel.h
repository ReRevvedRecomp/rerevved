#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gpu/d3d12/native_draw_replay.h"

namespace rerevved::gpu
{

// The live capture supplies the complete Xenos register file and the raw
// guest streams.  Ucode and register dwords are already decoded as
// uint32_t values; the byte streams retain their guest endian representation.
struct NativeMenuPanelView
{
    std::span<const std::uint32_t> registers;
    std::span<const std::uint32_t> vertexUcodeDwords;
    std::span<const std::uint32_t> pixelUcodeDwords;
    std::span<const std::uint8_t>  vertexBytes;
    std::uint64_t                  vertexGuestBase = 0;
    std::span<const std::uint8_t>  indexBytes;
    std::uint32_t                  indexCount       = 0;
    std::uint32_t                  indexFormat      = 0;
    std::uint32_t                  indexEndian      = 0;
    std::uint64_t                  vertexShaderHash = 0;
    std::uint64_t                  pixelShaderHash  = 0;
};

inline constexpr std::size_t kNativeMenuPanelRegisterCount = 0x5003;

bool ValidateNativeMenuPanelShaders(std::span<const std::uint8_t> vertexDxil,
                                    std::span<const std::uint8_t> pixelDxil,
                                    std::string&                  outError);

// Decode the fixed two-attribute panel source stream without shader or draw
// admission.  BuildNativeMenuPanelRecipe performs the strict admission before
// calling this bounded format conversion.
bool DecodeNativeMenuPanelGeometry(const NativeMenuPanelView&  view,
                                   std::vector<std::uint8_t>&  outVertexData,
                                   std::vector<std::uint32_t>& outIndices,
                                   std::string&                outError);

// Decode the two guest color sample planes from the ROV EDRAM snapshot.  The
// output is sample 0 followed by sample 1, each as width * height RGBA bytes.
bool DecodeNativeMenuPanelColor(std::span<const std::uint8_t>  edram,
                                std::span<const std::uint32_t> registers,
                                std::vector<std::uint8_t>&     outCombinedSamples,
                                std::string&                   outError);

// Convert an admitted live panel draw into the host-independent replay recipe.
// The pixel DXIL must be the recorded coverage executable; the coordinator
// owns its lifetime and supplies a fresh byte span for each draw.
bool BuildNativeMenuPanelRecipe(const NativeMenuPanelView&    view,
                                std::span<const std::uint8_t> vertexDxil,
                                std::span<const std::uint8_t> pixelDxil,
                                std::span<const std::uint8_t> edramBefore,
                                NativeDrawReplayRecipe&       outRecipe,
                                std::string&                  outError);

} // namespace rerevved::gpu
