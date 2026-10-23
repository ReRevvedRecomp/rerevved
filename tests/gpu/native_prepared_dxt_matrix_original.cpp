// Parameterized from native_prepared_bc2_rect_original.cpp. The matrix shares
// its PM4 producer, callbacks, overlapping writes and copy retirement path.
/*
 * Fixture setup adapted from ReXGlue's installed_prepared_control/control.cpp
 * at df8ec1c544c1e8f391dcfaff295ef387a1a7fea9. Original notice:
 *
 * Copyright (c) 2026, Tom Clay <tomc@tctechstuff.com>
 *
 * Portions of this software are derived from the Xenia project:
 * Copyright (c) 2022, Ben Vanik and Xenia project contributors
 * https://xenia.jp
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "native_prepared_dxt_matrix_fixture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/init.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/presenter.h>

namespace
{

using rex::graphics::diagnostic::DrawCaptureExecutionStatus;
using rex::graphics::diagnostic::DrawCaptureFrameInputCopyView;
using rex::graphics::diagnostic::DrawCaptureFrameInputDrawView;
using rex::graphics::diagnostic::DrawCaptureFrameInputObserver;
using rex::graphics::diagnostic::DrawCaptureFrameInputSwapView;

void TraceOriginalPhase(std::string_view phase)
{
    std::cerr << "native_prepared_dxt_matrix_original: " << phase << '\n'
              << std::flush;
}

constexpr std::uint32_t kMmioBaseAddress                    = 0x7FC80000U;
constexpr std::uint32_t kCpRbWptr                           = 0x01C5U;
constexpr std::uint32_t kWptrAddress                        = kMmioBaseAddress + kCpRbWptr * 4U;
constexpr std::uint32_t kRingSizeLog2                       = 12U;
constexpr std::uint32_t kRingWordCount                      = 1U << (kRingSizeLog2 + 1U);
constexpr std::uint32_t kWritebackBlockSizeLog2             = 2U;
constexpr std::uint32_t kPageSize                           = 0x1000U;
constexpr std::uint32_t kNativePreparedDxtMatrixIndexCount  = 3U;
constexpr auto          kNativePreparedDxtMatrixIndexEndian = rex::graphics::xenos::Endian::k8in16;
constexpr std::uint32_t kNativePreparedDxtMatrixDmaSize =
    kNativePreparedDxtMatrixIndexCount | (static_cast<std::uint32_t>(kNativePreparedDxtMatrixIndexEndian) << 30U);
constexpr std::uint32_t kNativePreparedDxtMatrixDrawInitiator =
    static_cast<std::uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
    (static_cast<std::uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6U) |
    (kNativePreparedDxtMatrixIndexCount << 16U);

// This is the cataloged position/fetch-95 VS with only its point-of-use
// interpolator mapping widened to carry UV, lod0 and lod1 in r3.xyzw.
// r0 remains the position/scalar work register; the final export reads r3.
constexpr std::array<std::uint32_t, 45> kVertexShader = {
    0x30052003U, 0x00001200U, 0xC2000000U, 0x00006005U, 0x200B1200U, 0x12000000U, 0x00000000U, 0x100DC400U, 0x22000000U, 0x2DF82000U, 0x00393A88U, 0x0000000EU, 0x05F83000U, 0x40263688U, 0x0000040EU, 0xC8020001U, 0x00A7A700U, 0xAF020100U, 0xC8080001U, 0x00A7A700U, 0xAF020300U, 0xC8020000U, 0x00A7A700U, 0xAF020200U, 0xB8410101U, 0x01A7A741U, 0x8F020008U, 0xC801803EU, 0x00A7A700U, 0xAF010400U, 0xC802803EU, 0x00A7A700U, 0xAF010500U, 0xC804803EU, 0x00A7A700U, 0xAF010600U, 0xC808803EU, 0x00A7A700U, 0xAF010700U, 0xC80F8000U, 0x00000000U, 0xE2030300U, 0x4E4A0007U, 0x95A570DEU, 0x30B52E1DU
};

constexpr std::array<std::uint32_t, 3> MakePixelAddExport(std::uint32_t source0,
                                                          std::uint32_t source1)
{
    // AluInstruction fields: export_data=1, vector_write_mask=0xF, vector
    // opcode=kAdd, and temporary source selectors set for r[source0/r1].
    return { 0x000F8000U, 0U, 0xC0000000U | ((source0 & 0xFFU) << 16U) | ((source1 & 0xFFU) << 8U) };
}

constexpr auto kPixelAddExport = MakePixelAddExport(1U, 2U);

std::string LoadedModulePath(const wchar_t* module_name)
{
#if defined(_WIN32)
    const auto module = GetModuleHandleW(module_name);
    if (!module)
        return {};
    std::array<wchar_t, 32768> path{};
    const auto                 length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size())
        return {};
    return std::filesystem::path(std::wstring(path.data(), length)).string();
#else
    (void)module_name;
    return {};
#endif
}

// CF alloc + five instructions: setTexLOD(r0.z), tfetch2D(r1),
// setTexLOD(r0.w), tfetch2D(r2), add(r1,r2)->oC0. The two fetches use
// register LOD (bit 29) with computed LOD (bit 28) and gradients disabled.
constexpr std::array<std::uint32_t, 18> kPixelShader = {
    0x00000000U, 0x5001C400U, 0x22000255U, 0x08000018U, 0x00000000U, 0x00000000U, 0x10081001U, 0x2F1FF688U, 0x00004000U, 0x0C000018U, 0x00000000U, 0x00000000U, 0x10082001U, 0x2F1FF688U, 0x00004000U, kPixelAddExport[0], kPixelAddExport[1], kPixelAddExport[2]
};

// Match the clean installed control VS identity rows from SDK commit
// df8ec1c544c1e8f391dcfaff295ef387a1a7fea9,
// tests/installed_prepared_control/control.cpp:kConsumerVertexConstants.
constexpr std::array<std::uint32_t, 36> kVertexConstants = {
    0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U
};

constexpr std::array<std::uint32_t, 4> kPixelConstant0   = { 0U, 0U, 0U, 0x3F800000U };
constexpr std::array<std::uint32_t, 4> kPixelConstant255 = {
    0x3DCCCCCDU, 0x3E4CCCCDU, 0x3E99999AU, 0x3F800000U
};

std::uint32_t AlignToPage(std::uint32_t bytes)
{
    return bytes == 0U ? 0U : (bytes + kPageSize - 1U) & ~(kPageSize - 1U);
}

void AppendBigEndianFloat(std::vector<std::uint8_t>& bytes, float value)
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    bytes.push_back(static_cast<std::uint8_t>(bits >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(bits));
}

std::uint32_t ClampUnorm8(std::uint32_t value)
{
    return std::min(255U, value);
}

std::uint8_t QuantizeUnorm8(std::uint32_t numerator)
{
    constexpr std::uint32_t kUnorm8Denominator = 255U;
    // The render target is UNORM8. Match the D3D float-to-UNORM reference
    // conversion after the normalized source-alpha blend, rather than
    // truncating it.
    const auto quotient = (numerator + kUnorm8Denominator / 2U) / kUnorm8Denominator;
    return static_cast<std::uint8_t>(ClampUnorm8(quotient));
}

std::vector<std::uint8_t> MakeExpectedPixels(const rex::graphics::testing::NativePreparedDxtMatrixFixture& fixture,
                                             std::uint32_t                                                 first_lod,
                                             std::uint32_t                                                 second_lod)
{
    if (first_lod >= fixture.texture.mips.size() || second_lod >= fixture.texture.mips.size() ||
        fixture.texture.mips[first_lod].linear_rgba8.size() !=
            static_cast<std::size_t>(fixture.texture.mips[first_lod].width) *
                fixture.texture.mips[first_lod].height * 4U ||
        fixture.texture.mips[second_lod].linear_rgba8.size() !=
            static_cast<std::size_t>(fixture.texture.mips[second_lod].width) *
                fixture.texture.mips[second_lod].height * 4U ||
        fixture.width == 0U || fixture.height == 0U)
    {
        return {};
    }
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    std::memcpy(&fetch, fixture.texture.fetch_words.data(), sizeof(fetch));
    if (fetch.mag_filter != rex::graphics::xenos::TextureFilter::kPoint ||
        fetch.min_filter != rex::graphics::xenos::TextureFilter::kPoint ||
        fetch.mip_filter != rex::graphics::xenos::TextureFilter::kPoint ||
        fetch.clamp_x != rex::graphics::xenos::ClampMode::kClampToEdge ||
        fetch.clamp_y != rex::graphics::xenos::ClampMode::kClampToEdge ||
        fetch.clamp_z != rex::graphics::xenos::ClampMode::kClampToEdge)
    {
        return {};
    }
    const auto&               a     = fixture.texture.mips[first_lod].linear_rgba8;
    const auto&               b     = fixture.texture.mips[second_lod].linear_rgba8;
    const auto&               a_mip = fixture.texture.mips[first_lod];
    const auto&               b_mip = fixture.texture.mips[second_lod];
    std::vector<std::uint8_t> result(static_cast<std::size_t>(rex::graphics::testing::kNativePreparedDxtMatrixOutputWidth) *
                                         rex::graphics::testing::kNativePreparedDxtMatrixOutputHeight * 4U,
                                     0U);
    for (std::size_t offset = 3U; offset < result.size(); offset += 4U)
        result[offset] = 255U;
    // The authored fetch has point mag/min/mip filters and clamp-to-edge.
    // Integer arithmetic floors the triangle's quarter-base-texel bias
    // at the guest integer position independently of GPU interpolation.
    const auto nearestTexel = [&](std::uint32_t pixel, std::uint32_t sourceExtent, std::uint32_t viewportExtent, std::uint32_t baseExtent)
    {
        const auto bias = viewportExtent;
        // PA_SU_VTX_CNTL pix_center 0 (D3D) samples at integer pixel positions.
        const auto numerator = ((4ULL * pixel) * baseExtent + bias) * sourceExtent;
        return std::min(sourceExtent - 1U, static_cast<std::uint32_t>(numerator / (4ULL * viewportExtent * baseExtent)));
    };
    for (std::uint32_t y = 0; y < fixture.height; ++y)
    {
        const auto ay = nearestTexel(y, a_mip.height, fixture.height, fixture.texture.height);
        const auto by = nearestTexel(y, b_mip.height, fixture.height, fixture.texture.height);
        for (std::uint32_t x = 0; x < fixture.width; ++x)
        {
            const auto ax       = nearestTexel(x, a_mip.width, fixture.width, fixture.texture.width);
            const auto bx       = nearestTexel(x, b_mip.width, fixture.width, fixture.texture.width);
            const auto out      = (static_cast<std::size_t>(y) * rex::graphics::testing::kNativePreparedDxtMatrixOutputWidth + x) * 4U;
            const auto a_offset = (static_cast<std::size_t>(ay) * a_mip.width + ax) * 4U;
            const auto b_offset = (static_cast<std::size_t>(by) * b_mip.width + bx) * 4U;
            const auto alpha    = ClampUnorm8(static_cast<unsigned>(a[a_offset + 3U]) + b[b_offset + 3U]);
            for (std::size_t component = 0; component < 3U; ++component)
            {
                const auto source       = ClampUnorm8(static_cast<unsigned>(a[a_offset + component]) + b[b_offset + component]);
                result[out + component] = QuantizeUnorm8(source * alpha);
            }
            // The presenter capture path normalizes RGBA8 alpha to 255. The
            // RGB channels retain the source-alpha effect above, which is the
            // alpha proof used by the native and control routes.
            result[out + 3U] = 255U;
        }
    }
    return result;
}

std::size_t Bc2BlockOffset(const rex::graphics::texture_util::TextureGuestLayout& layout,
                           std::uint32_t                                          level,
                           std::uint32_t                                          block_x,
                           std::uint32_t                                          block_y)
{
    if (rex::graphics::testing::dxtMatrixForm->tiled)
    {
        const auto    stored     = level == 0U ? 0U : std::min(level, layout.packed_level);
        const auto&   allocation = level == 0U ? layout.base : layout.mips[stored];
        std::uint32_t x = 0U, y = 0U, z = 0U;
        if (level != 0U && level >= layout.packed_level)
            rex::graphics::texture_util::GetPackedMipOffset(rex::graphics::testing::kNativePreparedDxtMatrixWidth,
                                                            rex::graphics::testing::kNativePreparedDxtMatrixHeight,
                                                            1U,
                                                            rex::graphics::testing::DxtMatrixFormat(),
                                                            level,
                                                            x,
                                                            y,
                                                            z);
        return (level == 0U ? 0U : layout.mip_offsets_bytes[stored]) +
               rex::graphics::texture_util::GetTiledOffset2D(block_x + x, block_y + y, allocation.row_pitch_bytes / rex::graphics::testing::DxtMatrixBlockBytes(), rex::graphics::testing::DxtMatrixBlockBytes() == 8U ? 3U : 4U);
    }
    if (level == 0U)
    {
        return static_cast<std::size_t>(block_y) * layout.base.row_pitch_bytes + block_x * rex::graphics::testing::DxtMatrixBlockBytes();
    }
    const auto& level_layout = layout.mips[level];
    if (level < layout.packed_level)
        return layout.mip_offsets_bytes[level] + static_cast<std::size_t>(block_y) * level_layout.row_pitch_bytes + block_x * rex::graphics::testing::DxtMatrixBlockBytes();
    std::uint32_t packed_x = 0U;
    std::uint32_t packed_y = 0U;
    std::uint32_t packed_z = 0U;
    rex::graphics::texture_util::GetPackedMipOffset(rex::graphics::testing::kNativePreparedDxtMatrixWidth,
                                                    rex::graphics::testing::kNativePreparedDxtMatrixHeight,
                                                    1U,
                                                    rex::graphics::testing::DxtMatrixFormat(),
                                                    level,
                                                    packed_x,
                                                    packed_y,
                                                    packed_z);
    return layout.mip_offsets_bytes[layout.packed_level] +
           (static_cast<std::size_t>(packed_y) + block_y) * layout.mips[layout.packed_level].row_pitch_bytes +
           (static_cast<std::size_t>(packed_x) + block_x) * rex::graphics::testing::DxtMatrixBlockBytes();
}

struct Bc2BlockData
{
    std::uint64_t alpha_nibbles = 0;
    std::uint32_t color_indices = 0;
    std::uint16_t color0        = 0;
    std::uint16_t color1        = 0;
};

Bc2BlockData MakeBc2BlockData(std::uint32_t level, std::uint32_t block_x = 0U, std::uint32_t block_y = 0U)
{
    if (!rex::graphics::testing::DxtMatrixBc2())
    {
        // BC1 uses opaque endpoint 0. Skip unused alpha/index table
        // construction while decoding the complete 2048x2048 reference.
        Bc2BlockData data;
        data.color0 = static_cast<std::uint16_t>(((4U + ((level + block_x * 3U + block_y * 5U) & 7U)) << 11U) |
                                                 ((16U + ((block_x + block_y * 3U) & 7U)) << 5U) | (2U + ((block_x * 5U + block_y * 3U) & 7U)));
        if (rex::graphics::testing::dxtMatrixForm->tiled)
        {
            const auto ordinal = block_y * std::max(1U, (rex::graphics::testing::kNativePreparedDxtMatrixWidth >> level) / 4U) + block_x;
            data.color0        = static_cast<std::uint16_t>((((ordinal + level * 7U) & 31U) << 11U) |
                                                            ((((ordinal >> 5U) + level * 3U) & 63U) << 5U) | (1U + ((ordinal >> 11U) & 15U)));
        }
        data.color1        = 0U;
        data.color_indices = 0U;
        data.alpha_nibbles = UINT64_MAX;
        return data;
    }

    // This conservative endpoint set keeps each level's green channel
    // constant at 0 or full-scale. Red and blue still vary over exact 565
    // endpoints, while avoiding a vendor-specific green interpolation claim.
    constexpr std::array<std::array<std::uint16_t, 2>, 10> kEndpoints = {
        { { 0x0000U, 0xF800U }, { 0x0000U, 0x001FU }, { 0x0000U, 0xF81FU }, { 0x001FU, 0xF800U }, { 0x001FU, 0xF81FU }, { 0xF800U, 0xF81FU }, { 0x07E0U, 0xFFFFU }, { 0x07E0U, 0xFFE0U }, { 0x07FFU, 0xFFFFU }, { 0x07E0U, 0x07FFU } }
    };
    Bc2BlockData data;
    data.color0 = kEndpoints[level][0];
    data.color1 = kEndpoints[level][1];
    if (((block_x ^ block_y ^ (rex::graphics::testing::dxtMatrixForm->tiled ? ((block_x >> 3U) * 3U + (block_y >> 3U) * 5U + (block_x >> 5U) * 7U) : 0U)) & 1U) != 0U)
    {
        // Keep each block's green channel constant and each endpoint
        // representable while changing both spatial axes of the source.
        constexpr std::array<std::array<std::uint16_t, 2>, 2> kSpatialPairs = {
            { { 0x001FU, 0xF81FU }, { 0x07E0U, 0xFFE0U } }
        };
        const auto pair = kSpatialPairs[(level + block_x + block_y) & 1U];
        data.color0     = pair[0];
        data.color1     = pair[1];
    }
    for (std::uint32_t texel = 0; texel < 16U; ++texel)
    {
        data.alpha_nibbles |= static_cast<std::uint64_t>((texel * 3U + level + block_x * 5U + block_y * 7U + (rex::graphics::testing::dxtMatrixForm->tiled ? (block_x >> 3U) * 3U + (block_y >> 3U) * 5U : 0U)) & 0xFU) << (texel * 4U);
        data.color_indices |= ((texel + level + block_x + block_y * 2U) & (rex::graphics::testing::dxtMatrixForm->tiled ? 1U : 3U)) << (texel * 2U);
    }

    return data;
}

std::array<std::uint8_t, 4> DecodeBc2Color(std::uint16_t color0, std::uint16_t color1, std::uint32_t index)
{
    // BC2 always uses the four-color interpolation table; color0 <= color1
    // does not select BC1's three-color mode. The reference decoder promotes
    // the 5/6-bit endpoints to UNORM8 before deriving palette entries.
    const auto unpack = [](std::uint16_t color)
    {
        const auto red5   = static_cast<std::uint32_t>((color >> 11U) & 0x1FU);
        const auto green6 = static_cast<std::uint32_t>((color >> 5U) & 0x3FU);
        const auto blue5  = static_cast<std::uint32_t>(color & 0x1FU);
        const auto red    = (red5 << 3U) | (red5 >> 2U);
        const auto green  = (green6 << 2U) | (green6 >> 4U);
        const auto blue   = (blue5 << 3U) | (blue5 >> 2U);
        return std::array<std::uint32_t, 3>{ red, green, blue };
    };
    const auto                   a = unpack(color0);
    const auto                   b = unpack(color1);
    std::array<std::uint32_t, 3> value{};
    if (index == 0U)
        value = a;
    else if (index == 1U)
        value = b;
    else if (index == 2U)
        value = { (2U * a[0] + b[0] + 1U) / 3U, (2U * a[1] + b[1] + 1U) / 3U, (2U * a[2] + b[2] + 1U) / 3U };
    else
        value = { (a[0] + 2U * b[0] + 1U) / 3U, (a[1] + 2U * b[1] + 1U) / 3U, (a[2] + 2U * b[2] + 1U) / 3U };
    return { static_cast<std::uint8_t>(value[0]), static_cast<std::uint8_t>(value[1]), static_cast<std::uint8_t>(value[2]), 255U };
}

std::array<std::uint8_t, 4> DecodeBc2Pixel(std::uint32_t level,
                                           std::uint32_t texel,
                                           std::uint32_t block_x = 0U,
                                           std::uint32_t block_y = 0U)
{
    const auto data  = MakeBc2BlockData(level, block_x, block_y);
    auto       color = DecodeBc2Color(data.color0, data.color1, (data.color_indices >> (texel * 2U)) & 3U);
    color[3]         = static_cast<std::uint8_t>(((data.alpha_nibbles >> (texel * 4U)) & 0xFU) * 17U);
    return color;
}

void StoreBc2Block(std::vector<std::uint8_t>&                                storage,
                   std::size_t                                               offset,
                   std::uint32_t                                             level,
                   std::uint32_t                                             block_x,
                   std::uint32_t                                             block_y,
                   rex::graphics::testing::NativePreparedDxtMatrixUpdateKind update_kind)
{
    if (offset + rex::graphics::testing::DxtMatrixBlockBytes() > storage.size())
        return;
    auto data = MakeBc2BlockData(level, block_x, block_y);
    if (update_kind == rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kCombined)
    {
        data.alpha_nibbles = UINT64_MAX;
        data.color_indices = 0U;
        data.color0        = 0xF81FU;
        data.color1        = 0xF81FU;
    }
    else if (update_kind == rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kColorOnly)
    {
        data.color_indices = 0U;
        data.color0        = 0x07E0U;
        data.color1        = 0x07E0U;
    }
    else if (update_kind == rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kAlphaOnly)
    {
        data.alpha_nibbles = 0U;
    }
    else if (update_kind != rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kBaseline)
    {
        return;
    }

    if (!rex::graphics::testing::DxtMatrixBc2())
    {
        storage[offset]      = static_cast<std::uint8_t>(data.color0 >> 8U);
        storage[offset + 1U] = static_cast<std::uint8_t>(data.color0);
        storage[offset + 2U] = 0U;
        storage[offset + 3U] = 0U;
        std::fill(storage.begin() + offset + 4U, storage.begin() + offset + 8U, 0U);
        if (rex::graphics::testing::dxtMatrixForm->endian == rex::graphics::xenos::Endian::kNone)
            std::swap(storage[offset], storage[offset + 1U]);
        return;
    }
    for (std::uint32_t pair = 0; pair < 4U; ++pair)
    {
        const auto value                 = static_cast<std::uint16_t>(data.alpha_nibbles >> (pair * 16U));
        storage[offset + pair * 2U]      = static_cast<std::uint8_t>(value >> 8U);
        storage[offset + pair * 2U + 1U] = value;
    }
    const auto words = std::array<std::uint16_t, 2>{ data.color0, data.color1 };
    for (std::uint32_t index = 0; index < 2U; ++index)
    {
        const auto value                  = words[index];
        storage[offset + 8U + index * 2U] = static_cast<std::uint8_t>(value >> 8U);
        storage[offset + 9U + index * 2U] = static_cast<std::uint8_t>(value);
    }
    for (std::uint32_t pair = 0; pair < 2U; ++pair)
    {
        const auto value                  = static_cast<std::uint16_t>(data.color_indices >> (pair * 16U));
        storage[offset + 12U + pair * 2U] = static_cast<std::uint8_t>(value >> 8U);
        storage[offset + 13U + pair * 2U] = static_cast<std::uint8_t>(value);
    }
    if (rex::graphics::testing::dxtMatrixForm->endian == rex::graphics::xenos::Endian::kNone)
        for (std::uint32_t byte = 0U; byte < rex::graphics::testing::DxtMatrixBlockBytes(); byte += 2U)
            std::swap(storage[offset + byte], storage[offset + byte + 1U]);
}

void WriteMipStorage(std::vector<std::uint8_t>&                                storage,
                     const rex::graphics::texture_util::TextureGuestLayout&    layout,
                     std::uint32_t                                             level,
                     const rex::graphics::testing::NativePreparedDxtMatrixMip& mip)
{
    const auto width         = std::max(1U, rex::graphics::testing::kNativePreparedDxtMatrixWidth >> level);
    const auto height        = std::max(1U, rex::graphics::testing::kNativePreparedDxtMatrixHeight >> level);
    const auto blocks_x      = std::max(1U, (width + 3U) / 4U);
    const auto blocks_y      = std::max(1U, (height + 3U) / 4U);
    auto       update_kind   = rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kBaseline;
    bool       color_changed = false;
    bool       alpha_changed = false;
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const auto expected = DecodeBc2Pixel(level, (y & 3U) * 4U + (x & 3U), x / 4U, y / 4U);
            const auto at       = (static_cast<std::size_t>(y) * width + x) * 4U;
            color_changed       = color_changed || mip.linear_rgba8[at + 0U] != expected[0] ||
                                  mip.linear_rgba8[at + 1U] != expected[1] || mip.linear_rgba8[at + 2U] != expected[2];
            alpha_changed       = alpha_changed || mip.linear_rgba8[at + 3U] != expected[3];
        }
    }
    if (!rex::graphics::testing::DxtMatrixBc2() && color_changed)
        update_kind = rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kCombined;
    else if (color_changed && alpha_changed)
        update_kind = rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kCombined;
    else if (color_changed && !alpha_changed)
        update_kind = rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kColorOnly;
    else if (!color_changed && alpha_changed)
        update_kind = rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kAlphaOnly;

    for (std::uint32_t block_y = 0; block_y < blocks_y; ++block_y)
    {
        for (std::uint32_t block_x = 0; block_x < blocks_x; ++block_x)
        {
            StoreBc2Block(storage,
                          Bc2BlockOffset(layout, level, block_x, block_y),
                          level,
                          block_x,
                          block_y,
                          update_kind);
        }
    }
}

std::array<std::uint32_t, 6> MakeTextureFetch(std::uint32_t base_address,
                                              std::uint32_t mip_address)
{
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type   = rex::graphics::xenos::FetchConstantType::kTexture;
    fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w =
        rex::graphics::xenos::TextureSign::kUnsigned;
    fetch.pitch          = rex::graphics::testing::dxtMatrixForm->pitch;
    fetch.format         = rex::graphics::testing::DxtMatrixFormat();
    fetch.endianness     = rex::graphics::testing::dxtMatrixForm->endian;
    fetch.base_address   = base_address >> 12U;
    fetch.mip_address    = rex::graphics::testing::dxtMatrixForm->max_level ? mip_address >> 12U : 0U;
    fetch.size_2d.width  = rex::graphics::testing::kNativePreparedDxtMatrixWidth - 1U;
    fetch.size_2d.height = rex::graphics::testing::kNativePreparedDxtMatrixHeight - 1U;
    fetch.tiled          = rex::graphics::testing::dxtMatrixForm->tiled;
    fetch.packed_mips    = rex::graphics::testing::dxtMatrixForm->packed;
    fetch.mip_min_level  = 0U;
    fetch.mip_max_level  = rex::graphics::testing::kNativePreparedDxtMatrixMipCount - 1U;
    fetch.swizzle        = 0x688U;
    // Point/clamp filtering is authored for this synthetic spatial oracle;
    // selected source words retain format/layout identity, not a saved sampler.
    fetch.mag_filter = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.min_filter = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.mip_filter = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.clamp_x    = rex::graphics::xenos::ClampMode::kClampToEdge;
    fetch.clamp_y    = rex::graphics::xenos::ClampMode::kClampToEdge;
    fetch.clamp_z    = rex::graphics::xenos::ClampMode::kClampToEdge;
    fetch.dimension  = rex::graphics::xenos::DataDimension::k2DOrStacked;
    return { fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5 };
}

std::array<std::uint32_t, 6> MakeVertexFetch(std::uint32_t address, std::uint32_t bytes)
{
    rex::graphics::xenos::xe_gpu_vertex_fetch_t fetch{};
    fetch.type    = rex::graphics::xenos::FetchConstantType::kVertex;
    fetch.address = address / sizeof(std::uint32_t);
    fetch.endian  = rex::graphics::xenos::Endian::k8in32;
    fetch.size    = bytes / sizeof(std::uint32_t);
    return { fetch.dword_0, fetch.dword_1, 0U, 0U, 0U, 0U };
}

void SetRegisters(std::vector<std::uint32_t>&    registers,
                  std::span<const std::uint32_t> texture_fetch,
                  std::span<const std::uint32_t> vertex_fetch)
{
    registers.assign(rex::graphics::testing::kNativePreparedDxtMatrixRegisterCount, 0U);
    registers[0x2000U]                                      = 0x14000500U;
    registers[0x2002U]                                      = 0x530U;
    registers[0x200FU]                                      = 0x20002000U;
    registers[0x2080U]                                      = 0U;
    registers[0x2081U]                                      = 0U;
    registers[0x2082U]                                      = rex::graphics::testing::DxtMatrixViewportWidth() | (rex::graphics::testing::DxtMatrixViewportHeight() << 16U);
    registers[0x2100U]                                      = 2U;
    registers[0x2104U]                                      = 0xFU;
    registers[0x210FU]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::DxtMatrixViewportWidth()) / 2.0F);
    registers[0x2110U]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::DxtMatrixViewportWidth()) / 2.0F);
    registers[0x2111U]                                      = std::bit_cast<std::uint32_t>(-static_cast<float>(rex::graphics::testing::DxtMatrixViewportHeight()) / 2.0F);
    registers[0x2112U]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::DxtMatrixViewportHeight()) / 2.0F);
    registers[0x2180U]                                      = 0x10010002U;
    registers[0x2181U]                                      = 4U;
    registers[0x2182U]                                      = 1U;
    registers[rex::graphics::XE_GPU_REG_VGT_DMA_SIZE]       = kNativePreparedDxtMatrixDmaSize;
    registers[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR] = kNativePreparedDxtMatrixDrawInitiator;
    registers[0x2200U]                                      = 0x24F00730U;
    // Source RGB uses explicit BC2 alpha over the zero color background;
    // preserve the known opaque destination alpha for presenter parity.
    registers[0x2201U] = 0x01000006U;
    registers[0x2202U] = 0x87000000U;
    registers[0x2204U] = 0x80000U;
    registers[0x2205U] = 0x18000U;
    registers[0x2206U] = 0x43FU;
    registers[0x2208U] = 4U;
    registers[0x2302U] = 4U;
    registers[0x2307U] = 0xFF000U;
    registers[0x2308U] = 0xFF100U;
    std::copy(kVertexConstants.begin(), kVertexConstants.end(), registers.begin() + rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X);
    std::copy(kPixelConstant0.begin(), kPixelConstant0.end(), registers.begin() + rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X);
    std::copy(kPixelConstant255.begin(), kPixelConstant255.end(), registers.begin() + rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X + 255U * 4U);
    std::copy(texture_fetch.begin(), texture_fetch.end(), registers.begin() + rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    std::copy(vertex_fetch.begin(), vertex_fetch.end(), registers.begin() + rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U);
}

} // namespace

namespace rex::graphics::testing
{

NativePreparedDxtMatrixFixture MakeNativePreparedDxtMatrixFixture()
{
    NativePreparedDxtMatrixFixture fixture;
    fixture.vertex_shader_words.assign(kVertexShader.begin(), kVertexShader.end());
    fixture.pixel_shader_words.assign(kPixelShader.begin(), kPixelShader.end());

    const auto layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, dxtMatrixForm->pitch, kNativePreparedDxtMatrixWidth, kNativePreparedDxtMatrixHeight, 1U, dxtMatrixForm->tiled, DxtMatrixFormat(), dxtMatrixForm->packed, true, kNativePreparedDxtMatrixMipCount - 1U);
    fixture.texture.format      = static_cast<std::uint32_t>(DxtMatrixFormat());
    fixture.texture.endianness  = static_cast<std::uint32_t>(dxtMatrixForm->endian);
    fixture.texture.swizzle     = 0x688U;
    fixture.texture.dimension   = static_cast<std::uint32_t>(xenos::DataDimension::k2DOrStacked);
    fixture.texture.pitch       = dxtMatrixForm->pitch;
    fixture.texture.tiled       = dxtMatrixForm->tiled;
    fixture.texture.packed_mips = dxtMatrixForm->packed;
    fixture.texture.base_guest_bytes.assign(AlignToPage(layout.base.level_data_extent_bytes), dxtMatrixForm->tiled ? 0xD3U : 0U);
    fixture.texture.mip_guest_bytes.assign(std::max(kPageSize, AlignToPage(layout.mips_total_extent_bytes)), dxtMatrixForm->tiled ? 0x6BU : 0U);
    fixture.texture.mips.resize(kNativePreparedDxtMatrixMipCount);
    for (std::uint32_t level = 0; level < kNativePreparedDxtMatrixMipCount; ++level)
    {
        auto& mip     = fixture.texture.mips[level];
        mip.level     = level;
        mip.width     = std::max(1U, kNativePreparedDxtMatrixWidth >> level);
        mip.height    = std::max(1U, kNativePreparedDxtMatrixHeight >> level);
        mip.row_pitch = level == 0U ? layout.base.row_pitch_bytes
                                    : layout.mips[std::min(level, layout.packed_level)].row_pitch_bytes;
        if (level == 0U)
        {
            mip.guest_offset = 0U;
            mip.guest_extent = layout.base.level_data_extent_bytes;
        }
        else if (level < layout.packed_level)
        {
            mip.guest_offset = layout.mip_offsets_bytes[level];
            mip.guest_extent = layout.mips[level].level_data_extent_bytes;
        }
        else
        {
            std::uint32_t packed_x = 0U;
            std::uint32_t packed_y = 0U;
            std::uint32_t packed_z = 0U;
            texture_util::GetPackedMipOffset(kNativePreparedDxtMatrixWidth, kNativePreparedDxtMatrixHeight, 1U, DxtMatrixFormat(), level, packed_x, packed_y, packed_z);
            mip.guest_offset = layout.mip_offsets_bytes[layout.packed_level] +
                               packed_y * layout.mips[layout.packed_level].row_pitch_bytes + packed_x * DxtMatrixBlockBytes();
            mip.guest_extent = (std::max(1U, (mip.height + 3U) / 4U) - 1U) * mip.row_pitch +
                               std::max(1U, (mip.width + 3U) / 4U) * DxtMatrixBlockBytes();
        }
        if (dxtMatrixForm->tiled)
        {
            const auto stored = level == 0U ? 0U : std::min(level, layout.packed_level);
            mip.guest_offset  = dxtMatrixForm->stored_starts[level];
            mip.guest_extent  = level == 0U ? layout.base.level_data_extent_bytes : layout.mips[stored].level_data_extent_bytes;
        }
        mip.linear_rgba8.resize(static_cast<std::size_t>(mip.width) * mip.height * 4U);
        for (std::uint32_t y = 0; y < mip.height; ++y)
            for (std::uint32_t x = 0; x < mip.width; ++x)
            {
                const auto pixel  = DecodeBc2Pixel(level, (y & 3U) * 4U + (x & 3U), x / 4U, y / 4U);
                const auto offset = (static_cast<std::size_t>(y) * mip.width + x) * 4U;
                std::memcpy(mip.linear_rgba8.data() + offset, pixel.data(), pixel.size());
            }
        if (level == 0U)
            WriteMipStorage(fixture.texture.base_guest_bytes, layout, level, mip);
        else
            WriteMipStorage(fixture.texture.mip_guest_bytes, layout, level, mip);
    }
    fixture.texture.fetch_words = MakeTextureFetch(0U, 0U);

    // The fullscreen triangle covers the source viewport. Every form is biased
    // by a quarter base texel so no mip sample lies on an exact texel edge.
    const auto uBias = DxtMatrixUvBias(dxtMatrixForm->width);
    const auto vBias = DxtMatrixUvBias(dxtMatrixForm->height);
    for (auto value : std::array<std::array<float, 8>, 3>{
             { { -1.0F, -1.0F, 0.5F, 0.0F, 0.0F, 1.0F, 2.0F, 5.0F },
               { -1.0F, 3.0F, 0.5F, 0.0F, 0.0F, -1.0F, 2.0F, 5.0F },
               { 3.0F, -1.0F, 0.5F, 0.0F, 2.0F, 1.0F, 2.0F, 5.0F } } })
    {
        value[4] += uBias;
        value[5] += vBias;
        for (const auto component : value)
        {
            AppendBigEndianFloat(fixture.vertex_bytes, component);
        }
        for (std::size_t padding = value.size(); padding < kNativePreparedDxtMatrixVertexStride / 4U;
             ++padding)
        {
            AppendBigEndianFloat(fixture.vertex_bytes, 0.0F);
        }
    }
    fixture.index_bytes        = { 0U, 0U, 0U, 1U, 0U, 2U };
    fixture.vertex_fetch_words = MakeVertexFetch(0U, static_cast<std::uint32_t>(fixture.vertex_bytes.size()));
    SetRegisters(fixture.register_image, fixture.texture.fetch_words, fixture.vertex_fetch_words);
    fixture.expected_by_lod_pair.resize(kNativePreparedDxtMatrixMipCount * kNativePreparedDxtMatrixMipCount);
    const auto selected_index                    = fixture.selected_lods[0] * kNativePreparedDxtMatrixMipCount + fixture.selected_lods[1];
    fixture.expected_by_lod_pair[selected_index] = MakeExpectedPixels(fixture, fixture.selected_lods[0], fixture.selected_lods[1]);
    fixture.expected_selected_pixels             = fixture.expected_by_lod_pair[selected_index];
    return fixture;
}

std::vector<std::uint8_t> ExpectedNativePreparedDxtMatrixPixels(const NativePreparedDxtMatrixFixture& fixture,
                                                                std::uint32_t                         first_lod,
                                                                std::uint32_t                         second_lod)
{
    return MakeExpectedPixels(fixture, first_lod, second_lod);
}

namespace
{

struct ObservationDraw
{
    NativePreparedDxtMatrixDrawRecord record;
};

class OriginalObservation final : public DrawCaptureFrameInputObserver
{
public:
    explicit OriginalObservation(std::shared_ptr<DrawCaptureFrameInputObserver> forwarded)
    : forwarded_(std::move(forwarded))
    {
    }

    bool OnFrameBegin(const DrawCaptureFrameInputSwapView& boundary) override
    {
        {
            std::lock_guard lock(mutex_);
            ++begin_count_;
            ++event_ordinal_;
        }
        condition_.notify_all();
        return !forwarded_ || forwarded_->OnFrameBegin(boundary);
    }

    bool OnDraw(const DrawCaptureFrameInputDrawView& draw) override
    {
        {
            std::lock_guard lock(mutex_);
            draws_.push_back(MakeDraw(draw, ++event_ordinal_));
        }
        condition_.notify_all();
        return !forwarded_ || forwarded_->OnDraw(draw);
    }

    bool OnCopy(const DrawCaptureFrameInputCopyView& copy) override
    {
        {
            std::lock_guard lock(mutex_);
            copies_.push_back(MakeCopy(copy, ++event_ordinal_));
        }
        condition_.notify_all();
        return !forwarded_ || forwarded_->OnCopy(copy);
    }

    bool OnSwap(const DrawCaptureFrameInputSwapView& swap) override
    {
        {
            std::lock_guard lock(mutex_);
            swaps_.push_back(MakeSwap(swap, ++event_ordinal_));
        }
        condition_.notify_all();
        return !forwarded_ || forwarded_->OnSwap(swap);
    }

    bool OnFrameEnd(std::uint64_t frame, std::uint64_t submission, bool original_succeeded) override
    {
        {
            std::lock_guard lock(mutex_);
            frame_ends_.push_back({ ++event_ordinal_, frame, submission, original_succeeded });
        }
        condition_.notify_all();
        return !forwarded_ || forwarded_->OnFrameEnd(frame, submission, original_succeeded);
    }

    bool WantsPreparedDraws() const noexcept override
    {
        return true;
    }

    bool OnDrawPrepared(const DrawCaptureFrameInputDrawView& draw) override
    {
        {
            std::lock_guard lock(mutex_);
            prepared_draws_.push_back(MakeDraw(draw, ++event_ordinal_));
        }
        condition_.notify_all();
        return !forwarded_ || !forwarded_->WantsPreparedDraws() || forwarded_->OnDrawPrepared(draw);
    }

    bool WantsPreparedCopies() const noexcept override
    {
        return true;
    }

    bool OnCopyPrepared(const DrawCaptureFrameInputCopyView& copy) override
    {
        {
            std::lock_guard lock(mutex_);
            prepared_copies_.push_back(MakeCopy(copy, ++event_ordinal_));
        }
        condition_.notify_all();
        return !forwarded_ || !forwarded_->WantsPreparedCopies() || forwarded_->OnCopyPrepared(copy);
    }

    bool WantsPreparedSwaps() const noexcept override
    {
        return true;
    }

    bool OnSwapPrepared(const DrawCaptureFrameInputSwapView& swap) override
    {
        {
            std::lock_guard lock(mutex_);
            prepared_swaps_.push_back(MakeSwap(swap, ++event_ordinal_));
        }
        condition_.notify_all();
        return !forwarded_ || !forwarded_->WantsPreparedSwaps() || forwarded_->OnSwapPrepared(swap);
    }

    bool WaitForCompletion(std::uint32_t terminal_address, std::uint64_t after_event_ordinal = 0)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(20), [&]
                                   {
                                       for (const auto& copy : copies_)
                                       {
                                           if (copy.written_address != terminal_address || copy.written_length == 0U ||
                                               !copy.original_succeeded || copy.event_ordinal <= after_event_ordinal)
                                           {
                                               continue;
                                           }
                                           for (const auto& swap : swaps_)
                                           {
                                               if (swap.frame != copy.frame || swap.submission != copy.submission ||
                                                   swap.event_ordinal <= copy.event_ordinal || swap.event_ordinal <= after_event_ordinal ||
                                                   swap.frontbuffer_ptr != terminal_address ||
                                                   swap.width != kNativePreparedDxtMatrixOutputWidth ||
                                                   swap.height != kNativePreparedDxtMatrixOutputHeight)
                                               {
                                                   continue;
                                               }
                                               if (std::any_of(frame_ends_.begin(), frame_ends_.end(), [&](const auto& end)
                                                               {
                                                                   return end.frame == swap.frame && end.submission == swap.submission &&
                                                                          end.event_ordinal > swap.event_ordinal &&
                                                                          end.original_succeeded;
                                                               }))
                                               {
                                                   completion_copy_id_    = copy.id;
                                                   completion_swap_id_    = swap.id;
                                                   completion_frame_      = swap.frame;
                                                   completion_submission_ = swap.submission;
                                                   return true;
                                               }
                                           }
                                       }
                                       return false;
                                   });
    }

    struct Snapshot
    {
        std::size_t                                        begin_count = 0;
        std::vector<NativePreparedDxtMatrixDrawRecord>     draws;
        std::vector<NativePreparedDxtMatrixDrawRecord>     prepared_draws;
        std::vector<NativePreparedDxtMatrixCopyRecord>     copies;
        std::vector<NativePreparedDxtMatrixCopyRecord>     prepared_copies;
        std::vector<NativePreparedDxtMatrixSwapRecord>     swaps;
        std::vector<NativePreparedDxtMatrixSwapRecord>     prepared_swaps;
        std::vector<NativePreparedDxtMatrixFrameEndRecord> frame_ends;
        std::uint64_t                                      completion_copy_id    = 0;
        std::uint64_t                                      completion_swap_id    = 0;
        std::uint64_t                                      completion_frame      = 0;
        std::uint64_t                                      completion_submission = 0;
    };

    Snapshot SnapshotData() const
    {
        std::lock_guard lock(mutex_);
        return { begin_count_, draws_, prepared_draws_, copies_, prepared_copies_, swaps_, prepared_swaps_, frame_ends_, completion_copy_id_, completion_swap_id_, completion_frame_, completion_submission_ };
    }

private:
    static NativePreparedDxtMatrixDrawRecord MakeDraw(const DrawCaptureFrameInputDrawView& draw,
                                                      std::uint64_t                        event_ordinal)
    {
        NativePreparedDxtMatrixDrawRecord result;
        result.event_ordinal         = event_ordinal;
        result.id                    = draw.id;
        result.frame                 = draw.frame;
        result.submission            = draw.submission;
        result.original_host_issued  = draw.original_host_issued;
        result.cpu_data_complete     = draw.cpu_data_complete;
        result.execution_status      = static_cast<std::uint32_t>(draw.execution_status);
        result.execution_reason      = static_cast<std::uint32_t>(draw.execution_reason);
        result.primitive_type        = draw.primitive_type;
        result.requested_index_count = draw.requested_index_count;
        result.vertex_shader_hash    = draw.vertex_shader.hash;
        result.pixel_shader_hash     = draw.pixel_shader.hash;
        result.registers.assign(draw.registers.begin(), draw.registers.end());
        result.vertex_shader_words.assign(draw.vertex_shader.words.begin(), draw.vertex_shader.words.end());
        result.pixel_shader_words.assign(draw.pixel_shader.words.begin(), draw.pixel_shader.words.end());
        for (const auto& texture : draw.textures)
        {
            NativePreparedDxtMatrixDrawRecord::TextureRecord retained;
            retained.fetch_constant       = texture.fetch_constant;
            retained.format               = texture.format;
            retained.endianness           = texture.endianness;
            retained.dimension            = texture.dimension;
            retained.width                = texture.width;
            retained.height               = texture.height;
            retained.depth                = texture.depth;
            retained.pitch                = texture.pitch;
            retained.mip_min_level        = texture.mip_min_level;
            retained.mip_max_level        = texture.mip_max_level;
            retained.tiled                = texture.tiled;
            retained.packed_mips          = texture.packed_mips;
            retained.stage                = texture.stage;
            retained.shader_binding_index = texture.shader_binding_index;
            retained.sampler_value        = texture.sampler_value;
            retained.component_mapping    = texture.component_mapping;
            retained.swizzled_signs       = texture.swizzled_signs;
            retained.is_signed            = texture.is_signed;
            retained.required             = texture.required;
            retained.available            = texture.available;
            retained.gpu_written          = texture.gpu_written;
            retained.unavailable_reason   = static_cast<std::uint32_t>(texture.unavailable_reason);
            retained.base_address         = texture.base_address;
            retained.base_size            = texture.base_size;
            retained.mip_address          = texture.mip_address;
            retained.mip_size             = texture.mip_size;
            retained.fetch_words.assign(texture.fetch_words.begin(), texture.fetch_words.end());
            retained.memory_watch_supplied   = texture.memory_version.identity || texture.memory_version.generation ||
                                               texture.memory_version.context || texture.memory_version.check;
            retained.memory_watch_identity   = texture.memory_version.identity;
            retained.memory_watch_generation = texture.memory_version.generation;
            retained.memory_current_before   = texture.memory_version.IsCurrent();
            // Retain optional companion metadata without copying an unused
            // payload. Only a required, available CPU view owns raw ranges.
            if (retained.required && retained.available && !retained.gpu_written && retained.unavailable_reason == 0U)
            {
                retained.base_bytes.resize(texture.base_size);
                retained.mip_bytes.resize(texture.mip_size);
                retained.base_bytes_copied = texture.base_size == 0U ||
                                             texture.memory.Copy(texture.base_address, retained.base_bytes);
                retained.mip_bytes_copied  = texture.mip_size == 0U ||
                                             texture.memory.Copy(texture.mip_address, retained.mip_bytes);
            }
            retained.memory_current_after = texture.memory_version.IsCurrent();
            retained.memory_watch_stale   = retained.memory_watch_supplied &&
                                            (!retained.memory_current_before || !retained.memory_current_after);
            result.textures.push_back(std::move(retained));
        }
        return result;
    }

    static NativePreparedDxtMatrixCopyRecord MakeCopy(const DrawCaptureFrameInputCopyView& copy,
                                                      std::uint64_t                        event_ordinal)
    {
        NativePreparedDxtMatrixCopyRecord result;
        result.event_ordinal                  = event_ordinal;
        result.id                             = copy.id;
        result.frame                          = copy.frame;
        result.submission                     = copy.submission;
        result.mode                           = std::string(copy.copy_mode);
        result.original_succeeded             = copy.original_succeeded;
        result.cpu_data_complete              = copy.cpu_data_complete;
        result.unavailable_reason             = static_cast<std::uint32_t>(copy.unavailable_reason);
        result.source_rect_present            = copy.source_rect_present;
        result.source_x                       = copy.source_x;
        result.source_y                       = copy.source_y;
        result.source_width                   = copy.source_width;
        result.source_height                  = copy.source_height;
        result.source_geometry_fetch_constant = copy.source_geometry.fetch_constant;
        result.source_geometry_address        = copy.source_geometry.physical_address;
        result.source_geometry_size           = copy.source_geometry.size;
        result.written_address                = copy.written_address;
        result.written_length                 = copy.written_length;
        result.planned_address                = copy.planned_address;
        result.planned_length                 = copy.planned_length;
        result.registers.assign(copy.registers.begin(), copy.registers.end());
        return result;
    }

    static NativePreparedDxtMatrixSwapRecord MakeSwap(const DrawCaptureFrameInputSwapView& swap,
                                                      std::uint64_t                        event_ordinal)
    {
        return { event_ordinal, swap.id, swap.frame, swap.submission, swap.frontbuffer_ptr, swap.frontbuffer_width, swap.frontbuffer_height, swap.original_succeeded };
    }

    mutable std::mutex                                 mutex_;
    std::condition_variable                            condition_;
    std::size_t                                        begin_count_   = 0;
    std::uint64_t                                      event_ordinal_ = 0;
    std::vector<NativePreparedDxtMatrixDrawRecord>     draws_;
    std::vector<NativePreparedDxtMatrixDrawRecord>     prepared_draws_;
    std::vector<NativePreparedDxtMatrixCopyRecord>     copies_;
    std::vector<NativePreparedDxtMatrixCopyRecord>     prepared_copies_;
    std::vector<NativePreparedDxtMatrixSwapRecord>     swaps_;
    std::vector<NativePreparedDxtMatrixSwapRecord>     prepared_swaps_;
    std::vector<NativePreparedDxtMatrixFrameEndRecord> frame_ends_;
    std::uint64_t                                      completion_copy_id_    = 0;
    std::uint64_t                                      completion_swap_id_    = 0;
    std::uint64_t                                      completion_frame_      = 0;
    std::uint64_t                                      completion_submission_ = 0;
    std::shared_ptr<DrawCaptureFrameInputObserver>     forwarded_;
};

class GuestAllocation final
{
public:
    GuestAllocation() = default;

    GuestAllocation(rex::memory::Memory& memory, std::uint32_t bytes, std::uint32_t alignment)
    : memory_(&memory)
    , virtual_address_(memory.SystemHeapAlloc(
          bytes, alignment, rex::memory::kSystemHeapPhysical))
    {
        if (virtual_address_)
        {
            physical_address_ = memory.GetPhysicalAddress(virtual_address_);
        }
    }

    GuestAllocation(const GuestAllocation&)            = delete;
    GuestAllocation& operator=(const GuestAllocation&) = delete;

    GuestAllocation(GuestAllocation&& other) noexcept
    : memory_(std::exchange(other.memory_, nullptr))
    , virtual_address_(std::exchange(other.virtual_address_, 0U))
    , physical_address_(std::exchange(other.physical_address_, 0U))
    {
    }

    GuestAllocation& operator=(GuestAllocation&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            memory_           = std::exchange(other.memory_, nullptr);
            virtual_address_  = std::exchange(other.virtual_address_, 0U);
            physical_address_ = std::exchange(other.physical_address_, 0U);
        }
        return *this;
    }

    ~GuestAllocation()
    {
        Reset();
    }

    explicit operator bool() const noexcept
    {
        return memory_ && virtual_address_ && physical_address_;
    }

    std::uint32_t physical_address() const noexcept
    {
        return physical_address_;
    }

    std::uint8_t* virtual_bytes() const noexcept
    {
        return memory_ ? memory_->TranslateVirtual<std::uint8_t*>(virtual_address_) : nullptr;
    }

    std::uint8_t* physical_bytes() const noexcept
    {
        return memory_ ? memory_->TranslatePhysical<std::uint8_t*>(physical_address_) : nullptr;
    }

    void Reset() noexcept
    {
        if (memory_ && virtual_address_)
        {
            memory_->SystemHeapFree(virtual_address_);
        }
        memory_           = nullptr;
        virtual_address_  = 0U;
        physical_address_ = 0U;
    }

private:
    rex::memory::Memory* memory_           = nullptr;
    std::uint32_t        virtual_address_  = 0;
    std::uint32_t        physical_address_ = 0;
};

class PacketStream final
{
public:
    void Push(std::uint32_t word)
    {
        words_.push_back(word);
    }

    void Type0(std::uint32_t index, std::span<const std::uint32_t> values)
    {
        if (values.empty())
            return;
        Push(rex::graphics::xenos::MakePacketType0(static_cast<std::uint16_t>(index),
                                                   static_cast<std::uint16_t>(values.size())));
        words_.insert(words_.end(), values.begin(), values.end());
    }

    void Type0(std::uint32_t index, std::initializer_list<std::uint32_t> values)
    {
        Type0(index, std::span<const std::uint32_t>(values.begin(), values.size()));
    }

    void LoadShader(std::uint32_t address, rex::graphics::xenos::ShaderType type, std::uint32_t dword_count)
    {
        Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_IM_LOAD, 2));
        Push(address | static_cast<std::uint32_t>(type));
        Push(dword_count);
    }

    void DrawDma(std::uint32_t initiator, std::uint32_t index_address, std::uint32_t index_count, rex::graphics::xenos::Endian index_endian)
    {
        Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_DRAW_INDX, 4));
        Push(0U);
        Push(initiator);
        Push(index_address);
        Push(index_count | (static_cast<std::uint32_t>(index_endian) << 30U));
    }

    void DrawAuto(std::uint32_t initiator)
    {
        Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_DRAW_INDX, 2));
        Push(0U);
        Push(initiator);
    }

    void Swap(std::uint32_t address, std::uint32_t width, std::uint32_t height)
    {
        Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_XE_SWAP, 4));
        Push(rex::graphics::xenos::kSwapSignature);
        Push(address);
        Push(width);
        Push(height);
    }

    std::span<const std::uint32_t> words() const noexcept
    {
        return words_;
    }

private:
    std::vector<std::uint32_t> words_;
};

void WriteShaderWords(GuestAllocation& allocation, std::span<const std::uint32_t> words)
{
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        rex::memory::store_and_swap<std::uint32_t>(allocation.physical_bytes() + i * 4U, words[i]);
    }
}

void WritePacketWords(rex::memory::Memory& memory, std::uint32_t ring_address, std::span<const std::uint32_t> words)
{
    auto* ring = memory.TranslatePhysical<std::uint8_t*>(ring_address);
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        rex::memory::store_and_swap<std::uint32_t>(ring + i * 4U, words[i]);
    }
}

void PublishWritePointer(rex::memory::Memory& memory, std::uint32_t word_count)
{
    auto* pointer = memory.TranslateVirtual<volatile std::uint32_t*>(kWptrAddress);
    *pointer      = rex::byte_swap(word_count);
}

bool WaitForWriteback(rex::memory::Memory& memory, std::uint32_t address, std::uint32_t expected)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (rex::memory::load_and_swap<std::uint32_t>(memory.TranslatePhysical(address)) == expected)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool Require(bool value, std::string_view message, std::string& error)
{
    if (!value)
        error = std::string(message);
    return value;
}

} // namespace

namespace
{

std::array<std::uint32_t, 6> MakeOutputFetch(std::uint32_t address)
{
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type   = rex::graphics::xenos::FetchConstantType::kTexture;
    fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w =
        rex::graphics::xenos::TextureSign::kUnsigned;
    fetch.pitch          = kNativePreparedDxtMatrixOutputWidth / 32U;
    fetch.format         = rex::graphics::xenos::TextureFormat::k_8_8_8_8;
    fetch.endianness     = rex::graphics::xenos::Endian::kNone;
    fetch.base_address   = address >> 12U;
    fetch.size_2d.width  = kNativePreparedDxtMatrixOutputWidth - 1U;
    fetch.size_2d.height = kNativePreparedDxtMatrixOutputHeight - 1U;
    fetch.tiled          = true;
    fetch.swizzle        = rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    fetch.mag_filter     = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.min_filter     = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.mip_filter     = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.dimension      = rex::graphics::xenos::DataDimension::k2DOrStacked;
    return { fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5 };
}

std::vector<std::uint8_t> MakeResolveGeometry()
{
    std::vector<std::uint8_t> bytes;
    for (const auto value : std::array<float, 6>{ -0.5F,
                                                  -0.5F,
                                                  static_cast<float>(kNativePreparedDxtMatrixOutputWidth) - 0.5F,
                                                  -0.5F,
                                                  static_cast<float>(kNativePreparedDxtMatrixOutputWidth) - 0.5F,
                                                  static_cast<float>(kNativePreparedDxtMatrixOutputHeight) - 0.5F })
    {
        AppendBigEndianFloat(bytes, value);
    }
    return bytes;
}

void PatchFloat(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    if (offset + 4U > bytes.size())
        return;
    const auto bits    = std::bit_cast<std::uint32_t>(value);
    bytes[offset + 0U] = static_cast<std::uint8_t>(bits >> 24U);
    bytes[offset + 1U] = static_cast<std::uint8_t>(bits >> 16U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(bits >> 8U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(bits);
}

void PatchLods(NativePreparedDxtMatrixFixture& fixture, std::uint32_t first_lod, std::uint32_t second_lod)
{
    for (std::uint32_t vertex = 0; vertex < 3U; ++vertex)
    {
        const auto base = static_cast<std::size_t>(vertex) * fixture.vertex_stride;
        PatchFloat(fixture.vertex_bytes, base + 24U, static_cast<float>(first_lod));
        PatchFloat(fixture.vertex_bytes, base + 28U, static_cast<float>(second_lod));
    }
}

bool RebuildTextureGuestBytes(NativePreparedDxtMatrixFixture& fixture)
{
    if (fixture.texture.mips.size() != kNativePreparedDxtMatrixMipCount ||
        fixture.texture.base_guest_bytes.size() < kNativePreparedDxtMatrixBaseExtent ||
        fixture.texture.mip_guest_bytes.size() < kNativePreparedDxtMatrixMipExtent)
        return false;
    const auto layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, dxtMatrixForm->pitch, kNativePreparedDxtMatrixWidth, kNativePreparedDxtMatrixHeight, 1U, dxtMatrixForm->tiled, DxtMatrixFormat(), dxtMatrixForm->packed, true, kNativePreparedDxtMatrixMipCount - 1U);
    fixture.texture.base_guest_bytes.assign(AlignToPage(layout.base.level_data_extent_bytes), dxtMatrixForm->tiled ? 0xD3U : 0U);
    fixture.texture.mip_guest_bytes.assign(std::max(kPageSize, AlignToPage(layout.mips_total_extent_bytes)), dxtMatrixForm->tiled ? 0x6BU : 0U);
    for (std::uint32_t level = 0; level < kNativePreparedDxtMatrixMipCount; ++level)
    {
        const auto& mip = fixture.texture.mips[level];
        if (mip.width == 0U || mip.height == 0U ||
            mip.linear_rgba8.size() != static_cast<std::size_t>(mip.width) * mip.height * 4U)
        {
            return false;
        }
        if (level == 0U)
            WriteMipStorage(fixture.texture.base_guest_bytes, layout, level, mip);
        else
            WriteMipStorage(fixture.texture.mip_guest_bytes, layout, level, mip);
    }
    return true;
}

void RebuildExpectedPlanes(NativePreparedDxtMatrixFixture& fixture)
{
    fixture.expected_by_lod_pair.resize(kNativePreparedDxtMatrixMipCount * kNativePreparedDxtMatrixMipCount);
    const auto selected_index                    = fixture.selected_lods[0] * kNativePreparedDxtMatrixMipCount + fixture.selected_lods[1];
    fixture.expected_by_lod_pair[selected_index] = MakeExpectedPixels(fixture, fixture.selected_lods[0], fixture.selected_lods[1]);
    fixture.expected_selected_pixels             = fixture.expected_by_lod_pair[selected_index];
}

void ApplyCpuMipUpdate(NativePreparedDxtMatrixFixture&                           fixture,
                       std::uint32_t                                             level,
                       rex::graphics::testing::NativePreparedDxtMatrixUpdateKind update_kind)
{
    if (level >= fixture.texture.mips.size())
        return;
    auto& bytes = fixture.texture.mips[level].linear_rgba8;
    for (std::size_t offset = 0; offset + 3U < bytes.size(); offset += 4U)
    {
        if (update_kind == rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kCombined)
        {
            bytes[offset + 0U] = 255U;
            bytes[offset + 1U] = 0U;
            bytes[offset + 2U] = 255U;
        }
        else if (update_kind == rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kColorOnly)
        {
            bytes[offset + 0U] = 0U;
            bytes[offset + 1U] = 255U;
            bytes[offset + 2U] = 0U;
        }
        if (update_kind == rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kCombined)
            bytes[offset + 3U] = 255U;
        else if (update_kind == rex::graphics::testing::NativePreparedDxtMatrixUpdateKind::kAlphaOnly)
            bytes[offset + 3U] = 0U;
    }
}

void CopyAllocation(GuestAllocation& allocation, std::span<const std::uint8_t> bytes)
{
    std::memset(allocation.virtual_bytes(), 0, AlignToPage(static_cast<std::uint32_t>(bytes.size())));
    std::memcpy(allocation.virtual_bytes(), bytes.data(), bytes.size());
}

void CopyAllocationWithOverlap(GuestAllocation& allocation, std::span<const std::uint8_t> bytes, std::uint32_t overlap_anchor)
{
    // Model the producer's partial CPU writes while retaining one live guest
    // allocation. The second copy intentionally overlaps the preceding range.
    std::memset(allocation.virtual_bytes(), 0, AlignToPage(static_cast<std::uint32_t>(bytes.size())));
    if (bytes.empty())
        return;
    const auto anchor = std::min<std::size_t>(overlap_anchor, bytes.size());
    const auto first  = std::min<std::size_t>(bytes.size(), anchor + 8U);
    std::memcpy(allocation.virtual_bytes(), bytes.data(), first);
    const auto overlap       = anchor >= 8U ? anchor - 8U : 0U;
    const auto overlapLength = std::min<std::size_t>(bytes.size() - overlap, 16U);
    std::memcpy(allocation.virtual_bytes() + overlap, bytes.data() + overlap, overlapLength);
    if (overlap + overlapLength < bytes.size())
        std::memcpy(allocation.virtual_bytes() + overlap + overlapLength,
                    bytes.data() + overlap + overlapLength,
                    bytes.size() - overlap - overlapLength);
}

bool CopyChangedBc2RangeWithOverlap(GuestAllocation&              allocation,
                                    std::span<const std::uint8_t> old_bytes,
                                    std::span<const std::uint8_t> new_bytes,
                                    std::uint32_t                 offset,
                                    std::uint32_t                 length,
                                    bool&                         outside_unchanged,
                                    bool&                         overlap_written)
{
    outside_unchanged = false;
    overlap_written   = false;
    if (old_bytes.size() != new_bytes.size() ||
        static_cast<std::size_t>(offset) + length > old_bytes.size())
        return false;
    if (std::memcmp(allocation.physical_bytes(), old_bytes.data(), old_bytes.size()) != 0)
        return false;
    bool changed = false;
    for (std::size_t index = 0; index < old_bytes.size(); ++index)
    {
        if (index >= offset && index < static_cast<std::size_t>(offset) + length &&
            old_bytes[index] != new_bytes[index])
            changed = true;
        if ((index < offset || index >= static_cast<std::size_t>(offset) + length) &&
            old_bytes[index] != new_bytes[index])
            return false;
    }
    if (!changed || length == 0U)
        return false;

    // CPU writes must use the guest virtual alias. The physical view is the
    // SDK data-provider path and bypasses the invalidation watches that make
    // an admitted texture version stale after a real CPU rewrite.
    auto*      destination  = allocation.virtual_bytes();
    const auto begin        = static_cast<std::size_t>(offset);
    const auto end          = begin + length;
    const auto first_length = std::min<std::size_t>(8U, length);
    std::memcpy(destination + begin, new_bytes.data() + begin, first_length);
    const auto overlap_begin  = begin + std::min<std::size_t>(4U, length - 1U);
    const auto overlap_length = std::min<std::size_t>(8U, end - overlap_begin);
    std::memcpy(destination + overlap_begin, new_bytes.data() + overlap_begin, overlap_length);
    overlap_written = overlap_begin < begin + first_length && overlap_length != 0U;
    if (overlap_begin + overlap_length < end)
        std::memcpy(destination + overlap_begin + overlap_length,
                    new_bytes.data() + overlap_begin + overlap_length,
                    end - overlap_begin - overlap_length);

    outside_unchanged = true;
    for (std::size_t index = 0; index < old_bytes.size(); ++index)
    {
        if (index < begin || index >= end)
            outside_unchanged = outside_unchanged && destination[index] == old_bytes[index];
    }
    return std::memcmp(destination, new_bytes.data(), new_bytes.size()) == 0;
}

} // namespace

int RunNativePreparedDxtMatrixOriginal(
    NativePreparedDxtMatrixOriginalParams&                                           params,
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& forwarded,
    NativePreparedDxtMatrixOriginalResult&                                           result,
    std::string&                                                                     error)
{
    result                = {};
    result.source_fixture = std::move(params.fixture);
    error.clear();
    result.cpu_update_level = params.first_lod;
    result.update_kind      = params.update_kind;
    TraceOriginalPhase("entry_reset");
    if (params.first_lod >= kNativePreparedDxtMatrixMipCount || params.second_lod >= kNativePreparedDxtMatrixMipCount)
    {
        error          = "prepared rectangular BC2 must be in the nine-level fixture range";
        result.failure = error;
        return 1;
    }

    auto& fixture = result.source_fixture;
    TraceOriginalPhase("rebuild_texture_guest_bytes_begin");
    const bool texture_guest_bytes_rebuilt = RebuildTextureGuestBytes(fixture);
    TraceOriginalPhase(texture_guest_bytes_rebuilt ? "rebuild_texture_guest_bytes_end"
                                                   : "rebuild_texture_guest_bytes_failed");
    if (!texture_guest_bytes_rebuilt)
    {
        error          = "prepared rectangular BC2 fixture does not contain nine complete raw BC2 mips";
        result.failure = error;
        return 1;
    }
    PatchLods(fixture, params.first_lod, params.second_lod);
    RebuildExpectedPlanes(fixture);
    result.expected_pixels = ExpectedNativePreparedDxtMatrixPixels(fixture, params.first_lod, params.second_lod);
    if (result.expected_pixels.empty())
    {
        error          = "prepared BC2 fixture expected plane was empty";
        result.failure = error;
        return 1;
    }

    TraceOriginalPhase("load_gpu_plugin_begin");
    auto graphics = rex::system::LoadGpuPlugin("xenos", "d3d12");
    TraceOriginalPhase(graphics != nullptr ? "load_gpu_plugin_end" : "load_gpu_plugin_failed");
    if (!Require(graphics != nullptr, "xenos d3d12 plugin was not loaded", error))
    {
        result.failure = error;
        return 1;
    }
    result.gpu_module_path     = LoadedModulePath(L"rexgpu-xenos.dll");
    result.runtime_module_path = LoadedModulePath(L"rexruntime.dll");
    if (!Require(!result.gpu_module_path.empty() && !result.runtime_module_path.empty(),
                 "actual rexruntime.dll and rexgpu-xenos.dll module identities were not loaded",
                 error))
    {
        result.failure = error;
        graphics->Shutdown();
        return 1;
    }
    rex::Runtime runtime(std::filesystem::path{});
    TraceOriginalPhase("runtime_setup_begin");
    const auto runtime_status = runtime.Setup(rex::RuntimeConfig{ .kernel_init = rex::kernel::InitializeKernel,
                                                                  .tool_mode   = true });
    TraceOriginalPhase("runtime_setup_end");
    if (!Require(runtime_status == static_cast<rex::X_STATUS>(0),
                 "runtime setup failed",
                 error))
    {
        result.failure = error;
        return 1;
    }
    std::vector<GuestAllocation*> guest_allocations;
    const auto                    shutdown = [&]
    {
        TraceOriginalPhase("shutdown_begin");
        graphics->Shutdown();
        TraceOriginalPhase("shutdown_graphics_end");
        for (auto* allocation : guest_allocations)
        {
            allocation->Reset();
        }
        TraceOriginalPhase("shutdown_allocations_end");
        runtime.Shutdown();
        TraceOriginalPhase("shutdown_end");
    };
    TraceOriginalPhase("gpu_setup_begin");
    // PreparedCopy admits source resolves; resolve_readback copies are
    // rejected.
    if (!Require(rex::cvar::SetFlagByName("readback_resolve", "none"),
                 "readback_resolve=none was rejected",
                 error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }
    // The oracle models pix_center D3D with the SDK half-pixel offset applied.
    if (!Require(rex::cvar::SetFlagByName("half_pixel_offset", "true"), "half_pixel_offset=true was rejected", error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }
    if (!Require(rex::cvar::SetFlagByName("async_shader_compilation", "false"),
                 "synchronous shader compilation was rejected",
                 error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }
    TraceOriginalPhase("setup_presentation_begin");
    const auto presentation_status = graphics->SetupPresentation(nullptr);
    TraceOriginalPhase("setup_presentation_end");
    if (!Require(presentation_status == static_cast<rex::X_STATUS>(0),
                 "offscreen D3D12 presenter setup failed",
                 error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }
    TraceOriginalPhase("setup_guest_gpu_begin");
    const auto guest_gpu_status = graphics->SetupGuestGpu(runtime.function_dispatcher(), runtime.kernel_state());
    TraceOriginalPhase("setup_guest_gpu_end");
    if (!Require(guest_gpu_status == static_cast<rex::X_STATUS>(0),
                 "guest GPU setup failed",
                 error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }
    TraceOriginalPhase("gpu_setup_end");

    auto       mailbox        = rex::graphics::diagnostic::GetDrawCaptureMailbox();
    auto       observation    = std::make_shared<OriginalObservation>(forwarded);
    const auto observer_token = mailbox->RegisterFrameInputObserver(observation);
    if (!Require(static_cast<bool>(observer_token), "mailbox observer registration failed", error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }

    const auto freezeObservation = [&]
    {
        const auto snapshot          = observation->SnapshotData();
        result.draws                 = snapshot.draws;
        result.prepared_draws        = snapshot.prepared_draws;
        result.copies                = snapshot.copies;
        result.prepared_copies       = snapshot.prepared_copies;
        result.swaps                 = snapshot.swaps;
        result.prepared_swaps        = snapshot.prepared_swaps;
        result.frame_ends            = snapshot.frame_ends;
        result.completion_copy_id    = snapshot.completion_copy_id;
        result.completion_swap_id    = snapshot.completion_swap_id;
        result.completion_frame      = snapshot.completion_frame;
        result.completion_submission = snapshot.completion_submission;
    };
    const auto unregisterFrameObserver = [&]
    {
        freezeObservation();
        mailbox->UnregisterFrameInputObserver(observer_token);
    };

    rex::memory::Memory& memory          = *runtime.memory();
    const auto           terminal_layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, kNativePreparedDxtMatrixOutputWidth / 32U, kNativePreparedDxtMatrixOutputWidth, kNativePreparedDxtMatrixOutputHeight, 1U, true, xenos::TextureFormat::k_8_8_8_8, false, true, 0U);
    const auto terminal_extent = AlignToPage(terminal_layout.base.level_data_extent_bytes);
    TraceOriginalPhase("allocations_begin");
    GuestAllocation ring(memory, kRingWordCount * sizeof(std::uint32_t), kPageSize);
    GuestAllocation writeback(memory, kPageSize, kPageSize);
    GuestAllocation terminal(memory, terminal_extent, kPageSize);
    GuestAllocation mip_base(memory, static_cast<std::uint32_t>(fixture.texture.base_guest_bytes.size()), kPageSize);
    GuestAllocation mip_levels(memory, static_cast<std::uint32_t>(fixture.texture.mip_guest_bytes.size()), kPageSize);
    GuestAllocation vertex_bytes(memory, static_cast<std::uint32_t>(fixture.vertex_bytes.size()), kPageSize);
    GuestAllocation indices(memory, kPageSize, kPageSize);
    GuestAllocation resolve_geometry(memory, kPageSize, kPageSize);
    GuestAllocation vertex_shader(memory, static_cast<std::uint32_t>(fixture.vertex_shader_words.size() * 4U), kPageSize);
    GuestAllocation pixel_shader(memory, static_cast<std::uint32_t>(fixture.pixel_shader_words.size() * 4U), kPageSize);
    TraceOriginalPhase("allocations_end");
    guest_allocations      = { &ring, &writeback, &terminal, &mip_base, &mip_levels, &vertex_bytes, &indices, &resolve_geometry, &vertex_shader, &pixel_shader };
    const bool allocations = ring && writeback && terminal && mip_base && mip_levels && vertex_bytes &&
                             indices && resolve_geometry && vertex_shader && pixel_shader;
    if (!Require(allocations, "guest physical test allocations failed", error))
    {
        unregisterFrameObserver();
        shutdown();
        result.failure = error;
        return 1;
    }
    result.source_index_guest_base = indices.physical_address();

    result.old_base_bytes           = fixture.texture.base_guest_bytes;
    result.old_mip_bytes            = fixture.texture.mip_guest_bytes;
    result.baseline_expected_pixels = result.expected_pixels;
    CopyAllocationWithOverlap(mip_base, fixture.texture.base_guest_bytes, 0x400U);
    CopyAllocationWithOverlap(mip_levels, fixture.texture.mip_guest_bytes, 0x2000U);
    CopyAllocationWithOverlap(mip_levels, fixture.texture.mip_guest_bytes, kNativePreparedDxtMatrixTailOffset);
    result.cpu_partial_overlap_writes = true;
    result.source_raw_bytes_frozen =
        std::memcmp(mip_base.physical_bytes(), fixture.texture.base_guest_bytes.data(), fixture.texture.base_guest_bytes.size()) == 0 &&
        std::memcmp(mip_levels.physical_bytes(), fixture.texture.mip_guest_bytes.data(), fixture.texture.mip_guest_bytes.size()) == 0;
    if (!Require(result.source_raw_bytes_frozen, "CPU guest writes did not preserve the complete raw BC2 bytes", error))
    {
        unregisterFrameObserver();
        shutdown();
        result.failure = error;
        return 1;
    }
    CopyAllocation(vertex_bytes, fixture.vertex_bytes);
    CopyAllocation(indices, fixture.index_bytes);
    const auto resolve_bytes = MakeResolveGeometry();
    CopyAllocation(resolve_geometry, resolve_bytes);
    std::memset(terminal.physical_bytes(), 0, terminal_extent);
    WriteShaderWords(vertex_shader, fixture.vertex_shader_words);
    WriteShaderWords(pixel_shader, fixture.pixel_shader_words);
    graphics->InitializeRingBuffer(ring.physical_address(), kRingSizeLog2);
    graphics->EnableReadPointerWriteBack(writeback.physical_address(), kWritebackBlockSizeLog2);

    const auto terminal_texture       = MakeOutputFetch(terminal.physical_address());
    const auto source_texture         = MakeTextureFetch(mip_base.physical_address(), mip_levels.physical_address());
    const auto source_vertex_fetch    = MakeVertexFetch(vertex_bytes.physical_address(),
                                                        static_cast<std::uint32_t>(fixture.vertex_bytes.size()));
    const auto resolve_fetch          = MakeVertexFetch(resolve_geometry.physical_address(),
                                                        static_cast<std::uint32_t>(resolve_bytes.size()));
    result.source_texture_fetch_words = source_texture;
    result.source_vertex_fetch_words  = source_vertex_fetch;
    TraceOriginalPhase("command_stream_begin");
    PacketStream stream;
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, terminal_texture);
    stream.Swap(terminal.physical_address(), kNativePreparedDxtMatrixOutputWidth, kNativePreparedDxtMatrixOutputHeight);
    stream.LoadShader(vertex_shader.physical_address(), xenos::ShaderType::kVertex, static_cast<std::uint32_t>(fixture.vertex_shader_words.size()));
    stream.LoadShader(pixel_shader.physical_address(), xenos::ShaderType::kPixel, static_cast<std::uint32_t>(fixture.pixel_shader_words.size()));
    stream.Type0(0x2000U, { 0x14000500U, 0U, 0x530U });
    stream.Type0(0x200FU, { 0x20002000U });
    stream.Type0(0x2080U, { 0U, 0U });
    stream.Type0(0x2082U, { DxtMatrixViewportWidth() | (DxtMatrixViewportHeight() << 16U) });
    stream.Type0(0x2100U, { 2U });
    stream.Type0(0x2104U, { 0xFU });
    stream.Type0(0x210FU, { std::bit_cast<std::uint32_t>(static_cast<float>(DxtMatrixViewportWidth()) / 2.0F), std::bit_cast<std::uint32_t>(static_cast<float>(DxtMatrixViewportWidth()) / 2.0F), std::bit_cast<std::uint32_t>(-static_cast<float>(DxtMatrixViewportHeight()) / 2.0F), std::bit_cast<std::uint32_t>(static_cast<float>(DxtMatrixViewportHeight()) / 2.0F) });
    stream.Type0(0x2180U, { 0x10010002U });
    stream.Type0(0x2181U, { 4U });
    stream.Type0(0x2182U, { 1U });
    stream.Type0(0x2200U, { 0x24F00730U });
    stream.Type0(0x2201U, { 0x01000006U });
    stream.Type0(0x2202U, { 0x87000000U });
    stream.Type0(0x2204U, { 0x80000U });
    stream.Type0(0x2205U, { 0x18000U });
    stream.Type0(0x2206U, { 0x43FU });
    stream.Type0(0x2208U, { 4U });
    stream.Type0(0x2302U, { 4U });
    stream.Type0(0x2307U, { 0xFF000U });
    stream.Type0(0x2308U, { 0xFF100U });
    // The terminal copy leaves RB_COPY_* state live across submissions. Set
    // the complete copy register block explicitly before each source draw so
    // the issued draw has the fixture's authored zero state.
    stream.Type0(0x2318U, { 0U, 0U, 0U, 0U, 0U, 0U, 0U });
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_000_X, kVertexConstants);
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_256_X, kPixelConstant0);
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_256_X + 255U * 4U, kPixelConstant255);
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, source_texture);
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U, source_vertex_fetch);
    stream.Type0(0x21FCU, { kNativePreparedDxtMatrixDrawInitiator });
    stream.DrawDma(kNativePreparedDxtMatrixDrawInitiator,
                   indices.physical_address(),
                   kNativePreparedDxtMatrixIndexCount,
                   kNativePreparedDxtMatrixIndexEndian);
    stream.Type0(0x2000U, { 0x14000500U, 0U, 0x530U });
    stream.Type0(0x200FU, { 0x20002000U });
    stream.Type0(0x2080U, { 0U, 0U, DxtMatrixViewportWidth() | (DxtMatrixViewportHeight() << 16U) });
    stream.Type0(0x2082U, { kNativePreparedDxtMatrixOutputWidth | (kNativePreparedDxtMatrixOutputHeight << 16U) });
    stream.Type0(0x2205U, { 0x10000U });
    stream.Type0(0x2208U, { 6U });
    stream.Type0(0x2318U, { 0x00100000U });
    stream.Type0(0x2319U, { terminal.physical_address() });
    stream.Type0(0x231AU, { kNativePreparedDxtMatrixOutputWidth | (kNativePreparedDxtMatrixOutputHeight << 16U) });
    stream.Type0(0x231BU, { 0x00000300U });
    stream.Type0(0x231CU, { 0xFFFU });
    stream.Type0(0x231DU, { 0xFFFFFF00U });
    stream.Type0(0x231EU, { 0U, 0U });
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, resolve_fetch);
    stream.Type0(0x21FCU, { static_cast<std::uint32_t>(xenos::PrimitiveType::kRectangleList) | (static_cast<std::uint32_t>(xenos::SourceSelect::kAutoIndex) << 6U) | (3U << 16U) });
    stream.DrawAuto(0x00030088U);
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, terminal_texture);
    stream.Swap(terminal.physical_address(), kNativePreparedDxtMatrixOutputWidth, kNativePreparedDxtMatrixOutputHeight);
    if (!Require(stream.words().size() < kRingWordCount, "PM4 stream exceeds ring capacity", error))
    {
        unregisterFrameObserver();
        shutdown();
        result.failure = error;
        return 1;
    }
    TraceOriginalPhase("command_stream_end");
    result.published_word_count = static_cast<std::uint32_t>(stream.words().size());
    TraceOriginalPhase("publish_begin");
    WritePacketWords(memory, ring.physical_address(), stream.words());
    PublishWritePointer(memory, result.published_word_count);
    TraceOriginalPhase("publish_end");
    TraceOriginalPhase("wait_writeback_begin");
    result.progressed = WaitForWriteback(memory, writeback.physical_address(), result.published_word_count);
    TraceOriginalPhase(result.progressed ? "wait_writeback_end" : "wait_writeback_failed");
    TraceOriginalPhase("wait_completion_begin");
    result.completion_observed = result.progressed && observation->WaitForCompletion(terminal.physical_address());
    TraceOriginalPhase(result.completion_observed ? "wait_completion_end" : "wait_completion_failed");
    const auto baseline_snapshot              = observation->SnapshotData();
    result.baseline_completion_observed       = result.completion_observed;
    result.baseline_copy_id                   = baseline_snapshot.completion_copy_id;
    result.baseline_swap_id                   = baseline_snapshot.completion_swap_id;
    result.baseline_frame                     = baseline_snapshot.completion_frame;
    result.baseline_submission                = baseline_snapshot.completion_submission;
    std::uint64_t baseline_swap_event_ordinal = 0;
    for (const auto& swap : baseline_snapshot.swaps)
    {
        if (swap.id == result.baseline_swap_id)
        {
            baseline_swap_event_ordinal = swap.event_ordinal;
            break;
        }
    }
    rex::ui::RawImage baseline_image;
    result.baseline_terminal_capture = result.baseline_completion_observed && baseline_swap_event_ordinal != 0 &&
                                       graphics->presenter() && graphics->presenter()->CaptureGuestOutput(baseline_image);
    if (result.baseline_terminal_capture)
        result.baseline_terminal_pixels = baseline_image.data;
    if (!Require(result.baseline_terminal_capture &&
                     result.baseline_terminal_pixels == result.baseline_expected_pixels,
                 "baseline Prepared BC2 completion did not capture the expected pixels",
                 error))
    {
        unregisterFrameObserver();
        shutdown();
        result.failure = error;
        return 1;
    }

    // Perform the guarded CPU update against the same live allocations after
    // the first completion. The resulting callback must publish a new owner.
    ApplyCpuMipUpdate(fixture, params.first_lod, params.update_kind);
    if (!Require(RebuildTextureGuestBytes(fixture),
                 "CPU BC2 update could not rebuild all nine raw guest mips",
                 error))
    {
        unregisterFrameObserver();
        shutdown();
        result.failure = error;
        return 1;
    }
    fixture.selected_lods = { params.first_lod, params.second_lod };
    RebuildExpectedPlanes(fixture);
    result.expected_pixels     = ExpectedNativePreparedDxtMatrixPixels(fixture, params.first_lod, params.second_lod);
    result.new_base_bytes      = fixture.texture.base_guest_bytes;
    result.new_mip_bytes       = fixture.texture.mip_guest_bytes;
    result.cpu_update_observed = result.old_base_bytes != result.new_base_bytes || result.old_mip_bytes != result.new_mip_bytes;
    if (!Require(result.cpu_update_observed, "CPU BC2 update did not change retained raw guest bytes", error))
    {
        unregisterFrameObserver();
        shutdown();
        result.failure = error;
        return 1;
    }
    const auto update_width       = kNativePreparedDxtMatrixWidths[params.first_lod];
    const auto update_height      = kNativePreparedDxtMatrixHeights[params.first_lod];
    const auto update_blocks_wide = std::max(1U, (update_width + 3U) / 4U);
    const auto update_blocks_high = std::max(1U, (update_height + 3U) / 4U);
    auto       update_offset      = kNativePreparedDxtMatrixGuestOffsets[params.first_lod];
    auto       update_length      = (update_blocks_high - 1U) * kNativePreparedDxtMatrixRowPitches[params.first_lod] +
                                    update_blocks_wide * DxtMatrixBlockBytes();
    if (dxtMatrixForm->tiled)
    {
        std::size_t begin = SIZE_MAX, end = 0U;
        for (std::uint32_t y = 0; y < update_blocks_high; ++y)
            for (std::uint32_t x = 0; x < update_blocks_wide; ++x)
            {
                const auto at = DxtMatrixGuestBlockOffset(params.first_lod, x, y);
                begin         = std::min(begin, at);
                end           = std::max(end, at + DxtMatrixBlockBytes());
            }
        update_offset = static_cast<std::uint32_t>(begin);
        update_length = static_cast<std::uint32_t>(end - begin);
    }
    bool       update_outside_unchanged  = false;
    bool       update_overlap_written    = false;
    const bool update_applied            = params.first_lod == 0U
                                               ? CopyChangedBc2RangeWithOverlap(mip_base,
                                                                                result.old_base_bytes,
                                                                                result.new_base_bytes,
                                                                                update_offset,
                                                                                update_length,
                                                                                update_outside_unchanged,
                                                                                update_overlap_written)
                                               : CopyChangedBc2RangeWithOverlap(mip_levels,
                                                                                result.old_mip_bytes,
                                                                                result.new_mip_bytes,
                                                                                update_offset,
                                                                                update_length,
                                                                                update_outside_unchanged,
                                                                                update_overlap_written);
    result.cpu_update_guest_offset       = update_offset;
    result.cpu_update_guest_length       = update_length;
    result.cpu_update_overlap_written    = update_overlap_written;
    result.cpu_update_outside_unchanged  = update_outside_unchanged;
    result.cpu_update_opposite_unchanged = params.update_kind == NativePreparedDxtMatrixUpdateKind::kCombined;
    if (params.update_kind == NativePreparedDxtMatrixUpdateKind::kColorOnly ||
        params.update_kind == NativePreparedDxtMatrixUpdateKind::kAlphaOnly)
    {
        result.cpu_update_opposite_unchanged = true;
        const auto& old_bytes                = params.first_lod == 0U ? result.old_base_bytes : result.old_mip_bytes;
        const auto& new_bytes                = params.first_lod == 0U ? result.new_base_bytes : result.new_mip_bytes;
        const auto  blocks_width             = std::max(1U, (update_width + 3U) / 4U);
        const auto  blocks_height            = std::max(1U, (update_height + 3U) / 4U);
        const auto  unchanged_begin          = params.update_kind == NativePreparedDxtMatrixUpdateKind::kColorOnly ? 0U : 8U;
        const auto  unchanged_length         = 8U;
        for (std::uint32_t block_y = 0; block_y < blocks_height; ++block_y)
        {
            for (std::uint32_t block_x = 0; block_x < blocks_width; ++block_x)
            {
                const auto block_offset = DxtMatrixGuestBlockOffset(params.first_lod, block_x, block_y);
                result.cpu_update_opposite_unchanged =
                    result.cpu_update_opposite_unchanged &&
                    std::memcmp(old_bytes.data() + block_offset + unchanged_begin,
                                new_bytes.data() + block_offset + unchanged_begin,
                                unchanged_length) == 0;
            }
        }
    }
    result.cpu_update_observed = result.cpu_update_observed && update_applied;
    result.source_raw_bytes_frozen =
        std::memcmp(mip_base.physical_bytes(), fixture.texture.base_guest_bytes.data(), fixture.texture.base_guest_bytes.size()) == 0 &&
        std::memcmp(mip_levels.physical_bytes(), fixture.texture.mip_guest_bytes.data(), fixture.texture.mip_guest_bytes.size()) == 0;
    if (!Require(result.source_raw_bytes_frozen && result.cpu_update_overlap_written && result.cpu_update_outside_unchanged,
                 "CPU BC2 update did not preserve complete raw bytes outside its selected overlap range",
                 error))
    {
        unregisterFrameObserver();
        shutdown();
        result.failure = error;
        return 1;
    }

    const auto first_words = static_cast<std::uint32_t>(stream.words().size());
    const auto total_words = first_words * 2U;
    if (!Require(total_words < kRingWordCount, "two BC2 submissions exceed ring capacity", error))
    {
        unregisterFrameObserver();
        shutdown();
        result.failure = error;
        return 1;
    }
    TraceOriginalPhase("publish_update_begin");
    WritePacketWords(memory, ring.physical_address() + first_words * sizeof(std::uint32_t), stream.words());
    result.published_word_count = total_words;
    PublishWritePointer(memory, total_words);
    TraceOriginalPhase("publish_update_end");
    TraceOriginalPhase("wait_writeback_update_begin");
    const bool update_progressed = WaitForWriteback(memory, writeback.physical_address(), total_words);
    result.progressed            = result.progressed && update_progressed;
    TraceOriginalPhase(update_progressed ? "wait_writeback_update_end" : "wait_writeback_update_failed");
    TraceOriginalPhase("wait_completion_update_begin");
    result.completion_observed = result.progressed &&
                                 observation->WaitForCompletion(terminal.physical_address(), baseline_swap_event_ordinal);
    TraceOriginalPhase(result.completion_observed ? "wait_completion_update_end" : "wait_completion_update_failed");
    rex::ui::RawImage image;
    result.terminal_capture = result.completion_observed && graphics->presenter() &&
                              graphics->presenter()->CaptureGuestOutput(image);
    if (result.terminal_capture)
    {
        result.terminal_width  = image.width;
        result.terminal_height = image.height;
        result.terminal_stride = image.stride;
        result.terminal_pixels = image.data;
    }
    result.terminal_pixels_match_expected = result.terminal_capture &&
                                            result.terminal_pixels == result.expected_pixels;
    result.actual_readback_owner          = result.completion_observed && result.terminal_capture;
    result.readback_owner                 = result.actual_readback_owner
                                                ? "presenter CaptureGuestOutput after matched completed update"
                                                : "none";
    unregisterFrameObserver();
    for (auto draw = result.draws.rbegin(); draw != result.draws.rend(); ++draw)
    {
        if (draw->vertex_shader_words == fixture.vertex_shader_words &&
            draw->pixel_shader_words == fixture.pixel_shader_words)
        {
            result.source_registers = draw->registers;
            result.source_draw_issued =
                draw->original_host_issued &&
                draw->execution_status == static_cast<std::uint32_t>(DrawCaptureExecutionStatus::kIssued);
            if (result.source_registers.size() >=
                XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U + 6U)
            {
                std::copy_n(result.source_registers.begin() + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6, result.source_texture_fetch_words.begin());
                std::copy_n(result.source_registers.begin() + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U,
                            6,
                            result.source_vertex_fetch_words.begin());
            }
            break;
        }
    }
    if (result.source_draw_issued && result.source_registers.size() == kNativePreparedDxtMatrixRegisterCount)
    {
        result.source_fixture.register_image      = result.source_registers;
        result.source_fixture.texture.fetch_words = result.source_texture_fetch_words;
        result.source_fixture.vertex_fetch_words  = result.source_vertex_fetch_words;
    }
    const bool complete = result.progressed && result.baseline_completion_observed &&
                          result.baseline_terminal_capture && result.cpu_update_observed &&
                          result.completion_observed && result.terminal_capture &&
                          result.actual_readback_owner && result.source_draw_issued &&
                          result.terminal_pixels_match_expected &&
                          result.terminal_width == kNativePreparedDxtMatrixOutputWidth &&
                          result.terminal_height == kNativePreparedDxtMatrixOutputHeight &&
                          result.terminal_stride == kNativePreparedDxtMatrixOutputWidth * 4U;
    if (!complete)
    {
        error          = "original prepared BC2 GPU completion or expected pixels were incomplete";
        result.failure = error;
    }
    shutdown();
    return complete ? 0 : 1;
}

} // namespace rex::graphics::testing
