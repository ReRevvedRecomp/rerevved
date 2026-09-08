#include "main_menu_logo_asset.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include <rex/logging.h>

namespace rerevved::main_menu_logo
{

namespace
{

constexpr uint32_t kDdsWidth  = 1024;
constexpr uint32_t kDdsHeight = 256;

std::shared_mutex selectionMutex;
Payload           selectedLogo;

uint32_t readLittleEndianU32(const uint8_t* value)
{
    return uint32_t{ value[0] } | (uint32_t{ value[1] } << 8) |
           (uint32_t{ value[2] } << 16) | (uint32_t{ value[3] } << 24);
}

void clearSelection()
{
    std::unique_lock lock(selectionMutex);
    selectedLogo = {};
}

void logResolutionProvenance(const rex::system::AssetOverlayResolution& resolved)
{
    REXLOG_INFO("Selected main-menu logo asset from package '{}'",
                resolved.package_id);
    for (const auto& shadowed : resolved.shadowed_package_ids)
    {
        REXLOG_INFO("Main-menu logo asset shadowed package '{}'", shadowed);
    }
}

} // namespace

bool IsValidLogoDds(std::span<const uint8_t> data)
{
    if (data.data() == nullptr || data.size() != kLogoDdsSize ||
        std::memcmp(data.data(), "DDS ", 4) != 0)
    {
        return false;
    }

    const auto* bytes = data.data();
    if (readLittleEndianU32(bytes + 4) != 124 ||
        readLittleEndianU32(bytes + 12) != kDdsHeight ||
        readLittleEndianU32(bytes + 16) != kDdsWidth ||
        readLittleEndianU32(bytes + 24) != 0 ||
        readLittleEndianU32(bytes + 28) != 0)
    {
        return false;
    }

    // A nonzero FourCC includes the DX10 extension and every compressed DDS
    // format. This title path accepts only the legacy uncompressed payload.
    if (readLittleEndianU32(bytes + 76) != 32 ||
        readLittleEndianU32(bytes + 80) != 0x41 ||
        readLittleEndianU32(bytes + 84) != 0 ||
        readLittleEndianU32(bytes + 88) != 32 ||
        readLittleEndianU32(bytes + 92) != 0x00ff0000 ||
        readLittleEndianU32(bytes + 96) != 0x0000ff00 ||
        readLittleEndianU32(bytes + 100) != 0x000000ff ||
        readLittleEndianU32(bytes + 104) != 0xff000000)
    {
        return false;
    }

    constexpr uint32_t kDdsCapsComplex = 0x00000008;
    constexpr uint32_t kDdsCapsMipmap  = 0x00400000;
    return (readLittleEndianU32(bytes + 108) &
            (kDdsCapsComplex | kDdsCapsMipmap)) == 0 &&
           readLittleEndianU32(bytes + 112) == 0;
}

bool ResolveSelectedLogo(
    std::span<const rex::system::AssetOverlayPackage> packages,
    Resolver                                          resolver)
{
    clearSelection();
    if (!resolver)
    {
        resolver = &rex::system::ResolveAssetOverlay;
    }

    const auto result = resolver(packages, kAssetKey, kLogoDdsSize);
    if (!result)
    {
        if (result.error().category != rex::ErrorCategory::NotFound)
        {
            REXLOG_ERROR("Main-menu logo asset rejected: {}", result.error().what());
        }
        return false;
    }

    const auto& resolved = *result;
    logResolutionProvenance(resolved);
    if (!IsValidLogoDds(resolved.bytes))
    {
        REXLOG_ERROR("Main-menu logo asset from package '{}' is malformed; using retail asset",
                     resolved.package_id);
        return false;
    }

    auto payload = std::make_shared<const std::vector<uint8_t>>(resolved.bytes);
    {
        std::unique_lock lock(selectionMutex);
        selectedLogo = std::move(payload);
    }

    return true;
}

bool TryGetPayload(Payload& payload)
{
    std::shared_lock lock(selectionMutex);
    payload = selectedLogo;
    return static_cast<bool>(payload);
}

void ResetForTests()
{
    clearSelection();
}

} // namespace rerevved::main_menu_logo
