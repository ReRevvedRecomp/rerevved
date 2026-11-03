#include "maps_tab.h"

#include <algorithm>
#include <memory>

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

rex::system::AssetOverlayCatalog WithoutMapPackWarnings(rex::system::AssetOverlayCatalog catalog)
{
    for (auto& package : catalog.packages)
        std::erase_if(package.diagnostics, IsMapPackFieldWarning);
    return catalog;
}

} // namespace

Loaded Read(const rex::system::AssetOverlayCatalog& catalog, const std::filesystem::path& profileRoot, std::span<const std::string> startupOrder)
{
    Loaded loaded;
    loaded.packs = assets_list::FromCatalog(WithoutMapPackWarnings(catalog));

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

namespace
{

struct LoadoutState
{
    std::filesystem::path                             profileRoot;
    std::vector<std::string>                          startupOrder;
    std::function<rex::system::AssetOverlayCatalog()> discover;
    rex::system::AssetOverlayCatalog                  catalog;
    rex::system::AssetOverlayCatalog                  shown;
    Loaded                                            loaded;

    void refresh()
    {
        shown  = WithoutMapPackWarnings(catalog);
        loaded = Read(catalog, profileRoot, startupOrder);
    }

    void rescan()
    {
        catalog = discover();
        refresh();
    }
};

} // namespace

rex::ui::AssetOverlayLoadoutSource MakeLoadoutSource(std::filesystem::path profileRoot, std::vector<std::string> startupOrder, std::function<rex::system::AssetOverlayCatalog()> discover)
{
    auto state          = std::make_shared<LoadoutState>();
    state->profileRoot  = std::move(profileRoot);
    state->startupOrder = std::move(startupOrder);
    state->discover     = std::move(discover);
    state->rescan();

    rex::ui::AssetOverlayLoadoutSource source;
    source.title      = "Maps";
    source.noun       = "map packs";
    source.kind       = "map pack";
    source.order_file = std::string(map_packs::kOrderFileName);
    source.catalog    = [state]() -> const rex::system::AssetOverlayCatalog&
    {
        return state->shown;
    };
    source.order_ids = [state]()
    {
        return state->loaded.state.applied;
    };
    source.validate = [state](std::span<const std::string> ids)
    {
        return Validate(state->catalog, state->profileRoot, ids);
    };
    source.apply = [state](std::span<const std::string> ids, bool replaceInvalidCurrent)
    {
        auto result = Apply(state->catalog, state->profileRoot, ids, replaceInvalidCurrent);
        if (result.succeeded())
            state->refresh();
        return result;
    };
    source.rescan = [state]()
    {
        state->rescan();
    };
    source.diagnostics = [state]()
    {
        return state->loaded.diagnostics;
    };
    source.file_exists = [state]()
    {
        return state->loaded.state.orderFileExists;
    };
    source.file_invalid = [state]()
    {
        return state->loaded.state.orderFileInvalid;
    };
    source.restart_required = [state]()
    {
        return state->loaded.state.restartRequired;
    };
    return source;
}

} // namespace rerevved::maps_tab
