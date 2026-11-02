#include "game_language.h"

#include <string>

#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>

REXCVAR_DECLARE(uint32_t, game_language);
REXCVAR_DECLARE(uint32_t, user_language);

namespace gl = rerevved::game_language;

namespace
{

// Boot changes user_language's source/default, which restoring its value cannot undo; reset the registry per case.
struct ResetCvars
{
    ~ResetCvars()
    {
        rex::cvar::testing::ResetAllForTesting();
    }
};

} // namespace

TEST_CASE("game_language_offers_five_languages_in_id_order", "[ctest][game-language]")
{
    std::string names;
    std::string ids;
    for (const auto& language : gl::kLanguages)
    {
        names += std::string(language.name) + " ";
        ids += std::to_string(language.id) + " ";
    }
    CHECK(names == "English German French Spanish Italian ");
    CHECK(ids == "1 3 4 5 6 ");
    CHECK(gl::NameOf(4) == "French");
    CHECK(gl::NameOf(2).empty());
    CHECK(gl::NameOf(0).empty());
    CHECK(gl::IsOffered(gl::kDefaultLanguage));
    CHECK_FALSE(gl::IsOffered(7));
}

TEST_CASE("game_language_defaults_to_english_and_restarts", "[ctest][game-language]")
{
    const auto* info = rex::cvar::GetFlagInfo("game_language");
    REQUIRE(info != nullptr);
    CHECK(info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart);
    CHECK(info->default_value == "1");
}

TEST_CASE("game_language_boot_decision", "[ctest][game-language]")
{
    CHECK(gl::DecideBoot(4, false) == gl::BootAction::kCopy);
    CHECK(gl::DecideBoot(1, false) == gl::BootAction::kCopy);
    CHECK(gl::DecideBoot(4, true) == gl::BootAction::kKeepOverride);
    CHECK(gl::DecideBoot(2, true) == gl::BootAction::kKeepOverride);
    CHECK(gl::DecideBoot(2, false) == gl::BootAction::kSkipUnsupported);
    CHECK(gl::DecideBoot(0, false) == gl::BootAction::kSkipUnsupported);

    using rex::cvar::Source;
    CHECK(gl::IsExplicitSource(Source::kConfig));
    CHECK(gl::IsExplicitSource(Source::kEnvironment));
    CHECK(gl::IsExplicitSource(Source::kCommandLine));
    CHECK_FALSE(gl::IsExplicitSource(Source::kDefault));
    CHECK_FALSE(gl::IsExplicitSource(Source::kApplicationDefault));
}

TEST_CASE("game_language_boot_copies_the_setting_once_and_later_edits_stay_out", "[ctest][game-language]")
{
    const ResetCvars reset;
    REQUIRE(rex::cvar::GetFlagSource("user_language") == rex::cvar::Source::kDefault);

    REXCVAR_SET(game_language, 4u);
    gl::ApplyAtBoot();
    CHECK(REXCVAR_GET(user_language) == 4);
    CHECK(gl::ActiveId() == 4);

    CHECK(rex::cvar::SetFlagByName("game_language", "3"));
    CHECK(REXCVAR_GET(game_language) == 3);
    CHECK(REXCVAR_GET(user_language) == 4);
    CHECK(gl::ActiveId() == 4);
}

TEST_CASE("game_language_boot_leaves_an_explicit_user_language_alone", "[ctest][game-language]")
{
    const ResetCvars reset;

    REXCVAR_SET(game_language, 4u);
    REQUIRE(rex::cvar::SetFlagFromCommandLine("user_language", "5"));
    gl::ApplyAtBoot();
    CHECK(REXCVAR_GET(user_language) == 5);
    CHECK(gl::ActiveId() == 5);
    CHECK(rex::cvar::GetFlagSource("user_language") == rex::cvar::Source::kCommandLine);
}

TEST_CASE("game_language_boot_does_not_copy_an_unlisted_setting", "[ctest][game-language]")
{
    const ResetCvars reset;

    REXCVAR_SET(game_language, 2u);
    gl::ApplyAtBoot();
    CHECK(REXCVAR_GET(user_language) == 1);
    CHECK(gl::ActiveId() == 1);
    CHECK(REXCVAR_GET(game_language) == 2);
    CHECK(rex::cvar::GetFlagSource("user_language") == rex::cvar::Source::kDefault);
}
