#include "assets_list.h"

namespace rerevved::assets_list
{

std::vector<mods_list::Mod> FromCatalog(const rex::system::AssetOverlayCatalog& catalog)
{
    std::vector<mods_list::Mod> mods;
    mods.reserve(catalog.packages.size());
    for (const auto& package : catalog.packages)
    {
        mods_list::Mod mod;
        mod.id          = package.id;
        mod.name        = package.display_name.empty() ? package.id : package.display_name;
        mod.folder      = package.folder_name;
        mod.version     = package.version;
        mod.author      = package.author;
        mod.description = package.description;
        for (const auto& diagnostic : package.diagnostics)
            mod.messages.push_back({ diagnostic.blocking, diagnostic.message });
        mods.push_back(std::move(mod));
    }
    return mods;
}

} // namespace rerevved::assets_list
