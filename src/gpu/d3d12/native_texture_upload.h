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

struct NativeTextureMemoryRanges
{
    NativeTextureMemoryRange base;
    NativeTextureMemoryRange mips;
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

// Reports exact physical spans for base and mip storage. DecodeNativeTexture
// requires those lengths, packs array slices then mips, and decodes k_24_8 to
// R32Float values in [0, 1].
bool GetNativeTextureMemoryRanges(const NativeTextureFetch&  fetch,
                                  NativeTextureMemoryRanges& ranges,
                                  std::string&               error);
bool DecodeNativeTexture(const NativeTextureFetch&     fetch,
                         std::span<const std::uint8_t> baseSource,
                         std::span<const std::uint8_t> mipSource,
                         NativeDrawReplayTexture&      texture,
                         std::string&                  error);

// Publishes immutable owned packed bytes and their exact subresource layout.
bool DecodeNativeTextureVersioned(const NativeTextureFetch&     fetch,
                                  std::span<const std::uint8_t> baseSource,
                                  std::span<const std::uint8_t> mipSource,
                                  NativeDrawReplayTexture&      texture,
                                  std::string&                  error);

} // namespace rerevved::gpu
