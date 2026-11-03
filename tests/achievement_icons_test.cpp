#include "achievement_icons.h"
#include "custom_achievements.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

namespace
{

namespace icons = rerevved::achievement_icons;
namespace art   = rerevved::retail_ui_art;

rex::system::AchievementInfo info(const char* iconPath)
{
    rex::system::AchievementInfo a;
    a.icon_path = iconPath;
    return a;
}

} // namespace

TEST_CASE("achievement_icons_parse_icon_path", "[ctest][achievement-icons]")
{
    const auto source = icons::ParseIconPath("fpk:pedia.FPK/wond_apolloprogram.dds");
    REQUIRE(source);
    CHECK(source->archive == "pedia.FPK");
    CHECK(source->entry == "wond_apolloprogram.dds");

    CHECK_FALSE(icons::ParseIconPath(""));
    CHECK_FALSE(icons::ParseIconPath("icons/200-percent-concentrated-power-of-will.png"));
    CHECK_FALSE(icons::ParseIconPath("fpk:pedia.FPK"));
    CHECK_FALSE(icons::ParseIconPath("fpk:/a.dds"));
    CHECK_FALSE(icons::ParseIconPath("fpk:pedia.FPK/"));
    CHECK_FALSE(icons::ParseIconPath("fpk:pedia.FPK/a/b.dds"));
    CHECK_FALSE(icons::ParseIconPath("fpk:..\\pedia.FPK/a.dds"));
    CHECK_FALSE(icons::ParseIconPath("fpk:c:/a.dds"));
}

TEST_CASE("achievement_icons_provider_decodes_each_picture_once", "[ctest][achievement-icons]")
{
    int             reads = 0;
    std::string     archive;
    icons::Provider provider([&](const icons::Source& source) -> std::optional<art::Image>
                             {
                                 ++reads;
                                 archive = source.archive;
                                 if (source.entry == "missing.dds")
                                     return std::nullopt;
                                 return art::Image{ 2, 1, { 1, 2, 3, 4, 5, 6, 7, 8 } };
                             });

    const auto pixels = provider(info("fpk:Level.FPK/unit_barbarian.dds"));
    CHECK(pixels.width == 2);
    CHECK(pixels.height == 1);
    CHECK(pixels.rgba.size() == 8);
    CHECK(archive == "Level.FPK");
    CHECK(provider(info("fpk:Level.FPK/unit_barbarian.dds")).rgba == pixels.rgba);
    CHECK(reads == 1);

    CHECK(provider(info("fpk:pedia.FPK/missing.dds")).rgba.empty());
    CHECK(provider(info("fpk:pedia.FPK/missing.dds")).rgba.empty());
    CHECK(reads == 2);
}

TEST_CASE("achievement_icons_provider_ignores_other_paths", "[ctest][achievement-icons]")
{
    int             reads = 0;
    icons::Provider provider([&](const icons::Source&) -> std::optional<art::Image>
                             {
                                 ++reads;
                                 return std::nullopt;
                             });
    CHECK(provider(info("")).rgba.empty());
    CHECK(provider(info("icons/1.png")).rgba.empty());
    CHECK(reads == 0);
}

TEST_CASE("achievement_icons_every_custom_achievement_names_a_picture", "[ctest][achievement-icons]")
{
    const auto            all = rerevved::custom_achievements::AllAchievements();
    std::set<std::string> seen;
    for (const auto& achievement : all)
    {
        CAPTURE(achievement.label);
        CHECK(icons::ParseIconPath(achievement.icon_path));
        CHECK(achievement.image_id != 0);
        seen.insert(achievement.icon_path);
    }
    CHECK(seen.size() == all.size());
}
