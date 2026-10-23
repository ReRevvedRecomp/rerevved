#include "gpu/guest_d3d/guest_resolve_encoder.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace rerevved::gpu::guest_d3d::detail
{

// Test-only declarations keep the public encoder contract independent of the oracle.
std::uint32_t GuestResolveTiledOffset(std::uint32_t x, std::uint32_t y, std::uint32_t pitch);
bool          PlaceGuestResolveWord(std::span<std::uint8_t> bytes, std::uint32_t x, std::uint32_t y, std::uint32_t pitch, std::uint32_t word, std::uint32_t destInfo);
bool          PlaceGuestResolveWord(std::span<std::uint8_t> bytes, std::uint32_t x, std::uint32_t y, std::uint32_t pitch, std::uint32_t word, std::uint32_t destInfo, GuestResolvePacking packing);

} // namespace rerevved::gpu::guest_d3d::detail

using namespace rerevved::gpu::guest_d3d;

namespace
{

// SDK e19035b src/graphics/pipeline/texture/util.cpp:424, generic 2D formula.
std::uint32_t referenceOffset(std::uint32_t x, std::uint32_t y, std::uint32_t pitch)
{
    constexpr unsigned logBpp = 2;
    pitch                     = (pitch + 31) & ~31U;
    const auto macro          = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (logBpp + 7);
    const auto micro          = ((x & 7) + ((y & 14) << 2)) << logBpp;
    const auto offset         = macro + ((micro & ~15U) << 1) + (micro & 15) + ((y & 1) << 4);
    return ((offset & ~511U) << 3) + ((y & 16) << 7) + ((offset & 448) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63);
}

std::uint32_t referenceExtent(std::uint32_t width, std::uint32_t height, std::uint32_t pitch)
{
    return referenceOffset((width - 1) & ~31U, (height - 1) & ~31U, pitch) + 4096;
}

// Memory byte c of a stored word holds word byte endianBytes[endian][c].
constexpr std::array<std::array<unsigned, 4>, 4> endianBytes{ { { 0, 1, 2, 3 }, { 1, 0, 3, 2 }, { 3, 2, 1, 0 }, { 2, 3, 0, 1 } } };

// The destination fetch format of each packing.
std::uint32_t formatFor(GuestResolvePacking packing)
{
    switch (packing)
    {
        case GuestResolvePacking::Color2101010Words:
            return 7;
        case GuestResolvePacking::Depth24Stencil8Words:
            return 22;
        case GuestResolvePacking::Float16x2Words:
            return 31;
        default:
            return 6;
    }
}

// The copy number format: 7 (float) for k_16_16_FLOAT, as the fog copies name it.
std::uint32_t numberFor(GuestResolvePacking packing)
{
    return packing == GuestResolvePacking::Float16x2Words ? 7U << 13 : 0;
}

// The copy format Resolve (0x8269E7B0..0x8269E7C0) derives: depth copies as 6.
std::uint32_t copyFormatFor(GuestResolvePacking packing)
{
    return packing == GuestResolvePacking::Depth24Stencil8Words ? 6 : formatFor(packing);
}

GuestResolveCopy wholeCopy(std::uint32_t width, std::uint32_t height, GuestResolvePacking packing = GuestResolvePacking::HostRgba8)
{
    const auto       format = formatFor(packing);
    GuestResolveCopy copy;
    copy.copyControl = packing == GuestResolvePacking::Depth24Stencil8Words ? 0x00100004 : 0x00100000;
    copy.destBase    = 0x01000000;
    copy.destPitch   = (height << 16) | width;
    copy.destInfo    = (packing == GuestResolvePacking::Float16x2Words ? 0 : 0x01000000) | numberFor(packing) | (copyFormatFor(packing) << 7);
    copy.destFetch   = { 0x80000002U | ((width / 32) << 22), copy.destBase | format, (width - 1) | ((height - 1) << 13), 0xC14, 0, 0x200 };
    copy.rectWidth   = width;
    copy.rectHeight  = height;
    copy.packing     = packing;
    return copy;
}

// Sets the endian mode in both the copy and the fetch, and the red/blue swap bit.
void setEndianSwap(GuestResolveCopy& copy, unsigned endian, unsigned swap)
{
    const auto format = formatFor(copy.packing);
    copy.destInfo     = numberFor(copy.packing) | (copyFormatFor(copy.packing) << 7) | endian | (swap << 24);
    copy.destFetch[1] = copy.destBase | format | (endian << 6);
}

// The per-texel encoder production used before its grouped path, kept as the
// oracle that path must match byte for byte, refusals included.
bool scalarEncode(const GuestResolveCopy& copy, std::span<const std::uint8_t> hostTexels, std::uint32_t hostRowPitch, GuestResolveImage& out, std::string& error)
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
    // Resolve 0x8269E520 derives formats at 0x8269E750..0x8269E7C0: 54
    // (k_2_10_10_10_AS_16_16_16_16) maps to 7; 22/23 map to 6 by D3D9
    // convention.
    const auto destFormat  = (copy.destInfo >> 7) & 63;
    const auto fetchFormat = copy.destFetch[1] & 63;
    const bool halves      = packing == GuestResolvePacking::Float16x2Words;
    bool       admitted    = false;
    if (packing == GuestResolvePacking::Color2101010Words)
        admitted = destFormat == 7 && (fetchFormat == 7 || fetchFormat == 54);
    else if (halves)
        admitted = destFormat == 31 && fetchFormat == 31;
    // SDK depth uses k_24_8 from RB_DEPTH_INFO (util/draw.cpp:929);
    // k_24_8_FLOAT (23) stays refused until seen.
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
    // SDK writes 8-pixel expansions (util/draw.cpp:865-868); outside readback
    // is unmodeled EDRAM. Only destination-ending right/bottom edges may
    // misalign: pitch/32-row tiles leave unread, unwritten padding.
    if (((copy.rectX | copy.rectY | copy.destX | copy.destY) & 7) || ((copy.rectWidth & 7) && copy.destX + copy.rectWidth != width) ||
        ((copy.rectHeight & 7) && copy.destY + copy.rectHeight != height))
    {
        return reject(std::format("resolve_rect_unaligned: {},{} {}x{} at {},{}", copy.rectX, copy.rectY, copy.rectWidth, copy.rectHeight, copy.destX, copy.destY));
    }
    // 0x8269E6DC..0x8269E714 maps fetch dword 1 from 0xE0000000 via (base &
    // 0x1FFFFFFF)+0x1000; src/system/xmemory.cpp
    // PhysicalHeap::GetPhysicalAddress aliases A0000000/E0000000.
    auto fetchBase = copy.destFetch[1] & 0xFFFFF000U;
    if (fetchBase >= 0xE0000000U)
    {
        fetchBase = (fetchBase - 0xE0000000U) + 0x1000;
    }
    else if (fetchBase >= 0xA0000000U && fetchBase < 0xC0000000U)
    {
        fetchBase -= 0xA0000000U;
    }
    // C0000000 stays refused until seen.
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
    GuestResolveImage         image{ copy.destBase, std::vector<std::uint8_t>(extent), {} };
    std::vector<std::uint8_t> touched(extent / 4);
    for (std::uint32_t y = 0; y < copy.rectHeight; ++y)
    {
        for (std::uint32_t x = 0; x < copy.rectWidth; ++x)
        {
            const auto offset = static_cast<std::size_t>(y) * hostRowPitch + x * 4;
            // mapTargetFormat uses R8G8B8A8_UNORM. SDK dxbc_translator_om.cpp
            // packs k_8_8_8_8 RGBA at bits 0, 8, 16, 24 respectively; packed-word
            // and depth readbacks are R32_UINT, the same little-endian layout.
            const auto word = std::uint32_t(hostTexels[offset]) | (std::uint32_t(hostTexels[offset + 1]) << 8) |
                              (std::uint32_t(hostTexels[offset + 2]) << 16) | (std::uint32_t(hostTexels[offset + 3]) << 24);
            if (!detail::PlaceGuestResolveWord(image.bytes, copy.destX + x, copy.destY + y, pitch, word, copy.destInfo, packing))
            {
                return reject("tiled word placement exceeds the destination range");
            }
            touched[detail::GuestResolveTiledOffset(copy.destX + x, copy.destY + y, pitch) / 4] = 1;
        }
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

// A copy of a width x height destination with `pitch`, as wholeCopy builds it.
GuestResolveCopy shapedCopy(std::uint32_t width, std::uint32_t height, std::uint32_t pitch, GuestResolvePacking packing)
{
    auto copy         = wholeCopy(pitch, height, packing);
    copy.destFetch[2] = (width - 1) | ((height - 1) << 13);
    copy.rectWidth    = width;
    return copy;
}

// Host texels of every bit pattern, from a fixed xorshift sequence.
std::vector<std::uint8_t> noiseTexels(std::size_t size, std::uint32_t seed)
{
    std::vector<std::uint8_t> bytes(size);
    for (auto& byte : bytes)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        byte = static_cast<std::uint8_t>(seed >> 24);
    }
    return bytes;
}

// Runs both encoders on one input and requires the same verdict, error,
// bytes and runs; returns whether it encoded.
bool matchesScalar(const GuestResolveCopy& copy, std::span<const std::uint8_t> host, std::uint32_t rowPitch, const std::string& what)
{
    GuestResolveImage fast, slow;
    fast.physical = slow.physical = 0xDEAD;
    std::string fastError, slowError;
    const bool  fastDone = EncodeGuestResolve(copy, host, rowPitch, fast, fastError);
    const bool  slowDone = scalarEncode(copy, host, rowPitch, slow, slowError);
    UNSCOPED_INFO(std::format("{}: verdicts differ ({} '{}' against {} '{}')", what, fastDone, fastError, slowDone, slowError));
    REQUIRE((fastDone == slowDone && fastError == slowError));
    UNSCOPED_INFO(what + ": bytes differ from the scalar encoder");
    REQUIRE((fast.physical == slow.physical && fast.bytes == slow.bytes));
    UNSCOPED_INFO(what + ": runs differ from the scalar encoder");
    REQUIRE((fast.runs.size() == slow.runs.size() && std::equal(fast.runs.begin(), fast.runs.end(), slow.runs.begin(), [](const GuestResolveRun& a, const GuestResolveRun& b)
                                                                {
                                                                    return a.offset == b.offset && a.length == b.length;
                                                                })));
    return fastDone;
}

// Grouped/scalar encoders match bytes and refusal text across every form.
void groupedMatchesScalar()
{
    struct Shape
    {
        const char*   name;
        std::uint32_t width, height, pitch, rectX, rectY, rectWidth, rectHeight;
    };

    const std::array<Shape, 11> shapes{ {
        { "whole", 64, 64, 64, 0, 0, 64, 64 },
        { "whole 1280x720", 1280, 720, 1280, 0, 0, 1280, 720 },
        { "padded pitch", 72, 40, 96, 0, 0, 72, 40 },
        { "offset", 128, 96, 128, 24, 16, 40, 48 },
        { "width edge", 100, 64, 128, 64, 8, 36, 40 },
        { "height edge", 64, 70, 64, 8, 64, 48, 6 },
        { "both edges", 45, 37, 64, 40, 32, 5, 5 },
        { "one texel", 1, 1, 32, 0, 0, 1, 1 },
        { "three-texel tail", 103, 40, 128, 64, 0, 39, 40 },
        { "two-texel tail", 30, 9, 32, 0, 0, 30, 9 },
        { "whole tile rows short of the pitch", 64, 64, 96, 0, 0, 64, 64 },
    } };
    unsigned                    encoded = 0;
    for (const auto packing : { GuestResolvePacking::HostRgba8, GuestResolvePacking::Color8888Words, GuestResolvePacking::Color2101010Words, GuestResolvePacking::Depth24Stencil8Words, GuestResolvePacking::Float16x2Words })
        for (unsigned endian = 0; endian < 4; ++endian)
            for (unsigned swap = 0; swap < 2; ++swap)
                for (const auto& shape : shapes)
                {
                    auto copy = shapedCopy(shape.width, shape.height, shape.pitch, packing);
                    setEndianSwap(copy, endian, swap);
                    copy.destX          = shape.rectX;
                    copy.destY          = shape.rectY;
                    copy.rectWidth      = shape.rectWidth;
                    copy.rectHeight     = shape.rectHeight;
                    const auto rowPitch = shape.rectWidth * 4 + 12;
                    const auto host     = noiseTexels(static_cast<std::size_t>(rowPitch) * shape.rectHeight, 0x9E3779B9U ^ (endian * 977 + swap * 131 + shape.width));
                    encoded += matchesScalar(copy, host, rowPitch, std::format("{} packing {} endian {} swap {}", shape.name, static_cast<unsigned>(packing), endian, swap));
                }
    // Swap refuses halves, so 5 packings x 4 endians x 2 swaps x 11 shapes less the 44 swapped halves.
    UNSCOPED_INFO(std::format("{} inputs encoded", encoded));
    REQUIRE(encoded == 5 * 4 * 2 * 11 - 4 * 11);

    const auto base = []
    {
        auto copy = shapedCopy(100, 64, 128, GuestResolvePacking::Color8888Words);
        setEndianSwap(copy, 2, 1);
        copy.destX     = 64;
        copy.rectWidth = 36;
        return copy;
    };
    const auto                                                                           host = noiseTexels(36 * 4 * 64, 7);
    const std::array<std::pair<const char*, std::function<void(GuestResolveCopy&)>>, 20> refusals{ {
        { "reserved control", [](auto& c)
          {
              c.copyControl |= 1U << 3;
          } },
        { "depth source for color", [](auto& c)
          {
              c.copyControl |= 4;
          } },
        { "clear command", [](auto& c)
          {
              c.copyControl |= 2U << 20;
          } },
        { "sample select", [](auto& c)
          {
              c.copyControl |= 5U << 4;
          } },
        { "reserved dest info", [](auto& c)
          {
              c.destInfo |= 1U << 25;
          } },
        { "format mismatch", [](auto& c)
          {
              c.destFetch[1] = (c.destFetch[1] & ~63U) | 7;
          } },
        { "array destination", [](auto& c)
          {
              c.destInfo |= 8;
          } },
        { "exponent bias", [](auto& c)
          {
              c.destInfo |= 1U << 16;
          } },
        { "number format", [](auto& c)
          {
              c.destInfo |= 1U << 13;
          } },
        { "64-bit endian", [](auto& c)
          {
              c.destInfo = (c.destInfo & ~7U) | 4;
          } },
        { "linear fetch", [](auto& c)
          {
              c.destFetch[0] &= ~0x80000000U;
          } },
        { "signed fetch", [](auto& c)
          {
              c.destFetch[0] |= 1U << 2;
          } },
        { "mipmapped fetch", [](auto& c)
          {
              c.destFetch[4] |= 1U << 2;
          } },
        { "endian disagrees", [](auto& c)
          {
              c.destFetch[1] ^= 1U << 6;
          } },
        { "pitch disagrees", [](auto& c)
          {
              c.destPitch += 32;
          } },
        { "rectangle overflows", [](auto& c)
          {
              c.rectWidth = 44;
          } },
        { "unaligned origin", [](auto& c)
          {
              c.destX = 60;
          } },
        { "base disagrees", [](auto& c)
          {
              c.destBase += 0x1000;
          } },
        { "stacked fetch", [](auto& c)
          {
              c.destFetch[1] |= 1U << 10;
          } },
        { "empty rectangle", [](auto& c)
          {
              c.rectHeight = 0;
          } },
    } };
    for (const auto& [name, mutate] : refusals)
    {
        auto copy = base();
        mutate(copy);
        UNSCOPED_INFO(std::string(name) + " is refused");
        REQUIRE(!matchesScalar(copy, host, 36 * 4, name));
    }
    UNSCOPED_INFO("a short host span is refused");
    REQUIRE(!matchesScalar(base(), std::span(host).first(host.size() - 1), 36 * 4, "short host span"));
    UNSCOPED_INFO("a short host pitch is refused");
    REQUIRE(!matchesScalar(base(), host, 35 * 4, "short host pitch"));
    std::cout << "The grouped encoder matches the scalar encoder over " << encoded << " inputs and " << refusals.size() + 2 << " refusals\n";
}

// Both encoders time the world 1280x720 k_8_8_8_8 destination.
void encodeTiming()
{
    // Copy info 0x01000300: red/blue swap, endian 0.
    auto copy = shapedCopy(1280, 720, 1280, GuestResolvePacking::Color8888Words);
    setEndianSwap(copy, 0, 1);
    const auto host = noiseTexels(1280 * 4 * 720, 11);
    const auto time = [&](auto&& encode)
    {
        double best = 1e30;
        for (int pass = 0; pass < 5; ++pass)
        {
            GuestResolveImage image;
            std::string       error;
            const auto        started = std::chrono::steady_clock::now();
            UNSCOPED_INFO(error);
            REQUIRE(encode(copy, host, 1280 * 4, image, error));
            best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        }
        return best;
    };
    const auto fast = time([](auto&&... a)
                           {
                               return EncodeGuestResolve(a...);
                           });
    const auto slow = time([](auto&&... a)
                           {
                               return scalarEncode(a...);
                           });
    std::cout << std::format("Encode timing 1280x720: grouped {:.2f} ms, scalar {:.2f} ms (best of 5)\n", fast, slow);
}

void addressingAndChannels()
{
    for (const auto size : { std::array{ 1U, 1U, 32U }, std::array{ 8U, 8U, 32U }, std::array{ 31U, 33U, 64U }, std::array{ 64U, 96U, 64U }, std::array{ 80U, 17U, 96U }, std::array{ 640U, 720U, 1280U }, std::array{ 1024U, 1024U, 1024U } })
    {
        const auto        extent = referenceExtent(size[0], size[1], size[2]);
        std::vector<bool> visited(extent / 4);
        for (unsigned y = 0; y < size[1]; ++y)
        {
            for (unsigned x = 0; x < size[0]; ++x)
            {
                const auto offset = detail::GuestResolveTiledOffset(x, y, size[2]);
                UNSCOPED_INFO("tiled offset differs from SDK reference");
                REQUIRE(offset == referenceOffset(x, y, size[2]));
                UNSCOPED_INFO("tiled addresses collide or exceed the SDK extent");
                REQUIRE((offset % 4 == 0 && offset < extent && !visited[offset / 4]));
                visited[offset / 4] = true;
            }
        }
    }
    for (unsigned endian = 0; endian < 4; ++endian)
    {
        for (unsigned swap = 0; swap < 2; ++swap)
        {
            auto copy     = wholeCopy(64, 32);
            copy.destInfo = 0x300 | endian | (swap << 24);
            copy.destFetch[1] |= endian << 6;
            constexpr unsigned        rowPitch = 64 * 4 + 64;
            std::vector<std::uint8_t> host(rowPitch * 32, 0xCD);
            std::vector<std::uint8_t> expected(64 * 32 * 4);
            for (unsigned y = 0; y < 32; ++y)
            {
                for (unsigned x = 0; x < 64; ++x)
                {
                    std::array<std::uint8_t, 4> rgba{ static_cast<std::uint8_t>(x),
                                                      static_cast<std::uint8_t>(y + 33),
                                                      static_cast<std::uint8_t>(x + y + 97),
                                                      231 };
                    std::copy(rgba.begin(), rgba.end(), host.begin() + y * rowPitch + x * 4);
                    if (swap)
                    {
                        std::swap(rgba[0], rgba[2]);
                    }
                    for (unsigned c = 0; c < 4; ++c)
                    {
                        expected[referenceOffset(x, y, 64) + c] = rgba[endianBytes[endian][c]];
                    }
                }
            }
            GuestResolveImage image;
            std::string       error;
            UNSCOPED_INFO(error);
            REQUIRE(EncodeGuestResolve(copy, host, rowPitch, image, error));
            UNSCOPED_INFO("host RGBA packing, swap, endian, padded row pitch or whole extent is wrong");
            REQUIRE((image.physical == copy.destBase && image.bytes == expected));
            UNSCOPED_INFO("a whole unpadded destination carries no runs");
            REQUIRE(image.runs.empty());
            // The same texels as packed words encode identically.
            copy.packing = GuestResolvePacking::Color8888Words;
            GuestResolveImage words;
            UNSCOPED_INFO("Color8888Words differs from the host RGBA bytes");
            REQUIRE((EncodeGuestResolve(copy, host, rowPitch, words, error) && words.bytes == expected && words.runs.empty()));
        }
    }
}

// R, G, B and A fields of a k_2_10_10_10 word, in the order the SDK packs them.
std::uint32_t pack2101010(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a)
{
    return r | (g << 10) | (b << 20) | (a << 30);
}

std::vector<std::uint8_t> hostWords(unsigned width, unsigned height, unsigned rowPitch, auto word)
{
    std::vector<std::uint8_t> host(rowPitch * height, 0xCD);
    for (unsigned y = 0; y < height; ++y)
    {
        for (unsigned x = 0; x < width; ++x)
        {
            const std::uint32_t value = word(x, y);
            for (unsigned c = 0; c < 4; ++c)
            {
                host[y * rowPitch + x * 4 + c] = static_cast<std::uint8_t>(value >> (8 * c));
            }
        }
    }
    return host;
}

void storeWord(std::vector<std::uint8_t>& bytes, unsigned offset, std::uint32_t word, unsigned endian)
{
    for (unsigned c = 0; c < 4; ++c)
    {
        bytes[offset + c] = static_cast<std::uint8_t>(word >> (8 * endianBytes[endian][c]));
    }
}

void packedColorWords()
{
    constexpr unsigned width = 64, height = 32, rowPitch = width * 4 + 16;
    // Distinct R and B (and G) per texel, so any field exchange is visible.
    const auto field = [](unsigned x, unsigned y)
    {
        return std::array<std::uint32_t, 4>{ (x * 13 + 1) & 1023, (y * 29 + x * 3 + 5) & 1023, (x * 7 + y * 17 + 300) & 1023, (x + y) & 3 };
    };
    const auto host = hostWords(width, height, rowPitch, [&](unsigned x, unsigned y)
                                {
                                    const auto f = field(x, y);
                                    return pack2101010(f[0], f[1], f[2], f[3]);
                                });
    for (unsigned endian = 0; endian < 4; ++endian)
    {
        for (unsigned swap = 0; swap < 2; ++swap)
        {
            auto copy = wholeCopy(width, height, GuestResolvePacking::Color2101010Words);
            setEndianSwap(copy, endian, swap);
            // Independent expectation, from the fields rather than the production masks.
            std::vector<std::uint8_t> expected(referenceExtent(width, height, width)), wrongSwap(expected.size());
            for (unsigned y = 0; y < height; ++y)
            {
                for (unsigned x = 0; x < width; ++x)
                {
                    const auto f = field(x, y);
                    storeWord(expected, referenceOffset(x, y, width), swap ? pack2101010(f[2], f[1], f[0], f[3]) : pack2101010(f[0], f[1], f[2], f[3]), endian);
                    // The mutation under test: exchanging R with G instead of R with B.
                    storeWord(wrongSwap, referenceOffset(x, y, width), swap ? pack2101010(f[1], f[0], f[2], f[3]) : pack2101010(f[0], f[1], f[2], f[3]), endian);
                }
            }
            GuestResolveImage image;
            std::string       error;
            UNSCOPED_INFO(error);
            REQUIRE(EncodeGuestResolve(copy, host, rowPitch, image, error));
            UNSCOPED_INFO("k_2_10_10_10 packing, swap or endian is wrong");
            REQUIRE((image.bytes == expected && image.runs.empty()));
            UNSCOPED_INFO("the R/G exchange mutation is undetectable");
            REQUIRE((swap == 0 || image.bytes != wrongSwap));
        }
    }
}

void depthWords()
{
    constexpr unsigned width = 64, height = 32, rowPitch = width * 4;
    const auto         depthWord = [](unsigned x, unsigned y)
    {
        return (((0xABCDEFU + x * 0x101 + y * 0x10001) & 0xFFFFFF) << 8) | ((0x5A + x + y) & 0xFF);
    };
    const auto host = hostWords(width, height, rowPitch, depthWord);
    for (unsigned endian = 0; endian < 4; ++endian)
    {
        // The swap bit is set on purpose: depth has nothing to swap.
        auto copy = wholeCopy(width, height, GuestResolvePacking::Depth24Stencil8Words);
        setEndianSwap(copy, endian, 1);
        std::vector<std::uint8_t> expected(referenceExtent(width, height, width));
        for (unsigned y = 0; y < height; ++y)
        {
            for (unsigned x = 0; x < width; ++x)
            {
                storeWord(expected, referenceOffset(x, y, width), depthWord(x, y), endian);
            }
        }
        GuestResolveImage image;
        std::string       error;
        UNSCOPED_INFO(error);
        REQUIRE(EncodeGuestResolve(copy, host, rowPitch, image, error));
        UNSCOPED_INFO("k_24_8 words or endian are wrong");
        REQUIRE((image.bytes == expected && image.runs.empty()));
        if (endian == 2)
        {
            // Texel (0, 0) is 0xABCDEF5A; endian 8in32 stores it big-endian.
            UNSCOPED_INFO("0xABCDEF5A does not round-trip through endian 2");
            REQUIRE((depthWord(0, 0) == 0xABCDEF5A && image.bytes[0] == 0xAB && image.bytes[1] == 0xCD && image.bytes[2] == 0xEF && image.bytes[3] == 0x5A));
        }
    }
}

// k_16_16_FLOAT keeps halves; endian reorders bytes, while red/blue swap fails.
void float16Words()
{
    constexpr unsigned width = 64, height = 32, rowPitch = width * 4 + 8;
    const auto         halves = [](unsigned x, unsigned y)
    {
        return ((0x3C00U + x * 7 + y * 131) & 0xFFFF) | (((0xBC00U + x * 257 + y * 3) & 0xFFFF) << 16);
    };
    const auto host = hostWords(width, height, rowPitch, halves);
    for (unsigned endian = 0; endian < 4; ++endian)
    {
        auto copy = wholeCopy(width, height, GuestResolvePacking::Float16x2Words);
        setEndianSwap(copy, endian, 0);
        std::vector<std::uint8_t> expected(referenceExtent(width, height, width));
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
                storeWord(expected, referenceOffset(x, y, width), halves(x, y), endian);
        GuestResolveImage image;
        std::string       error;
        UNSCOPED_INFO(error);
        REQUIRE(EncodeGuestResolve(copy, host, rowPitch, image, error));
        UNSCOPED_INFO("k_16_16_FLOAT words or endian are wrong");
        REQUIRE((image.bytes == expected && image.runs.empty()));
    }
    std::vector<std::uint8_t> refusedHost(4096, 0x11);
    const auto                refuses = [&](GuestResolveCopy copy, const std::string& expected)
    {
        GuestResolveImage image;
        std::string       error;
        UNSCOPED_INFO("expected " + expected + ", got " + error);
        REQUIRE((!EncodeGuestResolve(copy, refusedHost, 128, image, error) && error == expected));
    };
    auto swapped = wholeCopy(32, 32, GuestResolvePacking::Float16x2Words);
    setEndianSwap(swapped, 2, 1);
    refuses(swapped, "resolve_swap_unsupported: dest 31");
    auto unsignedNumber = wholeCopy(32, 32, GuestResolvePacking::Float16x2Words);
    unsignedNumber.destInfo &= ~(7U << 13);
    refuses(unsignedNumber, "resolve_number_format_unsupported: 0 for dest 31");
    auto biased = wholeCopy(32, 32, GuestResolvePacking::Float16x2Words);
    biased.destInfo |= 0x3F << 16;
    refuses(biased, "resolve_exp_bias_unsupported: 63");
    // A float number format on a k_8_8_8_8 copy is not what the guest writes.
    auto floatColor = wholeCopy(32, 32, GuestResolvePacking::Color8888Words);
    floatColor.destInfo |= 7U << 13;
    refuses(floatColor, "resolve_number_format_unsupported: 7 for dest 6");
    // Another packing over a format-31 copy, and a format-31 copy into another fetch.
    auto other    = wholeCopy(32, 32, GuestResolvePacking::Float16x2Words);
    other.packing = GuestResolvePacking::Color8888Words;
    refuses(other, "resolve_dest_format_unsupported: dest 31 packing 1");
    auto fetch         = wholeCopy(32, 32, GuestResolvePacking::Float16x2Words);
    fetch.destFetch[1] = (fetch.destFetch[1] & ~63U) | 32;
    refuses(fetch, "resolve_dest_format_unsupported: dest 31 packing 4 fetch 32");
    // Gamma components stay refused; unsigned and all-signed float fetches,
    // integer or not, are admitted.
    auto gamma = wholeCopy(32, 32, GuestResolvePacking::Float16x2Words);
    gamma.destFetch[0] |= 0x3FC;
    refuses(gamma, std::format("resolve_fetch_sign_unsupported: {:08X} {:08X}", gamma.destFetch[0], gamma.destFetch[3]));
    for (const auto sign : { 0U, 0x154U })
        for (const auto integer : { 0U, 1U })
        {
            auto              admitted = wholeCopy(32, 32, GuestResolvePacking::Float16x2Words);
            GuestResolveImage image;
            std::string       error;
            admitted.destFetch[0] |= sign;
            admitted.destFetch[3] |= integer;
            UNSCOPED_INFO(std::format("float fetch sign {:#x} integer {} was refused: {}", sign, integer, error));
            REQUIRE(EncodeGuestResolve(admitted, refusedHost, 128, image, error));
        }
}

// K12 fog: D7AA1B9B851613F9-0 event 668.
void fogDestination()
{
    // RB_COPY_DEST_INFO 0xEF82: format 31, number 7, endian 2.
    // r6+0x1C fetch: signed k_16_16_FLOAT at FE7AF000, physical 1E7B0000.
    constexpr unsigned width = 1280, height = 720;
    GuestResolveCopy   copy;
    copy.copyControl = 0x00100000;
    copy.destBase    = 0x1E7B0000;
    copy.destPitch   = 0x02D00500;
    copy.destInfo    = 0xEF82;
    copy.destFetch   = { 0x8A000156, 0xFE7AF09F, 0x0059E4FF, 0x00001691, 0x00000000, 0x00000200 };
    copy.rectWidth   = width;
    copy.rectHeight  = height;
    copy.packing     = GuestResolvePacking::Float16x2Words;
    const auto word  = [](unsigned x, unsigned y)
    {
        return (x * 0x00030001U + y * 0x01000100U) ^ 0x3C00BC00U;
    };
    const auto        host = hostWords(width, height, width * 4, word);
    GuestResolveImage image;
    std::string       error;
    UNSCOPED_INFO("the K12 fog copy was refused: " + error);
    REQUIRE(EncodeGuestResolve(copy, host, width * 4, image, error));
    std::vector<std::uint8_t> expected(referenceExtent(width, height, width));
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            storeWord(expected, referenceOffset(x, y, width), word(x, y), 2);
    // 720 rows end mid-tile, so the last tile row's padding stays outside the runs.
    UNSCOPED_INFO("the K12 fog copy's words differ from the tiled oracle");
    REQUIRE((image.physical == 0x1E7B0000 && image.bytes == expected && !image.runs.empty()));
    // Texel (0, 0) is 0x3C00BC00: endian 8in32 stores it big-endian.
    UNSCOPED_INFO("0x3C00BC00 does not round-trip through endian 2");
    REQUIRE((image.bytes[0] == 0x3C && image.bytes[1] == 0x00 && image.bytes[2] == 0xBC && image.bytes[3] == 0x00));
}

void packingMismatches()
{
    const std::vector<std::uint8_t> host(4096, 255);
    const auto                      rejects = [&](GuestResolveCopy copy, const char* expected)
    {
        GuestResolveImage image;
        std::string       error;
        UNSCOPED_INFO(std::string("missing rejection: ") + expected + " got " + error);
        REQUIRE((!EncodeGuestResolve(copy, host, 128, image, error) && error.find(expected) != std::string::npos));
    };
    // A 2_10_10_10 packing on a depth copy, and the other mismatches, fail by name.
    auto depth    = wholeCopy(32, 32, GuestResolvePacking::Depth24Stencil8Words);
    depth.packing = GuestResolvePacking::Color2101010Words;
    rejects(depth, "resolve_source_select_unsupported"); // source 4 is no color source
    depth.copyControl &= ~7U;
    rejects(depth, "resolve_dest_format_unsupported: dest 6 packing 2 fetch 22");
    depth.packing = GuestResolvePacking::HostRgba8;
    rejects(depth, "resolve_dest_format_unsupported: dest 6 packing 0 fetch 22");
    auto color    = wholeCopy(32, 32, GuestResolvePacking::Color2101010Words);
    color.packing = GuestResolvePacking::Color8888Words;
    rejects(color, "resolve_dest_format_unsupported: dest 7 packing 1");
    color.packing = GuestResolvePacking::Depth24Stencil8Words;
    color.copyControl |= 4;
    rejects(color, "resolve_dest_format_unsupported: dest 7 packing 3");
    // k_24_8_FLOAT stays refused, as the guest copies it (6) and as copy and
    // fetch might otherwise agree; so does a fetch format that disagrees with the copy.
    auto floatDepth = wholeCopy(32, 32, GuestResolvePacking::Depth24Stencil8Words);
    floatDepth.destFetch[1] += 1;
    rejects(floatDepth, "resolve_dest_format_unsupported: dest 6 packing 3 fetch 23");
    floatDepth.destInfo = (floatDepth.destInfo & ~(63U << 7)) | (23U << 7);
    rejects(floatDepth, "resolve_dest_format_unsupported: dest 23 packing 3");
    // A depth copy whose copy format equals the fetch's k_24_8 is not what the guest writes.
    auto rawDepth     = wholeCopy(32, 32, GuestResolvePacking::Depth24Stencil8Words);
    rawDepth.destInfo = (rawDepth.destInfo & ~(63U << 7)) | (22U << 7);
    rejects(rawDepth, "resolve_dest_format_unsupported: dest 22 packing 3");
    auto fetch = wholeCopy(32, 32, GuestResolvePacking::Color2101010Words);
    fetch.destFetch[1] ^= 1;
    rejects(fetch, "resolve_dest_format_unsupported: dest 7 packing 2 fetch 6");
    // Source and sample selects follow the packing.
    for (const auto packing : { GuestResolvePacking::HostRgba8, GuestResolvePacking::Color8888Words, GuestResolvePacking::Color2101010Words })
    {
        auto c = wholeCopy(32, 32, packing);
        c.copyControl |= 4;
        rejects(c, "resolve_source_select_unsupported");
    }
    auto source = wholeCopy(32, 32, GuestResolvePacking::Depth24Stencil8Words);
    source.copyControl &= ~7U;
    rejects(source, "resolve_source_select_unsupported");
    source.copyControl |= 5;
    rejects(source, "resolve_source_select_unsupported");
    for (unsigned sample = 4; sample < 8; ++sample)
    {
        auto c = wholeCopy(32, 32, GuestResolvePacking::Depth24Stencil8Words);
        c.copyControl |= sample << 4;
        rejects(c, "resolve_sample_select_unsupported");
    }
    for (unsigned sample = 5; sample < 8; ++sample)
    {
        auto c = wholeCopy(32, 32, GuestResolvePacking::Color2101010Words);
        c.copyControl |= sample << 4;
        rejects(c, "resolve_sample_select_unsupported");
    }
    // k0..k3 and k01 are accepted for color and k0..k3 for depth.
    for (unsigned sample = 0; sample < 5; ++sample)
    {
        auto              c = wholeCopy(32, 32, GuestResolvePacking::Color2101010Words);
        GuestResolveImage image;
        std::string       error;
        c.copyControl |= sample << 4;
        UNSCOPED_INFO("color sample select was refused: " + error);
        REQUIRE(EncodeGuestResolve(c, host, 128, image, error));
        if (sample < 4)
        {
            auto d = wholeCopy(32, 32, GuestResolvePacking::Depth24Stencil8Words);
            d.copyControl |= sample << 4;
            UNSCOPED_INFO("depth sample select was refused: " + error);
            REQUIRE(EncodeGuestResolve(d, host, 128, image, error));
        }
    }
}

// Texel (x, y) of a pitch-wide tiled texture as the byte offsets it owns.
struct StripCheck
{
    unsigned destX, destY, width, height, pitch, texHeight;
};

// True when `image` written through its runs over an old-bytes buffer changes
// exactly the strip's texels (to the oracle value) and nothing else.
bool stripMatches(const GuestResolveImage& image, const StripCheck& strip, auto word)
{
    const auto                tiles = ((strip.pitch + 31) / 32) * ((strip.texHeight + 31) / 32);
    std::vector<std::uint8_t> old(tiles * 4096, 0xA5), expected = old;
    for (unsigned y = 0; y < strip.height; ++y)
    {
        for (unsigned x = 0; x < strip.width; ++x)
        {
            storeWord(expected, referenceOffset(strip.destX + x, strip.destY + y, strip.pitch), word(x, y), 0);
        }
    }
    // Runs must be sorted, word aligned, in range and maximal (no touching neighbours).
    std::uint32_t end = 0;
    for (std::size_t i = 0; i < image.runs.size(); ++i)
    {
        const auto& run = image.runs[i];
        if ((run.offset | run.length) % 4 || !run.length || run.offset + run.length > image.bytes.size() || run.offset + run.length > old.size() ||
            (i && run.offset <= end))
        {
            return false;
        }
        end = run.offset + run.length;
        std::copy_n(image.bytes.begin() + run.offset, run.length, old.begin() + run.offset);
    }
    return old == expected;
}

void subRectangle()
{
    // A 448x720 tile strip at (448, 0) of a 1280x720 texture: pitch 1280, 720 rows.
    constexpr unsigned stripWidth = 448, textureWidth = 1280, textureHeight = 720, rowPitch = stripWidth * 4;
    const auto         word = [](unsigned x, unsigned y)
    {
        return pack2101010(x & 1023, y & 1023, (x * 3 + y) & 1023, 3);
    };
    const auto host = hostWords(stripWidth, textureHeight, rowPitch, word);
    auto       copy = wholeCopy(textureWidth, textureHeight, GuestResolvePacking::Color2101010Words);
    setEndianSwap(copy, 0, 0);
    copy.rectX = copy.destX = 448;
    copy.rectWidth          = stripWidth;
    GuestResolveImage image;
    std::string       error;
    UNSCOPED_INFO(error);
    REQUIRE(EncodeGuestResolve(copy, host, rowPitch, image, error));
    const StripCheck strip{ 448, 0, stripWidth, textureHeight, textureWidth, textureHeight };
    // The image ends at the last touched tile: row 22 (rows 704..719), column 27.
    UNSCOPED_INFO("sub-rectangle image extent is wrong");
    REQUIRE((image.physical == copy.destBase && image.bytes.size() == (22 * 40 + 27 + 1) * 4096));
    UNSCOPED_INFO("sub-rectangle writes more or fewer texels than the strip");
    REQUIRE(stripMatches(image, strip, word));
    // Whole 4 KiB tiles merge: tiles 14..27 of the first row are one run. The
    // last, half tile row gives one 2 KiB group per tile (rows 0..15 of a tile).
    UNSCOPED_INFO("sub-rectangle runs are not coalesced into maximal spans");
    REQUIRE((image.runs.size() == 22 + 14 && image.runs.front().offset == 14 * 4096 && image.runs.front().length == 14 * 4096 &&
             image.runs.back().offset == (22 * 40 + 27) * 4096 && image.runs.back().length == 2048));
    // Mutations: the strip 32 texels to the right, and a run list that drops the
    // last partial tile row, must both fail the same check.
    auto shifted = copy;
    shifted.destX += 32;
    GuestResolveImage shiftedImage;
    UNSCOPED_INFO(error);
    REQUIRE(EncodeGuestResolve(shifted, host, rowPitch, shiftedImage, error));
    UNSCOPED_INFO("a destX off by 32 passes the strip check");
    REQUIRE(!stripMatches(shiftedImage, strip, word));
    auto truncated = image;
    truncated.runs.resize(22);
    UNSCOPED_INFO("dropping the last partial tile row passes the strip check");
    REQUIRE(!stripMatches(truncated, strip, word));

    // A strip with a nonzero destination row: 448x320 at (448, 200), off the tile grid in rows.
    auto lowered       = copy;
    lowered.rectHeight = 320;
    lowered.destY      = 200;
    GuestResolveImage loweredImage;
    UNSCOPED_INFO(error);
    REQUIRE(EncodeGuestResolve(lowered, host, rowPitch, loweredImage, error));
    UNSCOPED_INFO("a strip at a nonzero destY writes the wrong texels");
    REQUIRE(stripMatches(loweredImage, { 448, 200, stripWidth, 320, textureWidth, textureHeight }, word));

    // A whole 1280x720 destination is padded, so it keeps its runs; padding rows
    // 720..735 stay untouched.
    const auto wholeHost = hostWords(textureWidth, textureHeight, textureWidth * 4, word);
    auto       whole     = wholeCopy(textureWidth, textureHeight, GuestResolvePacking::Color2101010Words);
    setEndianSwap(whole, 0, 0);
    GuestResolveImage wholeImage;
    UNSCOPED_INFO(error);
    REQUIRE(EncodeGuestResolve(whole, wholeHost, textureWidth * 4, wholeImage, error));
    UNSCOPED_INFO("a padded whole destination must write exactly its texels");
    REQUIRE((stripMatches(wholeImage, { 0, 0, textureWidth, textureHeight, textureWidth, textureHeight }, word) && !wholeImage.runs.empty()));

    // A rectangle that leaves its destination, or a zero one, is refused.
    const auto outside = [&](auto change)
    {
        auto bad = copy;
        change(bad);
        GuestResolveImage untouched;
        UNSCOPED_INFO("rectangle outside the destination was accepted: " + error);
        REQUIRE((!EncodeGuestResolve(bad, host, rowPitch, untouched, error) && error.find("does not fit") != std::string::npos));
    };
    outside([](auto& c)
            {
                c.destX = 833;
            });
    outside([](auto& c)
            {
                c.destX = 2000;
            });
    outside([](auto& c)
            {
                c.destY = 720;
            });
    outside([](auto& c)
            {
                c.destY = 2000;
            });
    outside([](auto& c)
            {
                c.rectWidth = 0;
            });
    outside([](auto& c)
            {
                c.rectHeight = 0;
            });
    outside([](auto& c)
            {
                c.rectHeight += 1;
            });
}

// Frame 428: raw_object_descriptor at r6+0x1C; log-derived copy bases.
void captureDestinations()
{
    struct Destination
    {
        GuestResolvePacking          packing;
        std::array<std::uint32_t, 6> fetch;
        std::uint32_t                physical, copyFormat, endian, control;
    };

    // Depth uses A0000000, colors E0000000, and tile widths are 448/416/416.
    const std::array<Destination, 3>                 destinations{ {
        { GuestResolvePacking::Depth24Stencil8Words, { 0x8A000002, 0xBD0F0096, 0x0059E4FF, 0x1690, 0x280, 0xBD488A00 }, 0x1D0F0000, 6, 2, 0x00100014 },
        { GuestResolvePacking::Color8888Words, { 0x8A000002, 0xFD817006, 0x0059E4FF, 0x1414, 0, 0x200 }, 0x1D818000, 6, 0, 0x00100340 },
        { GuestResolvePacking::Color2101010Words, { 0x8A000002, 0xFD47F036, 0x0059E4FF, 0xD10, 0, 0x200 }, 0x1D480000, 7, 0, 0x00100141 },
    } };
    constexpr std::array<std::array<unsigned, 2>, 3> tiles{ { { 0, 448 }, { 448, 416 }, { 864, 416 } } };
    constexpr unsigned                               width = 1280, height = 720, storage = 1280 * 736 * 4;
    for (std::size_t d = 0; d < destinations.size(); ++d)
    {
        const auto& destination = destinations[d];
        const auto  word        = [d](unsigned x, unsigned y)
        {
            return (x * 0x10001U + y * 0x01000100U + std::uint32_t(d) * 0x5A5A5A5AU) ^ 0x13579BDFU;
        };
        std::vector<std::uint8_t> memory(storage, 0xA5), expected(storage, 0xA5);
        for (const auto& [x0, w] : tiles)
        {
            GuestResolveCopy copy;
            copy.copyControl = destination.control;
            copy.destBase    = destination.physical;
            copy.destPitch   = (height << 16) | width;
            copy.destInfo    = (destination.copyFormat << 7) | destination.endian;
            copy.destFetch   = destination.fetch;
            copy.rectX = copy.destX = x0;
            copy.rectWidth          = w;
            copy.rectHeight         = height;
            copy.packing            = destination.packing;
            const auto        host  = hostWords(w, height, w * 4, [&](unsigned x, unsigned y)
                                                {
                                            return word(x0 + x, y);
                                                });
            GuestResolveImage image;
            std::string       error;
            UNSCOPED_INFO("capture tile resolve was refused: " + error);
            REQUIRE(EncodeGuestResolve(copy, host, w * 4, image, error));
            UNSCOPED_INFO("capture tile image shape is wrong");
            REQUIRE((image.physical == copy.destBase && image.bytes.size() <= storage && !image.runs.empty()));
            for (const auto& run : image.runs)
                std::copy_n(image.bytes.begin() + run.offset, run.length, memory.begin() + run.offset);
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = x0; x < x0 + w; ++x)
                    storeWord(expected, referenceOffset(x, y, width), word(x, y), destination.endian);
        }
        UNSCOPED_INFO("capture destination " + std::to_string(d) + " differs from the tiled oracle or lost a sentinel byte");
        REQUIRE(memory == expected);
    }
    // k_24_8_FLOAT behind the same depth path stays refused.
    GuestResolveCopy floatDepth;
    floatDepth.copyControl = 0x00100014;
    floatDepth.destBase    = 0x1D0F0000;
    floatDepth.destPitch   = (height << 16) | width;
    floatDepth.destInfo    = (6U << 7) | 2;
    floatDepth.destFetch   = destinations[0].fetch;
    floatDepth.destFetch[1] += 1;
    floatDepth.rectWidth  = 448;
    floatDepth.rectHeight = height;
    floatDepth.packing    = GuestResolvePacking::Depth24Stencil8Words;
    GuestResolveImage image;
    std::string       error;
    const auto        host = hostWords(448, height, 448 * 4, [](unsigned, unsigned)
                                       {
                                    return 0U;
                                       });
    UNSCOPED_INFO("k_24_8_FLOAT was not refused by name: " + error);
    REQUIRE((!EncodeGuestResolve(floatDepth, host, 448 * 4, image, error) && error == "resolve_dest_format_unsupported: dest 6 packing 3 fetch 23"));
}

// A0000000 maps directly; E0000000 adds 0x1000 at each admitted view edge.
void fetchAddressViews()
{
    const std::vector<std::uint8_t> host(32 * 4 * 32, 0x5A);
    for (const auto& [fetch, physical] : { std::array{ 0x00000000U, 0x00000000U }, std::array{ 0x1FFFF000U, 0x1FFFF000U }, std::array{ 0xA0000000U, 0x00000000U }, std::array{ 0xA1000000U, 0x01000000U }, std::array{ 0xBFFFF000U, 0x1FFFF000U }, std::array{ 0xE0000000U, 0x00001000U }, std::array{ 0xFD817000U, 0x1D818000U } })
    {
        auto copy         = wholeCopy(32, 32);
        copy.destFetch[1] = fetch | 6;
        copy.destBase     = physical;
        GuestResolveImage image;
        std::string       error;
        UNSCOPED_INFO(std::format("fetch base {:08X} was not admitted at {:08X}: {}", fetch, physical, error));
        REQUIRE((EncodeGuestResolve(copy, host, 32 * 4, image, error) && image.physical == physical));
    }
}

// Only destination-ending edges may expand into unwritten tiled padding.
void unalignedRectangles()
{
    const std::vector<std::uint8_t> host(64 * 4 * 64, 0x3C);
    const auto                      refused = [&](auto change, const std::string& expected)
    {
        auto copy = wholeCopy(64, 64);
        change(copy);
        GuestResolveImage image;
        std::string       error;
        UNSCOPED_INFO("expected " + expected + ", got " + error);
        REQUIRE((!EncodeGuestResolve(copy, host, 64 * 4, image, error) && error == expected));
    };
    refused([](auto& c)
            {
                c.rectWidth  = 32;
                c.rectHeight = 17;
            },
            "resolve_rect_unaligned: 0,0 32x17 at 0,0");
    refused([](auto& c)
            {
                c.rectWidth = 60;
            },
            "resolve_rect_unaligned: 0,0 60x64 at 0,0");
    refused([](auto& c)
            {
                c.rectX = c.destX = 4;
                c.rectWidth       = 56;
            },
            "resolve_rect_unaligned: 4,0 56x64 at 4,0");
    refused([](auto& c)
            {
                c.rectHeight = 40;
                c.destY      = 12;
            },
            "resolve_rect_unaligned: 0,0 64x40 at 0,12");
    auto              aligned = wholeCopy(64, 64);
    GuestResolveImage image;
    std::string       error;
    aligned.rectWidth  = 32;
    aligned.rectHeight = 24;
    UNSCOPED_INFO("an aligned 32x24 resolve was refused: " + error);
    REQUIRE(EncodeGuestResolve(aligned, host, 64 * 4, image, error));
    // An unaligned edge ending a 60x37 destination of pitch 64, below an
    // aligned origin.
    auto edge         = wholeCopy(64, 37);
    edge.destFetch[2] = 59 | (36 << 13);
    edge.rectWidth    = 60;
    edge.rectY = edge.destY = 8;
    edge.rectHeight         = 29;
    UNSCOPED_INFO("a resolve ending a 60x37 destination was refused: " + error);
    REQUIRE(EncodeGuestResolve(edge, host, 64 * 4, image, error));
}

// Hall of Glory panel from strict run strict-s25-1.
void hallPanelResolve()
{
    // 900x675 at 0,0 in a 928x688 surface; SDK expansion to 904x680 is padding.
    constexpr unsigned width = 900, height = 675, pitch = 928;
    GuestResolveCopy   copy;
    copy.copyControl = 0x00100000;
    copy.destBase    = 0x18670000;
    copy.destPitch   = (height << 16) | pitch;
    copy.destInfo    = 6U << 7;
    copy.destFetch   = { 0x87400002U, 0x18670006U, 0x00544383U, 0x00A80C14U, 0x00000003U, 0x00000218U };
    copy.rectWidth   = width;
    copy.rectHeight  = height;
    const auto word  = [](unsigned x, unsigned y)
    {
        return (x * 0x00010003U + y * 0x02000100U) ^ 0x2468ACE1U;
    };
    const auto        host = hostWords(width, height, width * 4, word);
    GuestResolveImage image;
    std::string       error;
    UNSCOPED_INFO("the Hall panel resolve was refused: " + error);
    REQUIRE(EncodeGuestResolve(copy, host, width * 4, image, error));
    UNSCOPED_INFO("the Hall panel resolve image extent is wrong");
    REQUIRE((image.physical == 0x18670000 && image.bytes.size() == 0x27E000 && image.bytes.size() == referenceExtent(width, height, pitch)));
    std::vector<std::uint8_t> memory(image.bytes.size(), 0xA5), expected(image.bytes.size(), 0xA5);
    for (const auto& run : image.runs)
        std::copy_n(image.bytes.begin() + run.offset, run.length, memory.begin() + run.offset);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            storeWord(expected, referenceOffset(x, y, pitch), word(x, y), 0);
    UNSCOPED_INFO("the Hall panel resolve differs from the tiled oracle or wrote its padding");
    REQUIRE(memory == expected);
    const auto refused = [&](auto change, const std::string& expected)
    {
        auto changed = copy;
        change(changed);
        GuestResolveImage unused;
        std::string       why;
        UNSCOPED_INFO("expected " + expected + ", got " + why);
        REQUIRE((!EncodeGuestResolve(changed, host, width * 4, unused, why) && why == expected));
    };
    refused([](auto& c)
            {
                c.rectWidth = 892;
            },
            "resolve_rect_unaligned: 0,0 892x675 at 0,0");
    refused([](auto& c)
            {
                c.rectHeight = 670;
            },
            "resolve_rect_unaligned: 0,0 900x670 at 0,0");
    refused([](auto& c)
            {
                c.rectX = c.destX = 4;
                c.rectWidth       = 896;
            },
            "resolve_rect_unaligned: 4,0 896x675 at 4,0");
}

void unsupportedInputs()
{
    const auto                      valid = wholeCopy(32, 32);
    const std::vector<std::uint8_t> host(4096, 255);
    const auto                      rejects = [&](const GuestResolveCopy& copy, std::span<const std::uint8_t> bytes, std::uint32_t pitch, const char* expected)
    {
        GuestResolveImage image{ 123, { 7, 8, 9 } };
        std::string       error;
        UNSCOPED_INFO("unsupported input was accepted");
        REQUIRE(!EncodeGuestResolve(copy, bytes, pitch, image, error));
        UNSCOPED_INFO("missing specific rejection: " + error);
        REQUIRE(error.find(expected) != std::string::npos);
        UNSCOPED_INFO("failed encoding changed the caller's output");
        REQUIRE((image.physical == 123 && image.bytes == std::vector<std::uint8_t>{ 7, 8, 9 }));
    };
    const auto mutate = [&](auto change, const char* expected)
    {
        auto copy = valid;
        change(copy);
        rejects(copy, host, 128, expected);
    };
    mutate([](auto& c)
           {
               c.copyControl |= 8;
           },
           "reserved RB_COPY_CONTROL");
    mutate([](auto& c)
           {
               c.copyControl |= 4;
           },
           "resolve_source_select_unsupported");
    mutate([](auto& c)
           {
               c.copyControl |= 0x200000;
           },
           "copy commands");
    for (unsigned sample = 5; sample < 8; ++sample)
    {
        mutate([sample](auto& c)
               {
                   c.copyControl |= sample << 4;
               },
               "resolve_sample_select_unsupported");
    }
    mutate([](auto& c)
           {
               c.destInfo |= 0x80000000U;
           },
           "reserved RB_COPY_DEST_INFO");
    mutate([](auto& c)
           {
               c.destInfo ^= 0x80;
           },
           "resolve_dest_format_unsupported: dest 7 packing 0 fetch 6");
    mutate([](auto& c)
           {
               c.destFetch[1] ^= 1;
           },
           "resolve_dest_format_unsupported: dest 6 packing 0 fetch 7");
    mutate([](auto& c)
           {
               c.destInfo |= 8;
           },
           "array and volume");
    mutate([](auto& c)
           {
               c.destInfo |= 0x2000;
           },
           "resolve_number_format_unsupported: 1 for dest 6");
    mutate([](auto& c)
           {
               c.destInfo |= 0x10000;
           },
           "resolve_exp_bias_unsupported: 1");
    mutate([](auto& c)
           {
               c.destInfo |= 4;
           },
           "endian modes");
    mutate([](auto& c)
           {
               c.destFetch[5] = 0x600;
           },
           "non-stacked 2D");
    mutate([](auto& c)
           {
               c.destFetch[1] |= 0x400;
           },
           "non-stacked 2D");
    mutate([](auto& c)
           {
               c.destFetch[0] &= ~0x80000000U;
           },
           "linear fetch");
    mutate([](auto& c)
           {
               c.destFetch[0] |= 4;
           },
           "resolve_fetch_sign_unsupported");
    {
        // A packed mip tail shares the base level once its smaller side is 16 or less.
        auto packed = wholeCopy(32, 16);
        packed.destFetch[5] |= 0x800;
        rejects(packed, host, 128, "mipmapped");
    }
    mutate([](auto& c)
           {
               c.destFetch[4] |= 4;
           },
           "mipmapped");
    mutate([](auto& c)
           {
               c.destInfo |= 1;
           },
           "endian modes disagree");
    mutate([](auto& c)
           {
               c.destPitch += 32;
           },
           "pitch or height");
    mutate([](auto& c)
           {
               c.rectWidth += 8;
           },
           "does not fit");
    mutate([](auto& c)
           {
               c.destX = 32;
           },
           "does not fit");
    mutate([](auto& c)
           {
               c.destFetch[2] += 32; // width 64 over a 32-texel pitch
           },
           "pitch is smaller");
    mutate([](auto& c)
           {
               c.destBase += 4096;
           },
           "physical base");
    // Fetch bases outside the physical range and the A0000000 and E0000000
    // views are refused by name, including the C0000000 view.
    for (const auto base : { 0x20000000U, 0x80000000U, 0x9FFFF000U, 0xC0000000U, 0xC1000000U, 0xDFFFF000U })
        mutate([base](auto& c)
               {
                   c.destFetch[1] = base | 6;
               },
               "destination fetch address is not physical or in the modeled A0000000 or E0000000 view");
    // The A0000000 view has no 0x1000 offset; only E0000000 does.
    mutate([](auto& c)
           {
               c.destFetch[1] = 0xA1000006U;
               c.destBase     = 0x01001000;
           },
           "physical base");
    mutate([](auto& c)
           {
               c.destFetch[1] = 0xE1000006U;
           },
           "physical base");
    mutate([](auto& c)
           {
               c.destBase     = 0x20000000;
               c.destFetch[1] = 0xFFFFF006U;
           },
           "exceeds guest physical");
    rejects(valid, host, 127, "host readback");
    rejects(valid, std::span(host).first(4095), 128, "host readback");
}

std::vector<std::uint8_t> readCapture(const std::filesystem::path& path, std::size_t size)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    UNSCOPED_INFO("missing or wrong-sized capture: " + path.string());
    REQUIRE((stream && stream.tellg() == static_cast<std::streamoff>(size)));
    std::vector<std::uint8_t> bytes(size);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    UNSCOPED_INFO("could not read capture: " + path.string());
    REQUIRE(static_cast<bool>(stream));
    return bytes;
}

std::uint32_t edramWord(std::span<const std::uint8_t> edram, unsigned x, unsigned y, unsigned sample)
{
    // Source attachment 0A010280/00000000: pitch 640, 2X MSAA, color base 0.
    // SDK xenos.h EDRAM layout: 80x16 samples per tile, sample 1 on the next row.
    const auto sy     = y * 2 + sample;
    const auto tile   = ((sy / 16) * 8 + x / 80) % 2048;
    const auto offset = (tile * 1280 + (sy % 16) * 80 + x % 80) * 4;
    return std::uint32_t(edram[offset]) | (std::uint32_t(edram[offset + 1]) << 8) |
           (std::uint32_t(edram[offset + 2]) << 16) | (std::uint32_t(edram[offset + 3]) << 24);
}

void captureOracle(const std::filesystem::path& directory)
{
    const auto dest0  = readCapture(directory / "swap.resolve0.dest.bin", 4194304);
    const auto edram1 = readCapture(directory / "swap.resolve1.edram.bin", 10485760);
    const auto dest1  = readCapture(directory / "swap.resolve1.dest.bin", 3686400);
    // 10000410 proves 1X/selection 0; white alone cannot prove these or pitch
    // 1040.
    auto copy      = wholeCopy(1024, 1024);
    copy.destBase  = 0x1E3B0000;
    copy.destFetch = { 0x88000002, 0xFE3AF006, 0x007FE3FF, 0xC14, 0, 0x200 };
    GuestResolveImage               image;
    std::string                     error;
    const std::vector<std::uint8_t> white(4194304, 255);
    UNSCOPED_INFO(error);
    REQUIRE(EncodeGuestResolve(copy, white, 4096, image, error));
    UNSCOPED_INFO("resolve 0 differs from the complete white oracle");
    REQUIRE((image.physical == 0x1E3B0000 && image.bytes == dest0));

    // Resolve 1 descriptor 8A000002 FF6F7006 0059E4FF 00001414 0 200:
    // tiled 1280x720, pitch 1280, base 1F6F8000, swap 1, endian 0, bias 0.
    constexpr std::uint32_t destInfo = 0x01000300;
    // Flags 300 and source 2X imply CONTROL 00100340, PITCH 02D00500.
    // Constants were not captured. Attachment words come from scene event 43;
    // they are assumptions for this capture, not uniquely fixed by byte matching.
    UNSCOPED_INFO("resolve 1 capture extent differs from SDK upper bound");
    REQUIRE(referenceExtent(640, 720, 1280) == dest1.size());
    std::vector<std::uint8_t> reconstructed(dest1.size(), 0xCD), wrongEndian(dest1.size(), 0xCD),
        wrongSwap(dest1.size(), 0xCD), wrongTile(dest1.size(), 0xCD);
    for (unsigned y = 0; y < 720; ++y)
    {
        for (unsigned x = 0; x < 640; ++x)
        {
            const auto word = edramWord(edram1, x, y, 0);
            // Equal samples make the inferred average exact without float rounding.
            UNSCOPED_INFO("resolve 1 unequal MSAA samples need proven conversion and rounding");
            REQUIRE(word == edramWord(edram1, x, y, 1));
            UNSCOPED_INFO("resolve 1 placement exceeded extent");
            REQUIRE(detail::PlaceGuestResolveWord(reconstructed, x, y, 1280, word, destInfo));
            UNSCOPED_INFO("endian mutation exceeded extent");
            REQUIRE(detail::PlaceGuestResolveWord(wrongEndian, x, y, 1280, word, destInfo | 2));
            UNSCOPED_INFO("swap mutation exceeded extent");
            REQUIRE(detail::PlaceGuestResolveWord(wrongSwap, x, y, 1280, word, destInfo ^ (1U << 24)));
            const auto offset = referenceOffset(x, y, 1280);
            // A one-byte error in the tiled destination address.
            if (offset + 5 <= wrongTile.size())
            {
                std::copy_n(reconstructed.begin() + offset, 4, wrongTile.begin() + offset + 1);
            }
        }
    }
    const auto touchedDifferences = [&](const auto& bytes)
    {
        std::size_t differences = 0;
        for (unsigned y = 0; y < 720; ++y)
        {
            for (unsigned x = 0; x < 640; ++x)
            {
                const auto offset = referenceOffset(x, y, 1280);
                for (unsigned c = 0; c < 4; ++c)
                {
                    differences += bytes[offset + c] != dest1[offset + c];
                }
            }
        }
        return differences;
    };
    UNSCOPED_INFO("resolve 1 reconstruction differs on touched bytes");
    REQUIRE(touchedDifferences(reconstructed) == 0);
    UNSCOPED_INFO("resolve 1 cannot detect the wrong endian mutation");
    REQUIRE(touchedDifferences(wrongEndian) != 0);
    UNSCOPED_INFO("resolve 1 cannot detect the off-by-one tile offset mutation");
    REQUIRE(touchedDifferences(wrongTile) != 0);
    // This capture's touched texels are opaque black, so red/blue order is
    // unobservable here; the synthetic fixtures cover swap until a colored one exists.
    if (touchedDifferences(wrongSwap) == 0)
    {
        std::cout << "resolve 1 capture has equal red and blue; red/blue order unproven against Xenos\n";
    }
}

} // namespace

TEST_CASE("guest_resolve_encoder", "[ctest][guest-resolve-encoder][timeout-120]")
{
    addressingAndChannels();
    packedColorWords();
    depthWords();
    float16Words();
    fogDestination();
    packingMismatches();
    subRectangle();
    unsupportedInputs();
    captureDestinations();
    fetchAddressViews();
    unalignedRectangles();
    hallPanelResolve();
    groupedMatchesScalar();
    encodeTiming();
#ifdef _WIN32
    char*       captureBuffer = nullptr;
    std::size_t captureSize   = 0;
    UNSCOPED_INFO("could not read REREVVED_RESOLVE_CAPTURE");
    REQUIRE(_dupenv_s(&captureBuffer, &captureSize, "REREVVED_RESOLVE_CAPTURE") == 0);
    const std::unique_ptr<char, decltype(&std::free)> captureOwner(captureBuffer, &std::free);
    const auto*                                       capture = captureOwner.get();
#else
    const auto* capture = std::getenv("REREVVED_RESOLVE_CAPTURE");
#endif
    if (!capture)
    {
        SKIP("Set REREVVED_RESOLVE_CAPTURE to a resolve capture directory to run the capture oracle");
    }
    UNSCOPED_INFO("REREVVED_RESOLVE_CAPTURE is set but empty");
    REQUIRE(*capture != '\0');
    captureOracle(capture);
}
