#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <rex/graphics/xenos.h>

namespace rex::graphics::testing
{

// Frozen byte extents and byte offsets from GetGuestTextureLayout/GetPackedMipOffset.
// Declared LODs remain in fetch_words; max_level is the effective reference LOD.
struct Rgba8MatrixForm
{
    const char*                   name;
    std::uint32_t                 format, width, height, pitch, max_level, declared_max;
    bool                          tiled, packed;
    std::uint32_t                 base_extent, base_data_extent, mip_extent, packed_level, tail_offset, tail_extent;
    std::uint32_t                 addressed_x, addressed_y, base_stride;
    std::array<std::uint32_t, 11> widths, heights, offsets, rows;
    const char*                   scene_ids;
    std::array<std::uint32_t, 6>  fetch_words;
    bool                          region_updates = false;
};

inline constexpr std::array<Rgba8MatrixForm, 10> kRgba8MatrixForms               = { { { "rgba8-8-packed-base", 6, 8, 8, 2, 0, 3, false, true, 2048, 1888, 0, 0, 0, 1888, 24, 8, 8192, { 8 }, { 8 }, { 64 }, { 256 }, "18", { 0x00800002, 0x1BB79086, 0x0000E007, 0x00000C14, 0x000000C0, 0x00000A00 } },
                                                                                       { "r8-512-packed", 2, 512, 512, 16, 9, 9, false, true, 262144, 262144, 126752, 5, 122880, 3872, 512, 512, 262144, { 512, 256, 128, 64, 32, 16, 8, 4, 2, 1 }, { 512, 256, 128, 64, 32, 16, 8, 4, 2, 1 }, { 0, 0, 65536, 98304, 114688, 122896, 122888, 122884, 124928, 123904 }, { 512, 256, 256, 256, 256, 256, 256, 256, 256, 256 }, "125,126", { 0x04000002, 0x1B195002, 0x003FE1FF, 0x00000248, 0x00000240, 0x1B175A00 } },
                                                                                       { "rgba8-256x4-packed-base", 6, 256, 4, 8, 0, 8, false, true, 20480, 20480, 0, 0, 0, 20480, 256, 20, 32768, { 256 }, { 4 }, { 16384 }, { 1024 }, "130", { 0x02000002, 0x1B10D086, 0x000060FF, 0x00000C14, 0x00000200, 0x00000A00 } },
                                                                                       { "rgba8-256x1-packed-base", 6, 256, 1, 8, 0, 8, false, true, 17408, 17408, 0, 0, 0, 17408, 256, 17, 32768, { 256 }, { 1 }, { 16384 }, { 1024 }, "131", { 0x02000002, 0x1B105086, 0x000000FF, 0x00000C14, 0x00000200, 0x00000A00 } },
                                                                                       { "rgba8-32-tiled", 6, 32, 32, 1, 0, 0, true, false, 4096, 4096, 0, 4294967295U, 0, 0, 32, 32, 4096, { 32 }, { 32 }, { 0 }, { 128 }, "133", { 0x80400002, 0x1BC8D086, 0x0003E01F, 0x00000C14, 0, 0x00000200 } },
                                                                                       { "rgba8-128x64-tiled", 6, 128, 64, 4, 0, 0, true, false, 32768, 32768, 0, 4294967295U, 0, 0, 128, 64, 32768, { 128 }, { 64 }, { 0 }, { 512 }, "134,136", { 0x81000002, 0x1BCAD086, 0x0007E07F, 0x00000C14, 0, 0x00000200 } },
                                                                                       { "rgba8-64-tiled", 6, 64, 64, 2, 0, 0, true, false, 16384, 16384, 0, 4294967295U, 0, 0, 64, 64, 16384, { 64 }, { 64 }, { 0 }, { 256 }, "137,139,141,142,143,144,147", { 0x80800002, 0x164DE086, 0x0007E03F, 0x00000C14, 0, 0x00000200 } },
                                                                                       { "rgba8-128-tiled", 6, 128, 128, 4, 0, 0, true, false, 65536, 65536, 0, 4294967295U, 0, 0, 128, 128, 65536, { 128 }, { 128 }, { 0 }, { 512 }, "138,145,146", { 0x81000002, 0x155BE086, 0x000FE07F, 0x00000C14, 0, 0x00000200 } },
                                                                                       { "rgba8-64x128-tiled", 6, 64, 128, 2, 0, 0, true, false, 32768, 32768, 0, 4294967295U, 0, 0, 64, 128, 32768, { 64 }, { 128 }, { 0 }, { 256 }, "148", { 0x80800002, 0x1BD2E086, 0x000FE03F, 0x00000C14, 0, 0x00000200 } },
                                                                                       { "r8-1024-regions", 2, 1024, 1024, 32, 0, 0, false, false, 1048576, 1048576, 0, 4294967295U, 0, 0, 1024, 1024, 1048576, { 1024 }, { 1024 }, { 0 }, { 1024 }, "135", { 0x08000002, 0x1FB17002, 0x007FE3FF, 0x00000248, 0, 0x200 }, true } } };
inline const Rgba8MatrixForm*                    rgba8MatrixForm                 = &kRgba8MatrixForms[0];
inline std::uint32_t                             kNativePreparedRgba8MatrixWidth = 8, kNativePreparedRgba8MatrixHeight = 8;
inline std::uint32_t                             kNativePreparedRgba8MatrixMipCount = 1, kNativePreparedRgba8MatrixBaseExtent = 2048;
inline std::uint32_t                             kNativePreparedRgba8MatrixMipExtent = 0, kNativePreparedRgba8MatrixTailOffset = 0, kNativePreparedRgba8MatrixTailExtent = 1888;
inline std::array<std::uint32_t, 11>             kNativePreparedRgba8MatrixWidths{}, kNativePreparedRgba8MatrixHeights{}, kNativePreparedRgba8MatrixGuestOffsets{}, kNativePreparedRgba8MatrixRowPitches{};

inline bool Rgba8MatrixR8()
{
    return rgba8MatrixForm->format == 2;
}

inline std::uint32_t Rgba8MatrixBlockBytes()
{
    return Rgba8MatrixR8() ? 1U : 4U;
}

inline xenos::TextureFormat Rgba8MatrixFormat()
{
    return static_cast<xenos::TextureFormat>(rgba8MatrixForm->format);
}

inline xenos::Endian Rgba8MatrixEndian()
{
    return Rgba8MatrixR8() ? xenos::Endian::kNone : xenos::Endian::k8in32;
}

inline std::uint32_t Rgba8MatrixSwizzle()
{
    return Rgba8MatrixR8() ? 0x124U : 0x60AU;
}

inline std::uint32_t Rgba8MatrixViewportWidth()
{
    return rgba8MatrixForm->region_updates ? 512U : rgba8MatrixForm->width;
}

inline std::uint32_t Rgba8MatrixViewportHeight()
{
    return rgba8MatrixForm->region_updates ? 512U : rgba8MatrixForm->height;
}

inline float Rgba8MatrixUvBias(std::uint32_t axisExtent)
{
    return 0.25F / static_cast<float>(axisExtent);
}

// Scalar 2D macro/micro formula, independent of SDK helpers and conversion.
// https://github.com/gildor2/UModel/blob/de8fbd3bc922427ea056b7340202dcdcc19ccff5/Unreal/UnTexture.cpp#L489
inline std::size_t Rgba8MatrixAddress(std::uint32_t level, std::uint32_t x, std::uint32_t y)
{
    const auto& f = *rgba8MatrixForm;
    if (!f.tiled)
        return f.offsets[level] + std::size_t(y) * f.rows[level] + x * Rgba8MatrixBlockBytes();
    const auto pitch = (f.pitch * 32U + 31U) & ~31U;
    const auto macro = ((x >> 5U) + (y >> 5U) * (pitch >> 5U)) << 9U;
    const auto micro = ((x & 7U) + ((y & 14U) << 2U)) << 2U;
    const auto mixed = macro + ((micro & ~15U) << 1U) + (micro & 15U) + ((y & 1U) << 4U);
    return ((mixed & ~511U) << 3U) + ((y & 16U) << 7U) + ((mixed & 448U) << 2U) + (((((y & 8U) >> 2U) + (x >> 3U)) & 3U) << 6U) + (mixed & 63U);
}

inline void SelectRgba8MatrixForm(const Rgba8MatrixForm& f)
{
    rgba8MatrixForm                        = &f;
    kNativePreparedRgba8MatrixWidth        = f.width;
    kNativePreparedRgba8MatrixHeight       = f.height;
    kNativePreparedRgba8MatrixMipCount     = f.max_level + 1;
    kNativePreparedRgba8MatrixBaseExtent   = f.base_extent;
    kNativePreparedRgba8MatrixMipExtent    = f.mip_extent;
    kNativePreparedRgba8MatrixTailOffset   = f.tail_offset;
    kNativePreparedRgba8MatrixTailExtent   = f.tail_extent;
    kNativePreparedRgba8MatrixWidths       = f.widths;
    kNativePreparedRgba8MatrixHeights      = f.heights;
    kNativePreparedRgba8MatrixGuestOffsets = f.offsets;
    kNativePreparedRgba8MatrixRowPitches   = f.rows;
}

} // namespace rex::graphics::testing
