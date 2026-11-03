#include "main_menu_options.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <vector>

namespace mo = rerevved::main_menu_options;

namespace
{

auto bytes(std::array<uint8_t, 2> text)
{
    return [text](uint32_t offset) -> std::optional<uint8_t>
    {
        return offset < text.size() ? std::optional<uint8_t>(text[offset]) : std::nullopt;
    };
}

mo::Entry accepted()
{
    mo::Entry entry;
    entry.hostBound           = true;
    entry.menuVtable          = mo::kMenuVtable;
    entry.parent              = 0x83000000;
    entry.parentVtable        = mo::kParentVtable;
    entry.parentHandler       = mo::kParentHandler;
    entry.pending             = 0;
    entry.exiting             = 0;
    entry.currentState        = entry.parent;
    entry.prompt              = 0;
    entry.descriptorIsOptions = true;
    entry.pageOpen            = false;
    return entry;
}

} // namespace

TEST_CASE("main_menu_options_descriptor_is_exactly_4_and_its_terminator", "[ctest][main-menu-options]")
{
    CHECK(mo::IsOptionsDescriptor(bytes({ '4', 0 })));
    CHECK_FALSE(mo::IsOptionsDescriptor(bytes({ '3', 0 })));
    CHECK_FALSE(mo::IsOptionsDescriptor(bytes({ '0', 0 })));
    CHECK_FALSE(mo::IsOptionsDescriptor(bytes({ '4', '4' })));
    CHECK_FALSE(mo::IsOptionsDescriptor(bytes({ '4', ' ' })));
    CHECK_FALSE(mo::IsOptionsDescriptor(bytes({ 0, 0 })));
}

TEST_CASE("main_menu_options_descriptor_never_reads_past_a_boundary", "[ctest][main-menu-options]")
{
    int                        reads = 0;
    const std::vector<uint8_t> page{ 0 };
    CHECK_FALSE(mo::IsOptionsDescriptor(
        [&](uint32_t offset) -> std::optional<uint8_t>
        {
            ++reads;
            return offset < page.size() ? std::optional<uint8_t>(page[offset]) : std::nullopt;
        }));
    CHECK(reads == 1);

    const std::vector<uint8_t> four{ '4' };
    CHECK_FALSE(mo::IsOptionsDescriptor(
        [&](uint32_t offset) -> std::optional<uint8_t>
        {
            return offset < four.size() ? std::optional<uint8_t>(four[offset]) : std::nullopt;
        }));
}

TEST_CASE("main_menu_options_validated_accept_opens_the_page", "[ctest][main-menu-options]")
{
    CHECK(mo::ChooseRoute(accepted()) == mo::Route::kPage);
}

TEST_CASE("main_menu_options_unvalidated_runs_the_retail_selector", "[ctest][main-menu-options]")
{
    {
        auto entry      = accepted();
        entry.hostBound = false;
        CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
    }
    {
        auto entry                = accepted();
        entry.descriptorIsOptions = false;
        CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
    }
    {
        auto entry   = accepted();
        entry.parent = 0;
        CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
    }
    {
        auto entry       = accepted();
        entry.menuVtable = 0x82139CC0;
        CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
    }
    {
        auto entry         = accepted();
        entry.parentVtable = 0x8213B910;
        CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
    }
    {
        auto entry          = accepted();
        entry.parentHandler = 0x82D6C058;
        CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
    }
    {
        auto entry    = accepted();
        entry.pending = 36;
        CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
    }
    {
        auto entry    = accepted();
        entry.exiting = 1;
        CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
    }
    {
        auto entry   = accepted();
        entry.prompt = 0x83200000;
        CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
    }
    {
        auto entry         = accepted();
        entry.currentState = 0x83000100;
        CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
        entry.currentState = 0;
        CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
    }
    CHECK(mo::ChooseRoute({}) == mo::Route::kRetail);
}

TEST_CASE("main_menu_options_accept_while_page_up_is_consumed", "[ctest][main-menu-options]")
{
    auto entry     = accepted();
    entry.pageOpen = true;
    CHECK(mo::ChooseRoute(entry) == mo::Route::kIgnore);
    entry.descriptorIsOptions = false;
    CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
    entry.descriptorIsOptions = true;
    entry.hostBound           = false;
    CHECK(mo::ChooseRoute(entry) == mo::Route::kRetail);
}

TEST_CASE("main_menu_options_session_tracks_the_page_for_its_parent", "[ctest][main-menu-options]")
{
    mo::Session session;
    CHECK(session.Current().page == 0);
    CHECK_FALSE(session.Active());
    session.Begin(3, 0x83000000, 7);
    CHECK(session.Active());
    CHECK(session.Current().page == 3);
    CHECK(session.Current().parent == 0x83000000);
    CHECK(session.Current().managerPending == 7);
    session.End(2);
    CHECK(session.Current().page == 3);
    session.End(3);
    CHECK_FALSE(session.Active());
    CHECK(session.Current().page == 0);
    CHECK(session.Current().parent == 0);
    session.End(3);
    CHECK(session.Current().page == 0);
}

TEST_CASE("main_menu_options_page_is_dismissed_only_when_its_menu_leaves", "[ctest][main-menu-options]")
{
    constexpr uint32_t       parent = 0x83000000;
    const mo::Session::State session{ 4, parent, 7 };
    CHECK_FALSE(mo::ShouldDismiss(session, { parent, 0, 0, 7, 0 }));
    CHECK(mo::ShouldDismiss(session, { parent, 36, 1, 36, 0 }));
    // Automatic transition 1 (0x82D704F0) sets R+0xC and the manager word, leaving R+0x10 zero.
    CHECK(mo::ShouldDismiss(session, { parent, 0, 1, 1, 0 }));
    CHECK(mo::ShouldDismiss(session, { parent, 0, 1, 7, 0 }));
    CHECK(mo::ShouldDismiss(session, { parent, 0, 0, 1, 0 }));
}

TEST_CASE("main_menu_options_page_is_dismissed_when_the_parent_is_replaced", "[ctest][main-menu-options]")
{
    constexpr uint32_t       parent = 0x83000000;
    const mo::Session::State session{ 4, parent, 7 };
    CHECK(mo::ShouldDismiss(session, { 0x83000100, 0, 0, 7, 0 }));
    CHECK(mo::ShouldDismiss(session, { 0, 0, 0, 7, 0 }));
}

TEST_CASE("main_menu_options_page_is_dismissed_when_a_guest_prompt_appears", "[ctest][main-menu-options]")
{
    constexpr uint32_t       parent = 0x83000000;
    const mo::Session::State session{ 4, parent, 7 };
    // Events 25 and 28 install a prompt at manager+0x2C and change nothing else.
    CHECK(mo::ShouldDismiss(session, { parent, 0, 0, 7, 0x83200000 }));
}

TEST_CASE("main_menu_options_no_session_is_never_dismissed", "[ctest][main-menu-options]")
{
    constexpr uint32_t parent = 0x83000000;
    CHECK_FALSE(mo::ShouldDismiss({}, { 0, 0, 1, 1, 1 }));
    CHECK_FALSE(mo::ShouldDismiss({ 4, 0, 7 }, { 0, 0, 1, 1, 1 }));
    CHECK_FALSE(mo::ShouldDismiss({ 0, parent, 7 }, { 0, 0, 1, 1, 1 }));
}
