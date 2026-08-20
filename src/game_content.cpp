#include "game_content.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <system_error>

#include <fmt/format.h>
#include <rex/crypto/sha256.h>

namespace rerevved
{

const char* const kBaseXexSha256    = "b59b8957a3ed9dd90e9296c96d5c7ab1b16078d3f08b015582714a06c7d6a7bd";
const char* const kUpdateXexpSha256 = "c1fc6149a63550987d991efdbb80e3697845a9a49d3f2ec180ea9817db8d12d4";

namespace
{

struct ManifestEntry
{
    const char*   relative_path;
    std::uint64_t size;
};

// Regenerate with scripts/gen-content-manifest.ps1 when this set changes.
constexpr ManifestEntry kResourceManifest[] = {
#include "content_manifest.inc"
};

// Header fields explain mismatches against the exact pinned hashes.
constexpr std::uint32_t kTitleId           = 0x545407E5u;
constexpr std::uint32_t kMediaId           = 0x7DC1293Bu;
constexpr std::uint32_t kBaseVersionValue  = 0x00000002u; // 0.0.0.2
constexpr std::uint32_t kUpdateTargetValue = 0x00000302u; // 0.0.3.2, game version 1.3
constexpr std::uint32_t kRegionFree        = 0xFFFFFFFFu;
constexpr std::uint64_t kBaseXexSize       = 16822272ull;
constexpr std::uint64_t kUpdateXexpSize    = 3291136ull;

constexpr std::uint32_t kXex2Magic          = 0x58455832u; // 'XEX2'
constexpr std::uint32_t kKeyExecutionInfo   = 0x00040006u;
constexpr std::uint32_t kKeyDeltaDescriptor = 0x000005FFu;

std::optional<std::vector<std::uint8_t>> ReadBytes(const std::filesystem::path& file, std::uint64_t offset, std::size_t count)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }

    stream.seekg(static_cast<std::streamoff>(offset));
    std::vector<std::uint8_t> bytes(count);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
    if (stream.gcount() != static_cast<std::streamsize>(count))
    {
        return std::nullopt;
    }
    return bytes;
}

std::uint32_t ReadBe32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::string FormatXexVersion(std::uint32_t value)
{
    return fmt::format("{}.{}.{}.{}", (value >> 28) & 0xF, (value >> 24) & 0xF, (value >> 8) & 0xFFFF, value & 0xFF);
}

// Multi-word XEX2 optional headers store a file offset in the table value.
std::optional<std::uint32_t> FindOptionalHeader(const std::filesystem::path& file, std::uint32_t key)
{
    auto base = ReadBytes(file, 0, 0x18);
    if (!base || ReadBe32(*base, 0) != kXex2Magic)
    {
        return std::nullopt;
    }

    auto header_count = ReadBe32(*base, 0x14);
    if (header_count == 0 || header_count > 1024)
    {
        return std::nullopt;
    }

    auto table = ReadBytes(file, 0x18, static_cast<std::size_t>(header_count) * 8);
    if (!table)
    {
        return std::nullopt;
    }

    for (std::uint32_t index = 0; index < header_count; ++index)
    {
        if (ReadBe32(*table, index * 8) == key)
        {
            return ReadBe32(*table, index * 8 + 4);
        }
    }
    return std::nullopt;
}

std::vector<std::string> DiagnoseBaseXex(const std::filesystem::path& file)
{
    std::vector<std::string> errors;

    auto base = ReadBytes(file, 0, 0x18);
    if (!base || ReadBe32(*base, 0) != kXex2Magic)
    {
        errors.push_back("default.xex is not an Xbox 360 executable.");
        return errors;
    }

    if (auto info_offset = FindOptionalHeader(file, kKeyExecutionInfo))
    {
        if (auto info = ReadBytes(file, *info_offset, 0x18))
        {
            auto media_id = ReadBe32(*info, 0x0);
            auto version  = ReadBe32(*info, 0x4);
            auto title_id = ReadBe32(*info, 0xC);
            if (title_id != kTitleId)
            {
                errors.push_back(fmt::format("default.xex is not Civilization Revolution (title ID {:08X}, expected {:08X}).", title_id, kTitleId));
                return errors;
            }
            if (version != kBaseVersionValue)
            {
                errors.push_back(fmt::format("default.xex is version {}; the supported base version is {}.", FormatXexVersion(version), FormatXexVersion(kBaseVersionValue)));
            }
            if (media_id != kMediaId)
            {
                errors.push_back(fmt::format("default.xex media ID {:08X} is not the supported release ({:08X}).", media_id, kMediaId));
            }
        }
    }

    auto security_offset = ReadBe32(*base, 0x10);
    if (auto region_bytes = ReadBytes(file, security_offset + 0x178u, 4))
    {
        auto region = ReadBe32(*region_bytes, 0);
        if (region != kRegionFree)
        {
            errors.push_back(fmt::format("default.xex region flags {:08X} do not match the supported region-free release.", region));
        }
    }

    if (errors.empty())
    {
        errors.push_back("default.xex does not match the supported content (modified or corrupted copy).");
    }
    return errors;
}

std::vector<std::string> DiagnoseUpdateXexp(const std::filesystem::path& file)
{
    std::vector<std::string> errors;

    auto base = ReadBytes(file, 0, 0x18);
    if (!base || ReadBe32(*base, 0) != kXex2Magic)
    {
        errors.push_back("default.xexp is not an Xbox 360 title update.");
        return errors;
    }

    if (auto descriptor_offset = FindOptionalHeader(file, kKeyDeltaDescriptor))
    {
        if (auto descriptor = ReadBytes(file, *descriptor_offset, 12))
        {
            auto target_version = ReadBe32(*descriptor, 4);
            if (target_version != kUpdateTargetValue)
            {
                errors.push_back(fmt::format("default.xexp updates the game to version {}; the supported title update is 1.3 ({}).", FormatXexVersion(target_version), FormatXexVersion(kUpdateTargetValue)));
                return errors;
            }
        }
    }

    errors.push_back("default.xexp does not match the supported 1.3 title update (modified or corrupted copy).");
    return errors;
}

void CheckRequiredFile(const std::filesystem::path& root, const char* relative, std::uint64_t expected_size, std::vector<std::string>& errors)
{
    std::error_code ec;
    auto            file = root / std::filesystem::path(relative);
    if (!std::filesystem::is_regular_file(file, ec))
    {
        errors.push_back(fmt::format("Missing: {}", relative));
        return;
    }

    auto size = std::filesystem::file_size(file, ec);
    if (ec || size != expected_size)
    {
        errors.push_back(fmt::format("Wrong size: {} (expected {} bytes, found {}).", relative, expected_size, ec ? 0 : size));
    }
}

// Diagnose all deep mismatches so foreign copies still report version details.
template <typename Diagnose>
void CheckExecutable(const std::filesystem::path& root, const char* relative, std::uint64_t expected_size, const char* expected_sha256, ContentDepth depth, Diagnose diagnose, std::vector<std::string>& errors)
{
    std::error_code ec;
    auto            file = root / std::filesystem::path(relative);
    if (!std::filesystem::is_regular_file(file, ec))
    {
        errors.push_back(fmt::format("Missing: {}", relative));
        return;
    }

    auto size    = std::filesystem::file_size(file, ec);
    bool size_ok = !ec && size == expected_size;
    if (depth == ContentDepth::kQuick)
    {
        if (!size_ok)
        {
            errors.push_back(fmt::format("Wrong size: {} (expected {} bytes, found {}).", relative, expected_size, ec ? 0 : size));
        }
        return;
    }

    if (size_ok && rex::crypto::sha256_file(file) == expected_sha256)
    {
        return;
    }

    auto details = diagnose(file);
    errors.insert(errors.end(), details.begin(), details.end());
}

} // namespace

ContentCheckResult VerifyContentRoot(const std::filesystem::path& root, ContentDepth depth)
{
    ContentCheckResult result;
    std::error_code    ec;

    if (root.empty() || !std::filesystem::is_directory(root, ec))
    {
        result.errors.push_back(fmt::format("Content folder does not exist: {}", root.string()));
        return result;
    }

    CheckExecutable(root, "default.xex", kBaseXexSize, kBaseXexSha256, depth, DiagnoseBaseXex, result.errors);
    CheckExecutable(root, "default.xexp", kUpdateXexpSize, kUpdateXexpSha256, depth, DiagnoseUpdateXexp, result.errors);
    for (const auto& entry : kResourceManifest)
    {
        CheckRequiredFile(root, entry.relative_path, entry.size, result.errors);
    }

    result.ok = result.errors.empty();
    return result;
}

} // namespace rerevved
