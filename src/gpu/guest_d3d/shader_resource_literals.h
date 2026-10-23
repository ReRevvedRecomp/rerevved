#pragma once

#include "solid_draw_adapter.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Unified constant file in hardware register space: vertex constants c0..c255
// at 0x4000, pixel constants c0..c255 at 0x4400, ending at 0x4800.
inline constexpr std::uint32_t kShaderLiteralRegisterBase  = 0x4000;
inline constexpr std::uint32_t kShaderLiteralRegisterEnd   = 0x4800;
inline constexpr std::uint32_t kShaderLiteralPixelFirst    = 256;  // first unified float4 of the pixel stage
inline constexpr std::uint32_t kShaderLiteralMaxTableBytes = 4096; // record bytes, 512 records

// One resource record: `dwordCount` dwords loaded from guest physical memory
// into the unified constant file starting at float4 `firstFloat4`.
struct ShaderLiteralRange
{
    std::uint16_t firstFloat4 = 0, dwordCount = 0;
    std::uint32_t physical = 0;
};

// Guest order as 0x826ADB98: later records override LOAD_ALU_CONSTANT ranges at
// 0x4000+first*4. Absent table succeeds empty; failure clears outputs. Reject
// base+offset beyond 32 bits; callers pin shaders.
bool ReadShaderResourceLiterals(const ReadMemory& read, std::uint32_t object, bool pixel, std::vector<ShaderLiteralRange>& ranges, std::vector<std::pair<std::uint32_t, std::uint32_t>>& registers, std::string& error);

} // namespace rerevved::gpu::guest_d3d
