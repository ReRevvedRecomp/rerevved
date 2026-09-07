#include "main_menu_logo_asset.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

#include <rex/logging.h>

namespace rerevved::main_menu_logo
{

namespace
{

constexpr uint32_t kDdsWidth  = 1024;
constexpr uint32_t kDdsHeight = 256;

std::shared_mutex selection_mutex;
Selection         selected_logo;

uint32_t ReadLittleEndianU32(const uint8_t* value)
{
    return uint32_t{ value[0] } | (uint32_t{ value[1] } << 8) |
           (uint32_t{ value[2] } << 16) | (uint32_t{ value[3] } << 24);
}

rex::Result<rex::system::AssetOverlayResolution> ResolveFromSdk(
    std::span<const rex::system::AssetOverlayPackage> packages,
    std::string_view                                  asset_key,
    size_t                                            max_bytes)
{
    return rex::system::ResolveAssetOverlay(packages, asset_key, max_bytes);
}

void ClearSelection()
{
    std::unique_lock lock(selection_mutex);
    selected_logo = {};
}

void LogResolutionProvenance(const rex::system::AssetOverlayResolution& resolved)
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
    if (ReadLittleEndianU32(bytes + 4) != 124 ||
        ReadLittleEndianU32(bytes + 12) != kDdsHeight ||
        ReadLittleEndianU32(bytes + 16) != kDdsWidth ||
        ReadLittleEndianU32(bytes + 24) != 0 ||
        ReadLittleEndianU32(bytes + 28) != 0)
    {
        return false;
    }

    // A nonzero FourCC includes the DX10 extension and every compressed DDS
    // format. This title path accepts only the legacy uncompressed payload.
    if (ReadLittleEndianU32(bytes + 76) != 32 ||
        ReadLittleEndianU32(bytes + 80) != 0x41 ||
        ReadLittleEndianU32(bytes + 84) != 0 ||
        ReadLittleEndianU32(bytes + 88) != 32 ||
        ReadLittleEndianU32(bytes + 92) != 0x00ff0000 ||
        ReadLittleEndianU32(bytes + 96) != 0x0000ff00 ||
        ReadLittleEndianU32(bytes + 100) != 0x000000ff ||
        ReadLittleEndianU32(bytes + 104) != 0xff000000)
    {
        return false;
    }

    constexpr uint32_t kDdsCapsComplex = 0x00000008;
    constexpr uint32_t kDdsCapsMipmap  = 0x00400000;
    return (ReadLittleEndianU32(bytes + 108) &
            (kDdsCapsComplex | kDdsCapsMipmap)) == 0 &&
           ReadLittleEndianU32(bytes + 112) == 0;
}

bool ResolveSelectedLogo(
    std::span<const rex::system::AssetOverlayPackage> packages,
    Resolver                                          resolver)
{
    ClearSelection();
    if (!resolver)
    {
        resolver = &ResolveFromSdk;
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
    LogResolutionProvenance(resolved);
    if (!IsValidLogoDds(resolved.bytes))
    {
        REXLOG_ERROR("Main-menu logo asset from package '{}' is malformed; using retail asset",
                     resolved.package_id);
        return false;
    }

    Selection selection;
    selection.payload              = std::make_shared<const std::vector<uint8_t>>(resolved.bytes);
    selection.package_id           = resolved.package_id;
    selection.shadowed_package_ids = resolved.shadowed_package_ids;
    {
        std::unique_lock lock(selection_mutex);
        selected_logo = std::move(selection);
    }

    return true;
}

bool TryGetSelection(Selection& selection)
{
    std::shared_lock lock(selection_mutex);
    if (!selected_logo.payload)
    {
        selection = {};
        return false;
    }
    selection = selected_logo;
    return true;
}

bool TryGetPayload(Payload& payload)
{
    std::shared_lock lock(selection_mutex);
    payload = selected_logo.payload;
    return static_cast<bool>(payload);
}

void ResetForTests()
{
    ClearSelection();
}

} // namespace rerevved::main_menu_logo
