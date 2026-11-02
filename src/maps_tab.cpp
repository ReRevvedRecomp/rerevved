#include "maps_tab.h"

#include <algorithm>

#include "assets_list.h"
#include "map_packs.h"

namespace rerevved::maps_tab
{

namespace
{

// Suppress the SDK's unknown-field warning for a valid [map_pack] manifest.
bool IsMapPackFieldWarning(const rex::system::AssetOverlayDiagnostic& diagnostic)
{
    return diagnostic.message == "unknown manifest field 'map_pack'";
}

} // namespace

Loaded Read(const rex::system::AssetOverlayCatalog& catalog, const std::filesystem::path& profileRoot, std::span<const std::string> startupOrder)
{
    Loaded loaded;
    // The SDK does not know the [map_pack] table, so it warns of an unknown
    // manifest field for it; a valid map pack shows no such warning.
    auto filtered = catalog;
    for (auto& package : filtered.packages)
        std::erase_if(package.diagnostics, IsMapPackFieldWarning);
    loaded.packs = assets_list::FromCatalog(filtered);

    // No bundled default: a profile without map_order.txt enables no map pack.
    const auto file        = rex::system::ReadAssetOverlayLoadout(profileRoot, {}, map_packs::kOrderFileName);
    const auto selection   = rex::system::SelectAssetOverlayLoadout(catalog, file);
    auto&      state       = loaded.state;
    state.orderFileExists  = file.exists;
    state.orderFileInvalid = file.exists && !selection.IsValid();
    state.currentInvalid   = !selection.IsValid();
    loaded.diagnostics     = selection.diagnostics;
    if (selection.IsValid())
        state.applied = selection.requested_ids;
    state.restartRequired = !std::equal(state.applied.begin(), state.applied.end(), startupOrder.begin(), startupOrder.end());
    return loaded;
}

rex::system::AssetOverlayLoadoutSelection Validate(const rex::system::AssetOverlayCatalog& catalog, const std::filesystem::path& profileRoot, std::span<const std::string> ids)
{
    return rex::system::ValidateAssetOverlayLoadout(catalog, ids, profileRoot / map_packs::kOrderFileName);
}

rex::system::AssetOverlayLoadoutApplyResult Apply(const rex::system::AssetOverlayCatalog& catalog, const std::filesystem::path& profileRoot, std::span<const std::string> ids, bool replaceInvalidCurrent)
{
    return rex::system::ApplyAssetOverlayLoadout(profileRoot, catalog, ids, replaceInvalidCurrent, map_packs::kOrderFileName);
}

} // namespace rerevved::maps_tab
