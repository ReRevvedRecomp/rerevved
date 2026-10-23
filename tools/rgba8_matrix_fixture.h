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

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rgba8_matrix_forms.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <span>

namespace rex::graphics::testing
{

inline constexpr std::uint32_t kNativePreparedRgba8MatrixOutputWidth   = 1280U;
inline constexpr std::uint32_t kNativePreparedRgba8MatrixOutputHeight  = 720U;
inline constexpr std::uint32_t kNativePreparedRgba8MatrixVertexStride  = 56U;
inline constexpr std::uint32_t kNativePreparedRgba8MatrixRegisterCount = 0x5003U;

struct NativePreparedRgba8MatrixMip
{
    std::uint32_t             level     = 0;
    std::uint32_t             width     = 0;
    std::uint32_t             height    = 0;
    std::uint32_t             row_pitch = 0;
    std::vector<std::uint8_t> linear_rgba8;
    // The bytes below are the exact guest layout for one independently stored
    // level. The base level lives in base_guest_bytes; all nonzero levels live in
    // mip_guest_bytes at their layout offsets.
    std::uint32_t guest_offset = 0;
    std::uint32_t guest_extent = 0;
};

struct NativePreparedRgba8MatrixTexture
{
    std::uint32_t                             width         = kNativePreparedRgba8MatrixWidth;
    std::uint32_t                             height        = kNativePreparedRgba8MatrixHeight;
    std::uint32_t                             pitch         = rgba8MatrixForm->pitch;
    std::uint32_t                             format        = 0;
    std::uint32_t                             endianness    = 0;
    std::uint32_t                             swizzle       = 0;
    std::uint32_t                             dimension     = 0;
    bool                                      tiled         = false;
    bool                                      packed_mips   = false;
    std::uint32_t                             mip_min_level = 0;
    std::uint32_t                             mip_max_level = kNativePreparedRgba8MatrixMipCount - 1U;
    std::array<std::uint32_t, 6>              fetch_words{};
    std::vector<std::uint8_t>                 base_guest_bytes;
    std::vector<std::uint8_t>                 mip_guest_bytes;
    std::vector<NativePreparedRgba8MatrixMip> mips;
};

struct NativePreparedRgba8MatrixFixture
{
    std::uint32_t                width           = Rgba8MatrixViewportWidth();
    std::uint32_t                height          = Rgba8MatrixViewportHeight();
    std::uint32_t                terminal_stride = kNativePreparedRgba8MatrixOutputWidth * 4U;
    std::uint32_t                vertex_stride   = kNativePreparedRgba8MatrixVertexStride;
    std::array<std::uint32_t, 2> selected_lods   = { 0U, 0U };

    std::uint32_t window_x = 0, window_y = 0;

    // Raw guest words and the complete register image are owned by the fixture
    // so a catalog/native consumer can use exactly the same input identity.
    std::vector<std::uint32_t>       vertex_shader_words;
    std::vector<std::uint32_t>       pixel_shader_words;
    std::vector<std::uint32_t>       register_image;
    std::array<std::uint32_t, 6>     vertex_fetch_words{};
    std::vector<std::uint8_t>        vertex_bytes;
    std::vector<std::uint8_t>        index_bytes;
    NativePreparedRgba8MatrixTexture texture;

    // RGB contains a weighted readout of all four selected guest components.
    // Presenter alpha is 255; only the selected pair owns its output canvas.
    std::vector<std::vector<std::uint8_t>> expected_by_lod_pair;
    std::vector<std::uint8_t>              expected_selected_pixels;
};

NativePreparedRgba8MatrixFixture MakeNativePreparedRgba8MatrixFixture();

// Returns an empty vector for an invalid level. This derives the expected
// pixels from the fixture's current mip bytes, allowing mutation checks to use
// the same owned source data that is sent to the original GPU.
std::vector<std::uint8_t> ExpectedNativePreparedRgba8MatrixPixels(const NativePreparedRgba8MatrixFixture& fixture,
                                                                  std::uint32_t                           first_lod,
                                                                  std::uint32_t                           second_lod);

namespace rgba8_matrix_detail
{

inline constexpr std::uint32_t kPageSize                             = 0x1000U;
inline constexpr std::uint32_t kNativePreparedRgba8MatrixIndexCount  = 3U;
inline constexpr auto          kNativePreparedRgba8MatrixIndexEndian = rex::graphics::xenos::Endian::k8in16;
inline constexpr std::uint32_t kNativePreparedRgba8MatrixDmaSize =
    kNativePreparedRgba8MatrixIndexCount | (static_cast<std::uint32_t>(kNativePreparedRgba8MatrixIndexEndian) << 30U);
inline constexpr std::uint32_t kNativePreparedRgba8MatrixDrawInitiator =
    static_cast<std::uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
    (static_cast<std::uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6U) |
    (kNativePreparedRgba8MatrixIndexCount << 16U);

// This is the cataloged position/fetch-95 VS with only its point-of-use
// interpolator mapping widened to carry UV, lod0 and lod1 in r3.xyzw.
// r0 remains the position/scalar work register; the final export reads r3.
inline constexpr std::array<std::uint32_t, 45> kVertexShader = {
    0x30052003U, 0x00001200U, 0xC2000000U, 0x00006005U, 0x200B1200U, 0x12000000U, 0x00000000U, 0x100DC400U, 0x22000000U, 0x2DF82000U, 0x00393A88U, 0x0000000EU, 0x05F83000U, 0x40263688U, 0x0000040EU, 0xC8020001U, 0x00A7A700U, 0xAF020100U, 0xC8080001U, 0x00A7A700U, 0xAF020300U, 0xC8020000U, 0x00A7A700U, 0xAF020200U, 0xB8410101U, 0x01A7A741U, 0x8F020008U, 0xC801803EU, 0x00A7A700U, 0xAF010400U, 0xC802803EU, 0x00A7A700U, 0xAF010500U, 0xC804803EU, 0x00A7A700U, 0xAF010600U, 0xC808803EU, 0x00A7A700U, 0xAF010700U, 0xC80F8000U, 0x00000000U, 0xE2030300U, 0x4E4A0007U, 0x95A570DEU, 0x30B52E1DU
};

// Four register-LOD fetch instructions, add into r3, weighted dp4 export.
// Distinct weights expose all guest components, including alpha-only R8.
inline constexpr std::array<std::uint32_t, 21> kPixelShader = {
    0x00000000U, 0x6001C400U, 0x22000A55U, 0x08000018U, 0, 0, 0x10081001U, 0x2F1FF688U, 0x00004000U, 0x0C000018U, 0, 0, 0x10082001U, 0x2F1FF688U, 0x00004000U, 0x000F0003U, 0, 0xC0010200U, 0x000F8000U, 0, 0x8F030000U
};
// Match the clean installed control VS identity rows from SDK commit
// df8ec1c544c1e8f391dcfaff295ef387a1a7fea9,
// tests/installed_prepared_control/control.cpp:kConsumerVertexConstants.
inline constexpr std::array<std::uint32_t, 36> kVertexConstants = {
    0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U
};

inline constexpr std::array<std::uint32_t, 4> kPixelConstant0   = { 0x3F800000U, 0x40000000U, 0x40800000U, 0x41000000U };
inline constexpr std::array<std::uint32_t, 4> kPixelConstant255 = {
    0x3DCCCCCDU, 0x3E4CCCCDU, 0x3E99999AU, 0x3F800000U
};

inline std::uint32_t AlignToPage(std::uint32_t bytes)
{
    return bytes == 0U ? 0U : (bytes + kPageSize - 1U) & ~(kPageSize - 1U);
}

inline void AppendBigEndianFloat(std::vector<std::uint8_t>& bytes, float value)
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    bytes.push_back(static_cast<std::uint8_t>(bits >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(bits));
}

inline std::uint32_t ClampUnorm8(std::uint32_t value)
{
    return std::min(255U, value);
}

inline std::vector<std::uint8_t> MakeExpectedPixels(const rex::graphics::testing::NativePreparedRgba8MatrixFixture& fixture,
                                                    std::uint32_t                                                   first_lod,
                                                    std::uint32_t                                                   second_lod)
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
    const auto&               a     = fixture.texture.mips[first_lod].linear_rgba8;
    const auto&               b     = fixture.texture.mips[second_lod].linear_rgba8;
    const auto&               a_mip = fixture.texture.mips[first_lod];
    const auto&               b_mip = fixture.texture.mips[second_lod];
    std::vector<std::uint8_t> result(static_cast<std::size_t>(rex::graphics::testing::kNativePreparedRgba8MatrixOutputWidth) *
                                         rex::graphics::testing::kNativePreparedRgba8MatrixOutputHeight * 4U,
                                     0U);
    for (std::size_t offset = 3U; offset < result.size(); offset += 4U)
        result[offset] = 255U;
    // The authored fetch has point mag/min/mip filters and repeat within the interior.
    // Match the triangle's quarter-base-texel bias for bounded large forms.
    // Integer arithmetic floors the biased coordinate independently of GPU
    // interpolation; every form uses the same interior bias.
    const auto nearestTexel = [&](std::uint32_t pixel, std::uint32_t sourceExtent, std::uint32_t viewportExtent, std::uint32_t baseExtent)
    {
        const auto bias = viewportExtent;
        // PA_SU_VTX_CNTL pix_center 0 (D3D) samples at integer pixel positions.
        const auto numerator = ((4ULL * pixel) * baseExtent + bias) * sourceExtent;
        return std::min(sourceExtent - 1U, static_cast<std::uint32_t>(numerator / (4ULL * viewportExtent * baseExtent)));
    };
    for (std::uint32_t y = 0; y < fixture.height; ++y)
    {
        const auto ay = fixture.window_y + nearestTexel(y, a_mip.height, rex::graphics::testing::rgba8MatrixForm->region_updates ? 1024U : fixture.height, fixture.texture.height);
        const auto by = fixture.window_y + nearestTexel(y, b_mip.height, rex::graphics::testing::rgba8MatrixForm->region_updates ? 1024U : fixture.height, fixture.texture.height);
        for (std::uint32_t x = 0; x < fixture.width; ++x)
        {
            const auto    ax       = fixture.window_x + nearestTexel(x, a_mip.width, rex::graphics::testing::rgba8MatrixForm->region_updates ? 1024U : fixture.width, fixture.texture.width);
            const auto    bx       = fixture.window_x + nearestTexel(x, b_mip.width, rex::graphics::testing::rgba8MatrixForm->region_updates ? 1024U : fixture.width, fixture.texture.width);
            const auto    out      = (static_cast<std::size_t>(y) * rex::graphics::testing::kNativePreparedRgba8MatrixOutputWidth + x) * 4U;
            const auto    a_offset = (static_cast<std::size_t>(ay) * a_mip.width + ax) * 4U;
            const auto    b_offset = (static_cast<std::size_t>(by) * b_mip.width + bx) * 4U;
            std::uint32_t value    = 0;
            for (std::size_t component = 0; component < 4; ++component)
                value += (unsigned(a[a_offset + component]) + b[b_offset + component]) << component;
            for (std::size_t component = 0; component < 3; ++component)
                result[out + component] = static_cast<std::uint8_t>(ClampUnorm8(value));
            result[out + 3U] = 255U;
        }
    }
    return result;
}

inline std::array<std::uint8_t, 4> PatternPixel(std::uint32_t level, std::uint32_t x, std::uint32_t y)
{
    // A hash avoids periodic aliasing: no nonzero shift or transpose of any
    // level leaves every texel unchanged, so row and tile offset bugs show.
    const auto p = [=](std::uint32_t c)
    {
        std::uint32_t h = x * 0x9E3779B1U ^ y * 0x85EBCA77U ^ level * 0xC2B2AE3DU ^ c * 0x27D4EB2FU ^ 11U;
        h ^= h >> 15U;
        h *= 0x2C1B3C6DU;
        h ^= h >> 12U;
        return static_cast<std::uint8_t>(1U + h % 7U);
    };
    if (rex::graphics::testing::Rgba8MatrixR8())
        return { 0, 0, 0, p(3) };
    return { p(0), p(1), p(2), p(3) };
}

inline void WriteMipStorage(std::vector<std::uint8_t>& storage,
                            const rex::graphics::texture_util::TextureGuestLayout&,
                            std::uint32_t                                               level,
                            const rex::graphics::testing::NativePreparedRgba8MatrixMip& mip)
{
    for (std::uint32_t y = 0; y < mip.height; ++y)
        for (std::uint32_t x = 0; x < mip.width; ++x)
        {
            const auto at    = rex::graphics::testing::Rgba8MatrixAddress(level, x, y);
            const auto pixel = (std::size_t(y) * mip.width + x) * 4U;
            if (rex::graphics::testing::Rgba8MatrixR8())
                storage.at(at) = mip.linear_rgba8[pixel + 3];
            else
            {
                // k8in32 reverses RGBA bytes; 0x60A then exchanges red/blue.
                storage.at(at)     = mip.linear_rgba8[pixel + 3];
                storage.at(at + 1) = mip.linear_rgba8[pixel];
                storage.at(at + 2) = mip.linear_rgba8[pixel + 1];
                storage.at(at + 3) = mip.linear_rgba8[pixel + 2];
            }
        }
}

inline std::array<std::uint32_t, 6> MakeTextureFetch(std::uint32_t base_address,
                                                     std::uint32_t mip_address)
{
    auto words = rex::graphics::testing::rgba8MatrixForm->fetch_words;
    words[1]   = (words[1] & 0xFFFU) | base_address;
    words[5]   = (words[5] & 0xFFFU) | (rex::graphics::testing::rgba8MatrixForm->max_level ? mip_address : 0U);
    return words;
}

inline std::array<std::uint32_t, 6> MakeVertexFetch(std::uint32_t address, std::uint32_t bytes)
{
    rex::graphics::xenos::xe_gpu_vertex_fetch_t fetch{};
    fetch.type    = rex::graphics::xenos::FetchConstantType::kVertex;
    fetch.address = address / sizeof(std::uint32_t);
    fetch.endian  = rex::graphics::xenos::Endian::k8in32;
    fetch.size    = bytes / sizeof(std::uint32_t);
    return { fetch.dword_0, fetch.dword_1, 0U, 0U, 0U, 0U };
}

inline void SetRegisters(std::vector<std::uint32_t>&    registers,
                         std::span<const std::uint32_t> texture_fetch,
                         std::span<const std::uint32_t> vertex_fetch)
{
    registers.assign(rex::graphics::testing::kNativePreparedRgba8MatrixRegisterCount, 0U);
    registers[0x2000U]                                      = 0x14000500U;
    registers[0x2002U]                                      = 0x530U;
    registers[0x200FU]                                      = 0x20002000U;
    registers[0x2080U]                                      = 0U;
    registers[0x2081U]                                      = 0U;
    registers[0x2082U]                                      = rex::graphics::testing::Rgba8MatrixViewportWidth() | (rex::graphics::testing::Rgba8MatrixViewportHeight() << 16U);
    registers[0x2100U]                                      = 2U;
    registers[0x2104U]                                      = 0xFU;
    registers[0x210FU]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::Rgba8MatrixViewportWidth()) / 2.0F);
    registers[0x2110U]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::Rgba8MatrixViewportWidth()) / 2.0F);
    registers[0x2111U]                                      = std::bit_cast<std::uint32_t>(-static_cast<float>(rex::graphics::testing::Rgba8MatrixViewportHeight()) / 2.0F);
    registers[0x2112U]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::Rgba8MatrixViewportHeight()) / 2.0F);
    registers[0x2180U]                                      = 0x10010002U;
    registers[0x2181U]                                      = 4U;
    registers[0x2182U]                                      = 1U;
    registers[rex::graphics::XE_GPU_REG_VGT_DMA_SIZE]       = kNativePreparedRgba8MatrixDmaSize;
    registers[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR] = kNativePreparedRgba8MatrixDrawInitiator;
    registers[0x2200U]                                      = 0x24F00730U;
    // Weighted component output replaces RGB without blending;
    // preserve the known opaque destination alpha for presenter parity.
    registers[0x2201U] = 0x01000001U;
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

} // namespace rgba8_matrix_detail

inline NativePreparedRgba8MatrixFixture MakeNativePreparedRgba8MatrixFixture()
{
    using namespace rgba8_matrix_detail;
    NativePreparedRgba8MatrixFixture fixture;
    fixture.vertex_shader_words.assign(kVertexShader.begin(), kVertexShader.end());
    fixture.pixel_shader_words.assign(kPixelShader.begin(), kPixelShader.end());

    const auto layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, rgba8MatrixForm->pitch, kNativePreparedRgba8MatrixWidth, kNativePreparedRgba8MatrixHeight, 1U, rgba8MatrixForm->tiled, Rgba8MatrixFormat(), rgba8MatrixForm->packed, true, kNativePreparedRgba8MatrixMipCount - 1U);
    fixture.texture.format      = static_cast<std::uint32_t>(Rgba8MatrixFormat());
    fixture.texture.endianness  = static_cast<std::uint32_t>(Rgba8MatrixEndian());
    fixture.texture.swizzle     = Rgba8MatrixSwizzle();
    fixture.texture.dimension   = static_cast<std::uint32_t>(xenos::DataDimension::k2DOrStacked);
    fixture.texture.pitch       = rgba8MatrixForm->pitch;
    fixture.texture.tiled       = rgba8MatrixForm->tiled;
    fixture.texture.packed_mips = rgba8MatrixForm->packed;
    fixture.texture.base_guest_bytes.assign(AlignToPage(layout.base.level_data_extent_bytes), 0xCDU);
    fixture.texture.mip_guest_bytes.assign(std::max(kPageSize, AlignToPage(layout.mips_total_extent_bytes)), 0xCDU);
    fixture.texture.mips.resize(kNativePreparedRgba8MatrixMipCount);
    for (std::uint32_t level = 0; level < kNativePreparedRgba8MatrixMipCount; ++level)
    {
        auto& mip        = fixture.texture.mips[level];
        mip.level        = level;
        mip.width        = std::max(1U, kNativePreparedRgba8MatrixWidth >> level);
        mip.height       = std::max(1U, kNativePreparedRgba8MatrixHeight >> level);
        mip.row_pitch    = level == 0U ? layout.base.row_pitch_bytes
                                       : layout.mips[std::min(level, layout.packed_level)].row_pitch_bytes;
        mip.guest_offset = rgba8MatrixForm->offsets[level];
        mip.guest_extent = rgba8MatrixForm->tiled ? rgba8MatrixForm->base_data_extent : (mip.height - 1U) * mip.row_pitch + mip.width * Rgba8MatrixBlockBytes();
        mip.linear_rgba8.resize(static_cast<std::size_t>(mip.width) * mip.height * 4U);
        for (std::uint32_t y = 0; y < mip.height; ++y)
            for (std::uint32_t x = 0; x < mip.width; ++x)
            {
                const auto pixel  = PatternPixel(level, x, y);
                const auto offset = (static_cast<std::size_t>(y) * mip.width + x) * 4U;
                std::memcpy(mip.linear_rgba8.data() + offset, pixel.data(), pixel.size());
            }
        if (level == 0U)
            WriteMipStorage(fixture.texture.base_guest_bytes, layout, level, mip);
        else
            WriteMipStorage(fixture.texture.mip_guest_bytes, layout, level, mip);
    }
    fixture.texture.fetch_words = MakeTextureFetch(0U, 0U);

    // Bias every form by a quarter base texel to avoid texel edges.
    const auto uBias = Rgba8MatrixUvBias(rgba8MatrixForm->width);
    const auto vBias = Rgba8MatrixUvBias(rgba8MatrixForm->height);
    for (auto value : std::array<std::array<float, 8>, 3>{
             { { -1.0F, -1.0F, 0.5F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F },
               { -1.0F, 3.0F, 0.5F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F },
               { 3.0F, -1.0F, 0.5F, 0.0F, 2.0F, 1.0F, 0.0F, 0.0F } } })
    {
        if (rgba8MatrixForm->region_updates)
        {
            value[4] *= 0.5F;
            value[5] *= 0.5F;
        }
        value[4] += uBias;
        value[5] += vBias;
        for (const auto component : value)
        {
            AppendBigEndianFloat(fixture.vertex_bytes, component);
        }
        for (std::size_t padding = value.size(); padding < kNativePreparedRgba8MatrixVertexStride / 4U;
             ++padding)
        {
            AppendBigEndianFloat(fixture.vertex_bytes, 0.0F);
        }
    }
    fixture.index_bytes        = { 0U, 0U, 0U, 1U, 0U, 2U };
    fixture.vertex_fetch_words = MakeVertexFetch(0U, static_cast<std::uint32_t>(fixture.vertex_bytes.size()));
    SetRegisters(fixture.register_image, fixture.texture.fetch_words, fixture.vertex_fetch_words);
    fixture.expected_by_lod_pair.resize(kNativePreparedRgba8MatrixMipCount * kNativePreparedRgba8MatrixMipCount);
    const auto selected_index                    = fixture.selected_lods[0] * kNativePreparedRgba8MatrixMipCount + fixture.selected_lods[1];
    fixture.expected_by_lod_pair[selected_index] = MakeExpectedPixels(fixture, fixture.selected_lods[0], fixture.selected_lods[1]);
    fixture.expected_selected_pixels             = fixture.expected_by_lod_pair[selected_index];
    return fixture;
}

inline std::vector<std::uint8_t> ExpectedNativePreparedRgba8MatrixPixels(const NativePreparedRgba8MatrixFixture& fixture,
                                                                         std::uint32_t                           first_lod,
                                                                         std::uint32_t                           second_lod)
{
    return rgba8_matrix_detail::MakeExpectedPixels(fixture, first_lod, second_lod);
}

} // namespace rex::graphics::testing
