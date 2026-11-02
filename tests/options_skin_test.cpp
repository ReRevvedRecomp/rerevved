#include "options_skin.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <utility>

namespace
{

namespace os = rerevved::options_skin;
using Catch::Matchers::WithinAbs;
using rerevved::button_icons::Family;

} // namespace

TEST_CASE("options_skin_files", "[ctest][options-skin]")
{
    const auto map = os::FileFor(Family::kXbox, os::Art::kMap);
    REQUIRE(map);
    CHECK(map->archive == "Common0.FPK");
    CHECK(map->name == "gfx_mapbackground_512x256.dds");
    CHECK(os::FileFor(Family::kKeyboard, os::Art::kMap)->name == map->name);
    CHECK(os::FileFor(Family::kPlayStation, os::Art::kBar)->name == "bardarkblue.dds");
    CHECK(os::FileFor(Family::kXbox, os::Art::kGoldArrow)->name == "goldarrow.dds");
    CHECK(os::FileFor(Family::kXbox, os::Art::kLs)->name == "ls.dds");
    CHECK(os::FileFor(Family::kPlayStation, os::Art::kLs)->name == "ps3ls.dds");
    CHECK_FALSE(os::FileFor(Family::kKeyboard, os::Art::kLs));
}

TEST_CASE("options_skin_scroll_arrows", "[ctest][options-skin]")
{
    auto a = os::ArrowsFor(0.0f, 0.0f);
    CHECK_FALSE(a.up);
    CHECK_FALSE(a.down);
    a = os::ArrowsFor(0.0f, 300.0f);
    CHECK_FALSE(a.up);
    CHECK(a.down);
    a = os::ArrowsFor(120.0f, 300.0f);
    CHECK(a.up);
    CHECK(a.down);
    a = os::ArrowsFor(300.0f, 300.0f);
    CHECK(a.up);
    CHECK_FALSE(a.down);
    a = os::ArrowsFor(0.4f, 0.8f);
    CHECK_FALSE(a.up);
    CHECK_FALSE(a.down);
}

TEST_CASE("options_skin_step_scroll_moves_a_row_and_stops_at_the_ends", "[ctest][options-skin]")
{
    CHECK_THAT(os::StepScroll(100.0f, 300.0f, 40.0f, 1), WithinAbs(140.0, 1e-4));
    CHECK_THAT(os::StepScroll(100.0f, 300.0f, 40.0f, -1), WithinAbs(60.0, 1e-4));
    CHECK_THAT(os::StepScroll(100.0f, 300.0f, 40.0f, 0), WithinAbs(100.0, 1e-4));
    CHECK_THAT(os::StepScroll(290.0f, 300.0f, 40.0f, 1), WithinAbs(300.0, 1e-4));
    CHECK_THAT(os::StepScroll(300.0f, 300.0f, 40.0f, 1), WithinAbs(300.0, 1e-4));
    CHECK_THAT(os::StepScroll(10.0f, 300.0f, 40.0f, -1), WithinAbs(0.0, 1e-4));
    CHECK_THAT(os::StepScroll(0.0f, 300.0f, 40.0f, -1), WithinAbs(0.0, 1e-4));
    CHECK_THAT(os::StepScroll(0.0f, 0.0f, 40.0f, 1), WithinAbs(0.0, 1e-4));
    CHECK_THAT(os::StepScroll(5.0f, -3.0f, 40.0f, 1), WithinAbs(0.0, 1e-4));
}

TEST_CASE("options_skin_footer_height", "[ctest][options-skin]")
{
    const float em   = 16.9f;
    const float line = 16.9f;
    CHECK_THAT(os::FooterHeight(em, line, 0, 0), WithinAbs(3.2 * em, 1e-3));
    CHECK_THAT(os::FooterHeight(em, line, 1, 0), WithinAbs(3.5 * em + line, 1e-3));
    CHECK_THAT(os::FooterHeight(em, line, 2, 1), WithinAbs(3.7 * em + 3.0 * line, 1e-3));
    CHECK_THAT(os::FooterHeight(em, line, 0, 1), WithinAbs(3.5 * em + line, 1e-3));
    CHECK(os::FooterHeight(em, line, 3, 1) > os::FooterHeight(em, line, 1, 1));
    CHECK(os::FooterHeight(em, line, -2, -1) == os::FooterHeight(em, line, 0, 0));
}

TEST_CASE("options_skin_action_width", "[ctest][options-skin]")
{
    CHECK_THAT(os::ActionWidth(3, 1000.0f, 10.0f, 150.0f), WithinAbs(150.0, 1e-4));
    const float w = os::ActionWidth(3, 400.0f, 10.0f, 150.0f);
    CHECK_THAT(w, WithinAbs((400.0 - 20.0) / 3.0, 1e-3));
    CHECK_THAT(3.0f * w + 2.0f * 10.0f, WithinAbs(400.0, 1e-3));
    CHECK(os::ActionWidth(2, 5.0f, 10.0f, 150.0f) == 0.0f);
    CHECK_THAT(os::ActionWidth(0, 100.0f, 10.0f, 150.0f), WithinAbs(150.0, 1e-4));
    // The 640x360 case: Back (9 em) leaves less than three full buttons.
    const float em     = 16.9f;
    const float back   = 9.0f * em;
    const float start  = 1.5f * em;
    const float gap    = 0.6f * em;
    const float avail  = 640.0f - 1.5f * em - back - gap - start;
    const float shrunk = os::ActionWidth(3, avail, gap, 9.0f * em);
    CHECK(shrunk < 9.0f * em);
    CHECK(start + 3.0f * shrunk + 2.0f * gap <= 640.0f - 1.5f * em - back - gap + 1e-3f);
}

TEST_CASE("options_skin_hint_height", "[ctest][options-skin]")
{
    const float em = 16.9f;
    CHECK_THAT(os::HintHeight(em, 2.8f * em), WithinAbs(2.4 * em, 1e-3));
    CHECK(os::HintHeight(em, 0.1f * em) == 0.0f);
    CHECK(os::HintHeight(em, 1.8f * em) < 1.8f * em);
}

TEST_CASE("options_skin_shoulder_hints_are_a_share_of_the_trigger_height", "[ctest][options-skin]")
{
    const float em    = 16.9f;
    const float strip = 2.8f * em;
    // 80 percent of the 2.4 em trigger height.
    CHECK_THAT(os::ShoulderHintHeight(em, strip), WithinAbs(1.92 * em, 1e-3));
    CHECK(os::ShoulderHintHeight(em, strip) < os::HintHeight(em, strip));
    const float top = (strip - os::ShoulderHintHeight(em, strip)) * 0.5f;
    CHECK(top > 0.0f);
    CHECK_THAT(top, WithinAbs(0.44 * em, 1e-3));
    CHECK(2.0f * os::ShoulderHintHeight(em, strip) < 2.0f * os::HintHeight(em, strip));
    const float smallEm = 360.0f / 40.0f;
    CHECK(os::ShoulderHintHeight(smallEm, 2.8f * smallEm) < 2.8f * smallEm);
    CHECK(os::ShoulderHintHeight(smallEm, 0.1f * smallEm) == 0.0f);
}

TEST_CASE("options_skin_chrome", "[ctest][options-skin]")
{
    const float em   = 16.9f;
    const float row  = 2.4f * em;
    const float full = os::FooterHeight(em, em, 2, 1);
    const float low  = os::FooterHeight(em, em, 1, 1);
    auto        c    = os::ChromeFor(1440.0f, em, 2, row, 3.0f, 3.0f, full, low);
    CHECK_THAT(c.banner, WithinAbs(3.6 * em, 1e-3));
    CHECK_THAT(c.strip, WithinAbs(2.8 * em, 1e-3));
    CHECK_THAT(c.footer, WithinAbs(full, 1e-3));
    CHECK_THAT(c.titleStrip, WithinAbs(2.2 * em, 1e-3));
    CHECK(c.viewport >= 3.0f * row);

    c = os::ChromeFor(360.0f, em, 2, row, 3.0f, 3.0f, full, low);
    CHECK(c.titleStrip == 0.0f);
    CHECK(c.margin == 0.0f);
    CHECK(c.viewport >= 3.0f * row - 1e-2f);
    CHECK_THAT(c.banner + 2.0f + 2.0f * c.strip + c.margin + c.titleStrip + 1.0f + 3.0f + c.viewport + c.footer, WithinAbs(360.0, 1e-2));

    c = os::ChromeFor(360.0f, em, 2, 4.0f * em, 3.0f, 3.0f, full, low);
    CHECK_THAT(c.banner, WithinAbs(2.4 * em, 1e-3));
    CHECK_THAT(c.strip, WithinAbs(1.8 * em, 1e-3));
    CHECK_THAT(c.footer, WithinAbs(low, 1e-3));
    CHECK(c.viewport > 1.0f * 4.0f * em);

    c = os::ChromeFor(360.0f, em, 2, 4.0f * em, 3.0f, 3.0f, low, full);
    CHECK(c.footer <= low + 1e-3f);

    CHECK(os::ChromeFor(100.0f, em, 2, row, 3.0f, 3.0f, full, low).viewport == 0.0f);

    CHECK(os::ChromeFor(360.0f, em, 1, row, 5.0f, 3.0f, full, low).viewport > os::ChromeFor(360.0f, em, 2, row, 5.0f, 3.0f, full, low).viewport);
    CHECK(os::ChromeFor(1440.0f, em, 0, row, 3.0f, 3.0f, full, low).viewport > os::ChromeFor(1440.0f, em, 1, row, 3.0f, 3.0f, full, low).viewport);

    CHECK(os::FooterHeight(em, em, 0, 0) < full - 3.0f * em);
}

TEST_CASE("options_skin_map_crop", "[ctest][options-skin]")
{
    auto r = os::MapCrop(0, 512, 1920.0f, 1080.0f);
    CHECK_THAT(r.u1 - r.u0, WithinAbs(1.0, 1e-6));
    r = os::MapCrop(1024, 512, 0.0f, 1080.0f);
    CHECK_THAT(r.v1 - r.v0, WithinAbs(1.0, 1e-6));

    for (const auto size : { std::pair{ 1920.0f, 1080.0f }, std::pair{ 640.0f, 480.0f }, std::pair{ 2560.0f, 1080.0f }, std::pair{ 800.0f, 1600.0f } })
    {
        r = os::MapCrop(1024, 512, size.first, size.second);
        CHECK(r.u0 >= 0.0f);
        CHECK(r.v0 >= 0.0f);
        CHECK(r.u1 <= 1.0f + 1e-6f);
        CHECK(r.v1 <= 1.0f + 1e-6f);
        const double texW = (r.u1 - r.u0) * 1024.0;
        const double texH = (r.v1 - r.v0) * 512.0;
        if (texW < 1023.0 && texH < 511.0)
            CHECK_THAT(texW / texH, WithinAbs(size.first / size.second, 1e-3));
    }

    r = os::MapCrop(1024, 512, 1920.0f, 1080.0f);
    CHECK_THAT(r.v1 - r.v0, WithinAbs(1.0 / os::kMapZoom, 1e-4));
    CHECK(r.u1 - r.u0 < 1.0f);
}
