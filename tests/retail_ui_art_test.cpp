#include "retail_ui_art.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

namespace art = rerevved::retail_ui_art;

void writeLittleEndianU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    bytes[offset + 0] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

void appendLittleEndianU32(std::vector<uint8_t>& bytes, uint32_t value)
{
    bytes.resize(bytes.size() + 4);
    writeLittleEndianU32(bytes, bytes.size() - 4, value);
}

void appendLittleEndianU16(std::vector<uint8_t>& bytes, uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void appendBytes(std::vector<uint8_t>& bytes, std::initializer_list<uint8_t> values)
{
    bytes.insert(bytes.end(), values.begin(), values.end());
}

constexpr uint32_t fourCc(char a, char b, char c, char d)
{
    return uint32_t{ static_cast<uint8_t>(a) } |
           (uint32_t{ static_cast<uint8_t>(b) } << 8) |
           (uint32_t{ static_cast<uint8_t>(c) } << 16) |
           (uint32_t{ static_cast<uint8_t>(d) } << 24);
}

struct DdsFormat
{
    uint32_t flags    = 0;
    uint32_t fourCc   = 0;
    uint32_t bitCount = 0;
    uint32_t rMask    = 0;
    uint32_t gMask    = 0;
    uint32_t bMask    = 0;
    uint32_t aMask    = 0;
};

constexpr DdsFormat kA8R8G8B8 = { 0x41, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000 };
constexpr DdsFormat kA8B8G8R8 = { 0x41, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000 };
constexpr DdsFormat kX8R8G8B8 = { 0x40, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0 };
constexpr DdsFormat kDxt1     = { 0x04, fourCc('D', 'X', 'T', '1'), 0, 0, 0, 0, 0 };
constexpr DdsFormat kDxt3     = { 0x04, fourCc('D', 'X', 'T', '3'), 0, 0, 0, 0, 0 };
constexpr DdsFormat kDxt5     = { 0x04, fourCc('D', 'X', 'T', '5'), 0, 0, 0, 0, 0 };

std::vector<uint8_t> makeDds(uint32_t                    width,
                             uint32_t                    height,
                             const DdsFormat&            format,
                             const std::vector<uint8_t>& payload,
                             uint32_t                    caps2 = 0)
{
    std::vector<uint8_t> bytes(128, 0);
    std::memcpy(bytes.data(), "DDS ", 4);
    writeLittleEndianU32(bytes, 4, 124);
    writeLittleEndianU32(bytes, 8, 0x00001007);
    writeLittleEndianU32(bytes, 12, height);
    writeLittleEndianU32(bytes, 16, width);
    writeLittleEndianU32(bytes, 76, 32);
    writeLittleEndianU32(bytes, 80, format.flags);
    writeLittleEndianU32(bytes, 84, format.fourCc);
    writeLittleEndianU32(bytes, 88, format.bitCount);
    writeLittleEndianU32(bytes, 92, format.rMask);
    writeLittleEndianU32(bytes, 96, format.gMask);
    writeLittleEndianU32(bytes, 100, format.bMask);
    writeLittleEndianU32(bytes, 104, format.aMask);
    writeLittleEndianU32(bytes, 108, 0x1000);
    writeLittleEndianU32(bytes, 112, caps2);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

// A DXT color block: two RGB565 endpoints and 16 two-bit indices.
std::vector<uint8_t> colorBlock(uint16_t c0, uint16_t c1, uint32_t indices)
{
    std::vector<uint8_t> bytes;
    appendLittleEndianU16(bytes, c0);
    appendLittleEndianU16(bytes, c1);
    appendLittleEndianU32(bytes, indices);
    return bytes;
}

constexpr uint16_t kRed565   = 0xf800;
constexpr uint16_t kGreen565 = 0x07e0;
constexpr uint16_t kBlue565  = 0x001f;

struct Rgba
{
    uint8_t r, g, b, a;
};

bool pixelIs(const art::Image& image, uint32_t x, uint32_t y, Rgba expected)
{
    const uint8_t* p = &image.rgba[(size_t{ y } * image.width + x) * 4];
    return p[0] == expected.r && p[1] == expected.g && p[2] == expected.b && p[3] == expected.a;
}

void requirePixel(const art::Image& image, uint32_t x, uint32_t y, Rgba expected, std::string_view message)
{
    const uint8_t* p = &image.rgba[(size_t{ y } * image.width + x) * 4];
    UNSCOPED_INFO(message << " (got " << int{ p[0] } << ',' << int{ p[1] } << ',' << int{ p[2] } << ',' << int{ p[3] } << ")");
    REQUIRE(pixelIs(image, x, y, expected));
}

art::Image requireDecoded(const std::vector<uint8_t>& dds,
                          uint32_t                    width,
                          uint32_t                    height,
                          std::string_view            message)
{
    std::string error;
    auto        image = art::DecodeDds(dds, &error);
    UNSCOPED_INFO(message);
    REQUIRE(image.has_value());
    UNSCOPED_INFO(message);
    REQUIRE((image->width == width && image->height == height &&
             image->rgba.size() == size_t{ width } * height * 4));
    return std::move(*image);
}

void requireRejected(const std::vector<uint8_t>& dds, std::string_view message)
{
    std::string error;
    UNSCOPED_INFO(message);
    REQUIRE(!art::DecodeDds(dds, &error).has_value());
    UNSCOPED_INFO("rejection did not set an error message");
    REQUIRE(!error.empty());
    // The error pointer is optional.
    UNSCOPED_INFO(message);
    REQUIRE(!art::DecodeDds(dds, nullptr).has_value());
}

void TestUncompressed()
{
    // A8R8G8B8 pixels are stored as B, G, R, A bytes.
    std::vector<uint8_t> payload;
    appendBytes(payload, { 0x30, 0x20, 0x10, 0x80, // R=0x10 G=0x20 B=0x30 A=0x80
                           0x00,
                           0x00,
                           0xff,
                           0xff, // red, opaque
                           0x01,
                           0x02,
                           0x03,
                           0x04,
                           0xff,
                           0xff,
                           0xff,
                           0x00 });
    auto image = requireDecoded(makeDds(2, 2, kA8R8G8B8, payload), 2, 2, "A8R8G8B8 rejected");
    requirePixel(image, 0, 0, { 0x10, 0x20, 0x30, 0x80 }, "A8R8G8B8 pixel (0,0) wrong");
    requirePixel(image, 1, 0, { 0xff, 0x00, 0x00, 0xff }, "A8R8G8B8 pixel (1,0) wrong");
    requirePixel(image, 0, 1, { 0x03, 0x02, 0x01, 0x04 }, "A8R8G8B8 row order wrong");
    requirePixel(image, 1, 1, { 0xff, 0xff, 0xff, 0x00 }, "A8R8G8B8 alpha wrong");

    // A8B8G8R8 pixels are stored as R, G, B, A bytes.
    payload.clear();
    appendBytes(payload, { 0x10, 0x20, 0x30, 0x80 });
    image = requireDecoded(makeDds(1, 1, kA8B8G8R8, payload), 1, 1, "A8B8G8R8 rejected");
    requirePixel(image, 0, 0, { 0x10, 0x20, 0x30, 0x80 }, "A8B8G8R8 channel order wrong");

    // No alpha flag: the fourth byte is padding and the pixel is opaque.
    payload.clear();
    appendBytes(payload, { 0x30, 0x20, 0x10, 0x00 });
    image = requireDecoded(makeDds(1, 1, kX8R8G8B8, payload), 1, 1, "X8R8G8B8 rejected");
    requirePixel(image, 0, 0, { 0x10, 0x20, 0x30, 0xff }, "X8R8G8B8 not opaque");

    // 6x5 is not block aligned; rows are tightly packed and the last pixel is
    // the last four bytes.
    payload.assign(6 * 5 * 4, 0);
    for (size_t i = 0; i < 30; ++i)
    {
        payload[i * 4 + 2] = static_cast<uint8_t>(i); // red
        payload[i * 4 + 3] = 0xff;
    }
    image = requireDecoded(makeDds(6, 5, kA8R8G8B8, payload), 6, 5, "6x5 A8R8G8B8 rejected");
    requirePixel(image, 5, 4, { 29, 0, 0, 0xff }, "6x5 last pixel wrong");
    requirePixel(image, 0, 1, { 6, 0, 0, 0xff }, "6x5 row stride wrong");

    // Extra bytes after the top level (lower mips) are ignored.
    payload.assign(4 + 16, 0xaa);
    appendBytes(payload, { 0, 0, 0, 0 });
    requireDecoded(makeDds(1, 1, kA8R8G8B8, payload), 1, 1, "trailing mip data rejected");
}

void TestDxt1()
{
    // Four-color mode (c0 > c1): indices 0,1,2,3 across the first row.
    auto block = colorBlock(kRed565, kBlue565, 0b11'10'01'00);
    auto image = requireDecoded(makeDds(4, 4, kDxt1, block), 4, 4, "DXT1 rejected");
    requirePixel(image, 0, 0, { 255, 0, 0, 255 }, "DXT1 color0 wrong");
    requirePixel(image, 1, 0, { 0, 0, 255, 255 }, "DXT1 color1 wrong");
    requirePixel(image, 2, 0, { 170, 0, 85, 255 }, "DXT1 color2 wrong");
    requirePixel(image, 3, 0, { 85, 0, 170, 255 }, "DXT1 color3 wrong");
    // Rows above are index 0 for the remaining pixels.
    requirePixel(image, 3, 3, { 255, 0, 0, 255 }, "DXT1 bottom-right wrong");

    // Three-color mode (c0 <= c1): midpoint plus transparent black.
    block = colorBlock(kGreen565, kRed565, 0b11'10'01'00);
    image = requireDecoded(makeDds(4, 4, kDxt1, block), 4, 4, "DXT1 3-color rejected");
    requirePixel(image, 0, 0, { 0, 255, 0, 255 }, "DXT1 3-color c0 wrong");
    requirePixel(image, 1, 0, { 255, 0, 0, 255 }, "DXT1 3-color c1 wrong");
    requirePixel(image, 2, 0, { 127, 127, 0, 255 }, "DXT1 3-color midpoint wrong");
    requirePixel(image, 3, 0, { 0, 0, 0, 0 }, "DXT1 transparent texel wrong");

    // Equal endpoints also select the three-color mode.
    block = colorBlock(kBlue565, kBlue565, 0b11'11'11'11);
    image = requireDecoded(makeDds(4, 4, kDxt1, block), 4, 4, "DXT1 equal endpoints rejected");
    requirePixel(image, 0, 0, { 0, 0, 0, 0 }, "DXT1 equal endpoints not transparent");

    // 6x5: 2x2 blocks. Each block is a solid color so clipping and block
    // addressing are both checked.
    std::vector<uint8_t> payload;
    for (uint16_t color : { kRed565, kGreen565, kBlue565, uint16_t{ 0xffff } })
    {
        const auto solid = colorBlock(color, 0, 0); // c0 > c1, index 0 everywhere
        payload.insert(payload.end(), solid.begin(), solid.end());
    }
    image = requireDecoded(makeDds(6, 5, kDxt1, payload), 6, 5, "DXT1 6x5 rejected");
    requirePixel(image, 0, 0, { 255, 0, 0, 255 }, "DXT1 6x5 block (0,0) wrong");
    requirePixel(image, 4, 3, { 0, 255, 0, 255 }, "DXT1 6x5 block (1,0) wrong");
    requirePixel(image, 3, 4, { 0, 0, 255, 255 }, "DXT1 6x5 block (0,1) wrong");
    requirePixel(image, 5, 4, { 255, 255, 255, 255 }, "DXT1 6x5 block (1,1) wrong");
}

void TestDxt3()
{
    // Explicit alpha: pixel i of the row-major block uses nibble i, low first.
    std::vector<uint8_t> payload;
    appendBytes(payload, { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe });
    // c0 <= c1 must still decode as four-color mode for DXT3.
    auto color = colorBlock(kGreen565, kRed565, 0b11'10'01'00);
    payload.insert(payload.end(), color.begin(), color.end());

    auto image = requireDecoded(makeDds(4, 4, kDxt3, payload), 4, 4, "DXT3 rejected");
    requirePixel(image, 0, 0, { 0, 255, 0, 0 }, "DXT3 alpha 0 wrong");
    requirePixel(image, 1, 0, { 255, 0, 0, 17 }, "DXT3 alpha 1 wrong");
    requirePixel(image, 2, 0, { 85, 170, 0, 34 }, "DXT3 uses transparent mode");
    requirePixel(image, 3, 0, { 170, 85, 0, 51 }, "DXT3 four-color mode wrong");
    requirePixel(image, 3, 3, { 0, 255, 0, 255 }, "DXT3 alpha 15 wrong");
}

void TestDxt5()
{
    // a0 > a1: six interpolated alphas, codes 0..7 across the first eight texels.
    std::vector<uint8_t> payload;
    appendBytes(payload, { 255, 0 });
    // 3-bit codes 0..7 for the first eight pixels, little-endian bit packing.
    uint64_t bits = 0;
    for (uint64_t i = 0; i < 8; ++i)
    {
        bits |= i << (3 * i);
    }
    for (size_t i = 0; i < 6; ++i)
    {
        payload.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    }
    auto color = colorBlock(kBlue565, 0, 0);
    payload.insert(payload.end(), color.begin(), color.end());

    auto                             image     = requireDecoded(makeDds(4, 4, kDxt5, payload), 4, 4, "DXT5 rejected");
    constexpr std::array<uint8_t, 8> kExpected = { 255, 0, 218, 182, 145, 109, 72, 36 };
    for (uint32_t i = 0; i < 8; ++i)
    {
        requirePixel(image, i % 4, i / 4, { 0, 0, 255, kExpected[i] }, "DXT5 six-step alpha wrong");
    }

    // a0 <= a1: four interpolated alphas plus explicit 0 and 255.
    payload[0]                                  = 0;
    payload[1]                                  = 255;
    image                                       = requireDecoded(makeDds(4, 4, kDxt5, payload), 4, 4, "DXT5 8-alpha rejected");
    constexpr std::array<uint8_t, 8> kExpected4 = { 0, 255, 51, 102, 153, 204, 0, 255 };
    for (uint32_t i = 0; i < 8; ++i)
    {
        requirePixel(image, i % 4, i / 4, { 0, 0, 255, kExpected4[i] }, "DXT5 four-step alpha wrong");
    }
}

void TestDdsRejections()
{
    const std::vector<uint8_t> pixel = { 0, 0, 0, 0 };
    const auto                 valid = makeDds(1, 1, kA8R8G8B8, pixel);
    requireDecoded(valid, 1, 1, "baseline DDS rejected");

    requireRejected({}, "empty DDS accepted");
    requireRejected(std::vector<uint8_t>(valid.begin(), valid.begin() + 100), "short header accepted");

    auto bad = valid;
    bad[0]   = 'X';
    requireRejected(bad, "bad DDS magic accepted");

    bad = valid;
    writeLittleEndianU32(bad, 4, 100);
    requireRejected(bad, "bad DDS header size accepted");

    requireRejected(makeDds(2, 2, kA8R8G8B8, pixel), "truncated uncompressed data accepted");
    requireRejected(makeDds(4, 4, kDxt1, std::vector<uint8_t>(7, 0)), "truncated DXT1 data accepted");
    requireRejected(makeDds(4, 4, kDxt5, std::vector<uint8_t>(15, 0)), "truncated DXT5 data accepted");

    DdsFormat dx10 = kDxt1;
    dx10.fourCc    = fourCc('D', 'X', '1', '0');
    requireRejected(makeDds(4, 4, dx10, std::vector<uint8_t>(64, 0)), "DX10 DDS accepted");

    DdsFormat ati = kDxt1;
    ati.fourCc    = fourCc('A', 'T', 'I', '2');
    requireRejected(makeDds(4, 4, ati, std::vector<uint8_t>(64, 0)), "unsupported FourCC accepted");

    DdsFormat dxt2 = kDxt1;
    dxt2.fourCc    = fourCc('D', 'X', 'T', '2');
    requireRejected(makeDds(4, 4, dxt2, std::vector<uint8_t>(64, 0)), "premultiplied DXT2 accepted");

    requireRejected(makeDds(1, 1, kA8R8G8B8, pixel, 0x200), "cube map accepted");
    requireRejected(makeDds(1, 1, kA8R8G8B8, pixel, 0x200000), "volume texture accepted");

    requireRejected(makeDds(0, 1, kA8R8G8B8, pixel), "zero width accepted");
    requireRejected(makeDds(1, 0, kA8R8G8B8, pixel), "zero height accepted");
    requireRejected(makeDds(4097, 1, kDxt1, std::vector<uint8_t>(8, 0)), "oversized width accepted");
    requireRejected(makeDds(1, 4097, kDxt1, std::vector<uint8_t>(8, 0)), "oversized height accepted");
    requireRejected(makeDds(0xffffffffu, 0xffffffffu, kDxt1, std::vector<uint8_t>(8, 0)),
                    "huge dimensions accepted");

    DdsFormat sixteenBit = { 0x41, 0, 16, 0x7c00, 0x03e0, 0x001f, 0x8000 };
    requireRejected(makeDds(1, 1, sixteenBit, pixel), "16-bit RGB accepted");

    DdsFormat luminance = { 0x20000, 0, 8, 0xff, 0, 0, 0 };
    requireRejected(makeDds(1, 1, luminance, pixel), "luminance accepted");

    DdsFormat holey = kA8R8G8B8;
    holey.rMask     = 0x00ff00f0;
    requireRejected(makeDds(1, 1, holey, pixel), "non-contiguous mask accepted");

    DdsFormat overlap = kA8R8G8B8;
    overlap.gMask     = overlap.rMask;
    requireRejected(makeDds(1, 1, overlap, pixel), "overlapping masks accepted");

    DdsFormat noColor = kA8R8G8B8;
    noColor.bMask     = 0;
    requireRejected(makeDds(1, 1, noColor, pixel), "missing color mask accepted");
}

struct TestEntry
{
    std::string          name;
    std::vector<uint8_t> payload;
};

// Mirrors the retail layout: 14-byte header, entries, two bytes of padding,
// then the raw payloads back to back.
std::vector<uint8_t> makeFpk(const std::vector<TestEntry>& entries)
{
    std::vector<uint8_t> bytes;
    appendLittleEndianU32(bytes, 6);
    bytes.insert(bytes.end(), { 'F', 'P', 'K', '_' });
    appendBytes(bytes, { 0, 0 });
    appendLittleEndianU32(bytes, static_cast<uint32_t>(entries.size()));

    size_t tableBytes = bytes.size();
    for (const auto& entry : entries)
    {
        tableBytes += 4 + ((entry.name.size() + 3) & ~size_t{ 3 }) + 16;
    }
    size_t offset = tableBytes + 2;

    for (const auto& entry : entries)
    {
        appendLittleEndianU32(bytes, static_cast<uint32_t>(entry.name.size()));
        bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
        bytes.resize(bytes.size() + (((entry.name.size() + 3) & ~size_t{ 3 }) - entry.name.size()), 0);
        appendLittleEndianU32(bytes, 1204742308); // timestamp-like field
        appendLittleEndianU32(bytes, 0);
        appendLittleEndianU32(bytes, static_cast<uint32_t>(entry.payload.size()));
        appendLittleEndianU32(bytes, static_cast<uint32_t>(offset));
        offset += entry.payload.size();
    }
    bytes.resize(bytes.size() + 2, 0);
    for (const auto& entry : entries)
    {
        bytes.insert(bytes.end(), entry.payload.begin(), entry.payload.end());
    }
    return bytes;
}

std::vector<TestEntry> makeThreeEntries()
{
    // Name lengths 8, 5 and 14 give padding of 0, 3 and 2 bytes. The last
    // entry is empty so that its offset equals the end of the file.
    return {
        { "tile.dds", { 'D', 'D', 'S', ' ', 1, 2, 3 } },
        { "abcde", { 9, 8 } },
        { "Case_Mixed.dds", {} },
    };
}

void TestParseFpkTable()
{
    const auto  bytes = makeFpk(makeThreeEntries());
    std::string error;
    const auto  table = art::ParseFpkTable(bytes, &error);
    UNSCOPED_INFO("valid FPK table rejected");
    REQUIRE((table.has_value() && table->size() == 3));
    UNSCOPED_INFO("FPK entry 0 wrong");
    REQUIRE(((*table)[0].name == "tile.dds" && (*table)[0].size == 7));
    UNSCOPED_INFO("FPK entry 1 wrong");
    REQUIRE(((*table)[1].name == "abcde" && (*table)[1].size == 2));
    UNSCOPED_INFO("FPK entry 2 wrong");
    REQUIRE(((*table)[2].name == "Case_Mixed.dds" && (*table)[2].size == 0));
    UNSCOPED_INFO("FPK offsets wrong");
    REQUIRE(((*table)[0].offset + (*table)[0].size == (*table)[1].offset &&
             (*table)[1].offset + (*table)[1].size == (*table)[2].offset &&
             (*table)[2].offset + (*table)[2].size == bytes.size()));
    for (const auto& entry : *table)
    {
        // Entry offsets are validated against the file size by ReadFpkEntry only;
        // here they must at least point past the table.
        UNSCOPED_INFO("FPK offset inside the header");
        REQUIRE(entry.offset >= 14);
    }

    // A prefix that holds the whole table parses even without the payloads.
    const size_t tableEnd = static_cast<size_t>((*table)[0].offset) - 2;
    UNSCOPED_INFO("table-only prefix rejected");
    REQUIRE(art::ParseFpkTable(std::span(bytes).first(tableEnd), &error).has_value());

    // An empty archive is valid.
    UNSCOPED_INFO("empty FPK rejected");
    REQUIRE(art::ParseFpkTable(makeFpk({}), &error)->empty());

    // Every shorter prefix of the table is malformed.
    for (size_t length = 0; length < tableEnd; ++length)
    {
        error.clear();
        UNSCOPED_INFO("truncated FPK table accepted");
        REQUIRE(!art::ParseFpkTable(std::span(bytes).first(length), &error).has_value());
        UNSCOPED_INFO("truncated FPK table set no error");
        REQUIRE(!error.empty());
    }
    UNSCOPED_INFO("null error pointer changed the result");
    REQUIRE(!art::ParseFpkTable(std::span(bytes).first(tableEnd - 1), nullptr).has_value());
}

void TestParseFpkRejections()
{
    std::string error;
    const auto  valid = makeFpk(makeThreeEntries());

    auto bad = valid;
    bad[4]   = 'X';
    UNSCOPED_INFO("bad FPK signature accepted");
    REQUIRE(!art::ParseFpkTable(bad, &error).has_value());

    bad = valid;
    writeLittleEndianU32(bad, 10, 0x7fffffffu);
    UNSCOPED_INFO("absurd entry count accepted");
    REQUIRE(!art::ParseFpkTable(bad, &error).has_value());

    bad = valid;
    writeLittleEndianU32(bad, 10, 4); // one more entry than the table holds
    UNSCOPED_INFO("entry count past the table accepted");
    REQUIRE(!art::ParseFpkTable(std::span(bad).first(14 + 3 * 20 + 8), &error).has_value());

    bad = valid;
    writeLittleEndianU32(bad, 14, 0xfffffff0u); // first name length
    UNSCOPED_INFO("huge name length accepted");
    REQUIRE(!art::ParseFpkTable(bad, &error).has_value());

    bad = valid;
    writeLittleEndianU32(bad, 14, 5000); // longer than any real name
    UNSCOPED_INFO("overlong name accepted");
    REQUIRE(!art::ParseFpkTable(bad, &error).has_value());
}

class TempFile
{
public:
    explicit TempFile(std::string_view stem)
    {
        path_ = std::filesystem::temp_directory_path() /
                (std::string(stem) + "_rerevved_retail_ui_art_test.fpk");
    }

    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    TempFile(const TempFile&)            = delete;
    TempFile& operator=(const TempFile&) = delete;

    void write(const std::vector<uint8_t>& bytes) const
    {
        std::ofstream file(path_, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        UNSCOPED_INFO("could not write temporary FPK");
        REQUIRE(static_cast<bool>(file));
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void TestReadFpkEntry()
{
    const auto entries = makeThreeEntries();
    const auto bytes   = makeFpk(entries);
    TempFile   file("read");
    file.write(bytes);

    std::string error;
    auto        data = art::ReadFpkEntry(file.path(), "tile.dds", &error);
    UNSCOPED_INFO("entry read wrong");
    REQUIRE((data.has_value() && *data == entries[0].payload));

    data = art::ReadFpkEntry(file.path(), "TILE.DDS", &error);
    UNSCOPED_INFO("case-insensitive lookup failed");
    REQUIRE((data.has_value() && *data == entries[0].payload));

    data = art::ReadFpkEntry(file.path(), "case_mixed.dds", nullptr);
    UNSCOPED_INFO("empty entry read wrong");
    REQUIRE((data.has_value() && data->empty()));

    data = art::ReadFpkEntry(file.path(), "abcde", &error);
    UNSCOPED_INFO("middle entry wrong");
    REQUIRE((data.has_value() && *data == entries[1].payload));

    error.clear();
    UNSCOPED_INFO("missing entry accepted");
    REQUIRE((!art::ReadFpkEntry(file.path(), "missing.dds", &error).has_value() && !error.empty()));
    UNSCOPED_INFO("prefix of an entry name matched");
    REQUIRE(!art::ReadFpkEntry(file.path(), "tile.dd", &error).has_value());
    UNSCOPED_INFO("empty name matched");
    REQUIRE(!art::ReadFpkEntry(file.path(), "", &error).has_value());

    error.clear();
    UNSCOPED_INFO("missing archive accepted");
    REQUIRE((!art::ReadFpkEntry(file.path().string() + ".absent", "tile.dds", &error).has_value() &&
             !error.empty()));
}

void TestReadFpkEntryDecodesDds()
{
    std::vector<uint8_t> pixel = { 0x30, 0x20, 0x10, 0x80 };
    const auto           dds   = makeDds(1, 1, kA8R8G8B8, pixel);
    TempFile             file("dds");
    file.write(makeFpk({ { "other.bin", { 1, 2, 3, 4, 5 } }, { "Panel.dds", dds } }));

    std::string error;
    const auto  data = art::ReadFpkEntry(file.path(), "panel.DDS", &error);
    UNSCOPED_INFO("DDS entry read failed");
    REQUIRE(data.has_value());
    const auto image = art::DecodeDds(*data, &error);
    UNSCOPED_INFO("DDS entry decode failed");
    REQUIRE(image.has_value());
    requirePixel(*image, 0, 0, { 0x10, 0x20, 0x30, 0x80 }, "decoded entry pixel wrong");
}

void TestReadFpkEntryRejections()
{
    std::string error;

    // Payload cut short: the last entry reaches past EOF.
    auto bytes = makeFpk(makeThreeEntries());
    {
        TempFile file("eof");
        auto     cut = bytes;
        // Entry 1 holds the last payload byte; dropping it breaks that entry.
        cut.pop_back();
        file.write(cut);
        UNSCOPED_INFO("earlier entry rejected by later truncation");
        REQUIRE(art::ReadFpkEntry(file.path(), "tile.dds", &error).has_value());
        // The empty entry sits exactly at the old EOF; the file now ends before it.
        UNSCOPED_INFO("entry past EOF accepted");
        REQUIRE(!art::ReadFpkEntry(file.path(), "Case_Mixed.dds", &error).has_value());
        UNSCOPED_INFO("entry crossing EOF accepted");
        REQUIRE(!art::ReadFpkEntry(file.path(), "abcde", &error).has_value());
    }

    // Offset far past EOF.
    {
        TempFile file("offset");
        auto     bad = bytes;
        // First entry's offset field: header 14 + name length 4 + name 8 + 12.
        writeLittleEndianU32(bad, 14 + 4 + 8 + 12, 0xfffffff0u);
        file.write(bad);
        UNSCOPED_INFO("offset past EOF accepted");
        REQUIRE(!art::ReadFpkEntry(file.path(), "tile.dds", &error).has_value());
    }

    // Offset into the header.
    {
        TempFile file("header");
        auto     bad = bytes;
        writeLittleEndianU32(bad, 14 + 4 + 8 + 12, 4);
        file.write(bad);
        UNSCOPED_INFO("offset inside the header accepted");
        REQUIRE(!art::ReadFpkEntry(file.path(), "tile.dds", &error).has_value());
    }

    // Size above the entry limit, even though the file is small.
    {
        TempFile file("size");
        auto     bad = bytes;
        writeLittleEndianU32(bad, 14 + 4 + 8 + 8, static_cast<uint32_t>(art::kMaxEntryBytes + 1));
        file.write(bad);
        error.clear();
        UNSCOPED_INFO("oversized entry accepted");
        REQUIRE((!art::ReadFpkEntry(file.path(), "tile.dds", &error).has_value() && !error.empty()));
    }

    // Size that wraps a 32-bit sum with the offset.
    {
        TempFile file("wrap");
        auto     bad = bytes;
        writeLittleEndianU32(bad, 14 + 4 + 8 + 8, 0xffffff00u);
        file.write(bad);
        UNSCOPED_INFO("wrapping size accepted");
        REQUIRE(!art::ReadFpkEntry(file.path(), "tile.dds", &error).has_value());
    }

    // Truncated table and bad magic on disk.
    {
        TempFile file("table");
        file.write(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 40));
        UNSCOPED_INFO("truncated table accepted");
        REQUIRE(!art::ReadFpkEntry(file.path(), "tile.dds", &error).has_value());
        auto bad = bytes;
        bad[4]   = 'Z';
        file.write(bad);
        UNSCOPED_INFO("bad magic accepted");
        REQUIRE(!art::ReadFpkEntry(file.path(), "tile.dds", &error).has_value());
        file.write({});
        UNSCOPED_INFO("empty archive accepted");
        REQUIRE(!art::ReadFpkEntry(file.path(), "tile.dds", &error).has_value());
    }
}

} // namespace

TEST_CASE("retail_ui_art", "[ctest][retail-ui-art]")
{
    TestUncompressed();
    TestDxt1();
    TestDxt3();
    TestDxt5();
    TestDdsRejections();
    TestParseFpkTable();
    TestParseFpkRejections();
    TestReadFpkEntry();
    TestReadFpkEntryDecodesDds();
    TestReadFpkEntryRejections();
}
