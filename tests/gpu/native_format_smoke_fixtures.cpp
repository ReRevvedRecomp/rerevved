#include "native_format_smoke_fixtures.h"

#include "gpu/d3d12/native_draw_replay.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "native_pixel_lod_fixture.h"
#include "native_prepared_bc1_fixture.h"
#include "native_prepared_cube_matrix_fixture.h"
#include "native_prepared_float_matrix_fixture.h"
#include "native_prepared_rgba8_matrix_fixture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace rerevved::tests::format_smoke
{
namespace
{

using namespace rerevved::gpu;
namespace testing = rex::graphics::testing;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

float bigEndianFloat(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    const auto word = (std::uint32_t(bytes[offset]) << 24U) | (std::uint32_t(bytes[offset + 1U]) << 16U) |
                      (std::uint32_t(bytes[offset + 2U]) << 8U) | bytes[offset + 3U];
    return std::bit_cast<float>(word);
}

// The fixture's fetch words carry page zero; the upload needs live pages.
NativeTextureFetch liveFetch(const std::array<std::uint32_t, 6>& words, bool hasMips)
{
    auto live = words;
    live[1] |= 0x10000U;
    if (hasMips)
        live[5] |= 0x1000000U;
    return live;
}

// Runs the production full/versioned upload over the fixture's guest bytes,
// limited to the canonical ranges, and checks the ranges it reports.
NativeDrawReplayTexture uploadFixture(const NativeTextureFetch&     fetch,
                                      std::span<const std::uint8_t> baseBytes,
                                      std::span<const std::uint8_t> mipBytes,
                                      std::uint32_t                 baseExtent,
                                      std::uint32_t                 mipExtent)
{
    NativeTextureMemoryRanges ranges;
    std::string               error;
    require(GetNativeTextureMemoryRanges(fetch, ranges, error), "upload range: " + error);
    require(ranges.base.size == baseExtent && ranges.mips.size == mipExtent,
            "upload ranges differ from canonical extents");
    NativeDrawReplayTexture uploaded;
    require(DecodeNativeTextureVersioned(fetch, baseBytes.first(ranges.base.size), mipBytes.first(ranges.mips.size), uploaded, error),
            "upload decode: " + error);
    return uploaded;
}

NativeDrawReplayTexture packLevels(const std::vector<std::vector<std::uint8_t>>& levels,
                                   NativeDrawReplayTextureLayout                 layout)
{
    std::vector<std::span<const std::uint8_t>> spans;
    for (const auto& level : levels)
        spans.emplace_back(level);
    NativeDrawReplayTexture texture;
    std::string             error;
    require(PackNativeDrawReplayTextureVersion(layout, spans, texture, error), "independent packing: " + error);
    return texture;
}

// Every texel of the fixture's mips lies strictly inside a texel for the
// fixed UV, including the SDK's 1.5/1024 base-texel half-pixel margin.
void requireInteriorUv(std::uint32_t baseWidth, std::uint32_t baseHeight, std::uint32_t levelWidth, std::uint32_t levelHeight)
{
    for (const auto axis : std::array<std::array<std::uint32_t, 2>, 2>{ { { baseWidth, levelWidth }, { baseHeight, levelHeight } } })
    {
        const double margin   = (1.5 / 1024.0) * axis[1] / axis[0];
        const double original = (0.5 + 0.25 / axis[0]) * axis[1];
        const auto   texel    = static_cast<std::uint32_t>(original);
        require(original > texel + margin && original < texel + 1.0 - margin && original < axis[1],
                "fixed UV hits a texel edge or leaves the level");
    }
}

void requireOpaqueCanvas(const std::vector<std::uint8_t>& expected, std::size_t plane)
{
    require(expected.size() == plane && expected[3U] == 255U && expected[plane - 1U] == 255U,
            "fixture expected canvas does not retain presenter-normalized alpha 255");
}

// Checks the weighted add/dp4 pixel shader and fetch-95 vertex shader that the
// RGBA8 and float matrices share, then the register image around it.
void requireMatrixShaders(const std::vector<std::uint32_t>& vertexWords, const std::vector<std::uint32_t>& pixelWords)
{
    rex::graphics::ucode::VertexFetchInstruction uvFetch{};
    rex::graphics::ucode::AluInstruction         interpolatorExport{};
    require(vertexWords.size() == 45U, "fixture vertex shader word extent changed");
    std::memcpy(&uvFetch, vertexWords.data() + 12U, sizeof(uvFetch));
    std::memcpy(&interpolatorExport, vertexWords.data() + 39U, sizeof(interpolatorExport));
    require(uvFetch.dest() == 3U && uvFetch.fetch_constant_index() == 95U && uvFetch.dest_swizzle() == 0x688U,
            "fixture UV fetch did not retain r3 and its exact fetch fields");
    require(interpolatorExport.is_export() && interpolatorExport.src_is_temp(1U) &&
                interpolatorExport.src_is_temp(2U) && interpolatorExport.src_reg(1U) == 3U &&
                interpolatorExport.src_reg(2U) == 3U && vertexWords[41U] == 0xE2030300U,
            "fixture interpolator export did not retain r3/r3 and its exact raw field");
    rex::graphics::ucode::AluInstruction pixelAdd{}, pixelDot{};
    require(pixelWords.size() == 21U && pixelWords[1] == 0x6001C400U && pixelWords[2] == 0x22000A55U,
            "weighted shader lost its six-instruction serialized execution");
    std::memcpy(&pixelAdd, pixelWords.data() + 15, sizeof(pixelAdd));
    std::memcpy(&pixelDot, pixelWords.data() + 18, sizeof(pixelDot));
    require(!pixelAdd.is_export() && pixelAdd.vector_dest() == 3U && pixelAdd.vector_opcode() == rex::graphics::ucode::AluVectorOpcode::kAdd &&
                pixelAdd.src_is_temp(1) && pixelAdd.src_is_temp(2) && pixelAdd.src_reg(1) == 1U && pixelAdd.src_reg(2) == 2U &&
                pixelDot.is_export() && pixelDot.vector_opcode() == rex::graphics::ucode::AluVectorOpcode::kDp4 &&
                pixelDot.src_is_temp(1) && !pixelDot.src_is_temp(2) && pixelDot.src_reg(1) == 3U && pixelDot.src_reg(2) == 0U &&
                pixelDot.src_swizzle(1) == 0 && pixelDot.src_swizzle(2) == 0,
            "weighted shader no longer adds both samples and exports dp4 with c0");
}

void requireMatrixRegisters(const std::vector<std::uint32_t>& registers, std::size_t registerCount, std::uint32_t vpW, std::uint32_t vpH)
{
    require(registers.size() == registerCount, "fixture register image extent changed");
    require(registers[0x2000U] == 0x14000500U && registers[0x2082U] == (vpW | (vpH << 16U)) &&
                registers[0x210FU] == std::bit_cast<std::uint32_t>(static_cast<float>(vpW) / 2.0F) &&
                registers[0x2110U] == std::bit_cast<std::uint32_t>(static_cast<float>(vpW) / 2.0F) &&
                registers[0x2111U] == std::bit_cast<std::uint32_t>(-static_cast<float>(vpH) / 2.0F) &&
                registers[0x2112U] == std::bit_cast<std::uint32_t>(static_cast<float>(vpH) / 2.0F) &&
                registers[0x2201U] == 0x01000001U,
            "fixture canvas, viewport, or alpha-visible blend changed");
    require(registers[rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX] >= 2U, "vertex index clamp excludes fixture vertices");
}

template <typename Fixture, typename Form>
void requireMatrixGeometry(const Fixture& fixture, const Form& form, float uBias, float vBias, float uvScale)
{
    require(fixture.vertex_bytes.size() == 3U * 56U && fixture.index_bytes.size() == 6U,
            "fixture geometry extent changed");
    const auto expectedUv = std::array<std::array<float, 2>, 3>{ { { uBias, uvScale + vBias }, { uBias, -uvScale + vBias }, { 2.0F * uvScale + uBias, uvScale + vBias } } };
    for (std::size_t vertex = 0; vertex < expectedUv.size(); ++vertex)
    {
        const auto base = vertex * fixture.vertex_stride;
        require(bigEndianFloat(fixture.vertex_bytes, base + 16U) == expectedUv[vertex][0] &&
                    bigEndianFloat(fixture.vertex_bytes, base + 20U) == expectedUv[vertex][1] &&
                    bigEndianFloat(fixture.vertex_bytes, base + 24U) == static_cast<float>(fixture.selected_lods[0]) &&
                    bigEndianFloat(fixture.vertex_bytes, base + 28U) == static_cast<float>(fixture.selected_lods[1]),
                "fixture UV triangle lost its pixel-center coverage or interior bias");
    }
    (void)form;
}

// Layout, per-level location and tiled-address checks shared by the RGBA8 and
// float matrices; the SDK helpers are compared with the frozen form table.
template <typename Fixture, typename Form, typename Address>
void requireMatrixLayout(const Fixture& fixture, const Form& form, rex::graphics::xenos::TextureFormat format, std::uint32_t blockBytes, std::uint32_t mipCount, Address address)
{
    auto expectedFetch = form.fetch_words;
    expectedFetch[1] &= 0xFFFU;
    expectedFetch[5] &= 0xFFFU;
    require(fixture.texture.fetch_words == expectedFetch, "fixture changed exact descriptor words outside relocated pages");
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    std::memcpy(&fetch, fixture.texture.fetch_words.data(), sizeof(fetch));
    require(fetch.mip_max_level == form.declared_max && fetch.mip_min_level == 0, "declared fetch LOD range changed");
    const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(fetch.dimension, form.pitch, form.width, form.height, 1, form.tiled, format, form.packed, true, form.max_level);
    require(layout.base.level_data_extent_bytes == form.base_data_extent && layout.mips_total_extent_bytes == form.mip_extent && layout.packed_level == form.packed_level && layout.base.row_pitch_bytes == form.rows[0] && layout.base.x_extent_blocks == form.addressed_x && layout.base.y_extent_blocks == form.addressed_y && layout.base.array_slice_stride_bytes == form.base_stride,
            "SDK base layout differs from frozen extents");
    require(!form.max_level || (layout.mip_offsets_bytes[layout.packed_level] == form.tail_offset && layout.mips[layout.packed_level].level_data_extent_bytes == form.tail_extent), "SDK tail differs from frozen extents");
    for (std::uint32_t level = 0; level < mipCount; ++level)
    {
        const auto&   mip    = fixture.texture.mips[level];
        const auto&   stored = level ? layout.mips[std::min(level, layout.packed_level)] : layout.base;
        std::uint32_t px = 0, py = 0, pz = 0;
        if (form.packed && level >= layout.packed_level)
            require(rex::graphics::texture_util::GetPackedMipOffset(form.width, form.height, 1, format, level, px, py, pz), "SDK packed offset absent");
        const auto offset = (level ? layout.mip_offsets_bytes[std::min(level, layout.packed_level)] : 0U) + py * stored.row_pitch_bytes + px * blockBytes;
        require(offset == form.offsets[level] && stored.row_pitch_bytes == form.rows[level] && mip.guest_offset == offset && mip.width == form.widths[level] && mip.height == form.heights[level], "SDK level differs from frozen location");
        if (form.tiled)
            for (std::uint32_t y = 0; y < mip.height; ++y)
                for (std::uint32_t x = 0; x < mip.width; ++x)
                    require(address(level, x, y) == std::size_t(rex::graphics::texture_util::GetTiledOffset2D(x, y, form.pitch * 32, 2)), "SDK tiled address differs from pinned formula");
        for (const auto axis : std::array<std::array<std::uint32_t, 2>, 2>{ { { form.width, mip.width }, { form.height, mip.height } } })
            for (std::uint32_t pixel = 0; pixel < axis[0]; ++pixel)
            {
                const double a      = (pixel + 0.25) * axis[1] / axis[0];
                const double b      = (pixel + 0.75) * axis[1] / axis[0];
                const double margin = (1.5 / 1024.0) * axis[1] / axis[0];
                require(unsigned(a) == unsigned(b) && a - unsigned(a) > margin && unsigned(b) + 1.0 - b > margin && b < axis[1], "pixel-center conventions differ or hit a texel edge");
            }
    }
}

} // namespace

void CheckBc1Fixture()
{
    constexpr auto kMipCount     = testing::kNativePreparedBc1MipCount;
    constexpr auto kWidth        = testing::kNativePreparedBc1Width;
    constexpr auto kHeight       = testing::kNativePreparedBc1Height;
    constexpr auto kOutputWidth  = testing::kNativePreparedBc1OutputWidth;
    constexpr auto kOutputHeight = testing::kNativePreparedBc1OutputHeight;
    constexpr auto kPlane        = static_cast<std::size_t>(kOutputWidth) * kOutputHeight * 4U;

    const auto                                   fixture = testing::MakeNativePreparedBc1Fixture();
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    std::memcpy(&fetch, fixture.texture.fetch_words.data(), sizeof(fetch));
    require(fixture.width == kWidth && fixture.height == kHeight && fixture.terminal_stride == kOutputWidth * 4U &&
                fixture.vertex_stride == 56,
            "fixture dimensions or vertex stride changed");
    require(fixture.register_image.size() == testing::kNativePreparedBc1RegisterCount,
            "fixture register image extent changed");
    require(fixture.register_image[0x2000U] == 0x14000500U &&
                fixture.register_image[0x2082U] == (kWidth | (kHeight << 16U)),
            "fixture render canvas or source scissor changed");
    require(fixture.vertex_bytes.size() == 3 * 56 && fixture.index_bytes.size() == 6,
            "fixture geometry extent changed");
    for (std::size_t vertex = 0; vertex < 3U; ++vertex)
        require(bigEndianFloat(fixture.vertex_bytes, vertex * fixture.vertex_stride + 16U) == 0.5F + 0.25F / kWidth &&
                    bigEndianFloat(fixture.vertex_bytes, vertex * fixture.vertex_stride + 20U) == 0.5F + 0.25F / kHeight,
                "fixture fixed UV lost its quarter-base-texel bias");
    for (const auto& mip : fixture.texture.mips)
        requireInteriorUv(kWidth, kHeight, mip.width, mip.height);
    require(fixture.texture.mips.size() == kMipCount && fixture.expected_by_lod_pair.size() == kMipCount * kMipCount,
            "fixture mip or expected-pair extent changed");
    require(fixture.texture.format == static_cast<std::uint32_t>(rex::graphics::xenos::TextureFormat::k_DXT1) &&
                fixture.texture.pitch == 4U && fixture.texture.packed_mips && !fixture.texture.tiled &&
                fixture.texture.base_guest_bytes.size() >= 0x2000U && fixture.texture.mip_guest_bytes.size() >= 0x4340U,
            "fixture does not retain the exact packed BC1 guest footprint");
    require(fetch.format == rex::graphics::xenos::TextureFormat::k_DXT1 &&
                fetch.endianness == rex::graphics::xenos::Endian::k8in16 && fetch.pitch == 4U &&
                fetch.packed_mips && fetch.swizzle == 0x688U && fetch.mip_min_level == 0U &&
                fetch.mip_max_level == 7U,
            "fixture fetch words do not retain the exact BC1 interpretation");
    for (std::uint32_t level = 0; level < kMipCount; ++level)
    {
        const auto& mip = fixture.texture.mips[level];
        require(mip.level == level && mip.width == testing::kNativePreparedBc1Widths[level] &&
                    mip.height == testing::kNativePreparedBc1Heights[level] &&
                    mip.guest_offset == testing::kNativePreparedBc1GuestOffsets[level] &&
                    mip.row_pitch == testing::kNativePreparedBc1RowPitches[level] &&
                    mip.linear_rgba8.size() == static_cast<std::size_t>(mip.width) * mip.height * 4U &&
                    mip.guest_extent != 0U,
                "fixture BC1 mip dimensions, offsets, pitch, or independently decoded pixels are incomplete");
    }
    for (std::uint32_t first = 0; first < kMipCount; ++first)
        for (std::uint32_t second = 0; second < kMipCount; ++second)
            requireOpaqueCanvas(testing::ExpectedNativePreparedBc1Pixels(fixture, first, second), kPlane);

    // Payload: decode k8in16 directly from the guest bytes, without the
    // production block-swap helper, and require the upload to match it. Every
    // level's first endpoint must also expand to that level's pixel color.
    std::vector<std::vector<std::uint8_t>> levels;
    for (std::uint32_t level = 0; level < kMipCount; ++level)
    {
        const auto& mip        = fixture.texture.mips[level];
        const auto  blocksWide = std::max(1U, (mip.width + 3U) / 4U);
        const auto  blocksHigh = std::max(1U, (mip.height + 3U) / 4U);
        const auto  rowBytes   = static_cast<std::size_t>(blocksWide) * 8U;
        const auto  source     = level == 0U ? std::span<const std::uint8_t>(fixture.texture.base_guest_bytes)
                                             : std::span<const std::uint8_t>(fixture.texture.mip_guest_bytes);
        levels.emplace_back(rowBytes * blocksHigh, 0U);
        for (std::uint32_t blockY = 0; blockY < blocksHigh; ++blockY)
            for (std::uint32_t blockX = 0; blockX < blocksWide; ++blockX)
            {
                const auto sourceOffset = static_cast<std::size_t>(mip.guest_offset) + static_cast<std::size_t>(blockY) * mip.row_pitch + static_cast<std::size_t>(blockX) * 8U;
                require(sourceOffset + 8U <= source.size(), "BC1 subresource exceeds its selected guest range");
                const auto destination = static_cast<std::size_t>(blockY) * rowBytes + static_cast<std::size_t>(blockX) * 8U;
                for (std::size_t pair = 0; pair < 4U; ++pair)
                {
                    levels.back()[destination + pair * 2U]      = source[sourceOffset + pair * 2U + 1U];
                    levels.back()[destination + pair * 2U + 1U] = source[sourceOffset + pair * 2U];
                }
            }
        const auto endpoint = static_cast<unsigned>(levels.back()[0]) | (static_cast<unsigned>(levels.back()[1]) << 8U);
        const auto red5     = (endpoint >> 11U) & 0x1FU;
        const auto green6   = (endpoint >> 5U) & 0x3FU;
        const auto blue5    = endpoint & 0x1FU;
        require(mip.linear_rgba8[0] == ((red5 << 3U) | (red5 >> 2U)) && mip.linear_rgba8[1] == ((green6 << 2U) | (green6 >> 4U)) &&
                    mip.linear_rgba8[2] == ((blue5 << 3U) | (blue5 >> 2U)) && mip.linear_rgba8[3] == 255U,
                "BC1 endpoint does not expand to the level's pixel color");
    }
    NativeDrawReplayTextureLayout layout;
    layout.width        = kWidth;
    layout.height       = kHeight;
    layout.format       = NativeDrawReplayTextureFormat::Bc1;
    layout.mipLevels    = kMipCount;
    layout.dimension    = NativeDrawReplayTextureDimension::Texture2D;
    const auto oracle   = packLevels(levels, layout);
    const auto uploaded = uploadFixture(liveFetch(fixture.texture.fetch_words, true), fixture.texture.base_guest_bytes, fixture.texture.mip_guest_bytes, 0x2000U, 0x4340U);
    require(std::ranges::equal(uploaded.payloadBytes(), oracle.payloadBytes()) && uploaded.mipLevels == kMipCount,
            "BC1 upload differs from the independent k8in16 decode");
}

void CheckRgba8Fixture()
{
    testing::SelectRgba8MatrixForm(testing::kRgba8MatrixForms[0]);
    const auto& form = *testing::rgba8MatrixForm;
    require(std::string_view(form.name) == "rgba8-8-packed-base", "RGBA8 smoke form changed");
    const auto            fixture  = testing::MakeNativePreparedRgba8MatrixFixture();
    const auto            vpW      = testing::Rgba8MatrixViewportWidth();
    const auto            vpH      = testing::Rgba8MatrixViewportHeight();
    const auto            mipCount = testing::kNativePreparedRgba8MatrixMipCount;
    constexpr std::size_t kPlane   = static_cast<std::size_t>(testing::kNativePreparedRgba8MatrixOutputWidth) * testing::kNativePreparedRgba8MatrixOutputHeight * 4U;

    requireMatrixShaders(fixture.vertex_shader_words, fixture.pixel_shader_words);
    const auto constants = rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X;
    require(fixture.register_image[constants] == 0x3F800000U && fixture.register_image[constants + 1] == 0x40000000U &&
                fixture.register_image[constants + 2] == 0x40800000U && fixture.register_image[constants + 3] == 0x41000000U,
            "weighted readout constants changed");
    require(fixture.width == vpW && fixture.height == vpH && fixture.terminal_stride == testing::kNativePreparedRgba8MatrixOutputWidth * 4U &&
                fixture.vertex_stride == 56U,
            "fixture dimensions or vertex stride changed");
    requireMatrixRegisters(fixture.register_image, testing::kNativePreparedRgba8MatrixRegisterCount, vpW, vpH);
    requireMatrixGeometry(fixture, form, testing::Rgba8MatrixUvBias(form.width) + float(fixture.window_x) / float(form.width), testing::Rgba8MatrixUvBias(form.height) + float(fixture.window_y) / float(form.height), 1.0F);
    require(fixture.texture.mips.size() == mipCount && fixture.expected_by_lod_pair.size() == mipCount * mipCount,
            "fixture mip or expected-pair extent changed");
    require(fixture.texture.width == form.width && fixture.texture.height == form.height && fixture.texture.format == form.format && fixture.texture.pitch == form.pitch &&
                fixture.texture.endianness == unsigned(testing::Rgba8MatrixEndian()) && fixture.texture.swizzle == testing::Rgba8MatrixSwizzle() && fixture.texture.packed_mips == form.packed && fixture.texture.tiled == form.tiled && fixture.texture.mip_max_level == form.max_level,
            "fixture representation differs from frozen form");
    requireMatrixLayout(fixture, form, testing::Rgba8MatrixFormat(), testing::Rgba8MatrixBlockBytes(), mipCount, [](std::uint32_t level, std::uint32_t x, std::uint32_t y)
                        {
                            return testing::Rgba8MatrixAddress(level, x, y);
                        });

    // Payload: reverse k8in32 and select components 0x60A independently of the
    // upload, compare each texel with the pixel oracle, then require the
    // production upload to produce the same bytes.
    std::vector<std::vector<std::uint8_t>> levels;
    for (std::uint32_t level = 0; level < mipCount; ++level)
    {
        const auto& mip    = fixture.texture.mips[level];
        const auto& source = level ? fixture.texture.mip_guest_bytes : fixture.texture.base_guest_bytes;
        levels.emplace_back(std::size_t(mip.width) * mip.height * testing::Rgba8MatrixBlockBytes());
        for (std::uint32_t y = 0; y < mip.height; ++y)
            for (std::uint32_t x = 0; x < mip.width; ++x)
            {
                const auto at  = testing::Rgba8MatrixAddress(level, x, y);
                const auto out = (std::size_t(y) * mip.width + x) * testing::Rgba8MatrixBlockBytes();
                require(at + testing::Rgba8MatrixBlockBytes() <= source.size(), "oracle address exceeds guest allocation");
                for (unsigned c = 0; c < 4; ++c)
                    levels.back()[out + c] = source[at + 3 - c];
                const std::array<std::uint8_t, 4> interpreted{ levels.back()[out + 2], levels.back()[out + 1], levels.back()[out], levels.back()[out + 3] };
                require(std::equal(interpreted.begin(), interpreted.end(), mip.linear_rgba8.begin() + (std::size_t(y) * mip.width + x) * 4),
                        "independent raw decode differs from pixel oracle");
            }
    }
    NativeDrawReplayTextureLayout layout;
    layout.width        = form.width;
    layout.height       = form.height;
    layout.mipLevels    = mipCount;
    layout.format       = NativeDrawReplayTextureFormat::Rgba8;
    auto oracle         = packLevels(levels, layout);
    oracle.swizzle      = { 2, 1, 0, 3 };
    const auto live     = liveFetch(fixture.texture.fetch_words, form.max_level != 0U);
    const auto uploaded = uploadFixture(live, fixture.texture.base_guest_bytes, fixture.texture.mip_guest_bytes, form.base_data_extent, form.mip_extent);
    require(std::ranges::equal(uploaded.payloadBytes(), oracle.payloadBytes()) && uploaded.swizzle == oracle.swizzle && uploaded.mipLevels == mipCount,
            "upload selected wrong packed LOD0 bytes or changed interpretation");
    // The packed base keeps poisoned bytes before the LOD0 offset: they must
    // not reach the upload, while a byte at the offset must.
    require(form.packed && !form.max_level && form.offsets[0] != 0 && fixture.texture.base_guest_bytes[0] == 0xCDU, "packed-base prefix poison lost");
    auto                    poisoned = fixture.texture.base_guest_bytes;
    NativeDrawReplayTexture counter;
    std::string             error;
    poisoned[0] ^= 1;
    require(DecodeNativeTextureVersioned(live, std::span(poisoned).first(form.base_data_extent), {}, counter, error) && std::ranges::equal(counter.payloadBytes(), oracle.payloadBytes()),
            "packed prefix byte leaked into LOD0 upload");
    poisoned[0] ^= 1;
    poisoned[form.offsets[0]] ^= 1;
    require(DecodeNativeTextureVersioned(live, std::span(poisoned).first(form.base_data_extent), {}, counter, error) && !std::ranges::equal(counter.payloadBytes(), oracle.payloadBytes()),
            "packed LOD0 offset mutation did not affect upload");

    const auto& base    = fixture.texture.mips[0].linear_rgba8;
    const auto  pixelAt = [&](std::uint32_t x, std::uint32_t y)
    {
        const auto offset = (static_cast<std::size_t>(y) * form.width + x) * 4U;
        return std::array<std::uint8_t, 4>{ base[offset], base[offset + 1U], base[offset + 2U], base[offset + 3U] };
    };
    require(pixelAt(0U, 0U) != pixelAt(1U, 0U) && (form.height == 1U || pixelAt(0U, 0U) != pixelAt(0U, 1U)) &&
                pixelAt(form.width - 1U, form.height - 1U) != pixelAt(0U, 0U),
            "fixture lost asymmetric X/Y patterns through the last blocks");
    for (std::uint32_t first = 0; first < mipCount; ++first)
        for (std::uint32_t second = 0; second < mipCount; ++second)
            require(testing::ExpectedNativePreparedRgba8MatrixPixels(fixture, first, second).size() == kPlane,
                    "matrix independent expected-pair canvas is incomplete");
    requireOpaqueCanvas(testing::ExpectedNativePreparedRgba8MatrixPixels(fixture, 0U, mipCount > 1U ? 1U : 0U), kPlane);
    for (std::uint32_t level = 0; level < mipCount; ++level)
    {
        const auto baseline = testing::ExpectedNativePreparedRgba8MatrixPixels(fixture, level, level);
        for (unsigned component = 0; component < 4; ++component)
        {
            auto  mutated = fixture;
            auto& pixels  = mutated.texture.mips[level].linear_rgba8;
            for (std::size_t at = component; at < pixels.size(); at += 4)
                pixels[at] = 0;
            require(testing::ExpectedNativePreparedRgba8MatrixPixels(mutated, level, level) != baseline,
                    "a guest component became output-insensitive");
        }
    }
}

void CheckFloatFixture()
{
    testing::SelectFloatMatrixForm(testing::kFloatMatrixForms[0]);
    const auto& form = *testing::floatMatrixForm;
    require(std::string_view(form.name) == "r32-float-512-tiled", "float smoke form changed");
    const auto            fixture  = testing::MakeNativePreparedFloatMatrixFixture();
    const auto            vpW      = testing::FloatMatrixViewportWidth();
    const auto            vpH      = testing::FloatMatrixViewportHeight();
    const auto            mipCount = testing::kNativePreparedFloatMatrixMipCount;
    constexpr std::size_t kPlane   = static_cast<std::size_t>(testing::kNativePreparedFloatMatrixOutputWidth) * testing::kNativePreparedFloatMatrixOutputHeight * 4U;

    requireMatrixShaders(fixture.vertex_shader_words, fixture.pixel_shader_words);
    const auto constants = rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X;
    require(fixture.register_image[constants] == 0x3D800000U && fixture.register_image[constants + 1] == 0x3C000000U &&
                fixture.register_image[constants + 2] == 0x3D600000U && fixture.register_image[constants + 3] == 0x3D800000U,
            "weighted readout constants changed");
    require(fixture.width == vpW && fixture.height == vpH && fixture.terminal_stride == testing::kNativePreparedFloatMatrixOutputWidth * 4U &&
                fixture.vertex_stride == 56U,
            "fixture dimensions or vertex stride changed");
    requireMatrixRegisters(fixture.register_image, testing::kNativePreparedFloatMatrixRegisterCount, vpW, vpH);
    requireMatrixGeometry(fixture, form, testing::FloatMatrixUvBias(form.width), testing::FloatMatrixUvBias(form.height), 1.0F);
    require(fixture.texture.mips.size() == mipCount && fixture.expected_by_lod_pair.size() == mipCount * mipCount,
            "fixture mip or expected-pair extent changed");
    require(fixture.texture.width == form.width && fixture.texture.height == form.height && fixture.texture.format == form.format && fixture.texture.pitch == form.pitch &&
                fixture.texture.endianness == unsigned(testing::FloatMatrixEndian()) && fixture.texture.swizzle == testing::FloatMatrixSwizzle() && fixture.texture.packed_mips == form.packed && fixture.texture.tiled == form.tiled && fixture.texture.mip_max_level == form.max_level,
            "fixture representation differs from frozen form");
    requireMatrixLayout(fixture, form, testing::FloatMatrixFormat(), testing::FloatMatrixBlockBytes(), mipCount, [](std::uint32_t level, std::uint32_t x, std::uint32_t y)
                        {
                            return testing::FloatMatrixAddress(level, x, y);
                        });
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    std::memcpy(&fetch, fixture.texture.fetch_words.data(), sizeof(fetch));
    require(((fetch.dword_0 >> 2U) & 0xFFU) == 0x55U && fetch.num_format == 1U && !fetch.exp_adjust && !fetch.stacked && !fetch.size_2d.stack_depth, "float scalar tuple changed");

    // Payload: read every binary32 through the independent tiled address,
    // compare its bits with the fixture, and require the upload to match.
    const auto independentLevels = [&](const testing::NativePreparedFloatMatrixFixture& source)
    {
        std::vector<std::vector<std::uint8_t>> levels;
        for (std::uint32_t level = 0; level < mipCount; ++level)
        {
            const auto& mip   = source.texture.mips[level];
            const auto& bytes = level ? source.texture.mip_guest_bytes : source.texture.base_guest_bytes;
            levels.emplace_back(std::size_t(mip.width) * mip.height * testing::FloatMatrixBlockBytes());
            for (std::uint32_t y = 0; y < mip.height; ++y)
                for (std::uint32_t x = 0; x < mip.width; ++x)
                {
                    const auto at  = testing::FloatMatrixAddress(level, x, y);
                    const auto out = (std::size_t(y) * mip.width + x) * testing::FloatMatrixBlockBytes();
                    require(at + testing::FloatMatrixBlockBytes() <= bytes.size(), "oracle address exceeds guest allocation");
                    for (unsigned c = 0; c < 4U; ++c)
                        levels.back()[out + c] = bytes[at + 3U - c];
                    require(std::equal(levels.back().begin() + out, levels.back().begin() + out + 4U, mip.linear_r32.begin() + out), "independent float bits differ from fixture");
                }
        }
        return levels;
    };
    NativeDrawReplayTextureLayout layout;
    layout.width        = form.width;
    layout.height       = form.height;
    layout.mipLevels    = mipCount;
    layout.format       = NativeDrawReplayTextureFormat::R32Float;
    auto oracle         = packLevels(independentLevels(fixture), layout);
    oracle.swizzle      = { 0, 5, 5, 5 };
    const auto live     = liveFetch(fixture.texture.fetch_words, form.max_level != 0U);
    const auto uploaded = uploadFixture(live, fixture.texture.base_guest_bytes, fixture.texture.mip_guest_bytes, form.base_data_extent, form.mip_extent);
    require(std::ranges::equal(uploaded.payloadBytes(), oracle.payloadBytes()) && uploaded.swizzle == oracle.swizzle && uploaded.mipLevels == mipCount,
            "upload selected wrong bytes or changed interpretation");

    // Low mantissa bits must survive the decoder; the rendered fixture itself
    // keeps dyadic values with exact shader and byte expectations.
    auto payloadFixture = fixture;
    for (std::uint32_t y = 0; y < form.height; ++y)
        for (std::uint32_t x = 0; x < form.width; ++x)
        {
            const auto    linear = (std::size_t(y) * form.width + x) * 4U;
            std::uint32_t bits;
            std::memcpy(&bits, payloadFixture.texture.mips[0].linear_r32.data() + linear, 4U);
            bits |= ((x * 0x9E3779B1U ^ y * 0x85EBCA77U) & 0x000FFFFFU) | 1U;
            const auto address = testing::FloatMatrixAddress(0, x, y);
            for (unsigned c = 0; c < 4U; ++c)
            {
                payloadFixture.texture.mips[0].linear_r32[linear + c]     = std::uint8_t(bits >> (c * 8U));
                payloadFixture.texture.base_guest_bytes[address + 3U - c] = std::uint8_t(bits >> (c * 8U));
            }
        }
    const auto              payloadOracle = packLevels(independentLevels(payloadFixture), layout);
    NativeDrawReplayTexture payloadDecoded;
    std::string             error;
    require(DecodeNativeTextureVersioned(live, payloadFixture.texture.base_guest_bytes, {}, payloadDecoded, error) &&
                std::ranges::equal(payloadDecoded.payloadBytes(), payloadOracle.payloadBytes()),
            "float payload decoder lost low mantissa bits");

    const auto& base    = fixture.texture.mips[0].linear_r32;
    const auto  pixelAt = [&](std::uint32_t x, std::uint32_t y)
    {
        const auto offset = (static_cast<std::size_t>(y) * form.width + x) * 4U;
        return std::array<std::uint8_t, 4>{ base[offset], base[offset + 1U], base[offset + 2U], base[offset + 3U] };
    };
    require(pixelAt(0U, 0U) != pixelAt(1U, 0U) && pixelAt(0U, 0U) != pixelAt(0U, 1U) &&
                pixelAt(form.width - 1U, form.height - 1U) != pixelAt(0U, 0U),
            "fixture lost asymmetric X/Y patterns through the last blocks");
    for (std::uint32_t first = 0; first < mipCount; ++first)
        for (std::uint32_t second = 0; second < mipCount; ++second)
            require(testing::ExpectedNativePreparedFloatMatrixPixels(fixture, first, second).size() == kPlane,
                    "matrix independent expected-pair canvas is incomplete");
    requireOpaqueCanvas(testing::ExpectedNativePreparedFloatMatrixPixels(fixture, 0U, 0U), kPlane);
    // Each selected component contributes a distinct nonzero amount without
    // clipping, and no readout sits within 0.1 byte of a rounding boundary.
    for (int n = -7; n <= 7; n += 2)
        for (const auto contribution : { 0, 1, 7, 8 })
        {
            const auto numerator = (n + 16 - contribution) * 255;
            require(numerator > 0 && numerator < 255 * 64, "unclamped finite readout saturated");
            require(std::abs(numerator % 64 - 32) >= 7, "finite readout is within 0.1 byte of a rounding boundary");
        }
}

void CheckCubeFixture()
{
    using namespace rex::graphics;
    testing::SelectCubeMatrixForm(testing::kCubeMatrixForms[0]);
    const auto& f = *testing::cubeMatrixForm;
    require(std::string_view(f.name) == "rgba8-cube-64-packed", "cube smoke form changed");
    const auto fixture   = testing::MakeNativePreparedCubeMatrixFixture();
    const auto kMipCount = testing::kNativePreparedCubeMatrixMipCount;
    require(fixture.vertex_bytes.size() == 72 * 56 && fixture.index_bytes.size() == 144, "cube atlas geometry is incomplete");
    require(fixture.pixel_shader_words.size() == 24, "cube pixel word extent changed");
    const auto alu = [&](unsigned index)
    {
        ucode::AluInstruction instruction;
        std::memcpy(&instruction, fixture.pixel_shader_words.data() + index * 3, 12);
        return instruction;
    };
    const auto cube = alu(2), reciprocal = alu(3), mad = alu(4);
    require(cube.vector_opcode() == ucode::AluVectorOpcode::kCube && cube.vector_dest() == 1 && cube.vector_write_mask() == 15 &&
                cube.src_reg(1) == 0 && cube.src_reg(2) == 0 && cube.src_swizzle(1) == 0xA6 && cube.src_swizzle(2) == 0xCD &&
                cube.src_is_temp(1) && cube.src_is_temp(2) && !cube.is_predicated(),
            "decoded cube ALU fields changed");
    require(reciprocal.scalar_opcode() == ucode::AluScalarOpcode::kRcp && reciprocal.scalar_dest() == 1 && reciprocal.scalar_write_mask() == 4 && reciprocal.src_reg(3) == 0x81 && reciprocal.src_swizzle(3) == 0xC6, "decoded absolute cube reciprocal changed");
    require(mad.vector_opcode() == ucode::AluVectorOpcode::kMad && mad.vector_dest() == 1 && mad.vector_write_mask() == 3 && mad.src_reg(1) == 1 && mad.src_reg(2) == 1 && mad.src_reg(3) == 0 && mad.src_swizzle(2) == 0xC6 && mad.src_swizzle(3) == 0x6C, "decoded canonical cube MAD changed");
    const auto select = alu(6), output = alu(7);
    require(select.vector_opcode() == ucode::AluVectorOpcode::kDp4 && select.src_reg(1) == 2 && select.src_reg(2) == 1 &&
                select.src_is_temp(1) && !select.src_is_temp(2) && select.vector_write_mask() == 15 && select.vector_dest() == 3,
            "decoded cube channel projection changed");
    require(output.is_export() && output.vector_opcode() == ucode::AluVectorOpcode::kMad && output.vector_write_mask() == 7 &&
                output.src_reg(1) == 3 && output.src_reg(2) == 2 && output.src_reg(3) == 2 && output.src_swizzle(2) == 0x6C && output.src_swizzle(3) == 0xB1,
            "decoded cube safe readout changed");
    require(fixture.pixel_shader_words[0] == 0x01016001 && fixture.pixel_shader_words[1] == 0x10071800 && fixture.pixel_shader_words[2] == 0x28000000,
            "authored straight-line cube control flow changed");
    ucode::TextureFetchInstruction setter, fetch;
    std::memcpy(&setter, fixture.pixel_shader_words.data() + 3, 12);
    std::memcpy(&fetch, fixture.pixel_shader_words.data() + 15, 12);
    require(setter.opcode() == ucode::FetchOpcode::kSetTextureLod && setter.src() == 0 && setter.src_swizzle() == 3 && !setter.is_src_relative() && !setter.is_predicated(), "decoded cube LOD setter changed");
    require(fetch.opcode() == ucode::FetchOpcode::kTextureFetch && fetch.dimension() == xenos::FetchOpDimension::kCube && fetch.src() == 1 && fetch.src_swizzle() == 0x31 && fetch.dest() == 2 && fetch.fetch_constant_index() == 0 && fetch.use_register_lod() && !fetch.use_computed_lod() && !fetch.use_register_gradients() && !fetch.unnormalized_coordinates() && !fetch.is_predicated() && !fetch.is_src_relative() && !fetch.is_dest_relative() && !fetch.lod_bias() && !fetch.offset_x() && !fetch.offset_y() && !fetch.offset_z() && fetch.mag_filter() == xenos::TextureFilter::kPoint && fetch.min_filter() == xenos::TextureFilter::kPoint && fetch.mip_filter() == xenos::TextureFilter::kPoint && fetch.aniso_filter() == xenos::AnisoFilter::kDisabled, "decoded point cube fetch fields changed");
    const auto layout = texture_util::GetGuestTextureLayout(xenos::DataDimension::kCube, f.pitch, f.width, f.width, 6, false, static_cast<xenos::TextureFormat>(f.format), true, true, f.levels - 1);
    require(layout.base.level_data_extent_bytes == f.base_extent && layout.mips_total_extent_bytes == f.mip_extent && layout.packed_level == f.packed_level && layout.mip_offsets_bytes[f.packed_level] == f.tail_offset && layout.mips[f.packed_level].level_data_extent_bytes == f.tail_extent,
            "compiled SDK cube layout differs from the frozen extents");
    // Every texel of every face and level sits at the frozen address; texels
    // do not overlap, and every other guest byte keeps its poison.
    std::array<std::vector<bool>, 2> live{ std::vector<bool>(fixture.texture.base_guest_bytes.size()), std::vector<bool>(fixture.texture.mip_guest_bytes.size()) };
    for (unsigned face = 0; face < 6; ++face)
        for (unsigned level = 0; level < kMipCount; ++level)
        {
            const auto& stored = level ? layout.mips[std::min(level, f.packed_level)] : layout.base;
            unsigned    px = 0, py = 0, pz = 0;
            if (level >= f.packed_level)
                require(texture_util::GetPackedMipOffset(f.width, f.width, 1, static_cast<xenos::TextureFormat>(f.format), level, px, py, pz), "cube packed offset missing");
            require(!pz && stored.row_pitch_bytes == f.rows[level] && stored.array_slice_stride_bytes == f.strides[level], "cube stored stride differs from frozen table");
            for (unsigned y = 0; y < testing::CubeMatrixLevelUnits(level); ++y)
                for (unsigned x = 0; x < testing::CubeMatrixLevelUnits(level); ++x)
                {
                    const auto at = (level ? layout.mip_offsets_bytes[std::min(level, f.packed_level)] : 0) + face * stored.array_slice_stride_bytes + (py + y) * stored.row_pitch_bytes + (px + x) * f.bytes_per_texel;
                    require(at == testing::CubeMatrixAddress(f, face, level, x, y), "cube live address differs from frozen table");
                    for (unsigned byte = 0; byte < f.bytes_per_texel; ++byte)
                    {
                        require(at + byte < live[level != 0].size() && !live[level != 0][at + byte], "cube live ranges overlap or exceed the allocation");
                        live[level != 0][at + byte] = true;
                    }
                }
        }
    for (unsigned bank = 0; bank < 2; ++bank)
    {
        const auto& raw = bank ? fixture.texture.mip_guest_bytes : fixture.texture.base_guest_bytes;
        for (std::size_t at = 0; at < raw.size(); ++at)
            if (!live[bank][at])
                require(raw[at] == 0xCD, "cube padding lost poison");
    }
    // Sampling conventions: every panel stays on its face, away from seams and
    // texel boundaries, and covers every texel of its level.
    auto sampling = fixture;
    for (unsigned level = 0; level < kMipCount; ++level)
    {
        sampling.selected_lods = { level, level };
        for (unsigned panel = 0; panel < 24; ++panel)
        {
            std::vector<bool> texels(std::size_t(f.width >> level) * (f.width >> level));
            for (unsigned y = 0; y < f.width; ++y)
                for (unsigned x = 0; x < f.width; ++x)
                {
                    const auto original = testing::CubeOracleSample(sampling, panel, x, y, 0, 1.5 / (1024 * f.width));
                    const auto integer  = testing::CubeOracleSample(sampling, panel, x, y, 0);
                    const auto half     = testing::CubeOracleSample(sampling, panel, x, y, .5);
                    require(original.face == panel % 6 && integer.face == original.face && half.face == original.face && original.x == integer.x && original.x == half.x && original.y == integer.y && original.y == half.y && half.major_margin >= 1.0 / (2 * f.width), "cube conventions differ or cross a seam");
                    for (const auto p : { original, integer, half })
                    {
                        const auto tx = p.u * (f.width >> level), ty = p.v * (f.width >> level);
                        require(tx > std::floor(tx) && ty > std::floor(ty) && std::ceil(tx) - tx > 1.0 / 1024 / (2 * f.width) && std::ceil(ty) - ty > 1.0 / 1024 / (2 * f.width), "cube sampling reaches a texel boundary");
                    }
                    texels[original.y * (f.width >> level) + original.x] = true;
                }
            require(std::ranges::all_of(texels, [](bool sampled)
                                        {
                                            return sampled;
                                        }),
                    "cube atlas does not cover every mip texel");
        }
    }
    // Payload: the production upload must equal the independent raw oracle and
    // publish one span per face and level.
    auto words = fixture.texture.fetch_words;
    words[1] |= 0x100000;
    words[5] |= 0x180000;
    NativeDrawReplayTexture decoded;
    std::string             error;
    require(DecodeNativeTextureVersioned(words, std::span(fixture.texture.base_guest_bytes).first(f.base_extent), std::span(fixture.texture.mip_guest_bytes).first(f.mip_extent), decoded, error),
            error);
    const auto expectedPayload = testing::CubeOraclePayload(fixture);
    require(std::ranges::equal(decoded.payloadBytes(), expectedPayload), "full immutable cube payload differs from the independent raw oracle");
    require(decoded.textureVersion->Subresources().size() == 6 * kMipCount && decoded.payloadBytes().size() == f.payload_bytes, "cube production upload omitted a face/level span");
    // The last level still tells faces and channels apart.
    auto f6                                       = fixture;
    f6.selected_lods                              = { kMipCount - 1, kMipCount - 1 };
    const auto                             pixels = testing::CubeOraclePixels(f6);
    std::array<std::array<unsigned, 4>, 6> fingerprints{};
    for (unsigned c = 0; c < testing::CubeMatrixDataChannels(); ++c)
    {
        std::array<unsigned, 6> last{};
        for (unsigned face = 0; face < 6; ++face)
        {
            last[face]            = pixels[(std::size_t(c * f.width) * testing::kNativePreparedCubeMatrixOutputWidth + face * f.width) * 4];
            fingerprints[face][c] = last[face];
        }
        for (unsigned a = 0; a < 6; ++a)
            for (unsigned b = 0; b < a; ++b)
                require(last[a] != last[b], "LOD6 face/channel fingerprints collide");
    }
    for (const auto& channels : fingerprints)
        for (unsigned a = 0; a < testing::CubeMatrixDataChannels(); ++a)
            for (unsigned b = 0; b < a; ++b)
                require(channels[a] != channels[b], "LOD6 channels collide within a face");
}

void CheckLodFixture()
{
    constexpr auto kMipCount = testing::kNativePixelLodMipCount;
    constexpr auto kWidth    = testing::kNativePixelLodWidth;
    constexpr auto kHeight   = testing::kNativePixelLodHeight;
    constexpr auto kPlane    = static_cast<std::size_t>(kWidth) * kHeight * 4U;

    const auto fixture = testing::MakeNativePixelLodFixture();
    require(kMipCount == 8 && kPlane == 65536, "native pixel LOD fixture guards changed");
    require(fixture.width == kWidth && fixture.height == kHeight && fixture.vertex_stride == 56,
            "fixture dimensions or vertex stride changed");
    require(fixture.register_image.size() == testing::kNativePixelLodRegisterCount,
            "fixture register image extent changed");
    require(fixture.register_image[0x2000U] == 0x02000080U && fixture.register_image[0x2082U] == (kWidth | (kHeight << 16U)),
            "fixture render canvas or source scissor changed");
    require(fixture.vertex_bytes.size() == 3 * 56 && fixture.index_bytes.size() == 6,
            "fixture geometry extent changed");
    for (std::size_t vertex = 0; vertex < 3U; ++vertex)
        require(bigEndianFloat(fixture.vertex_bytes, vertex * fixture.vertex_stride + 16U) == 0.5F + 0.25F / kWidth &&
                    bigEndianFloat(fixture.vertex_bytes, vertex * fixture.vertex_stride + 20U) == 0.5F + 0.25F / kHeight &&
                    bigEndianFloat(fixture.vertex_bytes, vertex * fixture.vertex_stride + 24U) == 2.0F &&
                    bigEndianFloat(fixture.vertex_bytes, vertex * fixture.vertex_stride + 28U) == 5.0F,
                "fixture fixed UV or register LOD pair changed");
    for (const auto& mip : fixture.texture.mips)
        requireInteriorUv(kWidth, kHeight, mip.width, mip.height);
    require(fixture.texture.mips.size() == kMipCount && fixture.expected_by_lod_pair.size() == kMipCount * kMipCount,
            "fixture mip or expected-pair extent changed");
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    std::memcpy(&fetch, fixture.texture.fetch_words.data(), sizeof(fetch));
    require(fetch.format == rex::graphics::xenos::TextureFormat::k_8_8_8_8 && fetch.endianness == rex::graphics::xenos::Endian::kNone &&
                fetch.pitch == kWidth / 32U && !fetch.packed_mips && !fetch.tiled && fetch.mip_min_level == 0U &&
                fetch.mip_max_level == kMipCount - 1U && fetch.swizzle == rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
            "fixture fetch words do not retain the exact linear RGBA8 interpretation");
    for (std::uint32_t first = 0; first < kMipCount; ++first)
        for (std::uint32_t second = 0; second < kMipCount; ++second)
            require(testing::ExpectedNativePixelLodPixels(fixture, first, second).size() == kPlane,
                    "fixture expected-pair pixels are incomplete");
    const auto expected = testing::ExpectedNativePixelLodPixels(fixture, 2U, 5U);
    require(expected == fixture.expected_selected_pixels, "selected expected pixels differ from the lod 2/5 pair");

    // Payload: each level's rows sit at its offset and pitch in the guest
    // bytes and equal the level's pixels; the upload publishes the same bytes.
    const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
        rex::graphics::xenos::DataDimension::k2DOrStacked, kWidth / 32U, kWidth, kHeight, 1U, false, rex::graphics::xenos::TextureFormat::k_8_8_8_8, false, true, kMipCount - 1U);
    std::vector<std::vector<std::uint8_t>> levels;
    for (std::uint32_t level = 0; level < kMipCount; ++level)
    {
        const auto& mip    = fixture.texture.mips[level];
        const auto& source = level ? fixture.texture.mip_guest_bytes : fixture.texture.base_guest_bytes;
        require(mip.level == level && mip.width == std::max(1U, kWidth >> level) && mip.height == std::max(1U, kHeight >> level) &&
                    mip.row_pitch == (level ? layout.mips[level].row_pitch_bytes : layout.base.row_pitch_bytes) &&
                    mip.guest_offset == (level ? layout.mip_offsets_bytes[level] : 0U) && mip.linear_rgba8.size() == std::size_t(mip.width) * mip.height * 4U,
                "fixture mip dimensions, offsets or pitch differ from the SDK layout");
        levels.emplace_back(mip.linear_rgba8.size());
        for (std::uint32_t y = 0; y < mip.height; ++y)
        {
            const auto at = std::size_t(mip.guest_offset) + std::size_t(y) * mip.row_pitch;
            require(at + mip.width * 4U <= source.size(), "linear level exceeds its guest allocation");
            std::memcpy(levels.back().data() + std::size_t(y) * mip.width * 4U, source.data() + at, std::size_t(mip.width) * 4U);
        }
        require(levels.back() == mip.linear_rgba8, "guest bytes differ from the level's pixels");
    }
    const auto                uploaded = uploadFixture(liveFetch(fixture.texture.fetch_words, true), fixture.texture.base_guest_bytes, fixture.texture.mip_guest_bytes, layout.base.level_data_extent_bytes, layout.mips_total_extent_bytes);
    std::vector<std::uint8_t> expectedPayload;
    for (const auto& level : levels)
        expectedPayload.insert(expectedPayload.end(), level.begin(), level.end());
    require(uploaded.mipLevels == kMipCount && std::ranges::equal(uploaded.payloadBytes(), expectedPayload),
            "upload differs from the fixture's linear levels");
}

} // namespace rerevved::tests::format_smoke
