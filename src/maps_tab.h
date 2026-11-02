#pragma once

// The Mods page's Maps tab edits the map packs in <exe>/maps with the same
// model as the Assets tab (mods_list::Model). The runtime keeps no maps
// catalog, so this module scans the folder and reads and writes the profile's
// map_order.txt itself through the SDK's order-file functions. Pure apart from
// the file system; the page owns the model and the timing.

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <rex/system/asset_overlay_loadout.h>

#include "mods_list.h"

namespace rerevved::maps_tab
{

inline constexpr size_t kPackagesTab  = 0;
inline constexpr size_t kAssetsTab    = 1;
inline constexpr size_t kMapsTab      = 2;
inline constexpr size_t kModsTabCount = 3;

constexpr size_t StepModsTab(size_t tab, int direction)
{
    return direction < 0 ? (tab + kModsTabCount - 1) % kModsTabCount : (tab + 1) % kModsTabCount;
}

struct Loaded
{
    std::vector<mods_list::Mod> packs;
    // map_order.txt always needs a restart; restartRequired compares startup order.
    mods_list::RuntimeState state;
    // The page words these; the first becomes state.currentProblem.
    std::vector<rex::system::AssetOverlayLoadoutDiagnostic> diagnostics;
};

Loaded Read(const rex::system::AssetOverlayCatalog& catalog, const std::filesystem::path& profileRoot, std::span<const std::string> startupOrder);

rex::system::AssetOverlayLoadoutSelection Validate(const rex::system::AssetOverlayCatalog& catalog, const std::filesystem::path& profileRoot, std::span<const std::string> ids);

// Writes map_order.txt (never asset_order.txt).
rex::system::AssetOverlayLoadoutApplyResult Apply(const rex::system::AssetOverlayCatalog& catalog, const std::filesystem::path& profileRoot, std::span<const std::string> ids, bool replaceInvalidCurrent);

} // namespace rerevved::maps_tab
