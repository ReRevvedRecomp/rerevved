#include "options_glyphs.h"

#include "keyboard_icons.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace
{

namespace og = rerevved::options_glyphs;
using rerevved::button_icons::Family;

} // namespace

TEST_CASE("options_glyphs_table", "[ctest][options-glyphs]")
{
    constexpr og::Button  buttons[] = { og::Button::kA, og::Button::kB, og::Button::kLB, og::Button::kRB, og::Button::kLT, og::Button::kRT, og::Button::kX, og::Button::kY };
    constexpr const char* xbox[]    = { "a.dds", "b.dds", "lb.dds", "rb.dds", "lt.dds", "rt.dds", "x.dds", "y.dds" };
    constexpr const char* ps[]      = { "ps3x.dds", "ps3circle.dds", "ps3l1.dds", "ps3r1.dds", "ps3l2.dds", "ps3r2.dds", "ps3square.dds", "ps3triangle.dds" };
    constexpr const char* kb[]      = { "Return.dds", "Escape.dds", "PageUp.dds", "PageDown.dds", "Q.dds", "E.dds", "X.dds", "Y.dds" };
    for (int i = 0; i < og::kButtonCount; ++i)
    {
        const auto x = og::FileFor(Family::kXbox, buttons[i]);
        CHECK(x.source == og::Source::kRetailFpk);
        CHECK(x.name == xbox[i]);
        const auto p = og::FileFor(Family::kPlayStation, buttons[i]);
        CHECK(p.source == og::Source::kRetailFpk);
        CHECK(p.name == ps[i]);
        const auto k = og::FileFor(Family::kKeyboard, buttons[i]);
        CHECK(k.source == og::Source::kKeyboardArt);
        CHECK(k.name == kb[i]);
    }
}

#ifdef REREVVED_KEYBOARD_ICONS_DIR
TEST_CASE("options_glyphs_keyboard_files_ship", "[ctest][options-glyphs]")
{
    for (int i = 0; i < og::kButtonCount; ++i)
    {
        const auto file = og::FileFor(Family::kKeyboard, static_cast<og::Button>(i));
        CHECK(std::filesystem::exists(std::filesystem::path(REREVVED_KEYBOARD_ICONS_DIR) / std::string(file.name)));
    }
}
#endif

TEST_CASE("options_glyphs_aspect_and_fallback", "[ctest][options-glyphs]")
{
    using Catch::Matchers::WithinAbs;
    // Xbox shoulders are 2:1, triggers 1:2, PlayStation and keys 1:1.
    CHECK_THAT(og::WidthForHeight(128, 64, 20.0f), WithinAbs(40.0f, 1e-4));
    CHECK_THAT(og::WidthForHeight(64, 128, 20.0f), WithinAbs(10.0f, 1e-4));
    CHECK_THAT(og::WidthForHeight(64, 64, 20.0f), WithinAbs(20.0f, 1e-4));
    CHECK(og::WidthForHeight(0, 64, 20.0f) == 0.0f);
    CHECK(og::WidthForHeight(64, 0, 20.0f) == 0.0f);

    CHECK_THAT(og::PromptWidth(128, 64, 20.0f, 33.0f), WithinAbs(40.0f, 1e-4));
    CHECK_THAT(og::PromptWidth(0, 0, 20.0f, 33.0f), WithinAbs(33.0f, 1e-4));
}

TEST_CASE("options_glyphs_one_height_makes_a_set", "[ctest][options-glyphs]")
{
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(og::WidthForHeight(128, 64, 40.0f), WithinAbs(80.0f, 1e-4));
    CHECK_THAT(og::WidthForHeight(64, 128, 40.0f), WithinAbs(20.0f, 1e-4));
    CHECK_THAT(og::WidthForHeight(64, 64, 40.0f), WithinAbs(40.0f, 1e-4));
}

TEST_CASE("options_glyphs_keys_are_shipped_keys", "[ctest][options-glyphs]")
{
    const auto shipped = rerevved::keyboard_icons::ShippedKeys();
    for (int i = 0; i < og::kButtonCount; ++i)
    {
        const auto file = og::FileFor(Family::kKeyboard, static_cast<og::Button>(i));
        REQUIRE(file.name.ends_with(".dds"));
        const std::string_view key = file.name.substr(0, file.name.size() - 4);
        CHECK(std::find(shipped.begin(), shipped.end(), key) != shipped.end());
        CHECK(rerevved::keyboard_icons::PrimaryKey(key) == key);
    }
}
