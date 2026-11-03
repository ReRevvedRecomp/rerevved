#pragma once

// F2 Asset Overrides through the Mods edit model; packages have no lock, saves need a restart.

#include <vector>

#include <rex/system/asset_overlay_catalog.h>

#include "mods_list.h"

namespace rerevved::assets_list
{

// Blocking diagnostics are errors; assets have no icon and are never native.
std::vector<mods_list::Mod> FromCatalog(const rex::system::AssetOverlayCatalog& catalog);

} // namespace rerevved::assets_list
