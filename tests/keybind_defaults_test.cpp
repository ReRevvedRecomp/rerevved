#include "keybind_defaults.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <string_view>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(kd_test_e, "Old,Two", "Test", "Keybind defaults test e");

namespace kd = rerevved::keybind_defaults;

TEST_CASE("keybind_defaults_replace_the_sdk_defaults_without_sharing_a_key", "[ctest][keybind-defaults]")
{
    // The keys of the binds the title leaves at the SDK default.
    std::set<std::string, std::less<>> used{ "W", "A", "S", "D", "Up", "Down", "Left", "Right", "Shift+Up", "Shift+Down" };
    for (const auto& entry : kd::kDefaults)
    {
        CAPTURE(entry.key);
        CHECK(entry.value != entry.sdkDefault);
        std::string_view rest = entry.value;
        while (true)
        {
            const auto comma = rest.find(',');
            const auto key   = rest.substr(0, comma);
            CAPTURE(key);
            CHECK_FALSE(key.empty());
            CHECK(used.emplace(key).second);
            if (comma == std::string_view::npos)
            {
                break;
            }
            rest.remove_prefix(comma + 1);
        }
    }
    CHECK(kd::kDefaults.size() == 14);

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
