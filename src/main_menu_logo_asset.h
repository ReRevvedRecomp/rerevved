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
inline constexpr uint32_t         kLogoDdsSize   = 1048704u;

using Payload  = std::shared_ptr<const std::vector<uint8_t>>;
using Resolver = rex::Result<rex::system::AssetOverlayResolution> (*)(
    std::span<const rex::system::AssetOverlayPackage> selectedPackages,
    std::string_view                                  assetKey,
    size_t                                            maxBytes);

bool IsValidLogoDds(std::span<const uint8_t> data);

// In its 4:3 layout the game shows the main menu movie at its 16:9 scale and
// crops the sides, which cuts a logo that fills the texture.
inline constexpr float kNarrowScale = 0.85f;

// A copy of a valid logo with the image shrunk about its center and the
// uncovered border transparent. A scale of 1 or outside (0, 1) copies it.
std::vector<uint8_t> ScaleLogo(std::span<const uint8_t> dds, float scale);

// Resolve the selected package asset once before guest execution begins. The
// retained payload is immutable for the lifetime of the process.
bool ResolveSelectedLogo(
    std::span<const rex::system::AssetOverlayPackage> packages,
    Resolver                                          resolver = nullptr,
    float                                             scale    = 1.0f);

bool TryGetPayload(Payload& payload);
void ResetForTests();

} // namespace rerevved::main_menu_logo
