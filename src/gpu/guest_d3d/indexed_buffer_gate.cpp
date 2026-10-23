#include "indexed_buffer_gate.h"

namespace rerevved::gpu::guest_d3d
{

IndexedBufferGate IndexedBufferConsumeGate(IndexedBufferWordReader read, const std::uint8_t* base, std::uint32_t graphics) noexcept
{
    // The tiling flags are the top byte of the big-endian word at G+0x2ABC.
    if (!read || !graphics || graphics > 0xFFFFFFFFU - (kIndexedBufferTilingOffset + 3))
        return IndexedBufferGate::Unreadable;
    std::uint32_t word = 0;
    if (!read(base, graphics + kIndexedBufferTilingOffset, word))
        return IndexedBufferGate::Unreadable;
    const auto tiling = static_cast<std::uint8_t>(word >> 24);
    if (tiling == kIndexedBufferTiledByte)
        return IndexedBufferGate::TiledRecord;
    return IndexedBufferTilingAdmits(tiling) ? IndexedBufferGate::Admit : IndexedBufferGate::TilingSet;
}

} // namespace rerevved::gpu::guest_d3d
