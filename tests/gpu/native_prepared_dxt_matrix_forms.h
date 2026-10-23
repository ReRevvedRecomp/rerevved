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
struct DxtMatrixForm
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

inline constexpr std::array<DxtMatrixForm, 18> kDxtMatrixForms                    = { { { "bc1-8-single", 18U, 8U, 8U, 4U, 0U, 512U, 272U, 0U, 4294967295U, 0U, 0U, { 8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 256U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, "20", false, false, xenos::Endian::k8in16, { 0U }, { 32U }, { 0U }, { 0U } },
                                                                                        { "bc1-32-packed", 18U, 32U, 32U, 4U, 5U, 2048U, 1856U, 832U, 1U, 0U, 832U, { 32U, 16U, 8U, 4U, 2U, 1U, 0U, 0U, 0U, 0U, 0U }, { 32U, 16U, 8U, 4U, 2U, 1U, 0U, 0U, 0U, 0U, 0U }, { 0U, 32U, 16U, 8U, 512U, 256U, 0U, 0U, 0U, 0U, 0U }, { 256U, 256U, 256U, 256U, 256U, 256U, 0U, 0U, 0U, 0U, 0U }, "88", false, true, xenos::Endian::k8in16, { 0U, 0U, 0U, 0U, 0U, 0U }, { 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 4U, 2U, 1U, 0U, 0U }, { 0U, 0U, 0U, 0U, 2U, 1U } },
                                                                                        { "bc1-512x256-packed", 18U, 512U, 256U, 16U, 9U, 65536U, 65536U, 34624U, 4U, 32768U, 1856U, { 512U, 256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 0U }, { 256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 1U, 0U }, { 0U, 0U, 16384U, 24576U, 33792U, 33280U, 33024U, 32800U, 32784U, 32776U, 0U }, { 1024U, 512U, 256U, 256U, 256U, 256U, 256U, 256U, 256U, 256U, 0U }, "79,94,97", false, true, xenos::Endian::k8in16, { 0U, 0U, 16384U, 24576U, 32768U, 32768U, 32768U, 32768U, 32768U, 32768U }, { 128U, 64U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 4U, 2U, 1U }, { 0U, 0U, 0U, 0U, 4U, 2U, 1U, 0U, 0U, 0U } },
                                                                                        { "bc1-512-single", 18U, 512U, 512U, 16U, 0U, 131072U, 131072U, 0U, 4294967295U, 0U, 0U, { 512U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 512U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 1024U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, "13", false, false, xenos::Endian::k8in16, { 0U }, { 128U }, { 0U }, { 0U } },
                                                                                        { "bc1-1024-single", 18U, 1024U, 1024U, 32U, 0U, 524288U, 524288U, 0U, 4294967295U, 0U, 0U, { 1024U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 1024U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 2048U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, "68", false, false, xenos::Endian::k8in16, { 0U }, { 256U }, { 0U }, { 0U } },
                                                                                        { "bc1-1024-packed", 18U, 1024U, 1024U, 32U, 10U, 524288U, 524288U, 189248U, 6U, 188416U, 832U, { 1024U, 512U, 256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U }, { 1024U, 512U, 256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U }, { 0U, 0U, 131072U, 163840U, 172032U, 180224U, 188448U, 188432U, 188424U, 188928U, 188672U }, { 2048U, 1024U, 512U, 256U, 256U, 256U, 256U, 256U, 256U, 256U, 256U }, "56,58,60", false, true, xenos::Endian::k8in16, { 0U, 0U, 131072U, 163840U, 172032U, 180224U, 188416U, 188416U, 188416U, 188416U, 188416U }, { 256U, 128U, 64U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 0U, 0U, 0U, 4U, 2U, 1U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U, 1U } },
                                                                                        { "bc1-2048-single", 18U, 2048U, 2048U, 64U, 0U, 2097152U, 2097152U, 0U, 4294967295U, 0U, 0U, { 2048U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 2048U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 4096U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, "25", false, false, xenos::Endian::k8in16, { 0U }, { 512U }, { 0U }, { 0U } },
                                                                                        { "bc1-4096-single", 18U, 4096U, 4096U, 128U, 0U, 8388608U, 8388608U, 0U, 4294967295U, 0U, 0U, { 4096U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 4096U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 8192U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, "26", false, false, xenos::Endian::k8in16, { 0U }, { 1024U }, { 0U }, { 0U } },
                                                                                        { "bc2-128x64-packed", 19U, 128U, 64U, 4U, 7U, 8192U, 8192U, 20096U, 2U, 16384U, 3712U, { 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 0U, 0U, 0U }, { 64U, 32U, 16U, 8U, 4U, 2U, 1U, 1U, 0U, 0U, 0U }, { 0U, 0U, 18432U, 17408U, 16896U, 16448U, 16416U, 16400U, 0U, 0U, 0U }, { 512U, 512U, 512U, 512U, 512U, 512U, 512U, 512U, 0U, 0U, 0U }, "91", false, true, xenos::Endian::k8in16, { 0U, 0U, 16384U, 16384U, 16384U, 16384U, 16384U, 16384U }, { 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 0U, 0U, 4U, 2U, 1U }, { 0U, 0U, 4U, 2U, 1U, 0U, 0U, 0U } },
                                                                                        { "bc2-256x128-packed", 19U, 256U, 128U, 8U, 8U, 32768U, 32768U, 36480U, 3U, 32768U, 3712U, { 256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 0U, 0U }, { 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 1U, 0U, 0U }, { 0U, 0U, 16384U, 34816U, 33792U, 33280U, 32832U, 32800U, 32784U, 0U, 0U }, { 1024U, 512U, 512U, 512U, 512U, 512U, 512U, 512U, 512U, 0U, 0U }, "89", false, true, xenos::Endian::k8in16, { 0U, 0U, 16384U, 32768U, 32768U, 32768U, 32768U, 32768U, 32768U }, { 64U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 0U, 0U, 0U, 4U, 2U, 1U }, { 0U, 0U, 0U, 4U, 2U, 1U, 0U, 0U, 0U } },
                                                                                        { "bc2-256-single", 19U, 256U, 256U, 8U, 0U, 65536U, 65536U, 0U, 4294967295U, 0U, 0U, { 256U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 256U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, { 1024U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }, "140", false, false, xenos::Endian::k8in16, { 0U }, { 64U }, { 0U }, { 0U } },
                                                                                        { "bc2-256-packed", 19U, 256U, 256U, 8U, 8U, 65536U, 65536U, 50816U, 4U, 49152U, 1664U, { 256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 0U, 0U }, { 256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 0U, 0U }, { 0U, 0U, 16384U, 32768U, 49216U, 49184U, 49168U, 50176U, 49664U, 0U, 0U }, { 1024U, 512U, 512U, 512U, 512U, 512U, 512U, 512U, 512U, 0U, 0U }, "7,8,51,113,115", false, true, xenos::Endian::k8in16, { 0U, 0U, 16384U, 32768U, 49152U, 49152U, 49152U, 49152U, 49152U }, { 64U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 0U, 4U, 2U, 1U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U, 1U } },
                                                                                        { "bc2-512x256-packed", 19U, 512U, 256U, 16U, 9U, 131072U, 131072U, 69248U, 4U, 65536U, 3712U, { 512U, 256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 0U }, { 256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 1U, 0U }, { 0U, 0U, 32768U, 49152U, 67584U, 66560U, 66048U, 65600U, 65568U, 65552U, 0U }, { 2048U, 1024U, 512U, 512U, 512U, 512U, 512U, 512U, 512U, 512U, 0U }, "78,93,96", false, true, xenos::Endian::k8in16, { 0U, 0U, 32768U, 49152U, 65536U, 65536U, 65536U, 65536U, 65536U, 65536U }, { 128U, 64U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 4U, 2U, 1U }, { 0U, 0U, 0U, 0U, 4U, 2U, 1U, 0U, 0U, 0U } },
                                                                                        { "bc2-512-packed", 19U, 512U, 512U, 16U, 9U, 262144U, 262144U, 116352U, 5U, 114688U, 1664U, { 512U, 256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 0U }, { 512U, 256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 0U }, { 0U, 0U, 65536U, 81920U, 98304U, 114752U, 114720U, 114704U, 115712U, 115200U, 0U }, { 2048U, 1024U, 512U, 512U, 512U, 512U, 512U, 512U, 512U, 512U, 0U }, "64,65,66", false, true, xenos::Endian::k8in16, { 0U, 0U, 65536U, 81920U, 98304U, 114688U, 114688U, 114688U, 114688U, 114688U }, { 128U, 64U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U }, { 0U, 0U, 0U, 0U, 0U, 4U, 2U, 1U, 0U, 0U }, { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U, 1U } },
                                                                                        { "bc1-128x64-tiled-packed", 18, 128, 64, 4, 7, 8192, 8192, 16384, 2, 8192, 8192, { 128, 64, 32, 16, 8, 4, 2, 1 }, { 64, 32, 16, 8, 4, 2, 1, 1 }, { 0, 0, 8192, 8192, 8192, 8192, 8192, 8192 }, { 256, 256, 256, 256, 256, 256, 256, 256 }, "35", true, true, xenos::Endian::kNone, { 0, 0, 8192, 8192, 8192, 8192, 8192, 8192 }, { 32, 32, 32, 32, 32, 32, 32, 32 }, { 0, 0, 0, 0, 0, 4, 2, 1 }, { 0, 0, 4, 2, 1, 0, 0, 0 } },
                                                                                        { "bc1-256x128-tiled-packed", 18, 256, 128, 8, 8, 16384, 16384, 24576, 3, 16384, 8192, { 256, 128, 64, 32, 16, 8, 4, 2, 1 }, { 128, 64, 32, 16, 8, 4, 2, 1, 1 }, { 0, 0, 8192, 16384, 16384, 16384, 16384, 16384, 16384 }, { 512, 256, 256, 256, 256, 256, 256, 256, 256 }, "36", true, true, xenos::Endian::kNone, { 0, 0, 8192, 16384, 16384, 16384, 16384, 16384, 16384 }, { 64, 32, 32, 32, 32, 32, 32, 32, 32 }, { 0, 0, 0, 0, 0, 0, 4, 2, 1 }, { 0, 0, 0, 4, 2, 1, 0, 0, 0 } },
                                                                                        { "bc2-128x64-tiled-packed", 19, 128, 64, 4, 7, 16384, 16384, 32768, 2, 16384, 16384, { 128, 64, 32, 16, 8, 4, 2, 1 }, { 64, 32, 16, 8, 4, 2, 1, 1 }, { 0, 0, 16384, 16384, 16384, 16384, 16384, 16384 }, { 512, 512, 512, 512, 512, 512, 512, 512 }, "37", true, true, xenos::Endian::kNone, { 0, 0, 16384, 16384, 16384, 16384, 16384, 16384 }, { 32, 32, 32, 32, 32, 32, 32, 32 }, { 0, 0, 0, 0, 0, 4, 2, 1 }, { 0, 0, 4, 2, 1, 0, 0, 0 } },
                                                                                        { "bc2-32x64-tiled-padded-single", 19, 32, 64, 4, 0, 16384, 16384, 0, UINT32_MAX, 0, 0, { 32 }, { 64 }, { 0 }, { 512 }, "writer-bc2-padded", true, false, xenos::Endian::kNone, { 0 }, { 32 }, { 0 }, { 0 } } } };
inline const DxtMatrixForm*                    dxtMatrixForm                      = &kDxtMatrixForms[0];
inline std::uint32_t                           kNativePreparedDxtMatrixWidth      = 8U;
inline std::uint32_t                           kNativePreparedDxtMatrixHeight     = 8U;
inline std::uint32_t                           kNativePreparedDxtMatrixMipCount   = 1U;
inline std::uint32_t                           kNativePreparedDxtMatrixBaseExtent = 512U;
inline std::uint32_t                           kNativePreparedDxtMatrixMipExtent  = 0U;
inline std::uint32_t                           kNativePreparedDxtMatrixTailOffset = 0U;
inline std::uint32_t                           kNativePreparedDxtMatrixTailExtent = 0U;
inline std::array<std::uint32_t, 11>           kNativePreparedDxtMatrixWidths{};
inline std::array<std::uint32_t, 11>           kNativePreparedDxtMatrixHeights{};
inline std::array<std::uint32_t, 11>           kNativePreparedDxtMatrixGuestOffsets{};
inline std::array<std::uint32_t, 11>           kNativePreparedDxtMatrixRowPitches{};

inline bool DxtMatrixBc2()
{
    return dxtMatrixForm->format == 19U;
}

inline std::uint32_t DxtMatrixBlockBytes()
{
    return DxtMatrixBc2() ? 16U : 8U;
}

// Scalar block-coordinate address calculation; no SDK layout/untiling helpers.
inline std::size_t DxtMatrixGuestBlockOffset(std::uint32_t level, std::uint32_t x, std::uint32_t y)
{
    const auto& form = *dxtMatrixForm;
    if (!form.tiled)
        return form.offsets[level] + std::size_t(y) * form.rows[level] + x * DxtMatrixBlockBytes();
    x += form.packed_x[level];
    y += form.packed_y[level];
    const auto logBytes = DxtMatrixBlockBytes() == 8U ? 3U : 4U;
    const auto macro    = ((x >> 5U) + (y >> 5U) * (form.block_pitches[level] >> 5U)) << (logBytes + 7U);
    const auto micro    = ((x & 7U) + ((y & 14U) << 2U)) << logBytes;
    const auto mixed    = macro + ((micro & ~15U) << 1U) + (micro & 15U) + ((y & 1U) << 4U);
    const auto address  = ((mixed & ~511U) << 3U) + ((y & 16U) << 7U) + ((mixed & 448U) << 2U) +
                          (((((y & 8U) >> 2U) + (x >> 3U)) & 3U) << 6U) + (mixed & 63U);
    return form.stored_starts[level] + address;
}

inline xenos::TextureFormat DxtMatrixFormat()
{
    return static_cast<xenos::TextureFormat>(dxtMatrixForm->format);
}

inline std::uint32_t DxtMatrixViewportWidth()
{
    return std::min(512U, dxtMatrixForm->width);
}

inline std::uint32_t DxtMatrixViewportHeight()
{
    return std::min(512U, dxtMatrixForm->height);
}

// All forms carry a quarter-base-texel UV offset
// that keeps every sample off texel edges.
inline float DxtMatrixUvBias(std::uint32_t axisExtent)
{
    return 0.25F / static_cast<float>(axisExtent);
}

inline void SelectDxtMatrixForm(const DxtMatrixForm& form)
{
    dxtMatrixForm                        = &form;
    kNativePreparedDxtMatrixWidth        = form.width;
    kNativePreparedDxtMatrixHeight       = form.height;
    kNativePreparedDxtMatrixMipCount     = form.max_level + 1U;
    kNativePreparedDxtMatrixBaseExtent   = form.base_extent;
    kNativePreparedDxtMatrixMipExtent    = form.mip_extent;
    kNativePreparedDxtMatrixTailOffset   = form.tail_offset;
    kNativePreparedDxtMatrixTailExtent   = form.tail_extent;
    kNativePreparedDxtMatrixWidths       = form.widths;
    kNativePreparedDxtMatrixHeights      = form.heights;
    kNativePreparedDxtMatrixGuestOffsets = form.offsets;
    kNativePreparedDxtMatrixRowPitches   = form.rows;
}

} // namespace rex::graphics::testing
