#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rerevved::retail_ui_art
{

// Retail UI art lives in FPK archives as raw DDS payloads. This module reads
// one named entry and decodes a DDS image to RGBA8 for the settings skin. It
// is defensive about every length it reads and never throws.

inline constexpr uint64_t kMaxEntryBytes = 16ull * 1024 * 1024;

struct FpkEntry
{
    std::string name;
    uint64_t    offset = 0;
    uint64_t    size   = 0;
};

// Tightly packed RGBA8, straight alpha, row 0 is the top row.
struct Image
{
    uint32_t             width  = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> rgba;
};

// Parses the FPK header and entry table from the start of an archive. The bytes
// may be a prefix of the file; offsets are not checked against the file size.
// Returns nullopt with *error set on malformed or truncated input.
std::optional<std::vector<FpkEntry>> ParseFpkTable(std::span<const uint8_t> bytes,
                                                   std::string*             error);

// Reads a bounded prefix of the archive, finds entry_name (ASCII
// case-insensitive), validates offset + size against the file size, and returns
// that entry's bytes. Entries larger than kMaxEntryBytes are rejected.
std::optional<std::vector<uint8_t>> ReadFpkEntry(const std::filesystem::path& archive,
                                                 std::string_view             entry_name,
                                                 std::string*                 error);

// Decodes the top mip level of a DDS: 32-bit RGB(A) described by pixel-format
// masks, or DXT1, DXT3 and DXT5. Anything else returns nullopt with *error set.
// Dimensions must be nonzero and at most 4096.
std::optional<Image> DecodeDds(std::span<const uint8_t> bytes, std::string* error);

} // namespace rerevved::retail_ui_art
