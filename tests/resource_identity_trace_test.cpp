#include "resource_identity_trace.h"

#include <catch2/catch_test_macros.hpp>
#include <string_view>

namespace
{

namespace overrides                          = rerevved::resource_identity_overrides;
namespace trace                              = rerevved::resource_identity_trace;
int                  notifications           = 0;
int                  unpublishedObservations = 0;
bool                 published               = false;
overrides::Snapshot  observedPrevious;
overrides::Snapshot  observedNext;
std::source_location observedCaller;
trace::EventHistory  history;

void observe(bool wasPublished, const overrides::Snapshot& previous, const overrides::Snapshot& next, std::source_location caller) noexcept
{
    ++notifications;
    published        = wasPublished;
    observedPrevious = previous;
    observedNext     = next;
    observedCaller   = caller;
    history.Reset();
    // Catch assertions may throw, so the noexcept observer only records.
    if (!(*overrides::Current() == next))
    {
        ++unpublishedObservations;
    }
}

void testFiltering()
{
    const overrides::Snapshot snapshot = { { 0, 0 }, { 410, 9 }, { 1271, 18 } };
    UNSCOPED_INFO("Oxen ordinal zero is an exact match");
    CHECK(trace::Matches(snapshot, 0));
    UNSCOPED_INFO("high register bits do not alias tile zero");
    CHECK(!trace::Matches(snapshot, uint64_t{ 1 } << 32));
    UNSCOPED_INFO("empty snapshots do not match");
    CHECK(!trace::Matches({}, 0));
    UNSCOPED_INFO("every exact override tile matches");
    CHECK((trace::Matches(snapshot, 410) && trace::Matches(snapshot, 1271)));
    UNSCOPED_INFO("adjacent no-record tiles are filtered");
    CHECK((!trace::Matches(snapshot, 411) && !trace::Matches(snapshot, 450)));
    UNSCOPED_INFO("nearby wrapped coordinates do not match");
    CHECK((!trace::Matches(snapshot, 1240) && !trace::Matches(snapshot, 1280)));
    UNSCOPED_INFO("distant tiles are filtered");
    CHECK(!trace::Matches(snapshot, 412));
}

void testDeduplication()
{
    trace::EventHistory events;
    constexpr auto      decision = trace::EventKind::Decision;
    constexpr auto      returned = trace::EventKind::Return;
    UNSCOPED_INFO("first decision emits");
    CHECK(events.ShouldEmit(decision, 0x82CF1F10, 410, 0, 1, "applied"));
    UNSCOPED_INFO("first return emits independently");
    CHECK(events.ShouldEmit(returned, 0x82CF1F10, 410, 0, 9, "applied"));
    for (int frame = 0; frame < 156; ++frame)
    {
        UNSCOPED_INFO("unchanged decision is suppressed across frames");
        CHECK(!events.ShouldEmit(decision, 0x82CF1F10, 410, 0, 1, "applied"));
        UNSCOPED_INFO("unchanged return is suppressed across frames");
        CHECK(!events.ShouldEmit(returned, 0x82CF1F10, 410, 0, 9, "applied"));
    }
    UNSCOPED_INFO("changed r8_after emits");
    CHECK(events.ShouldEmit(decision, 0x82CF1F10, 410, 0, 2, "applied"));
    UNSCOPED_INFO("changed decision becomes the remembered value");
    CHECK(!events.ShouldEmit(decision, 0x82CF1F10, 410, 0, 2, "applied"));
    UNSCOPED_INFO("changed decision text emits with unchanged r8_after");
    CHECK(events.ShouldEmit(decision, 0x82CF1F10, 410, 0, 2, "terrain mismatch"));
    UNSCOPED_INFO("changed ordinal emits including Oxen zero");
    CHECK(events.ShouldEmit(returned, 0x82CF1F10, 410, 0, 0, "applied"));
    UNSCOPED_INFO("changed return becomes the remembered value");
    CHECK(!events.ShouldEmit(returned, 0x82CF1F10, 410, 0, 0, "applied"));
    UNSCOPED_INFO("changed decision text also emits a return");
    CHECK(events.ShouldEmit(returned, 0x82CF1F10, 410, 0, 0, "terrain mismatch"));
    UNSCOPED_INFO("returning to an earlier decision emits again");
    CHECK(events.ShouldEmit(decision, 0x82CF1F10, 410, 0, 1, "applied"));
    UNSCOPED_INFO("returning to an earlier ordinal emits again");
    CHECK(events.ShouldEmit(returned, 0x82CF1F10, 410, 0, 9, "applied"));
    UNSCOPED_INFO("callers have independent keys");
    CHECK(events.ShouldEmit(decision, 0x82CF1CB0, 410, 0, 1, "applied"));
    UNSCOPED_INFO("tile indices have independent keys");
    CHECK(events.ShouldEmit(decision, 0x82CF1F10, 411, 0, 1, "applied"));
    UNSCOPED_INFO("players have independent keys");
    CHECK(events.ShouldEmit(decision, 0x82CF1F10, 410, 1, 1, "applied"));
    UNSCOPED_INFO("other keys do not disturb the last decision");
    CHECK(!events.ShouldEmit(decision, 0x82CF1F10, 410, 0, 1, "applied"));
}

void testYieldCoordinates()
{
    const overrides::Snapshot snapshot = { { 658, 0 } };
    const auto                exact    = trace::MatchCoordinates(snapshot, 16, 18, 32, 32);
    UNSCOPED_INFO("known Oxen coordinate matches");
    CHECK((exact.ShouldTrace() && exact.exact && exact.wrapped && !exact.outOfBounds && exact.index == 658));
    const auto cityRelative = trace::MatchCoordinates(snapshot, 48, 18, 32, 32);
    UNSCOPED_INFO("city-relative a+32 reaches the watched tile");
    CHECK((cityRelative.ShouldTrace() && !cityRelative.exact && cityRelative.wrapped && cityRelative.outOfBounds));
    UNSCOPED_INFO("raw and wrapped indices remain distinguishable");
    CHECK((cityRelative.index == 1938 && cityRelative.wrappedIndex == 658));
    const auto negative = trace::MatchCoordinates(snapshot, static_cast<uint64_t>(-16), static_cast<uint64_t>(-14), 32, 32);
    UNSCOPED_INFO("negative coordinates wrap without unsigned remainder");
    CHECK((negative.wrapped && negative.outOfBounds && negative.wrappedIndex == 658));
    const auto zeroExtendedNegative = trace::MatchCoordinates(snapshot, uint32_t(-16), uint32_t(-14), 32, 32);
    UNSCOPED_INFO("zero-extended signed PPC coordinates normalize identically");
    CHECK((zeroExtendedNegative.wrapped && zeroExtendedNegative.outOfBounds));
    UNSCOPED_INFO("b+32 reaches the watched tile");
    CHECK(trace::MatchCoordinates(snapshot, 16, 50, 32, 32).wrapped);
    const auto unrelatedOutside = trace::MatchCoordinates(snapshot, 33, 7, 32, 32);
    UNSCOPED_INFO("all out-of-map calls remain observable");
    CHECK((unrelatedOutside.ShouldTrace() && unrelatedOutside.outOfBounds && !unrelatedOutside.wrapped));
    UNSCOPED_INFO("ordinary unrelated tile stays filtered");
    CHECK(!trace::MatchCoordinates(snapshot, 16, 19, 32, 32).ShouldTrace());
    UNSCOPED_INFO("out-of-map calls survive an empty publication window");
    CHECK(trace::MatchCoordinates({}, 48, 18, 32, 32).ShouldTrace());
    const auto invalidBounds = trace::MatchCoordinates(snapshot, 48, 18, 0, 32);
    UNSCOPED_INFO("unreadable bounds never invent wrapping");
    CHECK((!invalidBounds.boundsRead && !invalidBounds.ShouldTrace()));
    UNSCOPED_INFO("bounds outside physical stride are rejected");
    CHECK(!trace::MatchCoordinates(snapshot, 48, 18, 41, 32).boundsRead);
    UNSCOPED_INFO("exact index remains usable without bounds");
    CHECK(trace::MatchCoordinates(snapshot, 16, 18, 0, 0).exact);
}

void testYieldDeduplication()
{
    trace::EventHistory events;
    const auto          emit = [&](uint32_t site, uint64_t caller, uint64_t a, uint64_t b, uint64_t city, uint64_t player, uint64_t value)
    {
        return events.ShouldEmitSample(site, caller, a, b, city, player, { value, 0, 0, 0 });
    };
    UNSCOPED_INFO("first food result emits");
    CHECK(emit(0x82CF1CE8, 0x82D13E28, 16, 18, 0, UINT64_MAX, 5));
    UNSCOPED_INFO("same helper result is suppressed");
    CHECK(!emit(0x82CF1CE8, 0x82D13E28, 16, 18, 0, UINT64_MAX, 5));
    UNSCOPED_INFO("different helper result emits");
    CHECK(emit(0x82CF1CE8, 0x82D13E28, 16, 18, 0, UINT64_MAX, 2));
    UNSCOPED_INFO("result tuple stays de-duplicated after another result");
    CHECK(!emit(0x82CF1CE8, 0x82D13E28, 16, 18, 0, UINT64_MAX, 5));
    UNSCOPED_INFO("raw a+32 does not alias canonical a");
    CHECK(emit(0x82CF1CE8, 0x82D13E28, 48, 18, 0, UINT64_MAX, 5));
    UNSCOPED_INFO("raw b+32 has an independent key");
    CHECK(emit(0x82CF1CE8, 0x82D13E28, 16, 50, 0, UINT64_MAX, 5));
    UNSCOPED_INFO("callers remain independent");
    CHECK(emit(0x82CF1CE8, 0x82D931B8, 16, 18, 0, UINT64_MAX, 5));
    UNSCOPED_INFO("yield owners remain independent");
    CHECK(emit(0x82CF1AF0, 0x82D13E28, 16, 18, 0, UINT64_MAX, 5));
    UNSCOPED_INFO("city input remains independent");
    CHECK(emit(0x82CF1CE8, 0x82D13E28, 16, 18, 1, UINT64_MAX, 5));
    UNSCOPED_INFO("player input remains independent");
    CHECK(emit(0x82CF1CE8, 0x82D13E28, 16, 18, 0, 1, 5));
    events.Reset();
    UNSCOPED_INFO("publication reset permits a repeated yield tuple");
    CHECK(emit(0x82CF1CE8, 0x82D13E28, 16, 18, 0, UINT64_MAX, 5));
    events.Reset();
    for (uint64_t key = 0; key < trace::kEventCap; ++key)
    {
        UNSCOPED_INFO("bounded sample history admits distinct events");
        CHECK(emit(0x82CF1CE8, key, 16, 18, 0, 0, 5));
    }
    UNSCOPED_INFO("full sample history still suppresses a known tuple");
    CHECK(!emit(0x82CF1CE8, 0, 16, 18, 0, 0, 5));
    UNSCOPED_INFO("full sample history permits the shared truncation marker");
    CHECK(emit(0x82CF1CE8, trace::kEventCap, 16, 18, 0, 0, 5));
}

void testBudget()
{
    UNSCOPED_INFO("trace permits 4096 events per process");
    CHECK(trace::kEventCap == 4096);
    trace::EventBudget  budget;
    trace::EventHistory events;
    for (uint64_t sequence = 1; sequence <= trace::kEventCap; ++sequence)
    {
        UNSCOPED_INFO("each new key can consume one event");
        CHECK(events.ShouldEmit(trace::EventKind::Decision, sequence, 410, 0, 1, "applied"));
        UNSCOPED_INFO("events share one increasing sequence through the cap");
        CHECK(budget.Next() == sequence);
    }
    UNSCOPED_INFO("full history still suppresses a repeated key");
    CHECK(!events.ShouldEmit(trace::EventKind::Decision, 1, 410, 0, 1, "applied"));
    UNSCOPED_INFO("full history still permits the truncation marker for a new key");
    CHECK(events.ShouldEmit(trace::EventKind::Decision, trace::kEventCap + 1, 410, 0, 1, "applied"));
    UNSCOPED_INFO("one truncation marker follows the cap");
    CHECK(budget.Next() == trace::kEventCap + 1);
    UNSCOPED_INFO("events after truncation stay suppressed");
    CHECK((budget.Next() == 0 && budget.Next() == 0));
}

void testPublicationObservation()
{
    overrides::Clear();
    overrides::SetObserver(observe);
    const overrides::Snapshot records = { { 401, 0 } };
    UNSCOPED_INFO("valid snapshot publishes with observer installed");
    CHECK(overrides::Publish(records));
    UNSCOPED_INFO("publish notifies once with both tile lists");
    CHECK((notifications == 1 && published && observedPrevious.empty() && observedNext == records));
    UNSCOPED_INFO("default source location identifies the caller");
    CHECK(std::string_view(observedCaller.function_name()).find("testPublicationObservation") != std::string_view::npos);
    const auto checkReads = [](bool expected)
    {
        UNSCOPED_INFO("publication history gates each decision independently");
        CHECK(history.ShouldEmit(trace::EventKind::Decision, 0x82CF1F10, 401, 0, 0, "applied") == expected);
        UNSCOPED_INFO("publication history gates each return independently");
        CHECK(history.ShouldEmit(trace::EventKind::Return, 0x82CF1F10, 401, 0, 0, "applied") == expected);
        UNSCOPED_INFO("publication history also resets yield samples");
        CHECK(history.ShouldEmitSample(0x82CF1CE8, 0x82D13E28, 10, 1, 0, UINT64_MAX, { 2, 0, 0, 0 }) == expected);
    };
    checkReads(true);
    checkReads(false);
    const auto retained = overrides::Current();
    UNSCOPED_INFO("rejected publication is not a state event");
    CHECK((!overrides::Publish(overrides::Snapshot{ { 32, 0 } }) && notifications == 1 && overrides::Current() == retained));
    checkReads(false);
    UNSCOPED_INFO("identical publication still notifies");
    CHECK((overrides::Publish(records) && notifications == 2));
    checkReads(true);
    checkReads(false);
    overrides::Clear();
    UNSCOPED_INFO("clear notifies with removed tiles");
    CHECK((notifications == 3 && !published && observedPrevious == records && observedNext.empty()));
    checkReads(true);
    checkReads(false);
    overrides::Clear();
    UNSCOPED_INFO("empty clear still notifies");
    CHECK((notifications == 4 && observedPrevious.empty() && observedNext.empty()));
    checkReads(true);
    checkReads(false);
    overrides::SetObserver(nullptr);
    overrides::Clear();
    UNSCOPED_INFO("observer removal and retained readers preserve behavior");
    CHECK((notifications == 4 && *retained == records));
    UNSCOPED_INFO("observer sees the already-published snapshot");
    CHECK(unpublishedObservations == 0);
}

} // namespace

TEST_CASE("resource_identity_trace", "[ctest][resource-identity-trace]")
{
    testFiltering();
    testDeduplication();
    testYieldCoordinates();
    testYieldDeduplication();
    testBudget();
    testPublicationObservation();
}
