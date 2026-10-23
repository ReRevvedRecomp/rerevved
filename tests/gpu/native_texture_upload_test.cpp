#include "gpu/d3d12/native_texture_upload.h"
#include "manual_cases.hpp"
#include "native_prepared_bc3_matrix_forms.h"
#include "native_prepared_dxt_matrix_forms.h"
#include "native_prepared_float_matrix_forms.h"
#include "native_prepared_rgba8_matrix_forms.h"
#include <bit>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <toml++/toml.hpp>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>

using namespace rerevved::gpu;

namespace
{

NativeTextureFetch fetch(std::uint32_t format, std::uint32_t endian, std::uint32_t width, std::uint32_t height, bool tiled = false)
{
    return { 2U | (1U << 22) | (tiled ? 0x80000000U : 0),
             0x00100000U | (endian << 6) | format,
             (width - 1) | ((height - 1) << 13),
             0xD10,
             0,
             0x200 };
}

void setMipRange(NativeTextureFetch& descriptor, std::uint32_t page, std::uint32_t maxMip)
{
    descriptor[4] = (maxMip << 6);
    descriptor[5] = (descriptor[5] & ~0xFFFFF000U) | (page << 12);
}

void setCube(NativeTextureFetch& descriptor)
{
    descriptor[2] |= 5U << 26;
    descriptor[5] = (descriptor[5] & ~(3U << 9)) | (3U << 9);
}

struct GuestMipInfo
{
    rex::graphics::xenos::xe_gpu_texture_fetch_t    fetch{};
    rex::graphics::texture_util::TextureGuestLayout layout;
    std::uint32_t                                   width     = 0;
    std::uint32_t                                   height    = 0;
    std::uint32_t                                   maxMip    = 0;
    std::uint32_t                                   arraySize = 0;
};

GuestMipInfo guestMipInfo(const NativeTextureFetch& descriptor)
{
    GuestMipInfo info;
    std::memcpy(&info.fetch, descriptor.data(), sizeof(info.fetch));
    std::uint32_t width, height, depth, basePage, mipPage, minMip;
    rex::graphics::texture_util::GetSubresourcesFromFetchConstant(
        info.fetch, &width, &height, &depth, &basePage, &mipPage, &minMip, &info.maxMip);
    ++width;
    ++height;
    info.width     = width;
    info.height    = height;
    info.arraySize = info.fetch.dimension == rex::graphics::xenos::DataDimension::kCube ? 6 : 1;
    info.layout    = rex::graphics::texture_util::GetGuestTextureLayout(
        info.fetch.dimension, info.fetch.pitch, width, height, info.arraySize, info.fetch.tiled, info.fetch.format, info.fetch.packed_mips, true, info.maxMip);
    return info;
}

std::size_t mipPayloadBytes(const GuestMipInfo& info, std::uint32_t mip)
{
    const auto* format     = rex::graphics::FormatInfo::Get(info.fetch.format);
    const auto  width      = std::max(1U, info.width >> mip);
    const auto  height     = std::max(1U, info.height >> mip);
    const auto  blocksWide = (width + format->block_width - 1) / format->block_width;
    const auto  blocksHigh = (height + format->block_height - 1) / format->block_height;
    return std::size_t(blocksWide) * blocksHigh * format->bytes_per_block();
}

void requireVersionedDecode(const NativeTextureFetch&      descriptor,
                            std::span<const std::uint8_t>  base,
                            std::span<const std::uint8_t>  mips,
                            const NativeDrawReplayTexture& raw)
{
    std::string             error;
    NativeDrawReplayTexture versioned;
    UNSCOPED_INFO(error);
    REQUIRE(DecodeNativeTextureVersioned(descriptor, base, mips, versioned, error));
    const auto version = versioned.textureVersion;
    UNSCOPED_INFO("trusted decode publishes independent immutable bytes with the raw decoder's exact storage and swizzle");
    REQUIRE((version && version->MatchesOwner(version) && versioned.bytes.empty() &&
             version->Payload() == versioned.sharedBytes && std::ranges::equal(versioned.payloadBytes(), raw.payloadBytes()) &&
             versioned.swizzle == raw.swizzle &&
             version->Layout() == NativeDrawReplayTextureLayout{ raw.width, raw.height, raw.format, raw.mipLevels, raw.dimension }));
    const auto  info   = guestMipInfo(descriptor);
    const auto* format = rex::graphics::FormatInfo::Get(info.fetch.format);
    UNSCOPED_INFO("version bounds and retains every slice and mip record");
    REQUIRE(version->Subresources().size() == std::size_t(info.arraySize) * (info.maxMip + 1));
    std::size_t                                offset = 0;
    std::vector<std::span<const std::uint8_t>> sources;
    for (std::uint32_t slice = 0; slice < info.arraySize; ++slice)
        for (std::uint32_t mip = 0; mip <= info.maxMip; ++mip)
        {
            const auto& record   = version->Subresources()[slice * (info.maxMip + 1) + mip];
            const auto  rowBytes = std::size_t((std::max(1U, info.width >> mip) + format->block_width - 1) / format->block_width) * format->bytes_per_block();
            const auto  rows     = (std::max(1U, info.height >> mip) + format->block_height - 1) / format->block_height;
            UNSCOPED_INFO("version records match independent guest-format tight row arithmetic and slice-then-mip offsets");
            REQUIRE((record.arraySlice == slice && record.mipLevel == mip && record.sourceOffset == offset &&
                     record.rowBytes == rowBytes && record.rowCount == rows));
            sources.push_back(raw.payloadBytes().subspan(offset, rowBytes * rows));
            offset += rowBytes * rows;
        }
    UNSCOPED_INFO("version tight layout covers its complete payload");
    REQUIRE(offset == version->Payload()->size());
    NativeDrawReplayTexture packed;
    UNSCOPED_INFO("packed source spans are copied into a fresh immutable allocation");
    REQUIRE((PackNativeDrawReplayTextureVersion(version->Layout(), sources, packed, error) && packed.textureVersion != version &&
             packed.sharedBytes != versioned.sharedBytes && std::ranges::equal(packed.payloadBytes(), versioned.payloadBytes())));
    NativeDrawReplayTexture inPlace = raw;
    UNSCOPED_INFO(error);
    REQUIRE(CopyNativeDrawReplayTextureVersion(inPlace, error));
    const std::weak_ptr<const NativeDrawReplayTextureVersion> originalVersion = inPlace.textureVersion;
    std::vector<std::span<const std::uint8_t>>                inPlaceSources;
    for (const auto& record : inPlace.textureVersion->Subresources())
        inPlaceSources.push_back(inPlace.payloadBytes().subspan(record.sourceOffset, record.rowBytes * record.rowCount));
    UNSCOPED_INFO("in-place packing keeps borrowed layout and source spans alive until fresh staging is complete");
    REQUIRE((PackNativeDrawReplayTextureVersion(inPlace.textureVersion->Layout(), inPlaceSources, inPlace, error) &&
             originalVersion.expired() && std::ranges::equal(inPlace.payloadBytes(), raw.payloadBytes())));
    if (!sources.empty())
    {
        sources.back() = sources.back().first(sources.back().size() - 1);
        UNSCOPED_INFO("incomplete packed subresource cannot publish a version");
        REQUIRE((!PackNativeDrawReplayTextureVersion(version->Layout(), sources, packed, error) && !packed.textureVersion && !packed.sharedBytes));
    }
}

void fillGuestMip(std::vector<std::uint8_t>& source,
                  const GuestMipInfo&        info,
                  std::uint32_t              slice,
                  std::uint32_t              mip,
                  std::uint8_t               value)
{
    const auto*   format       = rex::graphics::FormatInfo::Get(info.fetch.format);
    const auto    width        = std::max(1U, info.width >> mip);
    const auto    height       = std::max(1U, info.height >> mip);
    const auto    blocksWide   = (width + format->block_width - 1) / format->block_width;
    const auto    blocksHigh   = (height + format->block_height - 1) / format->block_height;
    const auto    packedLevel  = info.layout.packed_level;
    const bool    packed       = mip == 0 ? packedLevel == 0
                                          : packedLevel != std::numeric_limits<std::uint32_t>::max() && mip >= packedLevel;
    const auto    storageLevel = mip == 0 ? 0 : (packed ? packedLevel : mip);
    const auto&   level        = mip == 0 ? info.layout.base : info.layout.mips[storageLevel];
    std::uint32_t offsetX      = 0;
    std::uint32_t offsetY      = 0;
    std::uint32_t offsetZ      = 0;
    if (packed)
    {
        UNSCOPED_INFO("packed mip offset available");
        REQUIRE(rex::graphics::texture_util::GetPackedMipOffset(info.width, info.height, 1, info.fetch.format, mip, offsetX, offsetY, offsetZ));
    }
    const auto levelOffset = mip == 0 ? 0 : info.layout.mip_offsets_bytes[storageLevel];
    auto       levelBase   = std::size_t(levelOffset) + std::size_t(slice) * level.array_slice_stride_bytes;
    for (std::uint32_t row = 0; row < blocksHigh; ++row)
        for (std::uint32_t block = 0; block < blocksWide; ++block)
        {
            const auto                inputOffset = levelBase + std::size_t(offsetY + row) * level.row_pitch_bytes +
                                                    std::size_t(offsetX + block) * format->bytes_per_block();
            std::vector<std::uint8_t> expected(format->bytes_per_block(), value);
            rex::graphics::texture_conversion::CopySwapBlock(
                info.fetch.endianness, source.data() + inputOffset, expected.data(), expected.size());
        }
}

void requireMipValues(const NativeDrawReplayTexture&   texture,
                      const GuestMipInfo&              info,
                      NativeDrawReplayTextureDimension dimension)
{
    UNSCOPED_INFO("full texture metadata");
    REQUIRE((texture.width == info.width && texture.height == info.height && texture.mipLevels == info.maxMip + 1 &&
             texture.dimension == dimension));
    std::size_t outputOffset = 0;
    for (std::uint32_t slice = 0; slice < info.arraySize; ++slice)
        for (std::uint32_t mip = 0; mip <= info.maxMip; ++mip)
        {
            const auto size  = mipPayloadBytes(info, mip);
            const auto value = static_cast<std::uint8_t>(0x10 + slice * 0x20 + mip);
            UNSCOPED_INFO("packed slice-then-mip payload ordering");
            REQUIRE((texture.bytes.size() >= outputOffset + size && texture.bytes[outputOffset] == value));
            UNSCOPED_INFO("decoded mip bytes");
            REQUIRE(std::all_of(texture.bytes.begin() + outputOffset,
                                texture.bytes.begin() + outputOffset + size,
                                [value](std::uint8_t byte)
                                {
                                    return byte == value;
                                }));
            outputOffset += size;
        }
    UNSCOPED_INFO("tight full texture payload size");
    REQUIRE(texture.bytes.size() == outputOffset);
}

void linearRgba()
{
    auto descriptor = fetch(6, 2, 3, 2);
    descriptor[3]   = 0xC14; // BGRA selection.
    NativeTextureMemoryRange range;
    NativeDrawReplayTexture  texture;
    std::string              error;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRange(descriptor, range, error));
    UNSCOPED_INFO("linear rows preserve guest pitch");
    REQUIRE((range.address == 0x100000 && range.size == 140));
    std::vector<std::uint8_t>       input(range.size, 0xEE);
    const std::vector<std::uint8_t> expected = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24
    };
    for (std::size_t row = 0; row < 2; ++row)
        for (std::size_t byte = 0; byte < 12; ++byte)
            input[row * 128 + (byte ^ 3)] = expected[row * 12 + byte];
    UNSCOPED_INFO(error);
    REQUIRE(DecodeNativeTexture(descriptor, input, texture, error));
    UNSCOPED_INFO("RGBA rows decoded without pitch padding");
    REQUIRE(texture.bytes == expected);
    UNSCOPED_INFO("guest BGRA swizzle applied once");
    REQUIRE((texture.swizzle == std::array<std::uint8_t, 4>{ 2, 1, 0, 3 }));
    input.pop_back();
    UNSCOPED_INFO("truncated input cannot retain an earlier successful texture");
    REQUIRE((!DecodeNativeTexture(descriptor, input, texture, error) && texture.bytes.empty()));
}

void tiledRgba()
{
    const auto               descriptor = fetch(6, 2, 32, 32, true);
    NativeTextureMemoryRange range;
    NativeDrawReplayTexture  texture;
    std::string              error;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRange(descriptor, range, error));
    std::vector<std::uint8_t> input(range.size);
    // Independent locations cover microtile row order, bank selection, and the
    // last texel of a 32x32 RGBA tile in the Xenos address layout.
    const std::array<std::array<std::size_t, 3>, 4> samples = {
        { { 0, 0, 0 }, { 8, 0, 64 }, { 0, 1, 16 }, { 31, 31, 3964 } }
    };
    for (std::size_t i = 0; i < samples.size(); ++i)
        for (std::size_t channel = 0; channel < 4; ++channel)
            input[samples[i][2] + (channel ^ 3)] = static_cast<std::uint8_t>(4 * i + channel + 1);
    UNSCOPED_INFO(error);
    REQUIRE(DecodeNativeTexture(descriptor, input, texture, error));
    for (std::size_t i = 0; i < samples.size(); ++i)
        for (std::size_t channel = 0; channel < 4; ++channel)
        {
            UNSCOPED_INFO("tiled RGBA coordinates and endian");
            REQUIRE(texture.bytes[(samples[i][1] * 32 + samples[i][0]) * 4 + channel] == 4 * i + channel + 1);
        }
}

void compressedAndFont()
{
    std::string              error;
    NativeTextureMemoryRange range;
    NativeDrawReplayTexture  texture;
    for (const auto format : { 18U, 19U })
    {
        const auto descriptor = fetch(format, 1, 4, 4);
        UNSCOPED_INFO(error);
        REQUIRE(GetNativeTextureMemoryRange(descriptor, range, error));
        UNSCOPED_INFO("compressed block extent");
        REQUIRE(range.size == (format == 18 ? 8U : 16U));
        std::vector<std::uint8_t> input(range.size);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i ^ 1] = static_cast<std::uint8_t>(i);
        UNSCOPED_INFO(error);
        REQUIRE(DecodeNativeTexture(descriptor, input, texture, error));
        UNSCOPED_INFO("block compression remains compressed for native upload");
        REQUIRE(texture.format == (format == 18 ? NativeDrawReplayTextureFormat::Bc1 : NativeDrawReplayTextureFormat::Bc2));
        for (std::size_t i = 0; i < input.size(); ++i)
        {
            UNSCOPED_INFO("compressed block byte order");
            REQUIRE(texture.bytes[i] == i);
        }
    }
    auto descriptor = fetch(2, 0, 3, 2);
    descriptor[3]   = (5U | (5U << 3) | (5U << 6)) << 1; // White RGB, sampled red alpha.
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRange(descriptor, range, error));
    UNSCOPED_INFO("font rows use byte pitch");
    REQUIRE(range.size == 35);
    std::vector<std::uint8_t> input(range.size, 0xEE);
    input[0]  = 1;
    input[1]  = 2;
    input[2]  = 3;
    input[32] = 4;
    input[33] = 5;
    input[34] = 6;
    UNSCOPED_INFO(error);
    REQUIRE(DecodeNativeTexture(descriptor, input, texture, error));
    UNSCOPED_INFO("font pitch padding removed");
    REQUIRE(texture.bytes == std::vector<std::uint8_t>{ 1, 2, 3, 4, 5, 6 });
    UNSCOPED_INFO("font alpha and constant RGB preserved");
    REQUIRE((texture.swizzle == std::array<std::uint8_t, 4>{ 5, 5, 5, 0 }));
}

void fullPackedMipChain()
{
    auto descriptor = fetch(6, 2, 32, 32);
    descriptor[5] |= 0x800;
    setMipRange(descriptor, 0x240, 5);
    const auto                info = guestMipInfo(descriptor);
    NativeTextureMemoryRanges ranges;
    NativeDrawReplayTexture   texture;
    std::string               error;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRanges(descriptor, ranges, error));
    UNSCOPED_INFO("separate packed base and mip addresses");
    REQUIRE((ranges.base.address == 0x100000 && ranges.mips.address == 0x240000));
    std::vector<std::uint8_t> base(ranges.base.size);
    std::vector<std::uint8_t> mips(ranges.mips.size);
    for (std::uint32_t mip = 0; mip <= info.maxMip; ++mip)
        fillGuestMip(mip == 0 ? base : mips, info, 0, mip, static_cast<std::uint8_t>(0x10 + mip));
    UNSCOPED_INFO(error);
    REQUIRE(DecodeNativeTexture(descriptor, base, mips, texture, error));
    requireMipValues(texture, info, NativeDrawReplayTextureDimension::Texture2D);
    UNSCOPED_INFO("packed RGBA8 format mapping");
    REQUIRE(texture.format == NativeDrawReplayTextureFormat::Rgba8);
    requireVersionedDecode(descriptor, base, mips, texture);
}

void fullUnpackedMipChain()
{
    auto descriptor = fetch(19, 1, 64, 32);
    setMipRange(descriptor, 0x280, 3);
    const auto                info = guestMipInfo(descriptor);
    NativeTextureMemoryRanges ranges;
    NativeDrawReplayTexture   texture;
    std::string               error;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRanges(descriptor, ranges, error));
    std::vector<std::uint8_t> base(ranges.base.size);
    std::vector<std::uint8_t> mips(ranges.mips.size);
    for (std::uint32_t mip = 0; mip <= info.maxMip; ++mip)
        fillGuestMip(mip == 0 ? base : mips, info, 0, mip, static_cast<std::uint8_t>(0x10 + mip));
    UNSCOPED_INFO(error);
    REQUIRE(DecodeNativeTexture(descriptor, base, mips, texture, error));
    requireMipValues(texture, info, NativeDrawReplayTextureDimension::Texture2D);
    UNSCOPED_INFO("unpacked BC2 format mapping");
    REQUIRE(texture.format == NativeDrawReplayTextureFormat::Bc2);
    requireVersionedDecode(descriptor, base, mips, texture);
}

void fullCubeFaces()
{
    auto descriptor = fetch(2, 0, 32, 32);
    descriptor[5] |= 0x800;
    setMipRange(descriptor, 0x2C0, 3);
    setCube(descriptor);
    const auto                info = guestMipInfo(descriptor);
    NativeTextureMemoryRanges ranges;
    NativeDrawReplayTexture   texture;
    std::string               error;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRanges(descriptor, ranges, error));
    UNSCOPED_INFO("cube ranges include all six faces");
    REQUIRE((ranges.base.size > 5 * 4096 && ranges.mips.size > 5 * 4096));
    std::vector<std::uint8_t> base(ranges.base.size);
    std::vector<std::uint8_t> mips(ranges.mips.size);
    for (std::uint32_t slice = 0; slice < 6; ++slice)
        for (std::uint32_t mip = 0; mip <= info.maxMip; ++mip)
            fillGuestMip(mip == 0 ? base : mips, info, slice, mip, static_cast<std::uint8_t>(0x10 + slice * 0x20 + mip));
    UNSCOPED_INFO(error);
    REQUIRE(DecodeNativeTexture(descriptor, base, mips, texture, error));
    requireMipValues(texture, info, NativeDrawReplayTextureDimension::TextureCube);
    UNSCOPED_INFO("cube R8 format mapping");
    REQUIRE(texture.format == NativeDrawReplayTextureFormat::R8);
    requireVersionedDecode(descriptor, base, mips, texture);
}

void fullFormatMappings()
{
    const std::array<std::tuple<std::uint32_t, std::uint32_t, NativeDrawReplayTextureFormat>, 9> formats = {
        { { 2, 0, NativeDrawReplayTextureFormat::R8 },
          { 6, 2, NativeDrawReplayTextureFormat::Rgba8 },
          { 54, 0, NativeDrawReplayTextureFormat::Rgb10a2 },
          { 18, 1, NativeDrawReplayTextureFormat::Bc1 },
          { 19, 1, NativeDrawReplayTextureFormat::Bc2 },
          { 20, 1, NativeDrawReplayTextureFormat::Bc3 },
          { 32, 2, NativeDrawReplayTextureFormat::Rgba16Float },
          { 31, 1, NativeDrawReplayTextureFormat::Rg16Float },
          { 36, 2, NativeDrawReplayTextureFormat::R32Float } }
    };
    // Fetch selectors W, Z, Y and constant 0 compose with the SDK D3D12 host
    // format swizzle: RRRR for k_8 and k_32_FLOAT, RGGG for k_16_16_FLOAT.
    constexpr std::uint32_t kWzy0 = (3U | (2U << 3) | (1U << 6) | (4U << 9)) << 1;
    for (const auto [format, endian, expectedFormat] : formats)
    {
        auto descriptor                = fetch(format, endian, 4, 4);
        descriptor[3]                  = kWzy0;
        const auto                info = guestMipInfo(descriptor);
        NativeTextureMemoryRanges ranges;
        NativeDrawReplayTexture   texture;
        std::string               error;
        UNSCOPED_INFO(error);
        REQUIRE(GetNativeTextureMemoryRanges(descriptor, ranges, error));
        std::vector<std::uint8_t> base(ranges.base.size);
        fillGuestMip(base, info, 0, 0, 0xA5);
        UNSCOPED_INFO(error);
        REQUIRE(DecodeNativeTexture(descriptor, base, {}, texture, error));
        UNSCOPED_INFO("all native guest format mappings");
        REQUIRE(texture.format == expectedFormat);
        const bool single = expectedFormat == NativeDrawReplayTextureFormat::R8 || expectedFormat == NativeDrawReplayTextureFormat::R32Float;
        const auto host   = single ? std::array<std::uint8_t, 4>{ 0, 0, 0, 4 } : expectedFormat == NativeDrawReplayTextureFormat::Rg16Float ? std::array<std::uint8_t, 4>{ 1, 1, 1, 4 }
                                                                                                                                            : std::array<std::uint8_t, 4>{ 3, 2, 1, 4 };
        UNSCOPED_INFO("the fetch swizzle composes with the host format swizzle");
        REQUIRE(texture.swizzle == host);
        UNSCOPED_INFO("all native format bytes decode");
        REQUIRE(texture.bytes == std::vector<std::uint8_t>(mipPayloadBytes(info, 0), 0xA5));
        requireVersionedDecode(descriptor, base, {}, texture);
    }
}

// k_24_8 depth words (depth24 << 8 | stencil) against hand-computed
// UNorm24To32 values: n24 / 2^24, plus 2^-24 from bit 23 up, so 0xFFFFFF is 1.
struct DepthCase
{
    std::uint32_t depth;
    float         value;
};

constexpr std::array<DepthCase, 6> kDepthCases{ { { 0x000000, 0.0F },
                                                  { 0xFFFFFF, 1.0F },
                                                  { 0x800000, 0x1.000002p-1F },
                                                  { 0x7FFFFF, 0x1.fffffcp-2F },
                                                  { 0xC00000, 0x1.800002p-1F },
                                                  { 0x000001, 0x1p-24F } } };

// Stores the guest bytes of one k_24_8 word under the fetch's endian mode.
void storeDepthWord(std::vector<std::uint8_t>& source, std::size_t offset, std::uint32_t depth, rex::graphics::xenos::Endian endian)
{
    const std::uint32_t word = (depth << 8) | 0xA5;
    rex::graphics::texture_conversion::CopySwapBlock(endian, source.data() + offset, &word, 4);
}

float hostFloat(const NativeDrawReplayTexture& texture, std::size_t texel)
{
    float value;
    std::memcpy(&value, texture.bytes.data() + texel * 4, 4);
    return value;
}

void depthUnormTextures()
{
    namespace xenos = rex::graphics::xenos;
    {
        // Linear 8x2 base level: every texel holds its own case.
        const auto                descriptor = fetch(22, 2, 8, 2);
        const auto                info       = guestMipInfo(descriptor);
        NativeTextureMemoryRanges ranges;
        NativeDrawReplayTexture   texture;
        std::string               error;
        UNSCOPED_INFO(error);
        REQUIRE(GetNativeTextureMemoryRanges(descriptor, ranges, error));
        std::vector<std::uint8_t> base(ranges.base.size);
        for (std::uint32_t y = 0; y < 2; ++y)
            for (std::uint32_t x = 0; x < 8; ++x)
                storeDepthWord(base, std::size_t(y) * info.layout.base.row_pitch_bytes + x * 4, kDepthCases[(y * 8 + x) % 6].depth, info.fetch.endianness);
        UNSCOPED_INFO(error);
        REQUIRE(DecodeNativeTexture(descriptor, base, {}, texture, error));
        UNSCOPED_INFO("k_24_8 decodes to R32Float");
        REQUIRE((texture.format == NativeDrawReplayTextureFormat::R32Float && texture.bytes.size() == 64));
        for (std::size_t texel = 0; texel < 16; ++texel)
        {
            UNSCOPED_INFO(std::format("texel {} decodes depth {:06X} as UNorm24To32", texel, kDepthCases[texel % 6].depth));
            REQUIRE(hostFloat(texture, texel) == kDepthCases[texel % 6].value);
        }
        requireVersionedDecode(descriptor, base, {}, texture);
    }

    {
        // The pass-4 form: tiled, endian 2, a packed mip chain, swizzle XY11.
        auto descriptor = fetch(22, 2, 64, 32, true);
        descriptor[0]   = (descriptor[0] & ~(0x1FFU << 22)) | (2U << 22);
        descriptor[3]   = 0xB48U << 1;
        descriptor[5] |= 0x800;
        setMipRange(descriptor, 0x240, 6);
        const auto                info = guestMipInfo(descriptor);
        NativeTextureMemoryRanges ranges;
        NativeDrawReplayTexture   texture;
        std::string               error;
        UNSCOPED_INFO(error);
        REQUIRE(GetNativeTextureMemoryRanges(descriptor, ranges, error));
        std::vector<std::uint8_t> base(ranges.base.size);
        std::vector<std::uint8_t> mips(ranges.mips.size);
        // Uniform storage needs no tiled address model: the base holds depth
        // 0xFFFFFF and every packed mip word 0x40404040.
        for (std::size_t offset = 0; offset < base.size(); offset += 4)
            storeDepthWord(base, offset, kDepthCases[1].depth, info.fetch.endianness);
        std::ranges::fill(mips, std::uint8_t(0x40));
        UNSCOPED_INFO(error);
        REQUIRE(DecodeNativeTexture(descriptor, base, mips, texture, error));
        UNSCOPED_INFO("the packed tiled k_24_8 chain keeps every level and reads depth through R for X and Y");
        REQUIRE((texture.format == NativeDrawReplayTextureFormat::R32Float && texture.mipLevels == 7 &&
                 texture.swizzle == std::array<std::uint8_t, 4>{ 0, 0, 5, 5 }));
        std::size_t offset = 0;
        for (std::uint32_t mip = 0; mip <= info.maxMip; ++mip)
        {
            const auto  texels = mipPayloadBytes(info, mip) / 4;
            const float level  = mip == 0 ? 1.0F : xenos::UNorm24To32(0x404040);
            for (std::size_t texel = 0; texel < texels; ++texel)
            {
                UNSCOPED_INFO(std::format("mip {} texel {} decodes as depth", mip, texel));
                REQUIRE(hostFloat(texture, offset + texel) == level);
            }
            offset += texels;
        }
        UNSCOPED_INFO("the k_24_8 chain payload is tight");
        REQUIRE(offset * 4 == texture.bytes.size());
        requireVersionedDecode(descriptor, base, mips, texture);
    }
}

void versionedInPlaceDecode()
{
    const auto                descriptor = fetch(6, 0, 32, 32, true);
    NativeTextureMemoryRanges ranges;
    NativeDrawReplayTexture   texture, expected;
    std::string               error;
    UNSCOPED_INFO("a full tiled 32x32 RGBA8 payload is also a valid bounded guest source span");
    REQUIRE((GetNativeTextureMemoryRanges(descriptor, ranges, error) && ranges.base.size == 4096 && ranges.mips.size == 0));
    std::vector<std::uint8_t> base(ranges.base.size);
    for (std::size_t index = 0; index < base.size(); ++index)
        base[index] = static_cast<std::uint8_t>(index ^ (index >> 8));
    UNSCOPED_INFO(error);
    REQUIRE(DecodeNativeTextureVersioned(descriptor, base, {}, texture, error));
    const auto detachedSource = *texture.sharedBytes;
    UNSCOPED_INFO(error);
    REQUIRE(DecodeNativeTexture(descriptor, detachedSource, {}, expected, error));
    const std::weak_ptr<const NativeDrawReplayTextureVersion> originalVersion = texture.textureVersion;
    const std::weak_ptr<const std::vector<std::uint8_t>>      originalPayload = texture.sharedBytes;
    UNSCOPED_INFO("in-place versioned decoding retains its sole borrowed source until private staging is complete");
    REQUIRE((DecodeNativeTextureVersioned(descriptor, texture.payloadBytes(), {}, texture, error) &&
             originalVersion.expired() && originalPayload.expired() && texture.textureVersion &&
             std::ranges::equal(texture.payloadBytes(), expected.payloadBytes())));
}

void fullRejectedInputs()
{
    auto descriptor = fetch(6, 0, 32, 32);
    setMipRange(descriptor, 0x300, 2);
    NativeTextureMemoryRanges ranges;
    NativeDrawReplayTexture   texture;
    std::string               error;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRanges(descriptor, ranges, error));
    std::vector<std::uint8_t> base(ranges.base.size);
    std::vector<std::uint8_t> mips(ranges.mips.size);
    UNSCOPED_INFO("short mip source rejected before output");
    REQUIRE((!DecodeNativeTexture(descriptor, base, std::span<const std::uint8_t>(mips.data(), mips.size() - 1), texture, error) &&
             texture.bytes.empty()));
    UNSCOPED_INFO("failed versioned decode clears all unpublished ownership");
    REQUIRE((!DecodeNativeTextureVersioned(descriptor, base, std::span<const std::uint8_t>(mips.data(), mips.size() - 1), texture, error) &&
             !texture.textureVersion && !texture.sharedBytes && texture.bytes.empty()));
    descriptor[0] |= 1U << 2;
    UNSCOPED_INFO("signed texture rejected");
    REQUIRE((!GetNativeTextureMemoryRanges(descriptor, ranges, error) && ranges.base.size == 0 && ranges.mips.size == 0));
    descriptor[0] &= ~(1U << 2);
    descriptor[3] |= 1U << 13;
    UNSCOPED_INFO("exponent mode rejected");
    REQUIRE(!GetNativeTextureMemoryRanges(descriptor, ranges, error));
    descriptor    = fetch(6, 0, 64, 32);
    descriptor[0] = (descriptor[0] & ~(0x1FFU << 22)) | (2U << 22);
    descriptor[1] = (0x1FFFFU << 12) | 6U;
    UNSCOPED_INFO("physical range bound rejected");
    REQUIRE(!GetNativeTextureMemoryRanges(descriptor, ranges, error));
    descriptor = fetch(6, 0, 4096, 4096);
    UNSCOPED_INFO("row pitch and budget rejected");
    REQUIRE(!GetNativeTextureMemoryRanges(descriptor, ranges, error));
}

void signedFloatFetches()
{
    for (const auto format : { 31U, 32U, 36U })
    {
        auto descriptor = fetch(format, 2, 4, 4);
        descriptor[0] |= 0x55U << 2;
        descriptor[3] |= 1U;
        if (format == 32)
            setCube(descriptor);
        const auto                info = guestMipInfo(descriptor);
        NativeTextureMemoryRanges ranges;
        NativeDrawReplayTexture   texture;
        std::string               error;
        UNSCOPED_INFO(error);
        REQUIRE(GetNativeTextureMemoryRanges(descriptor, ranges, error));
        std::vector<std::uint8_t> base(ranges.base.size);
        for (std::uint32_t slice = 0; slice < info.arraySize; ++slice)
            fillGuestMip(base, info, slice, 0, static_cast<std::uint8_t>(0x10 + slice * 0x20));
        UNSCOPED_INFO(error);
        REQUIRE(DecodeNativeTexture(descriptor, base, {}, texture, error));
        requireMipValues(texture, info, format == 32 ? NativeDrawReplayTextureDimension::TextureCube : NativeDrawReplayTextureDimension::Texture2D);
        descriptor[0] = (descriptor[0] & ~(3U << 2)) | (2U << 2);
        UNSCOPED_INFO("biased float interpretation is unsupported");
        REQUIRE(!GetNativeTextureMemoryRanges(descriptor, ranges, error));
    }
    auto integerDescriptor = fetch(6, 0, 4, 4);
    integerDescriptor[3] |= 1U;
    NativeTextureMemoryRanges ranges;
    std::string               error;
    UNSCOPED_INFO("integer texture mode remains unsupported");
    REQUIRE(!GetNativeTextureMemoryRanges(integerDescriptor, ranges, error));
}

void creationWriterForms()
{
    using namespace rex::graphics::testing;
    for (bool floating : { false, true })
    {
        NativeTextureFetch descriptor = floating ? kFloatMatrixForms[0].fetch_words : kRgba8MatrixForms.back().fetch_words;
        SelectFloatMatrixForm(kFloatMatrixForms[0]);
        NativeTextureMemoryRanges ranges;
        std::string               error;
        UNSCOPED_INFO(error);
        REQUIRE(GetNativeTextureMemoryRanges(descriptor, ranges, error));
        UNSCOPED_INFO("creation writer exact full level range");
        REQUIRE((ranges.base.size == 1048576U && ranges.mips.size == 0));
        std::vector<std::uint8_t> guest(1048576), expected(1048576);
        const auto                extent = floating ? 512U : 1024U;
        for (std::uint32_t y = 0; y < extent; ++y)
            for (std::uint32_t x = 0; x < extent; ++x)
            {
                const auto linear = std::size_t(y) * extent + x;
                if (floating)
                {
                    std::uint32_t hash = x * 0x9E3779B1U ^ y * 0x85EBCA77U ^ 11U;
                    hash ^= hash >> 15U;
                    hash *= 0x2C1B3C6DU;
                    hash ^= hash >> 12U;
                    // Finite signed normals with all mantissa bits exercised.
                    const auto bits    = (hash & 0x807FFFFFU) | 0x3F000001U;
                    const auto address = FloatMatrixAddress(0, x, y);
                    for (unsigned c = 0; c < 4U; ++c)
                    {
                        expected[linear * 4U + c] = std::uint8_t(bits >> (8U * c));
                        guest[address + 3U - c]   = expected[linear * 4U + c];
                    }
                }
                else
                    guest[linear] = expected[linear] = std::uint8_t(x * 13U + y * 7U);
            }
        NativeDrawReplayTexture owner;
        UNSCOPED_INFO(error);
        REQUIRE(DecodeNativeTextureVersioned(descriptor, guest, {}, owner, error));
        UNSCOPED_INFO("creation writer all decoded bytes and selectors");
        REQUIRE((owner.format == (floating ? NativeDrawReplayTextureFormat::R32Float : NativeDrawReplayTextureFormat::R8) &&
                 owner.swizzle == (floating ? std::array<std::uint8_t, 4>{ 0, 5, 5, 5 } : std::array<std::uint8_t, 4>{ 4, 4, 4, 0 }) &&
                 std::ranges::equal(owner.payloadBytes(), expected)));
        const auto before = owner.textureVersion;
        guest[floating ? FloatMatrixAddress(0, 33, 31) : 1024U * 513U + 511U] ^= 1U;
        NativeDrawReplayTexture updated;
        UNSCOPED_INFO("creation writer mutation keeps prior immutable bytes");
        REQUIRE((DecodeNativeTextureVersioned(descriptor, guest, {}, updated, error) && updated.textureVersion != before &&
                 !std::ranges::equal(updated.payloadBytes(), expected) && std::ranges::equal(owner.payloadBytes(), expected)));
        for (const auto bytes : { 0U, 1048575U })
        {
            UNSCOPED_INFO("creation writer rejects incomplete full level");
            REQUIRE(!DecodeNativeTextureVersioned(descriptor, std::span(guest).first(bytes), {}, updated, error));
        }
        if (floating)
            for (auto sign : { 2U, 3U })
            {
                auto bad = descriptor;
                bad[0]   = (bad[0] & ~(3U << 2U)) | (sign << 2U);
                UNSCOPED_INFO("float writer rejects biased/gamma signs");
                REQUIRE(!DecodeNativeTextureVersioned(bad, guest, {}, updated, error));
            }
    }
}

void tiledWriterBcForms()
{
    using namespace rex::graphics::testing;
    const auto check = [&](const auto& form, auto blockOffset, std::uint32_t blockBytes)
    {
        auto descriptor = fetch(form.format, 0U, form.width, form.height, true);
        descriptor[0]   = (descriptor[0] & ~(0x1FFU << 22U)) | (form.pitch << 22U);
        descriptor[3]   = 0xD10U;
        if (form.packed)
            descriptor[5] |= 0x800U;
        if (form.max_level)
            setMipRange(descriptor, 0x200U, form.max_level);
        NativeTextureMemoryRanges ranges;
        NativeDrawReplayTexture   texture;
        std::string               error;
        UNSCOPED_INFO(std::string(form.name) + ": " + error);
        REQUIRE(GetNativeTextureMemoryRanges(descriptor, ranges, error));
        UNSCOPED_INFO("exact tiled writer allocation ranges");
        REQUIRE((ranges.base.size == form.base_extent && ranges.mips.size == form.mip_extent));
        std::vector<std::uint8_t> base(ranges.base.size, 0xD3U), mips(ranges.mips.size, 0x6BU), expected;
        for (std::uint32_t level = 0; level <= form.max_level; ++level)
        {
            auto&      storage = level == 0U ? base : mips;
            const auto width   = std::max(1U, (form.widths[level] + 3U) / 4U);
            const auto height  = std::max(1U, (form.heights[level] + 3U) / 4U);
            for (std::uint32_t y = 0; y < height; ++y)
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const auto at = blockOffset(level, x, y);
                    UNSCOPED_INFO("independent writer block range");
                    REQUIRE(at + blockBytes <= storage.size());
                    for (std::uint32_t byte = 0; byte < blockBytes; ++byte)
                    {
                        const auto value   = static_cast<std::uint8_t>(level * 37U + x * 13U + y * 19U + byte * 7U + (x >> 3U) * 11U);
                        storage[at + byte] = value;
                        expected.push_back(value);
                    }
                }
        }
        UNSCOPED_INFO(error);
        REQUIRE(DecodeNativeTexture(descriptor, base, mips, texture, error));
        UNSCOPED_INFO("all tiled writer blocks, packed levels and component selections decode exactly");
        REQUIRE((texture.bytes == expected && texture.mipLevels == form.max_level + 1U &&
                 texture.width == form.width && texture.height == form.height &&
                 texture.swizzle == std::array<std::uint8_t, 4>{ 0U, 1U, 2U, 3U }));
        requireVersionedDecode(descriptor, base, mips, texture);
        UNSCOPED_INFO("truncated tiled writer base rejects before publication");
        REQUIRE(!DecodeNativeTexture(descriptor, std::span<const std::uint8_t>(base).first(base.size() - 1U), mips, texture, error));
        if (!mips.empty())
        {
            UNSCOPED_INFO("truncated tiled writer tail rejects before publication");
            REQUIRE(!DecodeNativeTexture(descriptor, base, std::span<const std::uint8_t>(mips).first(mips.size() - 1U), texture, error));
        }
    };
    for (const auto& form : kDxtMatrixForms)
        if (form.tiled)
        {
            SelectDxtMatrixForm(form);
            check(form, DxtMatrixGuestBlockOffset, DxtMatrixBlockBytes());
        }
    for (const auto& form : kBc3MatrixForms)
        if (form.tiled)
        {
            SelectBc3MatrixForm(form);
            check(form, Bc3MatrixGuestBlockOffset, Bc3MatrixBlockBytes());
        }
}

void capturedFrameDescriptors(const std::filesystem::path& directory)
{
    const auto  manifest      = toml::parse_file((directory / "manifest.toml").string());
    const auto& textures      = *manifest["textures"].as_array();
    const auto& events        = *manifest["events"].as_array();
    const auto  readRegisters = [&](const toml::table& event)
    {
        const auto path = directory / event["registers"]["file"].value<std::string>().value();
        UNSCOPED_INFO("captured frame register extent");
        REQUIRE(std::filesystem::file_size(path) == 0x5003 * 4);
        std::vector<std::uint32_t> registers(0x5003);
        std::ifstream              file(path, std::ios::binary);
        UNSCOPED_INFO("frame registers readable");
        REQUIRE(bool(file.read(reinterpret_cast<char*>(registers.data()), registers.size() * 4)));
        return registers;
    };
    std::set<std::uint32_t> producedPages;
    for (const auto& node : events)
    {
        const auto& event = *node.as_table();
        if (event["kind"].value_or(std::string()) == "copy")
            producedPages.insert(readRegisters(event)[0x2319] >> 12);
    }
    std::set<NativeTextureFetch> checked;
    for (const auto& node : events)
    {
        const auto& event = *node.as_table();
        const auto* uses  = event["texture_uses"].as_array();
        if (!uses || uses->empty())
            continue;
        const auto registers = readRegisters(event);
        for (const auto& useNode : *uses)
        {
            const auto& use = *useNode.as_table();
            if (!use["required"].value_or(false) || !use["data_available"].value_or(false))
                continue;
            const auto& expected = *textures[use["texture_index"].value<std::size_t>().value()].as_table();
            const auto  page     = expected["guest_base_page"].value<std::uint32_t>().value();
            if (producedPages.contains(page))
                continue;
            const auto slot = use["fetch_constant"].value<std::uint32_t>().value();
            UNSCOPED_INFO("frame texture fetch ordinal");
            REQUIRE(slot < 32);
            NativeTextureFetch descriptor;
            std::copy_n(registers.begin() + 0x4800 + slot * 6, descriptor.size(), descriptor.begin());
            if (!checked.insert(descriptor).second)
                continue;
            NativeTextureMemoryRanges ranges;
            NativeDrawReplayTexture   actual;
            std::string               error;
            const auto                context   = "event " + std::to_string(event["id"].value<std::uint64_t>().value()) +
                                                  " texture fetch " + std::to_string(slot) + ": ";
            const bool                described = GetNativeTextureMemoryRanges(descriptor, ranges, error);
            UNSCOPED_INFO(context + error);
            REQUIRE(described);
            // Layout admission uses the actual guest descriptor. Zero source
            // bytes test extents only, not parity with captured host pixels.
            const std::vector<std::uint8_t> base(ranges.base.size), mips(ranges.mips.size);
            const bool                      decoded = DecodeNativeTexture(descriptor, base, mips, actual, error);
            UNSCOPED_INFO(context + error);
            REQUIRE(decoded);
            UNSCOPED_INFO(context + "guest and captured host texture extents differ");
            REQUIRE((ranges.base.address == page << 12 &&
                     actual.width == expected["host_width"].value<std::uint32_t>().value() &&
                     actual.height == expected["host_height"].value<std::uint32_t>().value() &&
                     actual.mipLevels == expected["host_mip_levels"].value<std::uint32_t>().value()));
            std::size_t payloadSize = 0;
            for (const auto& subresourceNode : *expected["subresources"].as_array())
            {
                const auto& subresource = *subresourceNode.as_table();
                payloadSize += subresource["row_size"].value<std::size_t>().value() *
                               subresource["row_count"].value<std::size_t>().value();
            }
            UNSCOPED_INFO(context + "captured subresource payload size differs");
            REQUIRE(actual.bytes.size() == payloadSize);
        }
    }
    UNSCOPED_INFO("frame CPU texture descriptors present");
    REQUIRE(!checked.empty());
    std::cout << "native full texture descriptors checked: " << checked.size() << '\n';
}

void rejectedInputs()
{
    NativeTextureMemoryRange range;
    std::string              error;
    auto                     descriptor = fetch(7, 0, 32, 32);
    UNSCOPED_INFO("unsupported format rejected");
    REQUIRE(!GetNativeTextureMemoryRange(descriptor, range, error));
    descriptor = fetch(6, 2, 32, 32, true);
    descriptor[5] |= 0x800;
    UNSCOPED_INFO("packed mip tail rejected");
    REQUIRE(!GetNativeTextureMemoryRange(descriptor, range, error));
    descriptor    = fetch(6, 2, 1024, 1024, true);
    descriptor[0] = 2U | (32U << 22) | 0x80000000;
    descriptor[1] = 0x1FFFF086;
    UNSCOPED_INFO("physical memory wrap rejected before source access");
    REQUIRE((!GetNativeTextureMemoryRange(descriptor, range, error) && range.size == 0));
}

void capturedDescriptors(const std::filesystem::path& directory)
{
    const auto  manifest = toml::parse_file((directory / "manifest.toml").string());
    const auto& textures = *manifest["textures"].as_array();
    std::size_t checked  = 0;
    for (const auto& eventNode : *manifest["events"].as_array())
    {
        const auto& event = *eventNode.as_table();
        const auto* uses  = event["texture_uses"].as_array();
        if (!uses || uses->empty())
            continue;
        const auto path = directory / event["registers"]["file"].value<std::string>().value();
        UNSCOPED_INFO("captured register extent");
        REQUIRE(std::filesystem::file_size(path) == 0x5003 * 4);
        std::vector<std::uint8_t> registers(0x5003 * 4);
        std::ifstream             file(path, std::ios::binary);
        UNSCOPED_INFO("captured registers readable");
        REQUIRE(bool(file.read(reinterpret_cast<char*>(registers.data()), registers.size())));
        for (const auto& useNode : *uses)
        {
            const auto& use = *useNode.as_table();
            if (!use["available"].value_or(false) || use["signed"].value_or(false))
                continue;
            const auto ordinal = use["fetch"].value<std::uint32_t>().value();
            UNSCOPED_INFO("texture fetch ordinal");
            REQUIRE(ordinal < 32);
            NativeTextureFetch descriptor;
            for (std::size_t i = 0; i < descriptor.size(); ++i)
            {
                const auto offset = (0x4800 + ordinal * 6 + i) * 4;
                descriptor[i]     = registers[offset] | (std::uint32_t(registers[offset + 1]) << 8) |
                                    (std::uint32_t(registers[offset + 2]) << 16) | (std::uint32_t(registers[offset + 3]) << 24);
            }
            NativeTextureMemoryRange range;
            NativeDrawReplayTexture  actual;
            std::string              error;
            UNSCOPED_INFO(error);
            REQUIRE(GetNativeTextureMemoryRange(descriptor, range, error));
            // This checks captured layout and binding metadata. Pixel fidelity
            // requires real guest source bytes from the matching draw boundary.
            const std::vector<std::uint8_t> source(range.size);
            UNSCOPED_INFO(error);
            REQUIRE(DecodeNativeTexture(descriptor, source, actual, error));
            const auto& expected = *textures[use["texture"].value<std::size_t>().value()].as_table();
            UNSCOPED_INFO("captured physical texture base");
            REQUIRE(range.address == (expected["guest_base_page"].value<std::uint32_t>().value() << 12));
            UNSCOPED_INFO("captured host extent and tight payload size");
            REQUIRE((actual.width == expected["width"].value<std::uint32_t>().value() &&
                     actual.height == expected["height"].value<std::uint32_t>().value() &&
                     actual.bytes.size() == expected["data"]["bytes"].value<std::size_t>().value()));
            auto mapping = use["component_mapping"].value<std::uint32_t>().value();
            for (std::size_t i = 0; i < 4; ++i)
            {
                UNSCOPED_INFO("captured host component mapping");
                REQUIRE(actual.swizzle[i] == ((mapping >> (3 * i)) & 7));
            }
            ++checked;
        }
    }
    UNSCOPED_INFO("captured texture bindings present");
    REQUIRE(checked > 0);
    std::cout << "native texture descriptors checked: " << checked << '\n';
}

} // namespace

TEST_CASE("native_texture_upload", "[ctest][native-texture-upload]")
{
    linearRgba();
    tiledRgba();
    compressedAndFont();
    fullPackedMipChain();
    fullUnpackedMipChain();
    fullCubeFaces();
    fullFormatMappings();
    depthUnormTextures();
    versionedInPlaceDecode();
    signedFloatFetches();
    creationWriterForms();
    tiledWriterBcForms();
    fullRejectedInputs();
    rejectedInputs();
}

// Checks saved texture descriptors: native-texture-upload.descriptors <capture dir>.
TEST_CASE("native_texture_upload_descriptors", "[.][manual][native-texture-upload]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass one capture directory through --helper native-texture-upload.descriptors");
    }
    UNSCOPED_INFO("pass exactly one capture directory");
    REQUIRE(args.argc() == 1);
    capturedDescriptors(args.argv()[1]);
}

// Checks saved frame descriptors: native-texture-upload.frame-descriptors <capture dir>.
TEST_CASE("native_texture_upload_frame_descriptors", "[.][manual][native-texture-upload]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass one capture directory through --helper native-texture-upload.frame-descriptors");
    }
    UNSCOPED_INFO("pass exactly one capture directory");
    REQUIRE(args.argc() == 1);
    capturedFrameDescriptors(args.argv()[1]);
}

REREVVED_MANUAL_HELPER(nativeTextureUploadDescriptors, "native-texture-upload.descriptors", "native_texture_upload_descriptors")
REREVVED_MANUAL_HELPER(nativeTextureUploadFrameDescriptors, "native-texture-upload.frame-descriptors", "native_texture_upload_frame_descriptors")
