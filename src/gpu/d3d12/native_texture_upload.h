#pragma once

#include "native_draw_replay.h"

#include <span>

namespace rerevved::gpu
{

using NativeTextureFetch = std::array<std::uint32_t, 6>;

struct NativeTextureMemoryRange
{
    std::uint32_t address = 0;
    std::uint32_t size    = 0;
};

// Decodes one unpacked 2D base level. Fetch words are host-order values from
// the guest's big-endian shadow. The caller must keep the source bytes stable
// until DecodeNativeTexture returns. The result contains no mip chain.
bool GetNativeTextureMemoryRange(const NativeTextureFetch& fetch,
                                 NativeTextureMemoryRange& range,
                                 std::string&              error);
bool DecodeNativeTexture(const NativeTextureFetch&     fetch,
                         std::span<const std::uint8_t> source,
                         NativeDrawReplayTexture&      texture,
                         std::string&                  error);

} // namespace rerevved::gpu
