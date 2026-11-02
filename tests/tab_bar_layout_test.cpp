#include "tab_bar_layout.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "page_set.h"

namespace
{

namespace tbl = rerevved::tab_bar_layout;

// Seven titles like the Options tabs, em of 20; 380 px fits between Xbox shoulders on 640 px.
const std::vector<float> kTabs = { 110, 90, 130, 70, 100, 90, 60 };

} // namespace

TEST_CASE("tab_bar_layout_roomy_row_keeps_padding_and_centers", "[ctest][tab-bar-layout]")
{
    const auto fit = tbl::FitTabBar(kTabs, 1000.0f, 5.0f, 20.0f, 0.6f, 3);
    CHECK(fit.pad == 20.0f);
    CHECK(fit.scale == 1.0f);
    CHECK(fit.offset > 0.0f);
    CHECK(fit.offset + tbl::RowWidth(kTabs, fit) <= 1000.0f);
}

TEST_CASE("tab_bar_layout_narrow_row_shrinks_pad_then_text", "[ctest][tab-bar-layout]")
{
    // 650 of text needs 720 at minimum padding; 380 forces the text to shrink.
    const auto fit = tbl::FitTabBar(kTabs, 380.0f, 5.0f, 20.0f, 0.4f, 6);
    CHECK(fit.pad == 5.0f);
    CHECK(fit.scale < 1.0f);
    CHECK(fit.scale >= 0.4f);
    CHECK(fit.offset >= 0.0f);
    CHECK(tbl::RowWidth(kTabs, fit) <= 380.0f + 0.01f);
}

TEST_CASE("tab_bar_layout_scrolls_to_current_when_text_floor_reached", "[ctest][tab-bar-layout]")
{
    constexpr float available = 300.0f;
    for (size_t current = 0; current < kTabs.size(); ++current)
    {
        const auto fit = tbl::FitTabBar(kTabs, available, 5.0f, 20.0f, 0.8f, current);
        CHECK(fit.scale == 0.8f);
        const float total = tbl::RowWidth(kTabs, fit);
        REQUIRE(total > available);
        float left = fit.offset;
        for (size_t i = 0; i < current; ++i)
            left += kTabs[i] * fit.scale + 2.0f * fit.pad;
        const float right = left + kTabs[current] * fit.scale + 2.0f * fit.pad;
        CHECK(left >= -0.01f);
        CHECK(right <= available + 0.01f);
        CHECK(fit.offset <= 0.0f);
        CHECK(fit.offset + total >= available - 0.01f);
    }
}

TEST_CASE("tab_bar_layout_repeated_next_reaches_last_tab_visible", "[ctest][tab-bar-layout]")
{
    constexpr float   available = 300.0f;
    rerevved::PageSet pages;
    pages.Add("Tab0");
    for (size_t i = 1; i < kTabs.size(); ++i)
        pages.Add("Tab" + std::to_string(i));

    size_t steps = 0;
    while (pages.Current() + 1 < kTabs.size())
    {
        REQUIRE(pages.Step(1));
        REQUIRE(++steps < 100);
        const auto fit = tbl::FitTabBar(kTabs, available, 5.0f, 20.0f, 0.8f, pages.Current());
        REQUIRE(tbl::RowWidth(kTabs, fit) > available);
        float left = fit.offset;
        for (size_t i = 0; i < pages.Current(); ++i)
            left += kTabs[i] * fit.scale + 2.0f * fit.pad;
        const float right = left + kTabs[pages.Current()] * fit.scale + 2.0f * fit.pad;
        CHECK(left >= -0.01f);
        CHECK(right <= available + 0.01f);
    }
    CHECK(pages.Current() == kTabs.size() - 1);
    CHECK(steps == kTabs.size() - 1);
    CHECK(pages.Step(1));
    CHECK(pages.Current() == 0);
    CHECK(pages.Step(-1));
    CHECK(pages.Current() == kTabs.size() - 1);
}
