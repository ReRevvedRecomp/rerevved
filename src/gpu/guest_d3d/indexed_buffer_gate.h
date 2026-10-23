#pragma once

#include <cstdint>

namespace rerevved::gpu::guest_d3d
{

// G+0x2ABC top byte:mask 0x01 selects tile-record branch; 0x20 also blocks
// untiled. World census saw only 0x00/0x21; exact 0x21 alone enters tiled gate.
inline constexpr std::uint32_t kIndexedBufferTilingOffset = 0x2ABC;
inline constexpr std::uint8_t  kIndexedBufferTilingMask   = 0x21;
inline constexpr std::uint8_t  kIndexedBufferTiledByte    = 0x21;

constexpr bool IndexedBufferTilingAdmits(std::uint8_t tilingByte) noexcept
{
    return (tilingByte & kIndexedBufferTilingMask) == 0;
}

// Reads one big-endian guest word as a host value; false when unreadable.
using IndexedBufferWordReader = bool (*)(const std::uint8_t* base, std::uint32_t address, std::uint32_t& value);

enum class IndexedBufferGate : std::uint8_t
{
    Admit,       // tiling byte clear: the untiled path
    TilingSet,   // another set tiling byte: the tile-record branch may run
    TiledRecord, // the tiling byte is exactly kIndexedBufferTiledByte
    Unreadable,  // the tiling word could not be read
};

// The single admission test for an IndexedBuffer root, read by the consuming
// path and by the developer effect probe, so both agree on which roots are
// untiled and which are the tiled-record ones.
IndexedBufferGate IndexedBufferConsumeGate(IndexedBufferWordReader read, const std::uint8_t* base, std::uint32_t graphics) noexcept;

} // namespace rerevved::gpu::guest_d3d
