#include "native_texture_upload.h"

#include <cstring>

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
    for (std::size_t i = 0; i < texture.swizzle.size(); ++i)
    {
        const auto selector = (fetch.swizzle >> (3 * i)) & 7;
        if (selector > 5)
            return fail("unsupported native texture component selector");
        // R8 supplies the same guest value for every data component. Constants
        // retain their selectors; the guest swizzle is composed exactly once.
        texture.swizzle[i] = static_cast<std::uint8_t>(
            texture.format == NativeDrawReplayTextureFormat::R8 && selector < 4 ? 0 : selector);
    }
    result.format        = rex::graphics::FormatInfo::Get(fetch.format);
    const auto layout    = util::GetGuestTextureLayout(fetch.dimension, fetch.pitch, width, height, 1, fetch.tiled, fetch.format, false, true, 0);
    result.rowPitch      = layout.base.row_pitch_bytes;
    result.rowBytes      = ((width + result.format->block_width - 1) / result.format->block_width) *
                           result.format->bytes_per_block();
    result.rows          = (height + result.format->block_height - 1) / result.format->block_height;
    result.range.address = basePage << 12;
    result.range.size    = layout.base.level_data_extent_bytes;
    if (!result.range.size || result.range.size > kMaxTextureBytes ||
        result.range.size > kPhysicalBytes - result.range.address ||
        result.rowPitch < result.rowBytes)
        return fail("native texture memory range is invalid");
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

} // namespace rerevved::gpu
