#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <rex/graphics/xenos.h>

namespace rex::graphics::testing
{

// Frozen single-level tiled R32_FLOAT layout, asserted against SDK helpers.
// Declared LODs remain in fetch_words; max_level is the effective reference LOD.
struct FloatMatrixForm
{
    const char*                   name;
    std::uint32_t                 format, width, height, pitch, max_level, declared_max;
    bool                          tiled, packed;
    std::uint32_t                 base_extent, base_data_extent, mip_extent, packed_level, tail_offset, tail_extent;
    std::uint32_t                 addressed_x, addressed_y, base_stride;
    std::array<std::uint32_t, 11> widths, heights, offsets, rows;
    const char*                   scene_ids;
    std::array<std::uint32_t, 6>  fetch_words;
};

inline constexpr std::array<FloatMatrixForm, 1> kFloatMatrixForms{ { { "r32-float-512-tiled", 36, 512, 512, 16, 0, 0, true, false, 1048576, 1048576, 0, 4294967295U, 0, 0, 512, 512, 1048576, { 512 }, { 512 }, { 0 }, { 2048 }, "52", { 0x84000156, 0x185920A4, 0x003FE1FF, 0x000016D1, 0, 0x200 } } } };

inline const FloatMatrixForm*        floatMatrixForm                 = &kFloatMatrixForms[0];
inline std::uint32_t                 kNativePreparedFloatMatrixWidth = 512, kNativePreparedFloatMatrixHeight = 512;
inline std::uint32_t                 kNativePreparedFloatMatrixMipCount = 1, kNativePreparedFloatMatrixBaseExtent = 1048576;
inline std::uint32_t                 kNativePreparedFloatMatrixMipExtent = 0, kNativePreparedFloatMatrixTailOffset = 0, kNativePreparedFloatMatrixTailExtent = 0;
inline std::array<std::uint32_t, 11> kNativePreparedFloatMatrixWidths{}, kNativePreparedFloatMatrixHeights{}, kNativePreparedFloatMatrixGuestOffsets{}, kNativePreparedFloatMatrixRowPitches{};

inline std::uint32_t FloatMatrixBlockBytes()
{
    return 4U;
}

inline xenos::TextureFormat FloatMatrixFormat()
{
    return static_cast<xenos::TextureFormat>(floatMatrixForm->format);
}

inline xenos::Endian FloatMatrixEndian()
{
    return xenos::Endian::k8in32;
}

inline std::uint32_t FloatMatrixSwizzle()
{
    return 0xB68U;
}

inline std::uint32_t FloatMatrixViewportWidth()
{
    return floatMatrixForm->width;
}

inline std::uint32_t FloatMatrixViewportHeight()
{
    return floatMatrixForm->height;
}

inline float FloatMatrixUvBias(std::uint32_t axisExtent)
{
    return 0.25F / static_cast<float>(axisExtent);
}

// Scalar 2D macro/micro formula, independent of SDK helpers and conversion.
// https://github.com/gildor2/UModel/blob/de8fbd3bc922427ea056b7340202dcdcc19ccff5/Unreal/UnTexture.cpp#L489
inline std::size_t FloatMatrixAddress(std::uint32_t level, std::uint32_t x, std::uint32_t y)
{
    const auto& f = *floatMatrixForm;
    if (!f.tiled)
        return f.offsets[level] + std::size_t(y) * f.rows[level] + x * FloatMatrixBlockBytes();
    const auto pitch = (f.pitch * 32U + 31U) & ~31U;
    const auto macro = ((x >> 5U) + (y >> 5U) * (pitch >> 5U)) << 9U;
    const auto micro = ((x & 7U) + ((y & 14U) << 2U)) << 2U;
    const auto mixed = macro + ((micro & ~15U) << 1U) + (micro & 15U) + ((y & 1U) << 4U);
    return ((mixed & ~511U) << 3U) + ((y & 16U) << 7U) + ((mixed & 448U) << 2U) + (((((y & 8U) >> 2U) + (x >> 3U)) & 3U) << 6U) + (mixed & 63U);
}

inline void SelectFloatMatrixForm(const FloatMatrixForm& f)
{
    floatMatrixForm                        = &f;
    kNativePreparedFloatMatrixWidth        = f.width;
    kNativePreparedFloatMatrixHeight       = f.height;
    kNativePreparedFloatMatrixMipCount     = f.max_level + 1;
    kNativePreparedFloatMatrixBaseExtent   = f.base_extent;
    kNativePreparedFloatMatrixMipExtent    = f.mip_extent;
    kNativePreparedFloatMatrixTailOffset   = f.tail_offset;
    kNativePreparedFloatMatrixTailExtent   = f.tail_extent;
    kNativePreparedFloatMatrixWidths       = f.widths;
    kNativePreparedFloatMatrixHeights      = f.heights;
    kNativePreparedFloatMatrixGuestOffsets = f.offsets;
    kNativePreparedFloatMatrixRowPitches   = f.rows;
}

} // namespace rex::graphics::testing
