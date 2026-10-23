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

#include "native_prepared_float_matrix_fixture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::testing
{

using namespace float_matrix_parts;

namespace
{

constexpr std::uint32_t kNativePreparedFloatMatrixDmaSize =
    kNativePreparedFloatMatrixIndexCount | (static_cast<std::uint32_t>(kNativePreparedFloatMatrixIndexEndian) << 30U);

// This is the cataloged position/fetch-95 VS with only its point-of-use
// interpolator mapping widened to carry UV, lod0 and lod1 in r3.xyzw.
// r0 remains the position/scalar work register; the final export reads r3.
constexpr std::array<std::uint32_t, 45> kVertexShader = {
    0x30052003U, 0x00001200U, 0xC2000000U, 0x00006005U, 0x200B1200U, 0x12000000U, 0x00000000U, 0x100DC400U, 0x22000000U, 0x2DF82000U, 0x00393A88U, 0x0000000EU, 0x05F83000U, 0x40263688U, 0x0000040EU, 0xC8020001U, 0x00A7A700U, 0xAF020100U, 0xC8080001U, 0x00A7A700U, 0xAF020300U, 0xC8020000U, 0x00A7A700U, 0xAF020200U, 0xB8410101U, 0x01A7A741U, 0x8F020008U, 0xC801803EU, 0x00A7A700U, 0xAF010400U, 0xC802803EU, 0x00A7A700U, 0xAF010500U, 0xC804803EU, 0x00A7A700U, 0xAF010600U, 0xC808803EU, 0x00A7A700U, 0xAF010700U, 0xC80F8000U, 0x00000000U, 0xE2030300U, 0x4E4A0007U, 0x95A570DEU, 0x30B52E1DU
};

// Two register-LOD texture samples, add into r3, weighted dp4 export.
// Dyadic weights expose R and all three constant-one guest selections.
constexpr std::array<std::uint32_t, 21> kPixelShader = {
    0x00000000U, 0x6001C400U, 0x22000A55U, 0x08000018U, 0, 0, 0x10081001U, 0x2F1FF688U, 0x00004000U, 0x0C000018U, 0, 0, 0x10082001U, 0x2F1FF688U, 0x00004000U, 0x000F0003U, 0, 0xC0010200U, 0x000F8000U, 0, 0x8F030000U
};

std::array<std::uint8_t, 4> PatternPixel(std::uint32_t, std::uint32_t x, std::uint32_t y)
{
    std::uint32_t h = x * 0x9E3779B1U ^ y * 0x85EBCA77U ^ 11U;
    h ^= h >> 15U;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12U;
    const auto value = float(2 * int(h % 8U) - 7) / 8.0F;
    const auto bits  = std::bit_cast<std::uint32_t>(value);
    return { std::uint8_t(bits), std::uint8_t(bits >> 8U), std::uint8_t(bits >> 16U), std::uint8_t(bits >> 24U) };
}

void SetRegisters(std::vector<std::uint32_t>&    registers,
                  std::span<const std::uint32_t> texture_fetch,
                  std::span<const std::uint32_t> vertex_fetch)
{
    registers.assign(rex::graphics::testing::kNativePreparedFloatMatrixRegisterCount, 0U);
    registers[0x2000U]                                      = 0x14000500U;
    registers[0x2002U]                                      = 0x530U;
    registers[0x200FU]                                      = 0x20002000U;
    registers[0x2080U]                                      = 0U;
    registers[0x2081U]                                      = 0U;
    registers[0x2082U]                                      = rex::graphics::testing::FloatMatrixViewportWidth() | (rex::graphics::testing::FloatMatrixViewportHeight() << 16U);
    registers[rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX]   = 2U;
    registers[0x2100U]                                      = 2U;
    registers[0x2104U]                                      = 0xFU;
    registers[0x210FU]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::FloatMatrixViewportWidth()) / 2.0F);
    registers[0x2110U]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::FloatMatrixViewportWidth()) / 2.0F);
    registers[0x2111U]                                      = std::bit_cast<std::uint32_t>(-static_cast<float>(rex::graphics::testing::FloatMatrixViewportHeight()) / 2.0F);
    registers[0x2112U]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::FloatMatrixViewportHeight()) / 2.0F);
    registers[0x2180U]                                      = 0x10010002U;
    registers[0x2181U]                                      = 4U;
    registers[0x2182U]                                      = 1U;
    registers[rex::graphics::XE_GPU_REG_VGT_DMA_SIZE]       = kNativePreparedFloatMatrixDmaSize;
    registers[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR] = kNativePreparedFloatMatrixDrawInitiator;
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

} // namespace

namespace float_matrix_parts
{

std::vector<std::uint8_t> MakeExpectedPixels(const rex::graphics::testing::NativePreparedFloatMatrixFixture& fixture,
                                             std::uint32_t                                                   first_lod,
                                             std::uint32_t                                                   second_lod)
{
    if (first_lod || second_lod || fixture.texture.mips.size() != 1U ||
        fixture.texture.mips[0].linear_r32.size() != 1048576U)
        return {};
    std::vector<std::uint8_t> result(1280U * 720U * 4U, 0U);
    for (std::size_t at = 3; at < result.size(); at += 4)
        result[at] = 255U;
    for (std::uint32_t y = 0; y < 512U; ++y)
        for (std::uint32_t x = 0; x < 512U; ++x)
        {
            float value;
            std::memcpy(&value, fixture.texture.mips[0].linear_r32.data() + (std::size_t(y) * 512U + x) * 4U, 4U);
            // R is an odd numerator / 8. Two samples and weights
            // (1/16,1/128,7/128,1/16) give (numerator + 16) / 64.
            const auto numerator = static_cast<int>(value * 8.0F);
            const auto byte      = static_cast<std::uint8_t>(((numerator + 16) * 255 + 32) / 64);
            const auto out       = (std::size_t(y) * 1280U + x) * 4U;
            std::fill_n(result.begin() + out, 3U, byte);
        }
    return result;
}

void WriteMipStorage(std::vector<std::uint8_t>& storage,
                     const rex::graphics::texture_util::TextureGuestLayout&,
                     std::uint32_t                                               level,
                     const rex::graphics::testing::NativePreparedFloatMatrixMip& mip)
{
    for (std::uint32_t y = 0; y < mip.height; ++y)
        for (std::uint32_t x = 0; x < mip.width; ++x)
        {
            const auto at    = rex::graphics::testing::FloatMatrixAddress(level, x, y);
            const auto pixel = (std::size_t(y) * mip.width + x) * 4U;
            // k8in32 storage is big-endian IEEE binary32.
            for (unsigned c = 0; c < 4; ++c)
                storage.at(at + c) = mip.linear_r32[pixel + 3U - c];
        }
}

std::array<std::uint32_t, 6> MakeTextureFetch(std::uint32_t base_address,
                                              std::uint32_t mip_address)
{
    auto words = rex::graphics::testing::floatMatrixForm->fetch_words;
    words[1]   = (words[1] & 0xFFFU) | base_address;
    words[5]   = (words[5] & 0xFFFU) | (rex::graphics::testing::floatMatrixForm->max_level ? mip_address : 0U);
    return words;
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

} // namespace float_matrix_parts

NativePreparedFloatMatrixFixture MakeNativePreparedFloatMatrixFixture()
{
    NativePreparedFloatMatrixFixture fixture;
    fixture.vertex_shader_words.assign(kVertexShader.begin(), kVertexShader.end());
    fixture.pixel_shader_words.assign(kPixelShader.begin(), kPixelShader.end());

    const auto layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, floatMatrixForm->pitch, kNativePreparedFloatMatrixWidth, kNativePreparedFloatMatrixHeight, 1U, floatMatrixForm->tiled, FloatMatrixFormat(), floatMatrixForm->packed, true, kNativePreparedFloatMatrixMipCount - 1U);
    fixture.texture.format      = static_cast<std::uint32_t>(FloatMatrixFormat());
    fixture.texture.endianness  = static_cast<std::uint32_t>(FloatMatrixEndian());
    fixture.texture.swizzle     = FloatMatrixSwizzle();
    fixture.texture.dimension   = static_cast<std::uint32_t>(xenos::DataDimension::k2DOrStacked);
    fixture.texture.pitch       = floatMatrixForm->pitch;
    fixture.texture.tiled       = floatMatrixForm->tiled;
    fixture.texture.packed_mips = floatMatrixForm->packed;
    fixture.texture.base_guest_bytes.assign(AlignToPage(layout.base.level_data_extent_bytes), 0xCDU);
    fixture.texture.mip_guest_bytes.assign(std::max(kPageSize, AlignToPage(layout.mips_total_extent_bytes)), 0xCDU);
    fixture.texture.mips.resize(kNativePreparedFloatMatrixMipCount);
    for (std::uint32_t level = 0; level < kNativePreparedFloatMatrixMipCount; ++level)
    {
        auto& mip        = fixture.texture.mips[level];
        mip.level        = level;
        mip.width        = std::max(1U, kNativePreparedFloatMatrixWidth >> level);
        mip.height       = std::max(1U, kNativePreparedFloatMatrixHeight >> level);
        mip.row_pitch    = level == 0U ? layout.base.row_pitch_bytes
                                       : layout.mips[std::min(level, layout.packed_level)].row_pitch_bytes;
        mip.guest_offset = floatMatrixForm->offsets[level];
        mip.guest_extent = floatMatrixForm->tiled ? floatMatrixForm->base_data_extent : (mip.height - 1U) * mip.row_pitch + mip.width * FloatMatrixBlockBytes();
        mip.linear_r32.resize(static_cast<std::size_t>(mip.width) * mip.height * 4U);
        for (std::uint32_t y = 0; y < mip.height; ++y)
            for (std::uint32_t x = 0; x < mip.width; ++x)
            {
                const auto pixel  = PatternPixel(level, x, y);
                const auto offset = (static_cast<std::size_t>(y) * mip.width + x) * 4U;
                std::memcpy(mip.linear_r32.data() + offset, pixel.data(), pixel.size());
            }
        if (level == 0U)
            WriteMipStorage(fixture.texture.base_guest_bytes, layout, level, mip);
        else
            WriteMipStorage(fixture.texture.mip_guest_bytes, layout, level, mip);
    }
    fixture.texture.fetch_words = MakeTextureFetch(0U, 0U);

    // Bias every form by a quarter base texel to avoid texel edges.
    const auto uBias = FloatMatrixUvBias(floatMatrixForm->width);
    const auto vBias = FloatMatrixUvBias(floatMatrixForm->height);
    for (auto value : std::array<std::array<float, 8>, 3>{
             { { -1.0F, -1.0F, 0.5F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F },
               { -1.0F, 3.0F, 0.5F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F },
               { 3.0F, -1.0F, 0.5F, 0.0F, 2.0F, 1.0F, 0.0F, 0.0F } } })
    {
        value[4] += uBias;
        value[5] += vBias;
        for (const auto component : value)
        {
            AppendBigEndianFloat(fixture.vertex_bytes, component);
        }
        for (std::size_t padding = value.size(); padding < kNativePreparedFloatMatrixVertexStride / 4U;
             ++padding)
        {
            AppendBigEndianFloat(fixture.vertex_bytes, 0.0F);
        }
    }
    fixture.index_bytes        = { 0U, 0U, 0U, 1U, 0U, 2U };
    fixture.vertex_fetch_words = MakeVertexFetch(0U, static_cast<std::uint32_t>(fixture.vertex_bytes.size()));
    SetRegisters(fixture.register_image, fixture.texture.fetch_words, fixture.vertex_fetch_words);
    fixture.expected_by_lod_pair.resize(kNativePreparedFloatMatrixMipCount * kNativePreparedFloatMatrixMipCount);
    const auto selected_index                    = fixture.selected_lods[0] * kNativePreparedFloatMatrixMipCount + fixture.selected_lods[1];
    fixture.expected_by_lod_pair[selected_index] = MakeExpectedPixels(fixture, fixture.selected_lods[0], fixture.selected_lods[1]);
    fixture.expected_selected_pixels             = fixture.expected_by_lod_pair[selected_index];
    return fixture;
}

std::vector<std::uint8_t> ExpectedNativePreparedFloatMatrixPixels(const NativePreparedFloatMatrixFixture& fixture,
                                                                  std::uint32_t                           first_lod,
                                                                  std::uint32_t                           second_lod)
{
    return MakeExpectedPixels(fixture, first_lod, second_lod);
}

} // namespace rex::graphics::testing
