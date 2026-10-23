#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// What the host texels of a resolve hold. The assembler sets it from the source
// slot; the destination format must agree.
enum class GuestResolvePacking : std::uint8_t
{
    HostRgba8,            // R8G8B8A8_UNORM bytes (k_8_8_8_8)
    Color8888Words,       // R | G << 8 | B << 16 | A << 24 (k_8_8_8_8)
    Color2101010Words,    // R | G << 10 | B << 20 | A << 30 (k_2_10_10_10)
    Depth24Stencil8Words, // depth24 << 8 | stencil (k_24_8)
    Float16x2Words,       // half(R) | half(G) << 16 (k_16_16_FLOAT from k_16_16)
};

// One guest resolve into guest memory, as Resolve (0x8269E520) computed it:
// the four copy words it stores in the device shadow (G+0x2A18..0x2A24) and
// the destination fetch descriptor at r6+0x1C.
struct GuestResolveCopy
{
    std::uint32_t                copyControl = 0, destBase = 0, destPitch = 0, destInfo = 0;
    std::array<std::uint32_t, 6> destFetch{};
    // Source rectangle in EDRAM pixels and its destination origin.
    std::uint32_t rectX = 0, rectY = 0, rectWidth = 0, rectHeight = 0;
    std::uint32_t destX = 0, destY = 0;
    // The EDRAM clear words beside them: G+0x2A2C RB_DEPTH_CLEAR (0x231D),
    // G+0x2A30 RB_COLOR_CLEAR (0x231E), G+0x2A34 RB_COLOR_CLEAR_LO (0x231F).
    // Logged only: their packing against the guest's clear values is unproven.
    std::uint32_t       depthClear = 0, colorClear = 0, colorClearLow = 0;
    GuestResolvePacking packing = GuestResolvePacking::HostRgba8;
};

// A span of the image's bytes the resolve wrote, in bytes from the image base.
struct GuestResolveRun
{
    std::uint32_t offset = 0, length = 0;
};

// Guest bytes for one physical range. `bytes` runs from `physical` through the
// last 32x32 tile the resolve touched; bytes outside `runs` are zero filler,
// not guest data, and must not be written.
struct GuestResolveImage
{
    std::uint32_t                physical = 0;
    std::vector<std::uint8_t>    bytes;
    std::vector<GuestResolveRun> runs; // coalesced written spans; empty means all of bytes
};

// Encode rect 32-bit RGBA/packed rows at hostRowPitch; host already
// selected/averaged samples. Texel(x, y)->(destX+x, destY+y); destination
// padding allowed, never written. Specific failure leaves out unchanged.
bool EncodeGuestResolve(const GuestResolveCopy& copy, std::span<const std::uint8_t> hostTexels, std::uint32_t hostRowPitch, GuestResolveImage& out, std::string& error);

} // namespace rerevved::gpu::guest_d3d
