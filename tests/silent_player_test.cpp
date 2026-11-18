#include "silent_player.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace ps = rerevved::silent_player;

TEST_CASE("silent_player_reports_a_connected_player_once_past_the_keep_alive", "[ctest][silent-player]")
{
    ps::Tracker tracker;
    CHECK_FALSE(tracker.Think(0x100, true, 1000));
    CHECK_FALSE(tracker.Received(0x100, 2000));
    CHECK_FALSE(tracker.Think(0x100, true, 2000 + ps::kSilentMs - 1));
    CHECK(tracker.Think(0x100, true, 2000 + ps::kSilentMs) == ps::kSilentMs);
    CHECK_FALSE(tracker.Think(0x100, true, 2000 + ps::kSilentMs + 500));

    // Heard again: the gap is reported once and the timer restarts.
    CHECK(tracker.Received(0x100, 60000) == 58000);
    CHECK_FALSE(tracker.Received(0x100, 61000));
    CHECK_FALSE(tracker.Think(0x100, true, 61000 + ps::kSilentMs - 1));
}

TEST_CASE("silent_player_waits_for_the_connection_to_connect", "[ctest][silent-player]")
{
    ps::Tracker tracker;
    CHECK_FALSE(tracker.Think(0x200, false, 0));
    CHECK_FALSE(tracker.Think(0x200, false, 50000));
    // Connected now; the timer starts from the last unconnected think.
    CHECK_FALSE(tracker.Think(0x200, true, 50000 + ps::kSilentMs - 1));
    CHECK(tracker.Think(0x200, true, 50000 + ps::kSilentMs).has_value());
}

TEST_CASE("silent_player_treats_a_long_unthought_address_as_new", "[ctest][silent-player]")
{
    ps::Tracker tracker;
    CHECK_FALSE(tracker.Think(0x300, true, 0));
    // No think for kForgetMs: the address was freed and reused by a new connection.
    CHECK_FALSE(tracker.Think(0x300, true, ps::kForgetMs));
    CHECK_FALSE(tracker.Think(0x300, true, ps::kForgetMs + ps::kSilentMs - 1));
}

TEST_CASE("silent_player_prunes_stale_connections", "[ctest][silent-player]")
{
    ps::Tracker tracker;
    for (uint32_t i = 0; i < ps::kPruneAt; ++i)
    {
        tracker.Think(i, true, 0);
    }
    CHECK(tracker.Size() == ps::kPruneAt);
    tracker.Think(0x999, true, ps::kForgetMs);
    CHECK(tracker.Size() == 1);
}
