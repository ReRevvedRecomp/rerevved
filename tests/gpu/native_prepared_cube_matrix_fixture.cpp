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

#include "native_prepared_cube_matrix_fixture.h"

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

using namespace cube_matrix_parts;

namespace
{

constexpr std::uint32_t kNativePreparedCubeMatrixDmaSize =
    kNativePreparedCubeMatrixIndexCount | (static_cast<std::uint32_t>(kNativePreparedCubeMatrixIndexEndian) << 30U);

// This is the cataloged position/fetch-95 VS with only its point-of-use
// interpolator mapping widened to carry direction XYZ and absolute LOD in r3.xyzw.
// r0 remains the position/scalar work register; the final export reads r3.
constexpr std::array<std::uint32_t, 45> kVertexShader = {
    0x30052003U, 0x00001200U, 0xC2000000U, 0x00006005U, 0x200B1200U, 0x12000000U, 0x00000000U, 0x100DC400U, 0x22000000U, 0x2DF82000U, 0x00393A88U, 0x0000000EU, 0x05F83000U, 0x40263688U, 0x0000040EU, 0xC8020001U, 0x00A7A700U, 0xAF020100U, 0xC8080001U, 0x00A7A700U, 0xAF020300U, 0xC8020000U, 0x00A7A700U, 0xAF020200U, 0xB8410101U, 0x01A7A741U, 0x8F020008U, 0xC801803EU, 0x00A7A700U, 0xAF010400U, 0xC802803EU, 0x00A7A700U, 0xAF010500U, 0xC804803EU, 0x00A7A700U, 0xAF010600U, 0xC808803EU, 0x00A7A700U, 0xAF010700U, 0xC80F8000U, 0x00000000U, 0xE2030300U, 0x4E4A0007U, 0x95A570DEU, 0x30B52E1DU
};

// Authored with SDK ucode structures; public accessors guard every selected field.
constexpr std::array<std::uint32_t, 24> kPixelShader = { 0x01016001, 0x10071800, 0x28000000, 0x0C000018, 0x00000000, 0x00000000, 0xC80F0001, 0x00A6CD00, 0xD2000000, 0x4C400100, 0x000000C6, 0x20000081, 0xC8030001, 0x0000C66C, 0xCB010100, 0xC4082021, 0x2F000688, 0x0000C000, 0xC80F0003, 0x00000000, 0x8F020100, 0xC8078000, 0x00006CB1, 0x8B030202 };

void WriteCubeBc3(rex::graphics::testing::NativePreparedCubeMatrixFixture& fixture, unsigned face, unsigned level, unsigned x, unsigned y, unsigned phase)
{
    using namespace rex::graphics::testing;
    std::array<std::uint8_t, 16> b{};
    // Color extremes make the endpoint readout independent of 565 rounding.
    const auto pattern = face + level + x * 3 + y * 7 + phase;
    b[0]               = std::uint8_t(16 + face * 13 + level + phase * 5);
    b[1]               = std::uint8_t(240 - face * 11 - level - phase * 7);
    unsigned c0 = 0xFFFFU, c1 = 0U;
    if (cubeMatrixInterpolationMode >= 0)
    {
        b[0] = cubeMatrixInterpolationMode == 0 ? 111 : 13;
        b[1] = cubeMatrixInterpolationMode == 0 ? 7 : 101;
        b[0] += phase;
        c0 = (14U << 11) | (30U << 5) | 6U;
        c1 = (3U << 11) | (9U << 5) | 13U;
    }
    std::uint64_t alpha = 0;
    unsigned      color = 0;
    for (unsigned i = 0; i < 16; ++i)
    {
        // At tiny mips, the six faces and two phases collectively expose all codes.
        const auto a = cubeMatrixInterpolationMode < 0 ? ((i + pattern) & 1) : ((i + face + phase * 3) & 7);
        const auto c = cubeMatrixInterpolationMode < 0 ? (i == 0 ? (pattern & 1) : ((0x2659U ^ (x * 31 + y * 47 + face * 71 + level * 19 + phase * 113)) >> i) & 1) : ((i + face + phase) & 3);
        alpha |= std::uint64_t(a) << (3 * i);
        color |= c << (2 * i);
    }
    for (unsigned i = 0; i < 6; ++i)
        b[i + 2] = std::uint8_t(alpha >> (8 * i));
    b[8]  = std::uint8_t(c0);
    b[9]  = std::uint8_t(c0 >> 8);
    b[10] = std::uint8_t(c1);
    b[11] = std::uint8_t(c1 >> 8);
    for (unsigned i = 0; i < 4; ++i)
        b[i + 12] = std::uint8_t(color >> (8 * i));
    auto&      raw = level ? fixture.texture.mip_guest_bytes : fixture.texture.base_guest_bytes;
    const auto at  = CubeMatrixAddress(*cubeMatrixForm, face, level, x, y);
    for (unsigned i = 0; i < 16; ++i)
        raw.at(at + (i ^ 1U)) = b[i];
}

void SetRegisters(std::vector<std::uint32_t>&    registers,
                  std::span<const std::uint32_t> texture_fetch,
                  std::span<const std::uint32_t> vertex_fetch)
{
    registers.assign(rex::graphics::testing::kNativePreparedCubeMatrixRegisterCount, 0U);
    registers[0x2000U]                                      = 0x14000500U;
    registers[0x2002U]                                      = 0x530U;
    registers[0x200FU]                                      = 0x20002000U;
    registers[0x2080U]                                      = 0U;
    registers[0x2081U]                                      = 0U;
    registers[0x2082U]                                      = rex::graphics::testing::CubeMatrixViewportWidth() | (rex::graphics::testing::CubeMatrixViewportHeight() << 16U);
    registers[0x2100U]                                      = kNativePreparedCubeMatrixMaxVertexIndex;
    registers[0x2104U]                                      = 0xFU;
    registers[0x210FU]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::CubeMatrixViewportWidth()) / 2.0F);
    registers[0x2110U]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::CubeMatrixViewportWidth()) / 2.0F);
    registers[0x2111U]                                      = std::bit_cast<std::uint32_t>(-static_cast<float>(rex::graphics::testing::CubeMatrixViewportHeight()) / 2.0F);
    registers[0x2112U]                                      = std::bit_cast<std::uint32_t>(static_cast<float>(rex::graphics::testing::CubeMatrixViewportHeight()) / 2.0F);
    registers[0x2180U]                                      = 0x10010002U;
    registers[0x2181U]                                      = 4U;
    registers[0x2182U]                                      = 1U;
    registers[rex::graphics::XE_GPU_REG_VGT_DMA_SIZE]       = kNativePreparedCubeMatrixDmaSize;
    registers[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR] = kNativePreparedCubeMatrixDrawInitiator;
    registers[0x2200U]                                      = 0x24F00730U;
    // Channel projection replaces RGB without blending;
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

namespace cube_matrix_parts
{

std::vector<std::uint8_t> MakeExpectedPixels(const rex::graphics::testing::NativePreparedCubeMatrixFixture& fixture, std::uint32_t, std::uint32_t)
{
    return rex::graphics::testing::CubeOraclePixels(fixture);
}

void WriteCubeTexel(rex::graphics::testing::NativePreparedCubeMatrixFixture& fixture, std::uint32_t face, std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t phase)
{
    using namespace rex::graphics::testing;
    if (cubeMatrixForm->format == 20)
    {
        WriteCubeBc3(fixture, face, level, x, y, phase);
        return;
    }
    const auto&   f    = *cubeMatrixForm;
    auto&         raw  = level ? fixture.texture.mip_guest_bytes : fixture.texture.base_guest_bytes;
    const auto    at   = CubeMatrixAddress(f, face, level, x, y);
    std::uint32_t hash = x * 0x9E3779B1U ^ y * 0x85EBCA77U ^ level * 0xC2B2AE3DU;
    hash ^= hash >> 15;
    hash *= 0x2C1B3C6DU;
    hash ^= hash >> 12;
    for (std::uint32_t c = 0; c < 4; ++c)
        if (f.format == 6)
            raw.at(at + 3 - c) = std::uint8_t(1 + (hash + face * 31 + c * 47 + (c < 3 ? phase : 0) * 19) % 254);
        else
        {
            const auto half        = kCubeHalfPalette[(hash + face * 2 + c + phase) % kCubeHalfPalette.size()];
            raw.at(at + c * 2)     = std::uint8_t(half >> 8);
            raw.at(at + c * 2 + 1) = std::uint8_t(half);
        }
}

std::array<std::uint32_t, 6> MakeTextureFetch(std::uint32_t base_address,
                                              std::uint32_t mip_address)
{
    auto words = rex::graphics::testing::cubeMatrixForm->fetch_words;
    words[1]   = (words[1] & 0xFFFU) | base_address;
    words[5]   = (words[5] & 0xFFFU) | mip_address;
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

} // namespace cube_matrix_parts

NativePreparedCubeMatrixFixture MakeNativePreparedCubeMatrixFixture()
{
    NativePreparedCubeMatrixFixture fixture;
    fixture.vertex_shader_words.assign(kVertexShader.begin(), kVertexShader.end());
    fixture.pixel_shader_words.assign(kPixelShader.begin(), kPixelShader.end());
    fixture.texture.format      = cubeMatrixForm->format;
    fixture.texture.endianness  = cubeMatrixForm->endian;
    fixture.texture.swizzle     = cubeMatrixForm->swizzle;
    fixture.texture.dimension   = 3;
    fixture.texture.packed_mips = true;
    fixture.texture.base_guest_bytes.assign(AlignToPage(cubeMatrixForm->base_extent), 0xCD);
    fixture.texture.mip_guest_bytes.assign(AlignToPage(cubeMatrixForm->mip_extent), 0xCD);
    for (std::uint32_t face = 0; face < 6; ++face)
        for (std::uint32_t level = 0; level < cubeMatrixForm->levels; ++level)
            for (std::uint32_t y = 0; y < CubeMatrixLevelUnits(level); ++y)
                for (std::uint32_t x = 0; x < CubeMatrixLevelUnits(level); ++x)
                    WriteCubeTexel(fixture, face, level, x, y, 0);
    fixture.texture.fetch_words = MakeTextureFetch(0, 0);
    for (std::uint32_t channel = 0; channel < 4; ++channel)
        for (std::uint32_t face = 0; face < 6; ++face)
        {
            const auto panel = channel * 6 + face;
            for (const auto local : std::array<std::array<float, 2>, 3>{ { { 0, 0 }, { 0, float(2 * cubeMatrixForm->width) }, { float(2 * cubeMatrixForm->width), 0 } } })
            {
                const auto                 direction = CubeMatrixDirection(face, (local[0] + .25F) / cubeMatrixForm->width, (local[1] + .25F) / cubeMatrixForm->width);
                const std::array<float, 8> vertex{
                    2 * local[0] / cubeMatrixForm->width - 1,
                    1 - 2 * local[1] / cubeMatrixForm->width,
                    .5F,
                    0,
                    direction[0],
                    direction[1],
                    direction[2],
                    0
                };
                for (auto value : vertex)
                    AppendBigEndianFloat(fixture.vertex_bytes, value);
                for (std::size_t padding = 8; padding < fixture.vertex_stride / 4; ++padding)
                    AppendBigEndianFloat(fixture.vertex_bytes, 0);
            }
            for (std::uint32_t index = panel * 3; index < panel * 3 + 3; ++index)
            {
                fixture.index_bytes.push_back(std::uint8_t(index >> 8));
                fixture.index_bytes.push_back(std::uint8_t(index));
            }
        }
    fixture.vertex_fetch_words = MakeVertexFetch(0, std::uint32_t(fixture.vertex_bytes.size()));
    SetRegisters(fixture.register_image, fixture.texture.fetch_words, fixture.vertex_fetch_words);
    fixture.expected_selected_pixels = CubeOraclePixels(fixture);
    return fixture;
}

std::vector<std::uint8_t> ExpectedNativePreparedCubeMatrixPixels(const NativePreparedCubeMatrixFixture& fixture,
                                                                 std::uint32_t                          first_lod,
                                                                 std::uint32_t                          second_lod)
{
    return MakeExpectedPixels(fixture, first_lod, second_lod);
}

} // namespace rex::graphics::testing
