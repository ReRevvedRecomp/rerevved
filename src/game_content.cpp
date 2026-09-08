#include "game_content.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <system_error>

#include <fmt/format.h>
#include <rex/crypto/sha256.h>

namespace rerevved
{

const char* const   kBaseXexSha256    = "b59b8957a3ed9dd90e9296c96d5c7ab1b16078d3f08b015582714a06c7d6a7bd";
const char* const   kUpdateXexpSha256 = "c1fc6149a63550987d991efdbb80e3697845a9a49d3f2ec180ea9817db8d12d4";
const std::uint32_t kTitleId          = 0x545407E5u;

namespace
{

struct ManifestEntry
{
    const char*   relativePath;
    std::uint64_t size;
};

// Regenerate with scripts/gen-content-manifest.ps1 when this set changes.
constexpr ManifestEntry kResourceManifest[] = {
#include "content_manifest.inc"
};

// Header fields explain mismatches against the exact pinned hashes.
constexpr std::uint32_t kMediaId           = 0x7DC1293Bu;
constexpr std::uint32_t kBaseVersionValue  = 0x00000002u; // 0.0.0.2
constexpr std::uint32_t kUpdateTargetValue = 0x00000302u; // 0.0.3.2, game version 1.3
constexpr std::uint32_t kRegionFree        = 0xFFFFFFFFu;
constexpr std::uint64_t kBaseXexSize       = 16822272ull;
constexpr std::uint64_t kUpdateXexpSize    = 3291136ull;

constexpr std::uint32_t kXex2Magic          = 0x58455832u; // 'XEX2'
constexpr std::uint32_t kKeyExecutionInfo   = 0x00040006u;
constexpr std::uint32_t kKeyDeltaDescriptor = 0x000005FFu;

std::optional<std::vector<std::uint8_t>> readBytes(const std::filesystem::path& file, std::uint64_t offset, std::size_t count)
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

std::uint32_t readBe32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::string formatXexVersion(std::uint32_t value)
{
    return fmt::format("{}.{}.{}.{}", (value >> 28) & 0xF, (value >> 24) & 0xF, (value >> 8) & 0xFFFF, value & 0xFF);
}

// Multi-word XEX2 optional headers store a file offset in the table value.
std::optional<std::uint32_t> findOptionalHeader(const std::filesystem::path& file, std::uint32_t key)
{
    auto base = readBytes(file, 0, 0x18);
    if (!base || readBe32(*base, 0) != kXex2Magic)
    {
        return std::nullopt;
    }

    auto headerCount = readBe32(*base, 0x14);
    if (headerCount == 0 || headerCount > 1024)
    {
        return std::nullopt;
    }

    auto table = readBytes(file, 0x18, static_cast<std::size_t>(headerCount) * 8);
    if (!table)
    {
        return std::nullopt;
    }

    for (std::uint32_t index = 0; index < headerCount; ++index)
    {
        if (readBe32(*table, index * 8) == key)
        {
            return readBe32(*table, index * 8 + 4);
        }
    }
    return std::nullopt;
}

std::vector<std::string> diagnoseBaseXex(const std::filesystem::path& file)
{
    std::vector<std::string> errors;

    auto base = readBytes(file, 0, 0x18);
    if (!base || readBe32(*base, 0) != kXex2Magic)
    {
        errors.push_back("default.xex is not an Xbox 360 executable.");
        return errors;
    }

    if (auto infoOffset = findOptionalHeader(file, kKeyExecutionInfo))
    {
        if (auto info = readBytes(file, *infoOffset, 0x18))
        {
            auto mediaId = readBe32(*info, 0x0);
            auto version = readBe32(*info, 0x4);
            auto titleId = readBe32(*info, 0xC);
            if (titleId != kTitleId)
            {
                errors.push_back(fmt::format("default.xex is not Civilization Revolution (title ID {:08X}, expected {:08X}).", titleId, kTitleId));
                return errors;
            }
            if (version != kBaseVersionValue)
            {
                errors.push_back(fmt::format("default.xex is version {}; the supported base version is {}.", formatXexVersion(version), formatXexVersion(kBaseVersionValue)));
            }
            if (mediaId != kMediaId)
            {
                errors.push_back(fmt::format("default.xex media ID {:08X} is not the supported release ({:08X}).", mediaId, kMediaId));
            }
        }
    }

    auto securityOffset = readBe32(*base, 0x10);
    if (auto regionBytes = readBytes(file, securityOffset + 0x178u, 4))
    {
        auto region = readBe32(*regionBytes, 0);
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

std::vector<std::string> diagnoseUpdateXexp(const std::filesystem::path& file)
{
    std::vector<std::string> errors;

    auto base = readBytes(file, 0, 0x18);
    if (!base || readBe32(*base, 0) != kXex2Magic)
    {
        errors.push_back("default.xexp is not an Xbox 360 title update.");
        return errors;
    }

    if (auto descriptorOffset = findOptionalHeader(file, kKeyDeltaDescriptor))
    {
        if (auto descriptor = readBytes(file, *descriptorOffset, 12))
        {
            auto targetVersion = readBe32(*descriptor, 4);
            if (targetVersion != kUpdateTargetValue)
            {
                errors.push_back(fmt::format("default.xexp updates the game to version {}; the supported title update is 1.3 ({}).", formatXexVersion(targetVersion), formatXexVersion(kUpdateTargetValue)));
                return errors;
            }
        }
    }

    errors.push_back("default.xexp does not match the supported 1.3 title update (modified or corrupted copy).");
    return errors;
}

void checkRequiredFile(const std::filesystem::path& root, const char* relative, std::uint64_t expectedSize, std::vector<std::string>& errors)
{
    std::error_code ec;
    auto            file = root / std::filesystem::path(relative);
    if (!std::filesystem::is_regular_file(file, ec))
    {
        errors.push_back(fmt::format("Missing: {}", relative));
        return;
    }

    auto size = std::filesystem::file_size(file, ec);
    if (ec || size != expectedSize)
    {
        errors.push_back(fmt::format("Wrong size: {} (expected {} bytes, found {}).", relative, expectedSize, ec ? 0 : size));
    }
}

// Diagnose all deep mismatches so foreign copies still report version details.
template <typename Diagnose>
void checkExecutable(const std::filesystem::path& root, const char* relative, std::uint64_t expectedSize, const char* expectedSha256, ContentDepth depth, Diagnose diagnose, std::vector<std::string>& errors)
{
    std::error_code ec;
    auto            file = root / std::filesystem::path(relative);
    if (!std::filesystem::is_regular_file(file, ec))
    {
        errors.push_back(fmt::format("Missing: {}", relative));
        return;
    }

    auto size   = std::filesystem::file_size(file, ec);
    bool sizeOk = !ec && size == expectedSize;
    if (depth == ContentDepth::Quick)
    {
        if (!sizeOk)
        {
            errors.push_back(fmt::format("Wrong size: {} (expected {} bytes, found {}).", relative, expectedSize, ec ? 0 : size));
        }
        return;
    }

    if (sizeOk && rex::crypto::sha256_file(file) == expectedSha256)
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

    checkExecutable(root, "default.xex", kBaseXexSize, kBaseXexSha256, depth, diagnoseBaseXex, result.errors);
    checkExecutable(root, "default.xexp", kUpdateXexpSize, kUpdateXexpSha256, depth, diagnoseUpdateXexp, result.errors);
    for (const auto& entry : kResourceManifest)
    {
        checkRequiredFile(root, entry.relativePath, entry.size, result.errors);
    }

    result.ok = result.errors.empty();
    return result;
}

} // namespace rerevved
