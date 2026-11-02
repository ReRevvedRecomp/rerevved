#pragma once

// Uses mods_list::Model for <exe>/maps; SDK I/O owns profile map_order.txt, the page owns the model and timing.

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <rex/system/asset_overlay_loadout.h>
#include <rex/ui/overlay/asset_overlay_manager.h>

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

// Shares F2's Read, Validate, Apply and restart rules; discover scans at creation and each Rescan.
rex::ui::AssetOverlayLoadoutSource MakeLoadoutSource(std::filesystem::path profileRoot, std::vector<std::string> startupOrder, std::function<rex::system::AssetOverlayCatalog()> discover);

} // namespace rerevved::maps_tab
