#include "native_texture_upload.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>

namespace rerevved::gpu
{
namespace
{

namespace xenos = rex::graphics::xenos;
namespace util  = rex::graphics::texture_util;

constexpr std::uint32_t kMaxTextureBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kPhysicalBytes   = 512U * 1024U * 1024U;

struct Layout
{
    xenos::xe_gpu_texture_fetch_t    fetch{};
    NativeTextureMemoryRange         range;
    NativeDrawReplayTexture          texture;
    std::uint32_t                    rowPitch = 0;
    std::uint32_t                    rowBytes = 0;
    std::uint32_t                    rows     = 0;
    const rex::graphics::FormatInfo* format   = nullptr;
};

struct FullLayout
{
    xenos::xe_gpu_texture_fetch_t    fetch{};
    NativeTextureMemoryRanges        ranges;
    NativeDrawReplayTexture          texture;
    util::TextureGuestLayout         guest;
    const rex::graphics::FormatInfo* format    = nullptr;
    std::uint32_t                    width     = 0;
    std::uint32_t                    height    = 0;
    std::uint32_t                    arraySize = 0;
    std::uint32_t                    maxMip    = 0;
};

bool setTextureFormat(xenos::TextureFormat           format,
                      NativeDrawReplayTextureFormat& hostFormat,
                      std::string&                   error)
{
    switch (format)
    {
        case xenos::TextureFormat::k_8:
            hostFormat = NativeDrawReplayTextureFormat::R8;
            return true;
        case xenos::TextureFormat::k_8_8_8_8:
            hostFormat = NativeDrawReplayTextureFormat::Rgba8;
            return true;
        // The SDK D3D12 texture cache copies k_2_10_10_10_AS_16_16_16_16 words
        // unchanged into R10G10B10A2_UNORM and has no signed form of it.
        case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
            hostFormat = NativeDrawReplayTextureFormat::Rgb10a2;
            return true;
        case xenos::TextureFormat::k_DXT1:
            hostFormat = NativeDrawReplayTextureFormat::Bc1;
            return true;
        case xenos::TextureFormat::k_DXT2_3:
            hostFormat = NativeDrawReplayTextureFormat::Bc2;
            return true;
        case xenos::TextureFormat::k_DXT4_5:
            hostFormat = NativeDrawReplayTextureFormat::Bc3;
            return true;
        case xenos::TextureFormat::k_16_16_16_16_FLOAT:
            hostFormat = NativeDrawReplayTextureFormat::Rgba16Float;
            return true;
        case xenos::TextureFormat::k_16_16_FLOAT:
            hostFormat = NativeDrawReplayTextureFormat::Rg16Float;
            return true;
        case xenos::TextureFormat::k_32_FLOAT:
        case xenos::TextureFormat::k_24_8:
            hostFormat = NativeDrawReplayTextureFormat::R32Float;
            return true;
        default:
            error = "unsupported native texture format";
            return false;
    }
}

// The SDK D3D12 texture cache samples k_24_8 as R32_FLOAT through its
// DepthUnorm load shader: the 24-bit unorm depth in bits 8..31 of each
// endian-swapped word becomes xenos::UNorm24To32, and stencil is dropped.
void convertDepthUnorm(std::span<std::uint8_t> bytes)
{
    for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4)
    {
        std::uint32_t word;
        std::memcpy(&word, bytes.data() + offset, 4);
        const float depth = xenos::UNorm24To32(word >> 8);
        std::memcpy(bytes.data() + offset, &depth, 4);
    }
}

// Guest selectors 4/5 map to zero/one; selectors 6/7 are refused rather than
// clamped.
bool setTextureSwizzle(const xenos::xe_gpu_texture_fetch_t& fetch,
                       NativeDrawReplayTexture&             texture,
                       std::string&                         error)
{
    std::array<std::uint8_t, 4> host = { 0, 1, 2, 3 };
    if (texture.format == NativeDrawReplayTextureFormat::R8 || texture.format == NativeDrawReplayTextureFormat::R32Float)
        host = { 0, 0, 0, 0 };
    else if (texture.format == NativeDrawReplayTextureFormat::Rg16Float)
        host = { 0, 1, 1, 1 };
    for (std::size_t i = 0; i < texture.swizzle.size(); ++i)
    {
        const auto selector = (fetch.swizzle >> (3 * i)) & 7;
        if (selector > 5)
        {
            error = "unsupported native texture component selector";
            return false;
        }
        texture.swizzle[i] = selector < 4 ? host[selector] : static_cast<std::uint8_t>(selector);
    }
    return true;
}

bool setTextureMetadata(const xenos::xe_gpu_texture_fetch_t& fetch,
                        std::uint32_t                        width,
                        std::uint32_t                        height,
                        std::uint32_t                        mipLevels,
                        NativeDrawReplayTextureDimension     dimension,
                        NativeDrawReplayTexture&             texture,
                        std::string&                         error)
{
    if (!setTextureFormat(fetch.format, texture.format, error))
        return false;
    if (fetch.format == xenos::TextureFormat::k_8 && fetch.endianness != xenos::Endian::kNone)
    {
        error = "native R8 texture requires byte-addressed source data";
        return false;
    }
    texture.width     = width;
    texture.height    = height;
    texture.mipLevels = mipLevels;
    texture.dimension = dimension;
    return setTextureSwizzle(fetch, texture, error);
}

bool setRange(std::uint32_t page, std::uint32_t size, NativeTextureMemoryRange& range)
{
    range = {};
    if (!page)
        return size == 0;
    const auto address = std::uint64_t(page) << 12;
    if (!size || size > kMaxTextureBytes || address >= kPhysicalBytes || size > kPhysicalBytes - address)
        return false;
    range.address = static_cast<std::uint32_t>(address);
    range.size    = size;
    return true;
}

bool describe(const NativeTextureFetch& words, Layout& result, std::string& error)
{
    error.clear();
    const auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    auto& fetch = result.fetch;
    std::memcpy(&fetch, words.data(), sizeof(fetch));
    if (fetch.type != xenos::FetchConstantType::kTexture ||
        fetch.dimension != xenos::DataDimension::k2DOrStacked || fetch.stacked ||
        fetch.packed_mips || fetch.num_format || fetch.exp_adjust || fetch.force_bc_w_to_max ||
        util::SwizzleSigns(fetch) != 0)
        return fail("unsupported native texture fetch representation");

    std::uint32_t width, height, depth, basePage, mipPage, minMip, maxMip;
    util::GetSubresourcesFromFetchConstant(fetch, &width, &height, &depth, &basePage, &mipPage, &minMip, &maxMip);
    ++width;
    ++height;
    if (!basePage || minMip || depth || width > 4096 || height > 4096 || !fetch.pitch)
        return fail("unsupported native texture base level or extent");

    auto& texture = result.texture;
    switch (fetch.format)
    {
        case xenos::TextureFormat::k_8:
            if (fetch.endianness != xenos::Endian::kNone)
                return fail("native R8 texture requires byte-addressed source data");
            texture.format = NativeDrawReplayTextureFormat::R8;
            break;
        case xenos::TextureFormat::k_8_8_8_8:
            texture.format = NativeDrawReplayTextureFormat::Rgba8;
            break;
        case xenos::TextureFormat::k_DXT1:
            texture.format = NativeDrawReplayTextureFormat::Bc1;
            break;
        case xenos::TextureFormat::k_DXT2_3:
            texture.format = NativeDrawReplayTextureFormat::Bc2;
            break;
        default:
            return fail("unsupported native texture format");
    }
    texture.width  = width;
    texture.height = height;
    if (!setTextureSwizzle(fetch, texture, error))
        return false;
    result.format        = rex::graphics::FormatInfo::Get(fetch.format);
    const auto layout    = util::GetGuestTextureLayout(fetch.dimension, fetch.pitch, width, height, 1, fetch.tiled, fetch.format, false, true, 0);
    result.rowPitch      = layout.base.row_pitch_bytes;
    result.rowBytes      = ((width + result.format->block_width - 1) / result.format->block_width) *
                           result.format->bytes_per_block();
    result.rows          = (height + result.format->block_height - 1) / result.format->block_height;
    result.range.address = basePage << 12;
    result.range.size    = layout.base.level_data_extent_bytes;
    if (!result.range.size || result.range.size > kMaxTextureBytes ||
        result.range.size > kPhysicalBytes - result.range.address || result.rowPitch < result.rowBytes)
        return fail("native texture memory range is invalid");
    return true;
}

bool describeFull(const NativeTextureFetch& words, FullLayout& result, std::string& error)
{
    error.clear();
    result          = {};
    const auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    std::memcpy(&result.fetch, words.data(), sizeof(result.fetch));
    const auto& fetch       = result.fetch;
    const bool  floatFormat = fetch.format == xenos::TextureFormat::k_16_16_FLOAT ||
                              fetch.format == xenos::TextureFormat::k_16_16_16_16_FLOAT ||
                              fetch.format == xenos::TextureFormat::k_32_FLOAT;
    const auto  signs       = util::SwizzleSigns(fetch);
    // Float formats use identical signed and unsigned host views in the SDK.
    // Their stored IEEE values need no integer normalization; biased and gamma
    // signs still require conversions this upload path does not implement.
    const bool unsupportedNumber = floatFormat ? (signs & 0xAAU) != 0
                                               : fetch.num_format || signs != 0;
    if (fetch.type != xenos::FetchConstantType::kTexture ||
        (fetch.dimension != xenos::DataDimension::k2DOrStacked && fetch.dimension != xenos::DataDimension::kCube) ||
        fetch.stacked || unsupportedNumber || fetch.exp_adjust || fetch.force_bc_w_to_max)
        return fail("unsupported native texture fetch representation");
    if (fetch.dimension == xenos::DataDimension::kCube && fetch.size_2d.stack_depth != 5)
        return fail("native cube texture must have six faces");

    std::uint32_t width, height, depth, basePage, mipPage, minMip, maxMip;
    util::GetSubresourcesFromFetchConstant(fetch, &width, &height, &depth, &basePage, &mipPage, &minMip, &maxMip);
    ++width;
    ++height;
    if (!basePage || minMip || (fetch.dimension == xenos::DataDimension::k2DOrStacked && depth) ||
        (fetch.dimension == xenos::DataDimension::kCube && depth != 5) || width > 4096 || height > 4096 || !fetch.pitch)
        return fail("unsupported native texture extent or mip range");
    if (fetch.dimension == xenos::DataDimension::kCube && width != height)
        return fail("native cube textures must be square");

    const auto dimension = fetch.dimension == xenos::DataDimension::kCube ? NativeDrawReplayTextureDimension::TextureCube
                                                                          : NativeDrawReplayTextureDimension::Texture2D;
    if (!setTextureMetadata(fetch, width, height, maxMip + 1, dimension, result.texture, error))
        return false;

    result.width             = width;
    result.height            = height;
    result.maxMip            = maxMip;
    result.arraySize         = dimension == NativeDrawReplayTextureDimension::TextureCube ? 6 : 1;
    result.format            = rex::graphics::FormatInfo::Get(fetch.format);
    result.guest             = util::GetGuestTextureLayout(fetch.dimension, fetch.pitch, width, height, result.arraySize, fetch.tiled, fetch.format, fetch.packed_mips, true, maxMip);
    const auto validateLevel = [&](const util::TextureGuestLayout::Level& level)
    {
        const auto requiredRowBytes = std::uint64_t(level.x_extent_blocks) * result.format->bytes_per_block();
        return level.row_pitch_bytes && level.row_pitch_bytes % result.format->bytes_per_block() == 0 &&
               requiredRowBytes <= level.row_pitch_bytes && level.array_slice_stride_bytes &&
               level.array_slice_data_extent_bytes;
    };
    if (!validateLevel(result.guest.base))
        return fail("native texture base layout is invalid");
    if (maxMip)
    {
        const auto firstStoredMip = result.guest.packed_level == std::numeric_limits<std::uint32_t>::max()
                                        ? 1
                                        : result.guest.packed_level;
        for (auto level = firstStoredMip; level <= result.guest.packed_level && level <= maxMip; ++level)
            if (!validateLevel(result.guest.mips[level]))
                return fail("native texture mip layout is invalid");
    }
    if (!result.guest.base.level_data_extent_bytes ||
        !setRange(basePage, result.guest.base.level_data_extent_bytes, result.ranges.base))
        return fail("native texture base memory range is invalid");
    if (maxMip)
    {
        if (!mipPage || !result.guest.mips_total_extent_bytes ||
            !setRange(mipPage, result.guest.mips_total_extent_bytes, result.ranges.mips))
            return fail("native texture mip memory range is invalid");
    }
    else if (mipPage || result.guest.mips_total_extent_bytes)
    {
        return fail("native texture has an unexpected mip memory range");
    }
    return true;
}

struct SubresourceStorage
{
    const util::TextureGuestLayout::Level* level       = nullptr;
    std::uint32_t                          levelOffset = 0;
    std::uint32_t                          offsetX     = 0;
    std::uint32_t                          offsetY     = 0;
    bool                                   packed      = false;
};

bool getSubresourceStorage(const FullLayout&   layout,
                           std::uint32_t       mip,
                           SubresourceStorage& storage,
                           std::string&        error)
{
    const auto packedLevel = layout.guest.packed_level;
    storage                = {};
    if (mip == 0)
    {
        storage.level  = &layout.guest.base;
        storage.packed = packedLevel == 0;
    }
    else
    {
        storage.packed   = packedLevel != std::numeric_limits<std::uint32_t>::max() && mip >= packedLevel;
        const auto level = storage.packed ? packedLevel : mip;
        if (level >= std::size(layout.guest.mips))
        {
            error = "native texture mip level is outside the guest layout";
            return false;
        }
        storage.level       = &layout.guest.mips[level];
        storage.levelOffset = layout.guest.mip_offsets_bytes[level];
    }
    if (!storage.level || !storage.level->row_pitch_bytes || !storage.level->array_slice_stride_bytes ||
        !storage.level->array_slice_data_extent_bytes)
    {
        error = "native texture mip layout is empty";
        return false;
    }
    if (storage.packed)
    {
        std::uint32_t offsetZ;
        if (!util::GetPackedMipOffset(layout.width, layout.height, 1, layout.fetch.format, mip, storage.offsetX, storage.offsetY, offsetZ))
        {
            error = "native texture packed mip offset is unsupported";
            return false;
        }
    }
    return true;
}

struct MipExtent
{
    std::uint32_t width      = 0;
    std::uint32_t height     = 0;
    std::uint32_t blocksWide = 0;
    std::uint32_t blocksHigh = 0;
    std::uint32_t rowBytes   = 0;
    std::uint32_t bytes      = 0;
};

bool getMipExtent(const FullLayout& layout, std::uint32_t mip, MipExtent& extent, std::string& error)
{
    extent.width           = std::max(1U, layout.width >> mip);
    extent.height          = std::max(1U, layout.height >> mip);
    const auto blockWidth  = layout.format->block_width;
    const auto blockHeight = layout.format->block_height;
    const auto bytesBlock  = layout.format->bytes_per_block();
    extent.blocksWide      = (extent.width + blockWidth - 1) / blockWidth;
    extent.blocksHigh      = (extent.height + blockHeight - 1) / blockHeight;
    const auto rowBytes    = std::uint64_t(extent.blocksWide) * bytesBlock;
    const auto bytes       = rowBytes * extent.blocksHigh;
    if (rowBytes > std::numeric_limits<std::uint32_t>::max() ||
        bytes > std::numeric_limits<std::uint32_t>::max())
    {
        error = "native texture mip payload is too large";
        return false;
    }
    extent.rowBytes = static_cast<std::uint32_t>(rowBytes);
    extent.bytes    = static_cast<std::uint32_t>(bytes);
    return true;
}

bool checkSourceSlice(std::span<const std::uint8_t> source,
                      const SubresourceStorage&     storage,
                      std::uint32_t                 slice,
                      std::size_t&                  sourceOffset,
                      std::string&                  error)
{
    const auto offset = std::uint64_t(storage.levelOffset) +
                        std::uint64_t(slice) * storage.level->array_slice_stride_bytes;
    const auto extent = storage.level->array_slice_data_extent_bytes;
    if (offset > source.size() || extent > source.size() - offset)
    {
        error = "native texture source does not contain its declared subresource";
        return false;
    }
    sourceOffset = static_cast<std::size_t>(offset);
    return true;
}

bool decodeSubresource(const FullLayout&             layout,
                       const SubresourceStorage&     storage,
                       const MipExtent&              extent,
                       std::span<const std::uint8_t> source,
                       std::size_t                   sourceOffset,
                       std::span<std::uint8_t>       destination,
                       std::string&                  error)
{
    const auto bytesBlock = layout.format->bytes_per_block();
    if (destination.size() != extent.bytes || storage.level->row_pitch_bytes < extent.rowBytes ||
        storage.level->row_pitch_bytes % bytesBlock != 0)
    {
        error = "native texture subresource layout is invalid";
        return false;
    }
    if (layout.fetch.tiled)
    {
        rex::graphics::texture_conversion::UntileInfo info{};
        info.offset_x           = storage.offsetX;
        info.offset_y           = storage.offsetY;
        info.width              = extent.blocksWide;
        info.height             = extent.blocksHigh;
        info.input_pitch        = storage.level->row_pitch_bytes / bytesBlock;
        info.output_pitch       = extent.blocksWide;
        info.input_format_info  = layout.format;
        info.output_format_info = layout.format;
        info.copy_callback      = [&](void* destinationBytes, const void* input, std::size_t size)
        {
            rex::graphics::texture_conversion::CopySwapBlock(layout.fetch.endianness, destinationBytes, input, size);
        };
        rex::graphics::texture_conversion::Untile(destination.data(), source.data() + sourceOffset, &info);
        return true;
    }

    for (std::uint32_t row = 0; row < extent.blocksHigh; ++row)
    {
        const auto inputOffset = std::uint64_t(sourceOffset) +
                                 std::uint64_t(storage.offsetY + row) * storage.level->row_pitch_bytes +
                                 std::uint64_t(storage.offsetX) * bytesBlock;
        if (inputOffset > source.size() || extent.rowBytes > source.size() - inputOffset)
        {
            error = "native texture source does not contain its linear subresource row";
            return false;
        }
        rex::graphics::texture_conversion::CopySwapBlock(
            layout.fetch.endianness, destination.data() + std::size_t(row) * extent.rowBytes, source.data() + static_cast<std::size_t>(inputOffset), extent.rowBytes);
    }
    return true;
}

bool getFullPayloadSize(const FullLayout& layout, std::size_t& payloadSize, std::string& error)
{
    std::uint64_t total = 0;
    for (std::uint32_t slice = 0; slice < layout.arraySize; ++slice)
        for (std::uint32_t mip = 0; mip <= layout.maxMip; ++mip)
        {
            MipExtent extent;
            if (!getMipExtent(layout, mip, extent, error))
                return false;
            total += extent.bytes;
            if (total > kMaxTextureBytes)
            {
                error = "native texture payload exceeds the texture budget";
                return false;
            }
        }
    payloadSize = static_cast<std::size_t>(total);
    return true;
}

bool decodeFull(const FullLayout&             layout,
                std::span<const std::uint8_t> baseSource,
                std::span<const std::uint8_t> mipSource,
                NativeDrawReplayTexture&      texture,
                std::string&                  error)
{
    if (baseSource.size() != layout.ranges.base.size || mipSource.size() != layout.ranges.mips.size)
    {
        error = "native texture source does not match its declared memory ranges";
        return false;
    }

    std::size_t payloadSize = 0;
    if (!getFullPayloadSize(layout, payloadSize, error))
        return false;
    for (std::uint32_t slice = 0; slice < layout.arraySize; ++slice)
        for (std::uint32_t mip = 0; mip <= layout.maxMip; ++mip)
        {
            SubresourceStorage storage;
            if (!getSubresourceStorage(layout, mip, storage, error))
                return false;
            const auto  source       = mip == 0 ? baseSource : mipSource;
            std::size_t sourceOffset = 0;
            if (!checkSourceSlice(source, storage, slice, sourceOffset, error))
                return false;
        }

    auto decoded = layout.texture;
    decoded.bytes.resize(payloadSize);
    std::size_t outputOffset = 0;
    for (std::uint32_t slice = 0; slice < layout.arraySize; ++slice)
        for (std::uint32_t mip = 0; mip <= layout.maxMip; ++mip)
        {
            SubresourceStorage storage;
            MipExtent          extent;
            if (!getSubresourceStorage(layout, mip, storage, error) || !getMipExtent(layout, mip, extent, error))
                return false;
            const auto  source       = mip == 0 ? baseSource : mipSource;
            std::size_t sourceOffset = 0;
            if (!checkSourceSlice(source, storage, slice, sourceOffset, error) ||
                !decodeSubresource(layout, storage, extent, source, sourceOffset, std::span<std::uint8_t>(decoded.bytes.data() + outputOffset, extent.bytes), error))
                return false;
            outputOffset += extent.bytes;
        }
    if (layout.fetch.format == xenos::TextureFormat::k_24_8)
        convertDepthUnorm(decoded.bytes);
    texture = std::move(decoded);
    return true;
}

} // namespace

bool GetNativeTextureMemoryRange(const NativeTextureFetch& fetch,
                                 NativeTextureMemoryRange& range,
                                 std::string&              error)
{
    range = {};
    Layout layout;
    if (!describe(fetch, layout, error))
        return false;
    range = layout.range;
    return true;
}

bool DecodeNativeTexture(const NativeTextureFetch&     fetch,
                         std::span<const std::uint8_t> source,
                         NativeDrawReplayTexture&      texture,
                         std::string&                  error)
{
    texture = {};
    Layout layout;
    if (!describe(fetch, layout, error))
        return false;
    if (source.size() != layout.range.size)
    {
        error = "native texture source does not match its declared memory range";
        return false;
    }
    auto& bytes = layout.texture.bytes;
    bytes.resize(std::size_t(layout.rowBytes) * layout.rows);
    if (layout.fetch.tiled)
    {
        rex::graphics::texture_conversion::UntileInfo info{};
        info.width              = layout.rowBytes / layout.format->bytes_per_block();
        info.height             = layout.rows;
        info.input_pitch        = layout.rowPitch / layout.format->bytes_per_block();
        info.output_pitch       = info.width;
        info.input_format_info  = layout.format;
        info.output_format_info = layout.format;
        info.copy_callback      = [&](void* destination, const void* input, std::size_t size)
        {
            rex::graphics::texture_conversion::CopySwapBlock(layout.fetch.endianness, destination, input, size);
        };
        rex::graphics::texture_conversion::Untile(bytes.data(), source.data(), &info);
    }
    else
    {
        for (std::uint32_t row = 0; row < layout.rows; ++row)
            rex::graphics::texture_conversion::CopySwapBlock(
                layout.fetch.endianness, bytes.data() + std::size_t(row) * layout.rowBytes, source.data() + std::size_t(row) * layout.rowPitch, layout.rowBytes);
    }
    texture = std::move(layout.texture);
    return true;
}

bool GetNativeTextureMemoryRanges(const NativeTextureFetch&  fetch,
                                  NativeTextureMemoryRanges& ranges,
                                  std::string&               error)
{
    ranges = {};
    FullLayout layout;
    if (!describeFull(fetch, layout, error))
        return false;
    ranges = layout.ranges;
    return true;
}

bool DecodeNativeTexture(const NativeTextureFetch&     fetch,
                         std::span<const std::uint8_t> baseSource,
                         std::span<const std::uint8_t> mipSource,
                         NativeDrawReplayTexture&      texture,
                         std::string&                  error)
{
    texture = {};
    FullLayout layout;
    if (!describeFull(fetch, layout, error))
        return false;
    return decodeFull(layout, baseSource, mipSource, texture, error);
}

bool DecodeNativeTextureVersioned(const NativeTextureFetch&     fetch,
                                  std::span<const std::uint8_t> baseSource,
                                  std::span<const std::uint8_t> mipSource,
                                  NativeDrawReplayTexture&      texture,
                                  std::string&                  error)
{
    NativeDrawReplayTexture staging;
    if (!DecodeNativeTexture(fetch, baseSource, mipSource, staging, error) ||
        !NativeDrawReplayTextureVersion::AdoptStaging(staging, error))
    {
        texture = {};
        return false;
    }
    texture = std::move(staging);
    return true;
}

} // namespace rerevved::gpu
