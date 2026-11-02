#include "retail_audio.h"

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/cvar.h>

namespace ra = rerevved::retail_audio;

TEST_CASE("retail_audio_converts_positions_like_the_retail_setter", "[ctest][retail-audio]")
{
    const float expected[] = { 0.0f, 0.11f, 0.22f, 0.33f, 0.44f, 0.55f, 0.66f, 0.77f, 0.88f };
    for (int position = 0; position <= 8; ++position)
    {
        CAPTURE(position);
        CHECK(ra::PositionToGain(position) == static_cast<float>(position) * 0.11f);
        CHECK(std::fabs(ra::PositionToGain(position) - expected[position]) < 1e-6f);
    }
    // 9 * 0.11 is 0.99, above the 0.98 snap, so the top step is exactly 1.
    CHECK(ra::PositionToGain(9) == 1.0f);
    CHECK(ra::PositionToGain(-5) == 0.0f);
    CHECK(ra::PositionToGain(40) == 1.0f);
    // The constants are the retail words 0x3DE147AE and 0x3F7AE148.
    CHECK(ra::kStep == std::bit_cast<float>(0x3DE147AEu));
    CHECK(ra::kSnap == std::bit_cast<float>(0x3F7AE148u));
}

TEST_CASE("retail_audio_reads_a_gain_back_as_a_position", "[ctest][retail-audio]")
{
    for (int position = 0; position <= 9; ++position)
    {
        CAPTURE(position);
        const auto reading = ra::ReadGain(ra::PositionToGain(position));
        CHECK(reading.position == position);
        CHECK(reading.exact);
    }
}

TEST_CASE("retail_audio_marks_a_gain_between_steps_as_off_grid", "[ctest][retail-audio]")
{
    const auto between = ra::ReadGain(0.40f);
    CHECK(between.position == 4);
    CHECK_FALSE(between.exact);

    const auto nearTop = ra::ReadGain(0.99f);
    CHECK(nearTop.position == 9);
    CHECK_FALSE(nearTop.exact);

    CHECK(ra::ReadGain(0.055f).position == 1);

    const auto outOfRange = ra::ReadGain(1.5f);
    CHECK(outOfRange.position == 9);
    CHECK_FALSE(outOfRange.exact);
    const auto negative = ra::ReadGain(-0.2f);
    CHECK(negative.position == 0);
    CHECK_FALSE(negative.exact);
    CHECK_FALSE(ra::ReadGain(std::nanf("")).exact);
}

TEST_CASE("retail_audio_channels_are_in_the_retail_order", "[ctest][retail-audio]")
{
    REQUIRE(ra::kChannels.size() == 4);
    CHECK(ra::kChannels[0].catalogKey == 0x821234A0); // USER_VOLUME_MUSIC
    CHECK(ra::kChannels[1].catalogKey == 0x821234B4); // USER_VOLUME_SFX
    CHECK(ra::kChannels[2].catalogKey == 0x821234C4); // USER_VOLUME_ADVISORS
    CHECK(ra::kChannels[3].catalogKey == 0x821234DC); // USER_VOLUME_LEADERS
}

TEST_CASE("retail_audio_overrides_default_to_none_and_accept_minus_one_to_nine", "[ctest][retail-audio]")
{
    for (size_t i = 0; i < ra::kChannelCount; ++i)
    {
        const std::string name(ra::kChannels[i].cvar);
        CAPTURE(name);
        CHECK(rex::cvar::GetFlagByName(name) == "-1");
        CHECK(ra::OverrideOf(i) == ra::kNoOverride);

        const auto info = rex::cvar::GetFlagInfo(name);
        REQUIRE(info);
        CHECK(info->constraints.min == -1.0);
        CHECK(info->constraints.max == 9.0);

        CHECK(rex::cvar::SetFlagByName(name, "0"));
        ra::PublishOverrides();
        CHECK(ra::OverrideOf(i) == 0);
        CHECK(rex::cvar::SetFlagByName(name, "9"));
        ra::PublishOverrides();
        CHECK(ra::OverrideOf(i) == 9);
        CHECK_FALSE(rex::cvar::SetFlagByName(name, "10"));
        CHECK_FALSE(rex::cvar::SetFlagByName(name, "-2"));
        CHECK(ra::OverrideOf(i) == 9);
        CHECK(rex::cvar::SetFlagByName(name, "-1"));
        ra::PublishOverrides();
        CHECK(ra::OverrideOf(i) == ra::kNoOverride);
    }
    CHECK(ra::OverrideOf(ra::kChannelCount) == ra::kNoOverride);
}

TEST_CASE("retail_audio_queue_keeps_the_latest_request_per_channel", "[ctest][retail-audio]")
{
    ra::ApplyQueue queue;
    CHECK_FALSE(queue.HasPending());
    CHECK(queue.Take().empty());

    CHECK(queue.Post(1, 3));
    CHECK(queue.Post(1, 7));
    CHECK(queue.Post(0, 0));
    CHECK(queue.HasPending());
    const auto taken = queue.Take();
    REQUIRE(taken.size() == 2);
    CHECK(taken[0].channel == 0);
    CHECK(taken[0].position == 0);
    CHECK(taken[1].channel == 1);
    CHECK(taken[1].position == 7);
    CHECK(taken[0].generation == queue.Generation());
    CHECK_FALSE(queue.HasPending());
    CHECK(queue.Take().empty());
}

TEST_CASE("retail_audio_queue_refuses_a_bad_channel_or_position", "[ctest][retail-audio]")
{
    ra::ApplyQueue queue;
    CHECK_FALSE(queue.Post(ra::kChannelCount, 5));
    CHECK_FALSE(queue.Post(0, -1));
    CHECK_FALSE(queue.Post(0, 10));
    CHECK_FALSE(queue.HasPending());
}

TEST_CASE("retail_audio_queue_drops_requests_of_an_older_generation", "[ctest][retail-audio]")
{
    ra::ApplyQueue queue;
    const auto     first = queue.Generation();
    CHECK(queue.Post(2, 4, first));

    const auto second = queue.Advance();
    CHECK(second == first + 1);
    CHECK(queue.Generation() == second);
    CHECK_FALSE(queue.HasPending());
    CHECK(queue.Take().empty());

    CHECK_FALSE(queue.Post(2, 4, first));
    CHECK_FALSE(queue.HasPending());
    CHECK(queue.Post(2, 5, second));
    const auto taken = queue.Take();
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].position == 5);
    CHECK(taken[0].generation == second);
}

TEST_CASE("retail_audio_queue_requeues_only_what_is_still_current", "[ctest][retail-audio]")
{
    ra::ApplyQueue queue;
    REQUIRE(queue.Post(0, 6));
    const auto taken = queue.Take();
    REQUIRE(taken.size() == 1);

    queue.Requeue(taken[0]);
    CHECK(queue.HasPending());

    auto again = queue.Take();
    REQUIRE(again.size() == 1);
    REQUIRE(queue.Post(0, 8));
    queue.Requeue(again[0]);
    auto latest = queue.Take();
    REQUIRE(latest.size() == 1);
    CHECK(latest[0].position == 8);

    queue.Advance();
    queue.Requeue(latest[0]);
    CHECK_FALSE(queue.HasPending());
    CHECK(queue.Take().empty());
}

TEST_CASE("retail_audio_live_store_changes_its_revision_only_with_its_content", "[ctest][retail-audio]")
{
    ra::LiveStore store;
    CHECK(store.Revision() == 0);
    CHECK_FALSE(store.Read().volume[0].available);

    ra::LiveSnapshot snapshot;
    snapshot.generation          = 3;
    snapshot.volume[0].available = true;
    snapshot.volume[0].gain      = ra::PositionToGain(4);
    store.Publish(snapshot);
    CHECK(store.Revision() == 1);
    store.Publish(snapshot);
    CHECK(store.Revision() == 1);

    const auto copy = store.Read();
    CHECK(copy == snapshot);
    CHECK(ra::ReadGain(copy.volume[0].gain).position == 4);

    snapshot.volume[0].gain = ra::PositionToGain(5);
    store.Publish(snapshot);
    CHECK(store.Revision() == 2);
    snapshot.generation = 4;
    store.Publish(snapshot);
    CHECK(store.Revision() == 3);
}

TEST_CASE("retail_audio_live_store_hands_a_refresh_request_to_one_consumer", "[ctest][retail-audio]")
{
    ra::LiveStore store;
    CHECK_FALSE(store.RefreshRequested());
    CHECK_FALSE(store.ConsumeRefresh());
    store.RequestRefresh();
    store.RequestRefresh();
    CHECK(store.RefreshRequested());
    CHECK(store.ConsumeRefresh());
    CHECK_FALSE(store.ConsumeRefresh());
    CHECK_FALSE(store.RefreshRequested());
}

TEST_CASE("retail_audio_reads_the_nearest_actual_step_near_the_top", "[ctest][retail-audio]")
{
    // The last step is 1.0, not 0.99: 0.937 is closer to 0.88 than to 1.0.
    CHECK(ra::ReadGain(0.937f).position == 8);
    CHECK(ra::ReadGain(0.939f).position == 8);
    CHECK(ra::ReadGain(0.941f).position == 9);
    CHECK(ra::ReadGain(0.99f).position == 9);
}

TEST_CASE("retail_audio_names_the_steps_on_each_side_of_an_off_grid_gain", "[ctest][retail-audio]")
{
    const auto between = ra::ReadGain(0.40f);
    CHECK(between.below == 3);
    CHECK(between.above == 4);
    const auto low = ra::ReadGain(0.05f);
    CHECK(low.below == 0);
    CHECK(low.above == 1);
    const auto high = ra::ReadGain(0.89f);
    CHECK(high.below == 8);
    CHECK(high.above == 9);
    const auto top = ra::ReadGain(0.99f);
    CHECK(top.below == 8);
    CHECK(top.above == 9);
    const auto exact = ra::ReadGain(ra::PositionToGain(5));
    CHECK(exact.below == 5);
    CHECK(exact.above == 5);
}

TEST_CASE("retail_audio_overrides_follow_the_published_snapshot", "[ctest][retail-audio]")
{
    REQUIRE(rex::cvar::SetFlagByName("retail_music_volume", "6"));
    CHECK(ra::OverrideOf(0) == ra::kNoOverride);
    ra::PublishOverrides();
    CHECK(ra::OverrideOf(0) == 6);
    REQUIRE(rex::cvar::SetFlagByName("retail_music_volume", "-1"));
    ra::PublishOverrides();
    CHECK(ra::OverrideOf(0) == ra::kNoOverride);
}
