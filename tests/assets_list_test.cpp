#include "assets_list.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace
{

namespace as = rerevved::assets_list;
namespace ml = rerevved::mods_list;
using rex::system::AssetOverlayCatalog;
using rex::system::AssetOverlayPackage;

AssetOverlayPackage package(const char* id, const char* name, const char* folder)
{
    AssetOverlayPackage p;
    p.id           = id;
    p.display_name = name;
    p.folder_name  = folder;
    return p;
}

ml::RuntimeState state(std::vector<std::string> applied, bool fileExists = true)
{
    ml::RuntimeState s;
    s.applied         = std::move(applied);
    s.orderFileExists = fileExists;
    return s;
}

} // namespace

TEST_CASE("assets_list_maps_the_catalog_as_f2_reads_it", "[ctest][assets-list]")
{
    AssetOverlayCatalog catalog;
    auto                first = package("pack.b", "Beta", "beta-folder");
    first.version             = "1.2";
    first.author              = "Someone";
    first.description         = "Retextures things.";
    first.diagnostics.push_back({ true, "missing manifest field", {} });
    first.diagnostics.push_back({ false, "unused file", {} });
    catalog.packages.push_back(first);
    catalog.packages.push_back(package("pack.a", "", "alpha-folder"));

    const auto mods = as::FromCatalog(catalog);
    REQUIRE(mods.size() == 2);
    CHECK(mods[0].id == "pack.b");
    CHECK(mods[0].name == "Beta");
    CHECK(mods[0].folder == "beta-folder");
    CHECK(mods[0].version == "1.2");
    CHECK(mods[0].author == "Someone");
    CHECK(mods[0].description == "Retextures things.");
    CHECK_FALSE(mods[0].native);
    CHECK(mods[0].iconPath.empty());
    REQUIRE(mods[0].messages.size() == 2);
    CHECK(mods[0].messages[0].error);
    CHECK(mods[0].messages[0].text == "missing manifest field");
    CHECK_FALSE(mods[0].messages[1].error);
    CHECK(mods[1].name == "pack.a");
    CHECK(as::FromCatalog({}).empty());
}

TEST_CASE("assets_list_edits_like_the_f2_asset_tab", "[ctest][assets-list]")
{
    AssetOverlayCatalog catalog;
    catalog.packages.push_back(package("pack.b", "Beta", "b"));
    catalog.packages.push_back(package("pack.a", "Alpha", "a"));
    catalog.packages.push_back(package("pack.c", "Gamma", "c"));

    ml::Model model;
    model.Update(as::FromCatalog(catalog), state({ "pack.c" }));
    REQUIRE(model.Rows().size() == 3);
    CHECK(model.Rows()[0].id == "pack.c");
    CHECK(model.Rows()[1].id == "pack.a");
    CHECK(model.Rows()[2].id == "pack.b");
    CHECK_FALSE(model.Locked());
    CHECK_FALSE(model.Dirty());
    CHECK_FALSE(model.CanApply());

    model.Select(1);
    CHECK(model.Activate() == ml::Press::kEnabled);
    CHECK(model.Staged() == std::vector<std::string>{ "pack.c", "pack.a" });
    CHECK(model.Dirty());
    CHECK(model.CanApply());
    // No live apply for assets: a save that changes nothing is not offered.
    model.Update(as::FromCatalog(catalog), state({ "pack.c", "pack.a" }));
    CHECK_FALSE(model.Dirty());
    CHECK_FALSE(model.CanApply());

    model.Select(1);
    CHECK(model.Grab());
    model.Move(-1);
    CHECK(model.Staged() == std::vector<std::string>{ "pack.a", "pack.c" });
    CHECK(model.Back() == ml::BackResult::kHandled);
    CHECK(model.Staged() == std::vector<std::string>{ "pack.c", "pack.a" });

    model.Select(2);
    model.Activate();
    CHECK(model.Dirty());
    CHECK(model.Back() == ml::BackResult::kHandled);
    CHECK(model.Confirmation() == ml::Confirm::kDiscard);
    CHECK(model.Accept() == ml::Accepted::kClose);
    CHECK_FALSE(model.Dirty());
    CHECK(model.Back() == ml::BackResult::kClose);
}

TEST_CASE("assets_list_saves_when_the_order_file_is_missing_or_invalid", "[ctest][assets-list]")
{
    AssetOverlayCatalog catalog;
    catalog.packages.push_back(package("pack.a", "Alpha", "a"));

    ml::Model model;
    model.Update(as::FromCatalog(catalog), state({ "pack.a" }, false));
    CHECK(model.CanApply());
    CHECK(model.Apply() == ml::ApplyStep::kApply);

    auto invalid             = state({}, true);
    invalid.orderFileInvalid = true;
    model.Update(as::FromCatalog(catalog), invalid);
    CHECK(model.CanApply());
    CHECK(model.Apply() == ml::ApplyStep::kAskReplace);
    CHECK(model.Confirmation() == ml::Confirm::kReplace);
    CHECK(model.Accept() == ml::Accepted::kApplyReplacing);
}
