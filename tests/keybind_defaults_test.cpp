#include "keybind_defaults.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(kd_test_e, "Old,Two", "Test", "Keybind defaults test e");

namespace kd = rerevved::keybind_defaults;

TEST_CASE("keybind_defaults_keep_one_main_key", "[ctest][keybind-defaults]")
{
    for (const auto& entry : kd::kDefaults)
    {
        CAPTURE(entry.key);
        const auto comma = entry.sdkDefault.find(',');
        REQUIRE(comma != std::string_view::npos);
        CHECK(entry.sdkDefault.substr(0, comma) == entry.value);
        CHECK(entry.value.find_first_of(",+") == std::string_view::npos);
    }
    CHECK(kd::kDefaults.size() == 6);

    for (const auto& entry : kd::kDefaults)
    {
        CAPTURE(entry.key);
        const auto* info = rex::cvar::GetFlagInfo(entry.key);
        REQUIRE(info != nullptr);
        CHECK(info->default_value == entry.sdkDefault);
        CHECK(rex::cvar::GetFlagByName(entry.key) == entry.sdkDefault);
        REQUIRE(kd::Apply(std::span(&entry, 1)) == 1);
        CHECK(rex::cvar::GetFlagInfo(entry.key)->default_value == entry.value);
        CHECK(rex::cvar::GetFlagByName(entry.key) == entry.value);
        REQUIRE(rex::cvar::SetFlagByName(entry.key, "Z9"));
        rex::cvar::ResetToDefault(entry.key);
        CHECK(rex::cvar::GetFlagByName(entry.key) == entry.value);
    }
}

TEST_CASE("keybind_defaults_keep_a_command_line_value_and_still_reset_to_the_title_default", "[ctest][keybind-defaults]")
{
    const kd::Default defaults[] = { { "kd_test_e", "Old,Two", "One" } };
    REQUIRE(rex::cvar::SetFlagFromCommandLine("kd_test_e", "Mine"));
    REQUIRE(kd::Apply(defaults) == 1);
    CHECK(rex::cvar::GetFlagByName("kd_test_e") == "Mine");
    CHECK(rex::cvar::GetFlagSource("kd_test_e") == rex::cvar::Source::kCommandLine);
    // F1 and Y reset to the title's default, not the SDK's pair.
    CHECK(rex::cvar::GetFlagInfo("kd_test_e")->default_value == "One");
    rex::cvar::ResetToDefault("kd_test_e");
    CHECK(rex::cvar::GetFlagByName("kd_test_e") == "One");
}
