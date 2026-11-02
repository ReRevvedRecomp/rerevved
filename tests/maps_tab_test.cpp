#include "maps_tab.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "map_packs.h"
#include "title_text.h"

namespace
{

namespace fs = std::filesystem;
namespace mt = rerevved::maps_tab;
using rex::system::AssetOverlayCatalog;
using rex::system::AssetOverlayPackage;

AssetOverlayCatalog catalogOf(std::initializer_list<const char*> ids)
{
    AssetOverlayCatalog catalog;
    for (const char* id : ids)
    {
        AssetOverlayPackage p;
        p.id           = id;
        p.display_name = std::string("Name ") + id;
        p.folder_name  = id;
        p.version      = "1.0";
        p.author       = "Author";
        p.description  = "A map pack.";
        catalog.packages.push_back(p);
    }
    return catalog;
}

struct TempProfile
{
    fs::path root;

    TempProfile()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root             = fs::temp_directory_path() / ("rerevved_maps_tab_test_" + std::to_string(stamp));
        fs::create_directories(root);
    }

    ~TempProfile()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const char* name, const std::string& text) const
    {
        std::ofstream(root / name, std::ios::binary) << text;
    }

    std::string read(const char* name) const
    {
        std::ifstream in(root / name, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }
};

} // namespace

TEST_CASE("maps tab indexes map to the three Mods tabs and wrap", "[ctest][maps-tab]")
{
    CHECK(mt::kModsTabCount == 3);
    CHECK(mt::kPackagesTab == 0);
    CHECK(mt::kAssetsTab == 1);
    CHECK(mt::kMapsTab == 2);
    CHECK(mt::StepModsTab(0, 1) == 1);
    CHECK(mt::StepModsTab(1, 1) == 2);
    CHECK(mt::StepModsTab(2, 1) == 0);
    CHECK(mt::StepModsTab(0, -1) == 2);
    CHECK(mt::StepModsTab(2, -1) == 1);
    CHECK(mt::StepModsTab(1, -1) == 0);
}

TEST_CASE("maps tab strings exist in every language", "[ctest][maps-tab]")
{
    namespace tt = rerevved::title_text;
    CHECK(tt::Entry(tt::Id::kModsTabMaps, tt::Language::kEnglish) == "Maps");
    for (const auto id : { tt::Id::kModsTabMaps, tt::Id::kMapsNone, tt::Id::kMapsMoving, tt::Id::kMapsSaveFailed, tt::Id::kMapsDiscardAsk, tt::Id::kMapsReplaceAsk })
    {
        for (const auto language : { tt::Language::kEnglish, tt::Language::kGerman, tt::Language::kFrench, tt::Language::kSpanish, tt::Language::kItalian })
            CHECK_FALSE(tt::Entry(id, language).empty());
    }
}

TEST_CASE("maps tab lists the maps catalog and the saved map order", "[ctest][maps-tab]")
{
    TempProfile profile;
    const auto  catalog = catalogOf({ "alpha", "beta", "gamma" });
    profile.write("map_order.txt", "gamma\nalpha\n");

    const std::vector<std::string> startup = { "gamma", "alpha" };
    const auto                     loaded  = mt::Read(catalog, profile.root, startup);
    REQUIRE(loaded.packs.size() == 3);
    CHECK(loaded.packs[0].id == "alpha");
    CHECK(loaded.packs[0].name == "Name alpha");
    CHECK(loaded.packs[0].version == "1.0");
    CHECK(loaded.packs[0].author == "Author");
    CHECK(loaded.packs[0].description == "A map pack.");
    CHECK(loaded.state.applied == std::vector<std::string>{ "gamma", "alpha" });
    CHECK(loaded.state.orderFileExists);
    CHECK_FALSE(loaded.state.currentInvalid);
    CHECK_FALSE(loaded.state.restartRequired);
    CHECK(loaded.diagnostics.empty());

    const std::vector<std::string> other = { "alpha" };
    CHECK(mt::Read(catalog, profile.root, other).state.restartRequired);
}

TEST_CASE("maps tab without an order file enables nothing and reports an unknown pack", "[ctest][maps-tab]")
{
    TempProfile profile;
    const auto  catalog = catalogOf({ "alpha" });

    const auto none = mt::Read(catalog, profile.root, {});
    CHECK(none.state.applied.empty());
    CHECK_FALSE(none.state.orderFileExists);
    CHECK_FALSE(none.state.restartRequired);

    profile.write("map_order.txt", "missing\n");
    const auto invalid = mt::Read(catalog, profile.root, {});
    CHECK(invalid.state.currentInvalid);
    CHECK(invalid.state.orderFileInvalid);
    CHECK_FALSE(invalid.diagnostics.empty());

    const std::vector<std::string> unknown = { "missing" };
    CHECK_FALSE(mt::Validate(catalog, profile.root, unknown).IsValid());
    const std::vector<std::string> known = { "alpha" };
    CHECK(mt::Validate(catalog, profile.root, known).IsValid());
}

TEST_CASE("maps tab saving writes map_order.txt and not asset_order.txt", "[ctest][maps-tab]")
{
    TempProfile profile;
    const auto  catalog = catalogOf({ "alpha", "beta" });
    profile.write("asset_order.txt", "keep\n");

    const std::vector<std::string> ids    = { "beta", "alpha" };
    const auto                     result = mt::Apply(catalog, profile.root, ids, false);
    REQUIRE(result.succeeded());
    CHECK(fs::exists(profile.root / "map_order.txt"));
    const auto saved = profile.read("map_order.txt");
    CHECK(saved.find("beta") != std::string::npos);
    CHECK(saved.find("beta") < saved.find("alpha"));
    CHECK(profile.read("asset_order.txt") == "keep\n");

    const auto reloaded = mt::Read(catalog, profile.root, {});
    CHECK(reloaded.state.applied == ids);
    CHECK(reloaded.state.restartRequired);
}

TEST_CASE("maps tab refuses an unknown pack and an invalid saved file without replacing", "[ctest][maps-tab]")
{
    TempProfile profile;
    const auto  catalog = catalogOf({ "alpha" });

    const std::vector<std::string> unknown = { "missing" };
    const auto                     bad     = mt::Apply(catalog, profile.root, unknown, false);
    CHECK_FALSE(bad.succeeded());
    CHECK_FALSE(fs::exists(profile.root / "map_order.txt"));

    profile.write("map_order.txt", "missing\n");
    const std::vector<std::string> ids  = { "alpha" };
    const auto                     kept = mt::Apply(catalog, profile.root, ids, false);
    CHECK(kept.status == rex::system::AssetOverlayLoadoutApplyStatus::kInvalidCurrent);
    CHECK(profile.read("map_order.txt") == "missing\n");

    CHECK(mt::Apply(catalog, profile.root, ids, true).succeeded());
}

TEST_CASE("maps tab hides only the map_pack table warning", "[ctest][maps-tab]")
{
    TempProfile profile;
    auto        catalog = catalogOf({ "alpha" });
    catalog.packages[0].diagnostics.push_back({ false, "unknown manifest field 'map_pack'", {} });
    catalog.packages[0].diagnostics.push_back({ false, "unknown manifest field 'other'", {} });
    catalog.packages[0].diagnostics.push_back({ true, "invalid manifest", {} });

    const auto loaded = mt::Read(catalog, profile.root, {});
    REQUIRE(loaded.packs.size() == 1);
    REQUIRE(loaded.packs[0].messages.size() == 2);
    CHECK(loaded.packs[0].messages[0].text == "unknown manifest field 'other'");
    CHECK_FALSE(loaded.packs[0].messages[0].error);
    CHECK(loaded.packs[0].messages[1].error);
    CHECK(loaded.packs[0].messages[1].text == "invalid manifest");
}

TEST_CASE("maps tab F2 source edits the same map_order.txt and rescans", "[ctest][maps-tab]")
{
    TempProfile profile;
    auto        catalog = catalogOf({ "alpha", "beta" });
    catalog.packages[0].diagnostics.push_back({ false, "unknown manifest field 'map_pack'", {} });
    int scans = 0;

    auto source = mt::MakeLoadoutSource(profile.root, {}, [&]
                                        {
                                            ++scans;
                                            return catalog;
                                        });
    CHECK(source.title == "Maps");
    CHECK(source.order_file == "map_order.txt");
    CHECK(scans == 1);
    CHECK(source.catalog().packages.size() == 2);
    CHECK(source.catalog().packages[0].diagnostics.empty());
    CHECK(source.order_ids().empty());
    CHECK_FALSE(source.file_exists());
    CHECK(source.restart_required() == false);

    const std::vector<std::string> ids = { "beta", "alpha" };
    CHECK(source.validate(ids).IsValid());
    REQUIRE(source.apply(ids, false).succeeded());
    CHECK(profile.read("map_order.txt").find("beta") < profile.read("map_order.txt").find("alpha"));
    CHECK_FALSE(fs::exists(profile.root / "asset_order.txt"));
    CHECK(source.order_ids() == ids);
    CHECK(source.file_exists());
    CHECK_FALSE(source.file_invalid());
    CHECK(source.restart_required());

    profile.write("map_order.txt", "missing\n");
    source.rescan();
    CHECK(scans == 2);
    CHECK(source.file_invalid());
    CHECK_FALSE(source.diagnostics().empty());
    const std::vector<std::string> one = { "alpha" };
    CHECK(source.apply(one, false).status == rex::system::AssetOverlayLoadoutApplyStatus::kInvalidCurrent);
    CHECK(source.apply(one, true).succeeded());
}
