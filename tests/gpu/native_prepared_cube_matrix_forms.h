#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace rex::graphics::testing
{

// 64x64 rows frozen from SDK 6aa8ab44dee68d9e9e667ac16dff8788b39b72ba;
// BC3 verified against SDK 3c3e1c3,
// texture/util.cpp:GetGuestTextureLayout and GetPackedMipOffset.
// Six-face extent corroboration (emulator research, not hardware evidence):
// https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/texture_util.cc#L198
struct CubeMatrixForm
{
    const char*                  name;
    const char*                  scene_ids;
    std::uint32_t                format, endian, swizzle, signs, num_format, bytes_per_texel;
    std::uint32_t                base_extent, mip_extent, base_stride, tail_extent, payload_bytes;
    std::array<std::uint32_t, 8> offsets, rows, strides;
    std::array<std::uint32_t, 6> fetch_words;
    std::uint32_t                width = 64, levels = 7, pitch = 2, packed_level = 2, tail_offset = 49152;
};

inline constexpr std::array<CubeMatrixForm, 3> kCubeMatrixForms{ { { "rgba8-cube-64-packed", "38", 6, 2, 0xA0A, 0, 0, 4, 98304, 94080, 16384, 44928, 131064, { 0, 0, 49216, 49184, 49168, 51200, 50176 }, { 256, 256, 256, 256, 256, 256, 256 }, { 16384, 8192, 8192, 8192, 8192, 8192, 8192 }, { 0x00824802, 0x1BB5B086, 0x1407E03F, 0x00A81414, 0x00000183, 0x1BB43E18 } },
                                                                   { "rgba16f-cube-64-packed", "39,40,41", 32, 1, 0x688, 0x55, 1, 8, 196608, 94208, 32768, 45056, 262128, { 0, 0, 49280, 49216, 49184, 51200, 50176 }, { 512, 256, 256, 256, 256, 256, 256 }, { 32768, 8192, 8192, 8192, 8192, 8192, 8192 }, { 0x00824956, 0x1BA0B060, 0x1407E03F, 0x00A80D11, 0x00000183, 0x1B9F3E18 } },
                                                                   { "bc3-cube-128-packed", "63", 20, 1, 0x688, 0, 0, 16, 98304, 280192, 16384, 83584, 131232, { 0, 0, 98304, 196672, 196640, 196624, 197632, 197120 }, { 512, 512, 512, 512, 512, 512, 512, 512 }, { 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384 }, { 0x01000002, 0x19F39054, 0x140FE07F, 0x00000D10, 0x000001C0, 0x19EF1E00 }, 128, 8, 4, 3, 196608 } } };

// k/4 - 2, k=1..15 excluding 8. All finite normal IEEE half values.
inline constexpr std::array<std::uint16_t, 14> kCubeHalfPalette{
    0xBF00, 0xBE00, 0xBD00, 0xBC00, 0xBA00, 0xB800, 0xB400, 0x3400, 0x3800, 0x3A00, 0x3C00, 0x3D00, 0x3E00, 0x3F00
};

inline const CubeMatrixForm* cubeMatrixForm                 = &kCubeMatrixForms[0];
inline std::uint32_t         kNativePreparedCubeMatrixWidth = 64, kNativePreparedCubeMatrixHeight = 64;
inline std::uint32_t         kNativePreparedCubeMatrixMipCount = 7, kNativePreparedCubeMatrixTailOffset = 49152;
inline std::uint32_t         kNativePreparedCubeMatrixBaseExtent = 98304, kNativePreparedCubeMatrixMipExtent = 94080;

inline void SelectCubeMatrixForm(const CubeMatrixForm& form)
{
    cubeMatrixForm                 = &form;
    kNativePreparedCubeMatrixWidth = kNativePreparedCubeMatrixHeight = form.width;
    kNativePreparedCubeMatrixMipCount                                = form.levels;
    kNativePreparedCubeMatrixTailOffset                              = form.tail_offset;
    kNativePreparedCubeMatrixBaseExtent                              = form.base_extent;
    kNativePreparedCubeMatrixMipExtent                               = form.mip_extent;
}

inline std::uint32_t CubeMatrixDataChannels()
{
    return cubeMatrixForm->format == 6 ? 3U : 4U;
}

inline std::size_t CubeMatrixAddress(const CubeMatrixForm& form, std::uint32_t face, std::uint32_t level, std::uint32_t x, std::uint32_t y)
{
    return form.offsets.at(level) + std::size_t(face) * form.strides.at(level) + std::size_t(y) * form.rows.at(level) + x * form.bytes_per_texel;
}

// -1 uses the endpoint oracle; 0/1 exercise the two device alpha modes.
inline int cubeMatrixInterpolationMode = -1;

inline std::uint32_t CubeMatrixLevelUnits(std::uint32_t level)
{
    const auto n = cubeMatrixForm->width >> level;
    return cubeMatrixForm->format == 20 ? std::max(1U, n / 4) : n;
}

inline std::uint32_t CubeMatrixViewportWidth()
{
    return 6 * cubeMatrixForm->width;
}

inline std::uint32_t CubeMatrixViewportHeight()
{
    return 4 * cubeMatrixForm->width;
}

} // namespace rex::graphics::testing
