#include "guest_resolve_encoder.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <format>
#include <utility>

namespace rerevved::gpu::guest_d3d
{
namespace detail
{

// SDK e19035b: texture/util.cpp GetTiledOffset2D, specialized to 32 bpp.
std::uint32_t GuestResolveTiledOffset(std::uint32_t x, std::uint32_t y, std::uint32_t pitch)
{
    const auto tile = (y / 32) * ((pitch + 31) / 32) + x / 32;
    const auto bank = ((x / 8) + 2 * (y / 8)) & 3;
    return tile * 4096 + (y & 16) * 128 + (y & 1) * 16 +
           (y & 6) * 128 + (y & 8) * 128 + bank * 64 +
           (x & 3) * 4 + (x & 4) * 8;
}

// Applies the copy's red/blue swap and endian mode to a host word, in the SDK
// shader's order: swap first, then endian conversion.
std::uint32_t GuestResolveStoredWord(std::uint32_t word, GuestResolvePacking packing, std::uint32_t destInfo)
{
    if (destInfo & (1U << 24))
    {
        if (packing == GuestResolvePacking::Color2101010Words)
        {
            // SDK src/graphics/shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_cs.h:263-277:
            // the k_2_10_10_10 case (render target formats 2, 3, 10 and 12) keeps
            // 0xC00FFC00 and exchanges bits 0..9 with bits 20..29.
            word = (word & 0xC00FFC00U) | ((word & 0x3FF) << 20) | ((word >> 20) & 0x3FF);
        }
        else if (packing != GuestResolvePacking::Depth24Stencil8Words)
        {
            // Same file, :279-293 (formats 0 and 1): 0xFF00FF00 kept, bytes 0 and 2 exchanged.
            // Depth has nothing to swap: SDK src/graphics/util/draw.cpp:1107-1108
            // clears the swap for single-component formats.
            word = (word & 0xFF00FF00U) | ((word & 0xFF) << 16) | ((word >> 16) & 0xFF);
        }
    }
    const auto endian = destInfo & 7;
    if (endian == 1 || endian == 2)
    {
        word = ((word & 0x00FF00FFU) << 8) | ((word & 0xFF00FF00U) >> 8);
    }
    if (endian == 2 || endian == 3)
    {
        word = (word << 16) | (word >> 16);
    }
    return word;
}

bool PlaceGuestResolveWord(std::span<std::uint8_t> bytes, std::uint32_t x, std::uint32_t y, std::uint32_t pitch, std::uint32_t word, std::uint32_t destInfo, GuestResolvePacking packing)
{
    const auto offset = GuestResolveTiledOffset(x, y, pitch);
    if ((destInfo & 7) > 3 || offset > bytes.size() || bytes.size() - offset < 4)
    {
        return false;
    }
    word = GuestResolveStoredWord(word, packing, destInfo);
    for (unsigned c = 0; c < 4; ++c)
    {
        bytes[offset + c] = static_cast<std::uint8_t>(word >> (8 * c));
    }
    return true;
}

// Kept out of the public header; the oracle declares this production primitive.
bool PlaceGuestResolveWord(std::span<std::uint8_t> bytes, std::uint32_t x, std::uint32_t y, std::uint32_t pitch, std::uint32_t word, std::uint32_t destInfo)
{
    return PlaceGuestResolveWord(bytes, x, y, pitch, word, destInfo, GuestResolvePacking::Color8888Words);
}

} // namespace detail

namespace
{

// dxbc_translator_om.cpp packs k_8_8_8_8 RGBA as bits 0/8/16/24; packed/depth
// reads are R32_UINT native words.
static_assert(std::endian::native == std::endian::little, "host texels and guest words are copied as little-endian words");

// The red/blue exchange a copy applies, as GuestResolveStoredWord chooses it.
enum class ResolveSwap : std::uint8_t
{
    None,
    Bytes,     // 8888 and host RGBA8 words: bytes 0 and 2
    TenBitRgb, // k_2_10_10_10: bits 0..9 with bits 20..29
};

// GuestResolveStoredWord with the swap and endian mode fixed.
template <ResolveSwap Swap, unsigned Endian>
std::uint32_t storedWord(std::uint32_t word)
{
    if constexpr (Swap == ResolveSwap::TenBitRgb)
        word = (word & 0xC00FFC00U) | ((word & 0x3FF) << 20) | ((word >> 20) & 0x3FF);
    else if constexpr (Swap == ResolveSwap::Bytes)
        word = (word & 0xFF00FF00U) | ((word & 0xFF) << 16) | ((word >> 16) & 0xFF);
    if constexpr (Endian == 1 || Endian == 2)
        word = ((word & 0x00FF00FFU) << 8) | ((word & 0xFF00FF00U) >> 8);
    if constexpr (Endian == 2 || Endian == 3)
        word = (word << 16) | (word >> 16);
    return word;
}

// 32bpp encoding stores four texels per 16-byte group at aligned x/destX; only
// the width tail is single.
template <ResolveSwap Swap, unsigned Endian>
bool placeTexels(const GuestResolveCopy& copy, std::span<const std::uint8_t> hostTexels, std::uint32_t hostRowPitch, std::uint32_t pitch, std::span<std::uint8_t> bytes, std::uint8_t* touched)
{
    const std::uint32_t tilesPerRow = (pitch + 31) / 32, groups = copy.rectWidth / 4;
    for (std::uint32_t y = 0; y < copy.rectHeight; ++y)
    {
        const auto          gy   = copy.destY + y;
        const std::uint32_t row  = (gy / 32) * tilesPerRow * 4096 + (gy & 16) * 128 + (gy & 1) * 16 + (gy & 6) * 128 + (gy & 8) * 128;
        const std::uint32_t bank = 2 * (gy / 8);
        const auto*         line = hostTexels.data() + static_cast<std::size_t>(y) * hostRowPitch;
        const auto          at   = [&](std::uint32_t gx)
        {
            return static_cast<std::size_t>(row) + (gx / 32) * 4096 + (((gx / 8) + bank) & 3) * 64 + (gx & 3) * 4 + (gx & 4) * 8;
        };
        for (std::uint32_t group = 0; group < groups; ++group)
        {
            const auto offset = at(copy.destX + group * 4);
            if (offset + 16 > bytes.size())
                return false;
            std::uint32_t words[4];
            std::memcpy(words, line + group * 16, 16);
            for (auto& word : words)
                word = storedWord<Swap, Endian>(word);
            std::memcpy(bytes.data() + offset, words, 16);
            if (touched)
                std::memset(touched + offset / 4, 1, 4);
        }
        for (auto x = groups * 4; x < copy.rectWidth; ++x)
        {
            const auto offset = at(copy.destX + x);
            if (offset + 4 > bytes.size())
                return false;
            std::uint32_t word;
            std::memcpy(&word, line + x * 4, 4);
            word = storedWord<Swap, Endian>(word);
            std::memcpy(bytes.data() + offset, &word, 4);
            if (touched)
                touched[offset / 4] = 1;
        }
    }
    return true;
}

template <ResolveSwap Swap>
bool placeTexels(unsigned endian, const GuestResolveCopy& copy, std::span<const std::uint8_t> hostTexels, std::uint32_t hostRowPitch, std::uint32_t pitch, std::span<std::uint8_t> bytes, std::uint8_t* touched)
{
    switch (endian)
    {
        case 0:
            return placeTexels<Swap, 0>(copy, hostTexels, hostRowPitch, pitch, bytes, touched);
        case 1:
            return placeTexels<Swap, 1>(copy, hostTexels, hostRowPitch, pitch, bytes, touched);
        case 2:
            return placeTexels<Swap, 2>(copy, hostTexels, hostRowPitch, pitch, bytes, touched);
        default:
            return placeTexels<Swap, 3>(copy, hostTexels, hostRowPitch, pitch, bytes, touched);
    }
}

} // namespace

bool EncodeGuestResolve(const GuestResolveCopy& copy, std::span<const std::uint8_t> hostTexels, std::uint32_t hostRowPitch, GuestResolveImage& out, std::string& error)
{
    error.clear();
    const auto reject = [&error](std::string message)
    {
        error = std::move(message);
        return false;
    };
    const auto packing = copy.packing;
    const auto depth   = packing == GuestResolvePacking::Depth24Stencil8Words;
    if (copy.copyControl & ~0x00300377U)
    {
        return reject("reserved RB_COPY_CONTROL bits are unsupported");
    }
    // Source select 4 is the depth surface; 0..3 are the color targets.
    const auto source = copy.copyControl & 7;
    if (depth ? source != 4 : source >= 4)
    {
        return reject("resolve_source_select_unsupported");
    }
    if (((copy.copyControl >> 20) & 3) > 1)
    {
        return reject("only raw or convert copy commands are supported");
    }
    // The host already chose and averaged the samples; only the select's
    // consistency with the source is checked. k23 and k0123 stay refused.
    if (((copy.copyControl >> 4) & 7) > (depth ? 3U : 4U))
    {
        return reject("resolve_sample_select_unsupported");
    }
    if (copy.destInfo & ~0x013FFFFFU)
    {
        return reject("reserved RB_COPY_DEST_INFO bits are unsupported");
    }
    // 0x8269E520 maps 54->7, depth 22/23->6(0x8269E750..0x8269E7C0). SDK
    // depth:k_24_8 from RB_DEPTH_INFO(util/draw.cpp:929). Admit D24S8/fetch
    // k_24_8 only; 23 FLOAT unseen/refused.
    const auto destFormat  = (copy.destInfo >> 7) & 63;
    const auto fetchFormat = copy.destFetch[1] & 63;
    const bool halves      = packing == GuestResolvePacking::Float16x2Words;
    bool       admitted    = false;
    if (packing == GuestResolvePacking::Color2101010Words)
        admitted = destFormat == 7 && (fetchFormat == 7 || fetchFormat == 54);
    else if (halves)
        admitted = destFormat == 31 && fetchFormat == 31;
    else if (depth)
        admitted = destFormat == 6 && fetchFormat == 22;
    else
        admitted = destFormat == 6 && fetchFormat == 6;
    if (!admitted)
    {
        return reject(std::format("resolve_dest_format_unsupported: dest {} packing {}{}", destFormat, static_cast<unsigned>(packing), fetchFormat == destFormat ? std::string() : std::format(" fetch {}", fetchFormat)));
    }
    if (copy.destInfo & 0x78)
    {
        return reject("array and volume resolve destinations are unsupported");
    }
    // The fog copies (0x82C2BF90) of k_16_16 name number format 7 (float), as
    // their k_16_16_FLOAT fetch implies; the SDK scales by the exponent bias
    // before packing, and only bias 0 is modeled.
    if ((copy.destInfo >> 16) & 63)
    {
        return reject(std::format("resolve_exp_bias_unsupported: {}", (copy.destInfo >> 16) & 63));
    }
    if (((copy.destInfo >> 13) & 7) != (halves ? 7U : 0U))
    {
        return reject(std::format("resolve_number_format_unsupported: {} for dest {}", (copy.destInfo >> 13) & 7, destFormat));
    }
    // Swap exchanges red and blue before packing; a two-channel format would
    // lose red to the empty blue.
    if (halves && (copy.destInfo & (1U << 24)))
    {
        return reject("resolve_swap_unsupported: dest 31");
    }
    if ((copy.destInfo & 7) > 3)
    {
        return reject("64-bit and 128-bit endian modes are unsupported");
    }
    if ((copy.destFetch[0] & 3) != 2 || ((copy.destFetch[5] >> 9) & 3) != 1 ||
        (copy.destFetch[1] & (1U << 10)) || (copy.destFetch[2] >> 26))
    {
        return reject("destination fetch must describe a non-stacked 2D texture");
    }
    if (!(copy.destFetch[0] & 0x80000000U))
    {
        return reject("linear fetch destinations are unsupported by the tiled resolve path");
    }
    // A float fetch's sign and number format leave its halves unchanged: the
    // fog fetches name every component signed (0x154) and integer.
    if (halves ? (copy.destFetch[0] & 0x3FC) != 0 && (copy.destFetch[0] & 0x3FC) != 0x154 : (copy.destFetch[0] & 0x3FC) || (copy.destFetch[3] & 1))
    {
        return reject(std::format("resolve_fetch_sign_unsupported: {:08X} {:08X}", copy.destFetch[0], copy.destFetch[3]));
    }
    const auto width  = (copy.destFetch[2] & 8191) + 1;
    const auto height = ((copy.destFetch[2] >> 13) & 8191) + 1;
    // The resolve writes the base level, whose layout ignores the mip chain
    // unless the packed mip tail shares it: SDK texture/util.h
    // GetPackedMipLevel is 0 when the smaller dimension is at most 16.
    if ((copy.destFetch[4] & 0x3C) || ((copy.destFetch[5] & 0x800) && std::min(width, height) <= 16))
    {
        return reject("mipmapped or packed destination fetches are unsupported");
    }
    if ((copy.destInfo & 7) != ((copy.destFetch[1] >> 6) & 3))
    {
        return reject("copy and destination fetch endian modes disagree");
    }
    const auto pitch = copy.destPitch & 0x3FFF;
    if ((copy.destPitch & 0xC000C000U) || pitch != ((copy.destFetch[0] >> 22) & 511) * 32 ||
        ((copy.destPitch >> 16) & 0x3FFF) != height)
    {
        return reject("copy pitch or height disagrees with the destination fetch");
    }
    // The source origin is unused: hostTexels is already cropped to the rectangle.
    // Padding past the width or height is allowed and never written.
    if (pitch < width)
    {
        return reject("destination pitch is smaller than its width");
    }
    if (!copy.rectWidth || !copy.rectHeight || copy.destX >= width || copy.destY >= height ||
        copy.rectWidth > width - copy.destX || copy.rectHeight > height - copy.destY)
    {
        return reject("resolve rectangle does not fit the destination at its origin");
    }
    // SDK rounds to8 pixels(util/draw.cpp:865-868), writing unmodeled EDRAM.
    // Only destination right/bottom edges may misalign: expansion stays in
    // unsampled pitch/32-row padding, left unwritten.
    if (((copy.rectX | copy.rectY | copy.destX | copy.destY) & 7) || ((copy.rectWidth & 7) && copy.destX + copy.rectWidth != width) ||
        ((copy.rectHeight & 7) && copy.destY + copy.rectHeight != height))
    {
        return reject(std::format("resolve_rect_unaligned: {},{} {}x{} at {},{}", copy.rectX, copy.rectY, copy.rectWidth, copy.rectHeight, copy.destX, copy.destY));
    }
    // 0x8269E6DC..0x8269E714:(base&0x1FFFFFFF)+0x1000 if base>=0xE0000000. C10
    // depth A0000000/color E0000000 alias; C0000000 refused. See
    // src/system/xmemory.cpp PhysicalHeap::GetPhysicalAddress.
    auto fetchBase = copy.destFetch[1] & 0xFFFFF000U;
    if (fetchBase >= 0xE0000000U)
    {
        fetchBase = (fetchBase - 0xE0000000U) + 0x1000;
    }
    else if (fetchBase >= 0xA0000000U && fetchBase < 0xC0000000U)
    {
        fetchBase -= 0xA0000000U;
    }
    else if (fetchBase >= 0x20000000U)
    {
        return reject("destination fetch address is not physical or in the modeled A0000000 or E0000000 view");
    }
    if (copy.destBase != fetchBase || (copy.destBase & 4095))
    {
        return reject("copy base disagrees with the destination fetch physical base");
    }
    // SDK GetTiledAddressUpperBound2D adds one 4096-byte tile to the last tile
    // origin; here that is the tile holding the rectangle's last texel.
    const auto lastX  = copy.destX + copy.rectWidth - 1;
    const auto lastY  = copy.destY + copy.rectHeight - 1;
    const auto extent = detail::GuestResolveTiledOffset(lastX & ~31U, lastY & ~31U, pitch) + 4096;
    if (static_cast<std::uint64_t>(copy.destBase) + extent > 0x20000000ULL)
    {
        return reject("destination range exceeds guest physical memory");
    }
    if (hostRowPitch < copy.rectWidth * 4 ||
        static_cast<std::uint64_t>(copy.rectHeight - 1) * hostRowPitch + copy.rectWidth * 4 > hostTexels.size())
    {
        return reject("host readback pitch or byte span is too small");
    }
    GuestResolveImage image{ copy.destBase, std::vector<std::uint8_t>(extent), {} };
    // A rectangle of whole tile rows across the full pitch from the origin
    // writes every word of the extent, which needs no runs.
    const bool                whole = copy.destX == 0 && copy.destY == 0 && copy.rectWidth == pitch && copy.rectHeight % 32 == 0;
    std::vector<std::uint8_t> touched(whole ? 0 : extent / 4);
    const auto                swap   = !(copy.destInfo & (1U << 24))                       ? ResolveSwap::None
                                       : packing == GuestResolvePacking::Color2101010Words ? ResolveSwap::TenBitRgb
                                       : depth                                             ? ResolveSwap::None
                                                                                           : ResolveSwap::Bytes;
    const auto                endian = copy.destInfo & 7;
    const auto                marks  = whole ? nullptr : touched.data();
    const bool                placed = swap == ResolveSwap::None    ? placeTexels<ResolveSwap::None>(endian, copy, hostTexels, hostRowPitch, pitch, image.bytes, marks)
                                       : swap == ResolveSwap::Bytes ? placeTexels<ResolveSwap::Bytes>(endian, copy, hostTexels, hostRowPitch, pitch, image.bytes, marks)
                                                                    : placeTexels<ResolveSwap::TenBitRgb>(endian, copy, hostTexels, hostRowPitch, pitch, image.bytes, marks);
    if (!placed)
    {
        return reject("tiled word placement exceeds the destination range");
    }
    // Coalesce touched texels into maximal runs. A single run over the whole
    // image is the unpadded whole-destination case and carries no runs.
    for (std::uint32_t word = 0; word < touched.size();)
    {
        if (!touched[word])
        {
            ++word;
            continue;
        }
        const auto first = word;
        while (word < touched.size() && touched[word])
        {
            ++word;
        }
        image.runs.push_back({ first * 4, (word - first) * 4 });
    }
    if (image.runs.size() == 1 && image.runs[0].offset == 0 && image.runs[0].length == extent)
    {
        image.runs.clear();
    }
    out = std::move(image);
    return true;
}

} // namespace rerevved::gpu::guest_d3d
