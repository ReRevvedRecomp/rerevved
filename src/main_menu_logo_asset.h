#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <rex/result.h>
#include <rex/system/asset_overlay.h>
#include <rex/system/asset_overlay_catalog.h>

namespace rerevved::main_menu_logo
{

inline constexpr std::string_view kAssetKey =
    "file-data/GFX_MainMenu_logo.dds";
inline constexpr std::string_view kGuestFileName = "GFX_MainMenu_logo.dds";
inline constexpr uint32_t kLogoDdsSize           = 1048704u;

using Payload  = std::shared_ptr<const std::vector<uint8_t>>;
using Resolver = rex::Result<rex::system::AssetOverlayResolution> (*)(
    std::span<const rex::system::AssetOverlayPackage> selected_packages,
    std::string_view asset_key,
    size_t max_bytes);

bool IsValidLogoDds(std::span<const uint8_t> data);

// Resolve the selected package asset once before guest execution begins. The
// retained payload is immutable for the lifetime of the process.
bool ResolveSelectedLogo(
    std::span<const rex::system::AssetOverlayPackage> packages,
    Resolver resolver = nullptr);

bool TryGetPayload(Payload& payload);
void ResetForTests();

} // namespace rerevved::main_menu_logo
