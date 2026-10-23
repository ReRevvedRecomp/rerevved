#pragma once

#include <array>
#include <cstdint>

namespace rerevved::gpu
{

// D3D12 ROV rasterization uses standard 4x positions, retaining 0 and 3 for
// guest 2x storage (SDK pipeline_cache.cpp and dxbc_translator_om.cpp).
inline constexpr std::array<std::array<std::int8_t, 2>, 4> kNativeStandardSamplePositions4x{ { { -2, -6 }, { 6, -2 }, { -6, 2 }, { 2, 6 } } };

constexpr std::uint32_t NativeRasterSampleMask(std::uint32_t schemaVersion, std::uint32_t sampleCount, std::uint32_t guestSampleMask)
{
    // Schema 2 translated shaders export SV_Coverage=9 after interpolation.
    // Samples 1 and 2 must participate in centroid selection, as with ROV.
    return schemaVersion == 2 && sampleCount == 4 && guestSampleMask == 9 ? 15 : guestSampleMask;
}

} // namespace rerevved::gpu
