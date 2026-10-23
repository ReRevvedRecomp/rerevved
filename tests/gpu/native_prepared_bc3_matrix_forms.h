#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <rex/graphics/xenos.h>
#include <string_view>

namespace rex::graphics::testing
{

// Frozen storage-layout expectations derived from texture/util.cpp's
// GetGuestTextureLayout and GetPackedMipOffset. Actual helpers must agree.
// A single parameterized PM4/Original/native proof implementation uses every row.
struct Bc3MatrixForm
{
    const char*                   name;
    std::uint32_t                 format, width, height, pitch, max_level;
    std::uint32_t                 base_extent, base_data_extent, mip_extent, packed_level, tail_offset, tail_extent;
    std::array<std::uint32_t, 11> widths, heights, offsets, rows;
    const char*                   scene_ids;
    bool                          tiled, packed;
    xenos::Endian                 endian;
    std::array<std::uint32_t, 11> stored_starts, block_pitches, packed_x, packed_y;
};

inline constexpr std::array<Bc3MatrixForm, 10> kBc3MatrixForms = { { { "bc3-64-packed", 20, 64, 64, 4, 6, 8192, 7936, 18048, 2, 16384, 1664, { 64, 32, 16, 8, 4, 2, 1 }, { 64, 32, 16, 8, 4, 2, 1 }, { 0, 0, 16448, 16416, 16400, 17408, 16896 }, { 512, 512, 512, 512, 512, 512, 512 }, "100", false, true, xenos::Endian::k8in16, { 0U, 0U, 16384U, 16384U, 16384U, 16384U, 16384U }, { 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 4U, 2U, 1U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 2U, 1U } },
                                                                     { "bc3-128-packed", 20, 128, 128, 4, 7, 16384, 16384, 34432, 3, 32768, 1664, { 128, 64, 32, 16, 8, 4, 2, 1 }, { 128, 64, 32, 16, 8, 4, 2, 1 }, { 0, 0, 16384, 32832, 32800, 32784, 33792, 33280 }, { 512, 512, 512, 512, 512, 512, 512, 512 }, "74,82", false, true, xenos::Endian::k8in16, { 0U, 0U, 16384U, 32768U, 32768U, 32768U, 32768U, 32768U }, { 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 4U, 2U, 1U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 2U, 1U } },
                                                                     { "bc3-256x128-single", 20, 256, 128, 8, 0, 32768, 32768, 0, UINT32_MAX, 0, 0, { 256 }, { 128 }, { 0 }, { 1024 }, "85", false, false, xenos::Endian::k8in16, { 0U }, { 64U }, { 0U }, { 0U } },
                                                                     { "bc3-256x128-packed", 20, 256, 128, 8, 8, 32768, 32768, 36480, 3, 32768, 3712, { 256, 128, 64, 32, 16, 8, 4, 2, 1 }, { 128, 64, 32, 16, 8, 4, 2, 1, 1 }, { 0, 0, 16384, 34816, 33792, 33280, 32832, 32800, 32784 }, { 1024, 512, 512, 512, 512, 512, 512, 512, 512 }, "80", false, true, xenos::Endian::k8in16, { 0U, 0U, 16384U, 32768U, 32768U, 32768U, 32768U, 32768U, 32768U }, { 64U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 0U, 0U, 0U, 4U, 2U, 1U }, { 0U, 0U, 0U, 4U, 2U, 1U, 0U, 0U, 0U } },
                                                                     { "bc3-256-packed", 20, 256, 256, 8, 8, 65536, 65536, 50816, 4, 49152, 1664, { 256, 128, 64, 32, 16, 8, 4, 2, 1 }, { 256, 128, 64, 32, 16, 8, 4, 2, 1 }, { 0, 0, 16384, 32768, 49216, 49184, 49168, 50176, 49664 }, { 1024, 512, 512, 512, 512, 512, 512, 512, 512 }, "55,57,59,102,104,106", false, true, xenos::Endian::k8in16, { 0U, 0U, 16384U, 32768U, 49152U, 49152U, 49152U, 49152U, 49152U }, { 64U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 0U, 4U, 2U, 1U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U, 1U } },
                                                                     { "bc3-512x256-packed", 20, 512, 256, 16, 9, 131072, 131072, 69248, 4, 65536, 3712, { 512, 256, 128, 64, 32, 16, 8, 4, 2, 1 }, { 256, 128, 64, 32, 16, 8, 4, 2, 1, 1 }, { 0, 0, 32768, 49152, 67584, 66560, 66048, 65600, 65568, 65552 }, { 2048, 1024, 512, 512, 512, 512, 512, 512, 512, 512 }, "95,98", false, true, xenos::Endian::k8in16, { 0U, 0U, 32768U, 49152U, 65536U, 65536U, 65536U, 65536U, 65536U, 65536U }, { 128U, 64U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 4U, 2U, 1U }, { 0U, 0U, 0U, 0U, 4U, 2U, 1U, 0U, 0U, 0U } },
                                                                     { "bc3-512-single", 20, 512, 512, 16, 0, 262144, 262144, 0, UINT32_MAX, 0, 0, { 512 }, { 512 }, { 0 }, { 2048 }, "61,62,149", false, false, xenos::Endian::k8in16, { 0U }, { 128U }, { 0U }, { 0U } },
                                                                     { "bc3-512-packed", 20, 512, 512, 16, 9, 262144, 262144, 116352, 5, 114688, 1664, { 512, 256, 128, 64, 32, 16, 8, 4, 2, 1 }, { 512, 256, 128, 64, 32, 16, 8, 4, 2, 1 }, { 0, 0, 65536, 81920, 98304, 114752, 114720, 114704, 115712, 115200 }, { 2048, 1024, 512, 512, 512, 512, 512, 512, 512, 512 }, "19,31,49", false, true, xenos::Endian::k8in16, { 0U, 0U, 65536U, 81920U, 98304U, 114688U, 114688U, 114688U, 114688U, 114688U }, { 128U, 64U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 0U, 0U, 4U, 2U, 1U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U, 1U } },
                                                                     { "bc3-128-tiled-single", 20, 128, 128, 4, 0, 16384, 16384, 0, UINT32_MAX, 0, 0, { 128 }, { 128 }, { 0 }, { 512 }, "75", true, false, xenos::Endian::kNone, { 0 }, { 32 }, { 0 }, { 0 } },
                                                                     { "bc3-128-tiled-packed", 20, 128, 128, 4, 7, 16384, 16384, 49152, 3, 32768, 16384, { 128, 64, 32, 16, 8, 4, 2, 1 }, { 128, 64, 32, 16, 8, 4, 2, 1 }, { 0, 0, 16384, 32768, 32768, 32768, 32768, 32768 }, { 512, 512, 512, 512, 512, 512, 512, 512 }, "writer-bc3-packed", true, true, xenos::Endian::kNone, { 0, 0, 16384, 32768, 32768, 32768, 32768, 32768 }, { 32, 32, 32, 32, 32, 32, 32, 32 }, { 0, 0, 0, 4, 2, 1, 0, 0 }, { 0, 0, 0, 0, 0, 0, 2, 1 } } } };
// -1 selects the endpoint oracle; 0/1 select device-only interpolation modes.
inline int                           bc3MatrixInterpolationMode         = -1;
inline const Bc3MatrixForm*          bc3MatrixForm                      = &kBc3MatrixForms[0];
inline std::uint32_t                 kNativePreparedBc3MatrixWidth      = 64U;
inline std::uint32_t                 kNativePreparedBc3MatrixHeight     = 64U;
inline std::uint32_t                 kNativePreparedBc3MatrixMipCount   = 7U;
inline std::uint32_t                 kNativePreparedBc3MatrixBaseExtent = 8192U;
inline std::uint32_t                 kNativePreparedBc3MatrixMipExtent  = 0U;
inline std::uint32_t                 kNativePreparedBc3MatrixTailOffset = 0U;
inline std::uint32_t                 kNativePreparedBc3MatrixTailExtent = 0U;
inline std::array<std::uint32_t, 11> kNativePreparedBc3MatrixWidths{};
inline std::array<std::uint32_t, 11> kNativePreparedBc3MatrixHeights{};
inline std::array<std::uint32_t, 11> kNativePreparedBc3MatrixGuestOffsets{};
inline std::array<std::uint32_t, 11> kNativePreparedBc3MatrixRowPitches{};

inline std::uint32_t Bc3MatrixBlockBytes()
{
    return 16U;
}

// Scalar block-coordinate address calculation; no SDK layout/untiling helpers.
inline std::size_t Bc3MatrixGuestBlockOffset(std::uint32_t level, std::uint32_t x, std::uint32_t y)
{
    const auto& form = *bc3MatrixForm;
    if (!form.tiled)
        return form.offsets[level] + std::size_t(y) * form.rows[level] + x * Bc3MatrixBlockBytes();
    x += form.packed_x[level];
    y += form.packed_y[level];
    const auto logBytes = Bc3MatrixBlockBytes() == 8U ? 3U : 4U;
    const auto macro    = ((x >> 5U) + (y >> 5U) * (form.block_pitches[level] >> 5U)) << (logBytes + 7U);
    const auto micro    = ((x & 7U) + ((y & 14U) << 2U)) << logBytes;
    const auto mixed    = macro + ((micro & ~15U) << 1U) + (micro & 15U) + ((y & 1U) << 4U);
    const auto address  = ((mixed & ~511U) << 3U) + ((y & 16U) << 7U) + ((mixed & 448U) << 2U) +
                          (((((y & 8U) >> 2U) + (x >> 3U)) & 3U) << 6U) + (mixed & 63U);
    return form.stored_starts[level] + address;
}

inline xenos::TextureFormat Bc3MatrixFormat()
{
    return static_cast<xenos::TextureFormat>(bc3MatrixForm->format);
}

inline std::uint32_t Bc3MatrixViewportWidth()
{
    return std::min(512U, bc3MatrixForm->width);
}

inline std::uint32_t Bc3MatrixViewportHeight()
{
    return std::min(512U, bc3MatrixForm->height);
}

// All forms carry a quarter-base-texel UV offset
// that keeps every sample off texel edges.
inline float Bc3MatrixUvBias(std::uint32_t axisExtent)
{
    return 0.25F / static_cast<float>(axisExtent);
}

inline void SelectBc3MatrixForm(const Bc3MatrixForm& form)
{
    bc3MatrixForm                        = &form;
    kNativePreparedBc3MatrixWidth        = form.width;
    kNativePreparedBc3MatrixHeight       = form.height;
    kNativePreparedBc3MatrixMipCount     = form.max_level + 1U;
    kNativePreparedBc3MatrixBaseExtent   = form.base_extent;
    kNativePreparedBc3MatrixMipExtent    = form.mip_extent;
    kNativePreparedBc3MatrixTailOffset   = form.tail_offset;
    kNativePreparedBc3MatrixTailExtent   = form.tail_extent;
    kNativePreparedBc3MatrixWidths       = form.widths;
    kNativePreparedBc3MatrixHeights      = form.heights;
    kNativePreparedBc3MatrixGuestOffsets = form.offsets;
    kNativePreparedBc3MatrixRowPitches   = form.rows;
}

} // namespace rex::graphics::testing
