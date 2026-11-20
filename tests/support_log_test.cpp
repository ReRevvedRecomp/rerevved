#include "support_log.h"

#include <catch2/catch_test_macros.hpp>

namespace sl = rerevved::support_log::detail;

TEST_CASE("support_log_crash_context_names_build_phase_and_turn", "[ctest][support-log]")
{
    CHECK(sl::ContextText("0.7.6", "abc123", "", std::nullopt) == "ReRevved 0.7.6 title=abc123 phase=startup");
    CHECK(sl::ContextText("0.7.6", "abc123", "new network game", 42u) == "ReRevved 0.7.6 title=abc123 phase=new network game turn=42");
}

TEST_CASE("support_log_package_list_keeps_load_order", "[ctest][support-log]")
{
    CHECK(sl::PackageList({}) == "none");
    CHECK(sl::PackageList({ { "b-mod", "1.2" }, { "a-mod", "" } }) == "b-mod 1.2, a-mod");
}
