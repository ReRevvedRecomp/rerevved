#include "retail_ui_art.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <string_view>
#include <system_error>

namespace rerevved::retail_ui_art
{

namespace
{

constexpr size_t   kFpkHeaderBytes   = 14;
constexpr size_t   kFpkMinEntryBytes = 4 + 16; // name length + four u32 fields
constexpr uint32_t kFpkMaxNameBytes  = 1024;
// The real tables are small (the largest, leaderhead.FPK, is about 376 KiB);
// this bound only stops a corrupt header from forcing a large read.
constexpr uint64_t kMaxTablePrefixBytes = 8ull * 1024 * 1024;

constexpr size_t   kDdsHeaderBytes = 128;
constexpr uint32_t kDdsMaxDim      = 4096;

constexpr uint32_t kDdsPfAlphaPixels = 0x00000001;
constexpr uint32_t kDdsPfFourCc      = 0x00000004;
constexpr uint32_t kDdsPfRgb         = 0x00000040;
constexpr uint32_t kDdsCaps2Cubemap  = 0x00000200;
constexpr uint32_t kDdsCaps2Volume   = 0x00200000;

constexpr uint32_t makeFourCc(char a, char b, char c, char d)
{
    return uint32_t{ static_cast<uint8_t>(a) } |
           (uint32_t{ static_cast<uint8_t>(b) } << 8) |
           (uint32_t{ static_cast<uint8_t>(c) } << 16) |
           (uint32_t{ static_cast<uint8_t>(d) } << 24);
}

constexpr uint32_t kFourCcDxt1 = makeFourCc('D', 'X', 'T', '1');
constexpr uint32_t kFourCcDxt3 = makeFourCc('D', 'X', 'T', '3');
constexpr uint32_t kFourCcDxt5 = makeFourCc('D', 'X', 'T', '5');

void setError(std::string* error, const char* message)
{
    if (error != nullptr)
    {
        *error = message;
    }
}

uint32_t readLittleEndianU32(const uint8_t* value)
{
    return uint32_t{ value[0] } | (uint32_t{ value[1] } << 8) |
           (uint32_t{ value[2] } << 16) | (uint32_t{ value[3] } << 24);
}

uint16_t readLittleEndianU16(const uint8_t* value)
{
    return static_cast<uint16_t>(uint32_t{ value[0] } | (uint32_t{ value[1] } << 8));
}

char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoreAsciiCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (asciiLower(a[i]) != asciiLower(b[i]))
        {
            return false;
        }
    }
    return true;
}

// One color or alpha channel of an uncompressed pixel, described by a mask.
struct ChannelMask
{
    uint32_t mask  = 0;
    uint32_t shift = 0;
    uint32_t max   = 0; // largest value the mask can hold

    uint8_t extract(uint32_t pixel) const
    {
        const uint32_t value = (pixel & mask) >> shift;
        // Round-to-nearest rescale; exact for the common 8-bit case.
        return static_cast<uint8_t>((uint64_t{ value } * 255u + max / 2u) / max);
    }
};

// Accepts one contiguous run of set bits. Other layouts are not worth decoding.
bool makeChannelMask(uint32_t mask, ChannelMask& out)
{
    if (mask == 0)
    {
        return false;
    }
    const uint32_t shift = static_cast<uint32_t>(std::countr_zero(mask));
    const uint32_t run   = mask >> shift;
    if ((run & (run + 1)) != 0)
    {
        return false;
    }
    out = { mask, shift, run };
    return true;
}

// Bit replication, so 0 maps to 0 and the maximum value maps to 255.
uint8_t expand565Red(uint32_t c)
{
    const uint32_t v = (c >> 11) & 31;
    return static_cast<uint8_t>((v << 3) | (v >> 2));
}

uint8_t expand565Green(uint32_t c)
{
    const uint32_t v = (c >> 5) & 63;
    return static_cast<uint8_t>((v << 2) | (v >> 4));
}

uint8_t expand565Blue(uint32_t c)
{
    const uint32_t v = c & 31;
    return static_cast<uint8_t>((v << 3) | (v >> 2));
}

using Rgba = std::array<uint8_t, 4>;

// Decodes the 8-byte color half of a DXT block into 16 pixels (row-major).
// DXT1 selects the three-color-plus-transparent mode when c0 <= c1; DXT3 and
// DXT5 always use the four-color mode because their alpha is stored separately.
void decodeColorBlock(const uint8_t* block, bool allowTransparent, std::array<Rgba, 16>& out)
{
    const uint32_t c0 = readLittleEndianU16(block);
    const uint32_t c1 = readLittleEndianU16(block + 2);

    std::array<Rgba, 4> palette{};
    palette[0] = { expand565Red(c0), expand565Green(c0), expand565Blue(c0), 255 };
    palette[1] = { expand565Red(c1), expand565Green(c1), expand565Blue(c1), 255 };
    if (!allowTransparent || c0 > c1)
    {
        for (size_t i = 0; i < 3; ++i)
        {
            palette[2][i] = static_cast<uint8_t>((2u * palette[0][i] + palette[1][i]) / 3u);
            palette[3][i] = static_cast<uint8_t>((palette[0][i] + 2u * palette[1][i]) / 3u);
        }
        palette[2][3] = 255;
        palette[3][3] = 255;
    }
    else
    {
        for (size_t i = 0; i < 3; ++i)
        {
            palette[2][i] = static_cast<uint8_t>((palette[0][i] + palette[1][i]) / 2u);
        }
        palette[2][3] = 255;
        palette[3]    = { 0, 0, 0, 0 };
    }

    const uint32_t indices = readLittleEndianU32(block + 4);
    for (size_t i = 0; i < 16; ++i)
    {
        out[i] = palette[(indices >> (2 * i)) & 3u];
    }
}

// DXT3: 4 bits of explicit alpha per pixel, low nibble first.
void decodeExplicitAlpha(const uint8_t* block, std::array<uint8_t, 16>& out)
{
    for (size_t i = 0; i < 16; ++i)
    {
        const uint8_t nibble = (block[i / 2] >> ((i % 2) * 4)) & 0x0f;
        out[i]               = static_cast<uint8_t>(nibble * 17);
    }
}

// DXT5: two endpoint alphas plus 3-bit indices into an interpolated ramp.
void decodeInterpolatedAlpha(const uint8_t* block, std::array<uint8_t, 16>& out)
{
    const uint32_t a0 = block[0];
    const uint32_t a1 = block[1];

    std::array<uint8_t, 8> ramp{};
    ramp[0] = static_cast<uint8_t>(a0);
    ramp[1] = static_cast<uint8_t>(a1);
    if (a0 > a1)
    {
        for (uint32_t i = 1; i <= 6; ++i)
        {
            ramp[1 + i] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
        }
    }
    else
    {
        for (uint32_t i = 1; i <= 4; ++i)
        {
            ramp[1 + i] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
        }
        ramp[6] = 0;
        ramp[7] = 255;
    }

    uint64_t bits = 0;
    for (size_t i = 0; i < 6; ++i)
    {
        bits |= uint64_t{ block[2 + i] } << (8 * i);
    }
    for (size_t i = 0; i < 16; ++i)
    {
        out[i] = ramp[(bits >> (3 * i)) & 7u];
    }
}

enum class BlockFormat
{
    Dxt1,
    Dxt3,
    Dxt5,
};

std::optional<Image> decodeBlockCompressed(const uint8_t* data,
                                           size_t         dataBytes,
                                           uint32_t       width,
                                           uint32_t       height,
                                           BlockFormat    format,
                                           std::string*   error)
{
    const size_t   blockBytes = (format == BlockFormat::Dxt1) ? 8 : 16;
    const uint32_t blocksX    = (width + 3) / 4;
    const uint32_t blocksY    = (height + 3) / 4;
    const uint64_t needed     = uint64_t{ blocksX } * blocksY * blockBytes;
    if (needed > dataBytes)
    {
        setError(error, "DDS pixel data is truncated");
        return std::nullopt;
    }

    Image image;
    image.width  = width;
    image.height = height;
    image.rgba.assign(size_t{ width } * height * 4, 0);

    for (uint32_t by = 0; by < blocksY; ++by)
    {
        for (uint32_t bx = 0; bx < blocksX; ++bx)
        {
            const uint8_t* block = data + (size_t{ by } * blocksX + bx) * blockBytes;

            std::array<Rgba, 16>    colors{};
            std::array<uint8_t, 16> alpha{};
            if (format == BlockFormat::Dxt1)
            {
                decodeColorBlock(block, true, colors);
            }
            else
            {
                if (format == BlockFormat::Dxt3)
                {
                    decodeExplicitAlpha(block, alpha);
                }
                else
                {
                    decodeInterpolatedAlpha(block, alpha);
                }
                decodeColorBlock(block + 8, false, colors);
            }

            // Blocks that overhang a non-multiple-of-4 edge are clipped.
            for (uint32_t py = 0; py < 4 && by * 4 + py < height; ++py)
            {
                for (uint32_t px = 0; px < 4 && bx * 4 + px < width; ++px)
                {
                    const size_t i = size_t{ py } * 4 + px;
                    uint8_t*     out =
                        &image.rgba[(size_t{ by * 4 + py } * width + (bx * 4 + px)) * 4];
                    out[0] = colors[i][0];
                    out[1] = colors[i][1];
                    out[2] = colors[i][2];
                    out[3] = (format == BlockFormat::Dxt1) ? colors[i][3] : alpha[i];
                }
            }
        }
    }
    return image;
}

std::optional<Image> decodeUncompressed32(const uint8_t* header,
                                          const uint8_t* data,
                                          size_t         dataBytes,
                                          uint32_t       width,
                                          uint32_t       height,
                                          std::string*   error)
{
    const uint32_t pfFlags = readLittleEndianU32(header + 80);
    if ((pfFlags & kDdsPfRgb) == 0 || readLittleEndianU32(header + 88) != 32)
    {
        setError(error, "DDS pixel format is not 32-bit RGB or a supported FourCC");
        return std::nullopt;
    }

    ChannelMask red, green, blue, alpha;
    if (!makeChannelMask(readLittleEndianU32(header + 92), red) ||
        !makeChannelMask(readLittleEndianU32(header + 96), green) ||
        !makeChannelMask(readLittleEndianU32(header + 100), blue))
    {
        setError(error, "DDS color masks are not contiguous");
        return std::nullopt;
    }

    const uint32_t usedBits = red.mask | green.mask | blue.mask;
    if ((red.mask & green.mask) != 0 || (red.mask & blue.mask) != 0 ||
        (green.mask & blue.mask) != 0)
    {
        setError(error, "DDS color masks overlap");
        return std::nullopt;
    }

    // Without the alpha flag the format is X8R8G8B8-style: fully opaque.
    const uint32_t alphaMask = readLittleEndianU32(header + 104);
    const bool     hasAlpha  = (pfFlags & kDdsPfAlphaPixels) != 0 && alphaMask != 0;
    if (hasAlpha)
    {
        if (!makeChannelMask(alphaMask, alpha) || (alphaMask & usedBits) != 0)
        {
            setError(error, "DDS alpha mask is invalid");
            return std::nullopt;
        }
    }

    const uint64_t needed = uint64_t{ width } * height * 4;
    if (needed > dataBytes)
    {
        setError(error, "DDS pixel data is truncated");
        return std::nullopt;
    }

    Image image;
    image.width  = width;
    image.height = height;
    image.rgba.resize(size_t{ width } * height * 4);
    const size_t pixelCount = size_t{ width } * height;
    for (size_t i = 0; i < pixelCount; ++i)
    {
        const uint32_t pixel = readLittleEndianU32(data + i * 4);
        uint8_t*       out   = &image.rgba[i * 4];
        out[0]               = red.extract(pixel);
        out[1]               = green.extract(pixel);
        out[2]               = blue.extract(pixel);
        out[3]               = hasAlpha ? alpha.extract(pixel) : 255;
    }
    return image;
}

std::optional<std::vector<uint8_t>> readEntry(const std::filesystem::path& archive,
                                              std::string_view             entryName,
                                              std::string*                 error)
{
    std::error_code ec;
    const uint64_t  fileSize = std::filesystem::file_size(archive, ec);
    if (ec)
    {
        setError(error, "cannot determine archive size");
        return std::nullopt;
    }

    std::ifstream file(archive, std::ios::binary);
    if (!file)
    {
        setError(error, "cannot open archive");
        return std::nullopt;
    }

    const size_t         prefixBytes = static_cast<size_t>(std::min(fileSize, kMaxTablePrefixBytes));
    std::vector<uint8_t> prefix(prefixBytes);
    file.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefixBytes));
    if (static_cast<size_t>(file.gcount()) != prefixBytes)
    {
        setError(error, "cannot read archive table");
        return std::nullopt;
    }

    const auto table = ParseFpkTable(prefix, error);
    if (!table)
    {
        return std::nullopt;
    }

    const FpkEntry* found = nullptr;
    for (const FpkEntry& entry : *table)
    {
        if (equalsIgnoreAsciiCase(entry.name, entryName))
        {
            found = &entry;
            break;
        }
    }
    if (found == nullptr)
    {
        setError(error, "entry not found in archive");
        return std::nullopt;
    }
    if (found->size > kMaxEntryBytes)
    {
        setError(error, "entry exceeds the size limit");
        return std::nullopt;
    }
    // Both values are at most 32 bits wide, so the sum cannot wrap.
    if (found->offset < kFpkHeaderBytes || found->offset + found->size > fileSize)
    {
        setError(error, "entry lies outside the archive");
        return std::nullopt;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(found->size));
    file.seekg(static_cast<std::streamoff>(found->offset));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || static_cast<size_t>(file.gcount()) != bytes.size())
    {
        setError(error, "cannot read entry data");
        return std::nullopt;
    }
    return bytes;
}

} // namespace

std::optional<std::vector<FpkEntry>> ParseFpkTable(std::span<const uint8_t> bytes,
                                                   std::string*             error)
{
    try
    {
        if (bytes.size() < kFpkHeaderBytes)
        {
            setError(error, "FPK header is truncated");
            return std::nullopt;
        }
        if (std::memcmp(bytes.data() + 4, "FPK_", 4) != 0)
        {
            setError(error, "FPK signature mismatch");
            return std::nullopt;
        }

        const uint32_t count = readLittleEndianU32(bytes.data() + 10);
        // Each entry needs at least kFpkMinEntryBytes, so a count that cannot fit
        // is corrupt; this also bounds the reserve below.
        if (count > (bytes.size() - kFpkHeaderBytes) / kFpkMinEntryBytes)
        {
            setError(error, "FPK entry count does not fit the table");
            return std::nullopt;
        }

        std::vector<FpkEntry> entries;
        entries.reserve(count);

        size_t pos = kFpkHeaderBytes;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (bytes.size() - pos < 4)
            {
                setError(error, "FPK table is truncated");
                return std::nullopt;
            }
            const uint32_t nameLength = readLittleEndianU32(bytes.data() + pos);
            pos += 4;
            if (nameLength > kFpkMaxNameBytes)
            {
                setError(error, "FPK entry name is too long");
                return std::nullopt;
            }

            // Names are padded to a 4-byte boundary.
            const size_t paddedLength = (size_t{ nameLength } + 3) & ~size_t{ 3 };
            if (bytes.size() - pos < paddedLength + 16)
            {
                setError(error, "FPK table is truncated");
                return std::nullopt;
            }

            FpkEntry entry;
            entry.name.assign(reinterpret_cast<const char*>(bytes.data() + pos), nameLength);
            pos += paddedLength;

            // Fields: two unknown u32 (a timestamp-like value and zero), size, offset.
            entry.size   = readLittleEndianU32(bytes.data() + pos + 8);
            entry.offset = readLittleEndianU32(bytes.data() + pos + 12);
            pos += 16;
            entries.push_back(std::move(entry));
        }
        return entries;
    }
    catch (const std::exception&)
    {
        setError(error, "out of memory while parsing FPK table");
        return std::nullopt;
    }
}

std::optional<std::vector<uint8_t>> ReadFpkEntry(const std::filesystem::path& archive,
                                                 std::string_view             entry_name,
                                                 std::string*                 error)
{
    try
    {
        return readEntry(archive, entry_name, error);
    }
    catch (const std::exception&)
    {
        setError(error, "failed to read archive entry");
        return std::nullopt;
    }
}

std::optional<Image> DecodeDds(std::span<const uint8_t> bytes, std::string* error)
{
    try
    {
        if (bytes.size() < kDdsHeaderBytes || std::memcmp(bytes.data(), "DDS ", 4) != 0)
        {
            setError(error, "not a DDS file");
            return std::nullopt;
        }

        const uint8_t* header = bytes.data();
        if (readLittleEndianU32(header + 4) != 124 || readLittleEndianU32(header + 76) != 32)
        {
            setError(error, "DDS header size is invalid");
            return std::nullopt;
        }

        const uint32_t height = readLittleEndianU32(header + 12);
        const uint32_t width  = readLittleEndianU32(header + 16);
        if (width == 0 || height == 0 || width > kDdsMaxDim || height > kDdsMaxDim)
        {
            setError(error, "DDS dimensions are out of range");
            return std::nullopt;
        }

        if ((readLittleEndianU32(header + 112) & (kDdsCaps2Cubemap | kDdsCaps2Volume)) != 0)
        {
            setError(error, "DDS cube maps and volumes are not supported");
            return std::nullopt;
        }

        const uint8_t* data      = bytes.data() + kDdsHeaderBytes;
        const size_t   dataBytes = bytes.size() - kDdsHeaderBytes;

        const uint32_t pfFlags = readLittleEndianU32(header + 80);
        if ((pfFlags & kDdsPfFourCc) == 0)
        {
            return decodeUncompressed32(header, data, dataBytes, width, height, error);
        }

        // Any other FourCC, including the "DX10" extension header, is rejected.
        const uint32_t fourCc = readLittleEndianU32(header + 84);
        if (fourCc == kFourCcDxt1)
        {
            return decodeBlockCompressed(data, dataBytes, width, height, BlockFormat::Dxt1, error);
        }
        if (fourCc == kFourCcDxt3)
        {
            return decodeBlockCompressed(data, dataBytes, width, height, BlockFormat::Dxt3, error);
        }
        if (fourCc == kFourCcDxt5)
        {
            return decodeBlockCompressed(data, dataBytes, width, height, BlockFormat::Dxt5, error);
        }
        setError(error, "unsupported DDS FourCC");
        return std::nullopt;
    }
    catch (const std::exception&)
    {
        setError(error, "out of memory while decoding DDS");
        return std::nullopt;
    }
}

} // namespace rerevved::retail_ui_art
