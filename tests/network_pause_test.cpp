#include "network_pause.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace np = rerevved::network_pause;

namespace
{

constexpr uint32_t kRoot = 0x40001000;

} // namespace

TEST_CASE("local_resume_applies_for_paused_network_game", "[ctest][network-pause]")
{
    CHECK(np::ShouldApplyLocalResume(1, 0, kRoot, np::kPausedFlag));
    CHECK(np::ShouldApplyLocalResume(1, 0, kRoot, 0xFFFFFFFF));
    CHECK(np::kResumeLocalArgument == 0xFFFFFFFF);
}

TEST_CASE("local_resume_ignores_pause_request", "[ctest][network-pause]")
{
    CHECK_FALSE(np::ShouldApplyLocalResume(1, 1, kRoot, np::kPausedFlag));
    CHECK_FALSE(np::ShouldApplyLocalResume(1, 0xFF, kRoot, np::kPausedFlag));
}

TEST_CASE("local_resume_ignores_single_player", "[ctest][network-pause]")
{
    CHECK_FALSE(np::ShouldApplyLocalResume(0, 0, kRoot, np::kPausedFlag));
}

TEST_CASE("local_resume_ignores_already_resumed_root", "[ctest][network-pause]")
{
    CHECK_FALSE(np::ShouldApplyLocalResume(1, 0, kRoot, 0));
    CHECK_FALSE(np::ShouldApplyLocalResume(1, 0, kRoot, ~np::kPausedFlag));
}

TEST_CASE("local_resume_ignores_missing_root", "[ctest][network-pause]")
{
    CHECK_FALSE(np::ShouldApplyLocalResume(1, 0, 0, np::kPausedFlag));
}
