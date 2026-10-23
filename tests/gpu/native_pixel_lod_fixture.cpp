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

#include "native_pixel_lod_fixture.h"

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

using namespace pixel_lod_parts;

namespace
{

constexpr std::uint32_t kNativePixelLodDmaSize =
    kNativePixelLodIndexCount | (static_cast<std::uint32_t>(kNativePixelLodIndexEndian) << 30U);

// This is the cataloged position/fetch-95 VS with only its point-of-use
// interpolator mapping widened to carry UV, lod0 and lod1 in r0.xyzw.
constexpr std::array<std::uint32_t, 45> kVertexShader = {
    0x30052003U, 0x00001200U, 0xC2000000U, 0x00006005U, 0x200B1200U, 0x12000000U, 0x00000000U, 0x100DC400U, 0x22000000U, 0x2DF82000U, 0x00393A88U, 0x0000000EU, 0x05F80000U, 0x40263688U, 0x0000040EU, 0xC8020001U, 0x00A7A700U, 0xAF020100U, 0xC8080001U, 0x00A7A700U, 0xAF020300U, 0xC8020000U, 0x00A7A700U, 0xAF020200U, 0xB8410101U, 0x01A7A741U, 0x8F020008U, 0xC801803EU, 0x00A7A700U, 0xAF010400U, 0xC802803EU, 0x00A7A700U, 0xAF010500U, 0xC804803EU, 0x00A7A700U, 0xAF010600U, 0xC808803EU, 0x00A7A700U, 0xAF010700U, 0xC80F8000U, 0x00000000U, 0xE2000000U, 0x4E4A0007U, 0x95A570DEU, 0x30B52E1DU
};

constexpr std::array<std::uint32_t, 3> MakePixelAddExport(std::uint32_t source0,
                                                          std::uint32_t source1)
{
    // AluInstruction fields: export_data=1, vector_write_mask=0xF, vector
    // opcode=kAdd, and temporary source selectors set for r[source0/r1].
    return { 0x000F8000U, 0U, 0xC0000000U | ((source0 & 0xFFU) << 16U) | ((source1 & 0xFFU) << 8U) };
}

constexpr auto kPixelAddExport = MakePixelAddExport(1U, 2U);

// CF alloc + five instructions: setTexLOD(r0.z), tfetch2D(r1),
// setTexLOD(r0.w), tfetch2D(r2), add(r1,r2)->oC0. The two fetches use
// register LOD (bit 29) with computed LOD (bit 28) and gradients disabled.
constexpr std::array<std::uint32_t, 18> kPixelShader = {
    0x00000000U, 0x5001C400U, 0x22000255U, 0x08000018U, 0x00000000U, 0x00000000U, 0x10081001U, 0x2F1FF688U, 0x00004000U, 0x0C000018U, 0x00000000U, 0x00000000U, 0x10082001U, 0x2F1FF688U, 0x00004000U, kPixelAddExport[0], kPixelAddExport[1], kPixelAddExport[2]
};

std::array<std::uint8_t, 4> MipColor(std::uint32_t level)
{
    return { static_cast<std::uint8_t>(8U + level * 7U),
             static_cast<std::uint8_t>(16U + level * 5U),
             static_cast<std::uint8_t>(32U + level * 3U),
             // Keep the baseline opaque while making XOR mutations alpha-sensitive.
             static_cast<std::uint8_t>(128U + level * 2U) };
}

void SetRegisters(std::vector<std::uint32_t>&    registers,
                  std::span<const std::uint32_t> texture_fetch,
                  std::span<const std::uint32_t> vertex_fetch)
{
    registers.assign(rex::graphics::testing::kNativePixelLodRegisterCount, 0U);
    registers[0x2000U]                                      = 0x02000080U;
    registers[0x2002U]                                      = 0x530U;
    registers[0x200FU]                                      = 0x20002000U;
    registers[0x2080U]                                      = 0U;
    registers[0x2081U]                                      = 0U;
    registers[0x2082U]                                      = rex::graphics::testing::kNativePixelLodWidth |
                                                              (rex::graphics::testing::kNativePixelLodHeight << 16U);
    registers[0x2100U]                                      = 2U;
    registers[0x2104U]                                      = 0xFU;
    registers[0x210FU]                                      = std::bit_cast<std::uint32_t>(64.0F);
    registers[0x2110U]                                      = std::bit_cast<std::uint32_t>(64.0F);
    registers[0x2111U]                                      = std::bit_cast<std::uint32_t>(-64.0F);
    registers[0x2112U]                                      = std::bit_cast<std::uint32_t>(64.0F);
    registers[0x2180U]                                      = 0x10010002U;
    registers[0x2181U]                                      = 4U;
    registers[0x2182U]                                      = 1U;
    registers[rex::graphics::XE_GPU_REG_VGT_DMA_SIZE]       = kNativePixelLodDmaSize;
    registers[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR] = kNativePixelLodDrawInitiator;
    registers[0x2200U]                                      = 0x24F00730U;
    registers[0x2201U]                                      = 0x00010001U;
    registers[0x2202U]                                      = 0x87000000U;
    registers[0x2204U]                                      = 0x80000U;
    registers[0x2205U]                                      = 0x18000U;
    registers[0x2206U]                                      = 0x43FU;
    registers[0x2208U]                                      = 4U;
    registers[0x2302U]                                      = 4U;
    registers[0x2307U]                                      = 0xFF000U;
    registers[0x2308U]                                      = 0xFF100U;
    std::copy(kVertexConstants.begin(), kVertexConstants.end(), registers.begin() + rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X);
    std::copy(kPixelConstant0.begin(), kPixelConstant0.end(), registers.begin() + rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X);
    std::copy(kPixelConstant255.begin(), kPixelConstant255.end(), registers.begin() + rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X + 255U * 4U);
    std::copy(texture_fetch.begin(), texture_fetch.end(), registers.begin() + rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    std::copy(vertex_fetch.begin(), vertex_fetch.end(), registers.begin() + rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U);
}

} // namespace

namespace pixel_lod_parts
{

std::vector<std::uint8_t> MakeExpectedPixels(const rex::graphics::testing::NativePixelLodFixture& fixture,
                                             std::uint32_t                                        first_lod,
                                             std::uint32_t                                        second_lod)
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
    std::vector<std::uint8_t> result(static_cast<std::size_t>(fixture.width) * fixture.height * 4U);
    for (std::uint32_t y = 0; y < fixture.height; ++y)
    {
        const auto ay = static_cast<std::uint32_t>(((2ULL * fixture.texture.height + 1U) * a_mip.height) / (4ULL * fixture.texture.height));
        const auto by = static_cast<std::uint32_t>(((2ULL * fixture.texture.height + 1U) * b_mip.height) / (4ULL * fixture.texture.height));
        for (std::uint32_t x = 0; x < fixture.width; ++x)
        {
            const auto ax       = static_cast<std::uint32_t>(((2ULL * fixture.texture.width + 1U) * a_mip.width) / (4ULL * fixture.texture.width));
            const auto bx       = static_cast<std::uint32_t>(((2ULL * fixture.texture.width + 1U) * b_mip.width) / (4ULL * fixture.texture.width));
            const auto out      = (static_cast<std::size_t>(y) * fixture.width + x) * 4U;
            const auto a_offset = (static_cast<std::size_t>(ay) * a_mip.width + ax) * 4U;
            const auto b_offset = (static_cast<std::size_t>(by) * b_mip.width + bx) * 4U;
            for (std::size_t component = 0; component < 4U; ++component)
            {
                result[out + component] = static_cast<std::uint8_t>(
                    std::min(255U, static_cast<unsigned>(a[a_offset + component]) + b[b_offset + component]));
            }
        }
    }
    return result;
}

void WriteLinearMip(std::vector<std::uint8_t>& storage, std::uint32_t offset, const rex::graphics::testing::NativePixelLodMip& mip)
{
    for (std::uint32_t y = 0; y < mip.height; ++y)
    {
        for (std::uint32_t x = 0; x < mip.width; ++x)
        {
            const auto dst = static_cast<std::size_t>(offset) +
                             static_cast<std::size_t>(y) * mip.row_pitch + x * 4U;
            const auto src = (static_cast<std::size_t>(y) * mip.width + x) * 4U;
            if (dst + 4U <= storage.size() && src + 4U <= mip.linear_rgba8.size())
            {
                std::memcpy(storage.data() + dst, mip.linear_rgba8.data() + src, 4U);
            }
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
    fetch.pitch          = rex::graphics::testing::kNativePixelLodWidth / 32U;
    fetch.format         = rex::graphics::xenos::TextureFormat::k_8_8_8_8;
    fetch.endianness     = rex::graphics::xenos::Endian::kNone;
    fetch.base_address   = base_address >> 12U;
    fetch.mip_address    = mip_address >> 12U;
    fetch.size_2d.width  = rex::graphics::testing::kNativePixelLodWidth - 1U;
    fetch.size_2d.height = rex::graphics::testing::kNativePixelLodHeight - 1U;
    fetch.tiled          = false;
    fetch.packed_mips    = false;
    fetch.mip_min_level  = 0U;
    fetch.mip_max_level  = rex::graphics::testing::kNativePixelLodMipCount - 1U;
    fetch.swizzle        = rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    fetch.mag_filter     = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.min_filter     = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.mip_filter     = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.clamp_x        = rex::graphics::xenos::ClampMode::kClampToEdge;
    fetch.clamp_y        = rex::graphics::xenos::ClampMode::kClampToEdge;
    fetch.clamp_z        = rex::graphics::xenos::ClampMode::kClampToEdge;
    fetch.dimension      = rex::graphics::xenos::DataDimension::k2DOrStacked;
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

} // namespace pixel_lod_parts

NativePixelLodFixture MakeNativePixelLodFixture()
{
    NativePixelLodFixture fixture;
    fixture.vertex_shader_words.assign(kVertexShader.begin(), kVertexShader.end());
    fixture.pixel_shader_words.assign(kPixelShader.begin(), kPixelShader.end());

    const auto layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, kNativePixelLodWidth / 32U, kNativePixelLodWidth, kNativePixelLodHeight, 1U, false, xenos::TextureFormat::k_8_8_8_8, false, true, kNativePixelLodMipCount - 1U);
    fixture.texture.format    = static_cast<std::uint32_t>(xenos::TextureFormat::k_8_8_8_8);
    fixture.texture.dimension = static_cast<std::uint32_t>(xenos::DataDimension::k2DOrStacked);
    fixture.texture.base_guest_bytes.assign(AlignToPage(layout.base.level_data_extent_bytes), 0U);
    fixture.texture.mip_guest_bytes.assign(AlignToPage(layout.mips_total_extent_bytes), 0U);
    fixture.texture.mips.resize(kNativePixelLodMipCount);
    for (std::uint32_t level = 0; level < kNativePixelLodMipCount; ++level)
    {
        auto& mip        = fixture.texture.mips[level];
        mip.level        = level;
        mip.width        = std::max(1U, fixture.width >> level);
        mip.height       = std::max(1U, fixture.height >> level);
        mip.row_pitch    = level == 0U ? layout.base.row_pitch_bytes : layout.mips[level].row_pitch_bytes;
        mip.guest_offset = level == 0U ? 0U : layout.mip_offsets_bytes[level];
        mip.guest_extent = level == 0U ? layout.base.level_data_extent_bytes
                                       : layout.mips[level].level_data_extent_bytes;
        mip.linear_rgba8.resize(static_cast<std::size_t>(mip.width) * mip.height * 4U);
        const auto color = MipColor(level);
        for (std::size_t offset = 0; offset < mip.linear_rgba8.size(); offset += 4U)
        {
            std::memcpy(mip.linear_rgba8.data() + offset, color.data(), color.size());
        }
        if (level == 0U)
        {
            WriteLinearMip(fixture.texture.base_guest_bytes, 0U, mip);
        }
        else
        {
            WriteLinearMip(fixture.texture.mip_guest_bytes, mip.guest_offset, mip);
        }
    }
    fixture.texture.fetch_words = MakeTextureFetch(0U, 0U);

    const auto uBias = 0.25F / static_cast<float>(fixture.texture.width);
    const auto vBias = 0.25F / static_cast<float>(fixture.texture.height);
    for (const auto value : std::array<std::array<float, 8>, 3>{
             { { -1.0F, -1.0F, 0.5F, 0.0F, 0.5F + uBias, 0.5F + vBias, 2.0F, 5.0F },
               { -1.0F, 3.0F, 0.5F, 0.0F, 0.5F + uBias, 0.5F + vBias, 2.0F, 5.0F },
               { 3.0F, -1.0F, 0.5F, 0.0F, 0.5F + uBias, 0.5F + vBias, 2.0F, 5.0F } } })
    {
        for (const auto component : value)
        {
            AppendBigEndianFloat(fixture.vertex_bytes, component);
        }
        for (std::size_t padding = value.size(); padding < kNativePixelLodVertexStride / 4U;
             ++padding)
        {
            AppendBigEndianFloat(fixture.vertex_bytes, 0.0F);
        }
    }
    fixture.index_bytes        = { 0U, 0U, 0U, 1U, 0U, 2U };
    fixture.vertex_fetch_words = MakeVertexFetch(0U, static_cast<std::uint32_t>(fixture.vertex_bytes.size()));
    SetRegisters(fixture.register_image, fixture.texture.fetch_words, fixture.vertex_fetch_words);
    fixture.expected_by_lod_pair.resize(kNativePixelLodMipCount * kNativePixelLodMipCount);
    for (std::uint32_t first = 0; first < kNativePixelLodMipCount; ++first)
    {
        for (std::uint32_t second = 0; second < kNativePixelLodMipCount; ++second)
        {
            fixture.expected_by_lod_pair[first * kNativePixelLodMipCount + second] =
                MakeExpectedPixels(fixture, first, second);
        }
    }
    fixture.expected_selected_pixels =
        fixture.expected_by_lod_pair[fixture.selected_lods[0] * kNativePixelLodMipCount +
                                     fixture.selected_lods[1]];
    return fixture;
}

std::vector<std::uint8_t> ExpectedNativePixelLodPixels(const NativePixelLodFixture& fixture,
                                                       std::uint32_t                first_lod,
                                                       std::uint32_t                second_lod)
{
    return MakeExpectedPixels(fixture, first_lod, second_lod);
}

} // namespace rex::graphics::testing
