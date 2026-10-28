#include "capital_grace.h"
#include "capital_grace_special.h"

#include <catch2/catch_test_macros.hpp>

namespace
{

namespace grace                  = rerevved::capital_grace;
constexpr uint32_t     kQueue    = 0x8314F1F8;
constexpr uint32_t     kProducer = 0x40008000;
constexpr uint32_t     kSubmit   = kProducer - 256 - 144;
constexpr grace::Words kWords{ 17, 5, 9, 2, 0x12345678 };

void append(grace::Tokens& tokens, uint32_t serial = 100)
{
    tokens.BeginSubmit(kSubmit, kQueue);
    CHECK_FALSE(tokens.Append(kSubmit, kProducer - 176, kWords, 32, 0));
    REQUIRE(tokens.Append(kSubmit, kProducer - 176, kWords, serial, serial));
    tokens.CompleteAppend(kSubmit);
}

void refuse(grace::Tokens& tokens, uint32_t serial = 100)
{
    tokens.DrainSerial(kSubmit, serial, serial + 1);
    REQUIRE(tokens.DrainRecord(kSubmit, kSubmit + 80, kWords));
    REQUIRE(tokens.Refuse(kSubmit + 80, kWords));
}

} // namespace

TEST_CASE("capital_grace_literal_command_decoders", "[ctest][capital-grace]")
{
    CHECK(grace::ActorSlot({ 0, 5, 255, 0, 0 }) == 255);
    CHECK_FALSE(grace::ActorSlot({ 0, 5, 256, 0, 0 }));
    CHECK(grace::ActorSlot({ 1, 5, 0xAB09, 7, 255 }) == 9);
    CHECK(grace::ActorSlot({ 1, 5, 65535, 7, 0 }) == 255);
    CHECK_FALSE(grace::ActorSlot({ 1, 5, 65536, 0, 0 }));
    CHECK_FALSE(grace::ActorSlot({ 27, 5, 3, 0, 9 }));
    CHECK(grace::Direction({ 0, 5, 9, 0xABCD0002, 0 }, 7) == 2);
    CHECK_FALSE(grace::Direction({ 0, 5, 9, 0xFFFF, 0 }, 0));
    CHECK(grace::Direction({ 1, 5, 0xAB09, 7, 0 }, 2) == 2);
    CHECK_FALSE(grace::Direction({ 1, 5, 9, 2, 0 }, -1));
    CHECK_FALSE(grace::Direction({ 17, 5, 9, 0x10002, 0 }, 2));
    CHECK(grace::Direction(kWords, -1) == 2);
    CHECK_FALSE(grace::UnitAddress(-1, 9));
    CHECK_FALSE(grace::UnitAddress(8, 9));
    CHECK_FALSE(grace::UnitAddress(5, -1));
    CHECK_FALSE(grace::CityAddress(128));
    CHECK(grace::UnitAddress(0, 1) == 0x830F2C44);
    CHECK(grace::CityAddress(1) == 0x8314FEE4);
}

TEST_CASE("capital_grace_calendar_capital_and_coordinates", "[ctest][capital-grace]")
{
    CHECK(grace::Active(true, -3001));
    CHECK_FALSE(grace::Active(true, -3000));
    CHECK_FALSE(grace::Active(true, -2999));
    CHECK_FALSE(grace::Active(false, -4000));
    CHECK((grace::Adjacent({ 39, 10 }, 40, 40, 1, 0) == grace::Point{ 0, 10 }));
    CHECK((grace::Adjacent({ 0, 10 }, 17, 23, -1, 0) == grace::Point{ 16, 10 }));
    CHECK_FALSE(grace::Adjacent({ 3, 0 }, 17, 23, 0, -1));
    CHECK_FALSE(grace::Adjacent({ 3, 22 }, 17, 23, 0, 1));
    CHECK_FALSE(grace::Adjacent({ 17, 1 }, 17, 23, 1, 0));
    CHECK_FALSE(grace::Adjacent({ 1, 1 }, 0, 23, 1, 0));
    CHECK_FALSE(grace::Adjacent({ 1, 1 }, 40, 41, 1, 0));
    grace::City city{ 1, 6, 1, { 11, 10 } };
    CHECK(grace::ProtectedCity(city, 5, grace::Point{ 11, 10 }));
    CHECK_FALSE(grace::ProtectedCity(city, 5, grace::Point{ 12, 10 }));
    CHECK(grace::ProtectedCity(city, 5)); // Capture27 has no coordinate equality clause.
    city.owner = 5;
    CHECK_FALSE(grace::ProtectedCity(city, 5));
    city.owner = 8;
    CHECK_FALSE(grace::ProtectedCity(city, 5));
    city.owner      = 1;
    city.population = 0;
    CHECK_FALSE(grace::ProtectedCity(city, 5));
    city.population = 6;
    city.buildings  = 2;
    CHECK_FALSE(grace::ProtectedCity(city, 5));
    CHECK(grace::SpecialArrival(17, 28, 1, 1, 5));
    CHECK(grace::SpecialArrival(17, 29, 1, 1, 5));
    CHECK_FALSE(grace::SpecialArrival(0, 28, 1, 1, 5));
    CHECK_FALSE(grace::SpecialArrival(17, 28, 0, 1, 5));
    CHECK_FALSE(grace::SpecialArrival(17, 28, 1, 5, 5));
}

TEST_CASE("capital_grace_fresh_retirement_and_busy_separation", "[ctest][capital-grace]")
{
    const grace::Actor actor{ 5, 3, 0x200, { 10, 10 }, 2 };
    CHECK(grace::LiveActor(actor, 5, 5));
    CHECK_FALSE(grace::LiveActor(actor, 5, 4));
    CHECK_FALSE(grace::LiveActor({ 255, 3, 0, { 10, 10 }, 2 }, 5, 5));
    CHECK_FALSE(grace::LiveActor({ 5, 3, 0x80000000, { 10, 10 }, 2 }, 5, 5));
    auto plan = grace::PlanRetirement(0x200, 5, 9, 5, 9, 9);
    CHECK(plan.actor);
    CHECK(plan.selection);
    CHECK(plan.cache);
    plan = grace::PlanRetirement(0x200, 5, 9, 4, 9, 9);
    CHECK(plan.actor);
    CHECK_FALSE(plan.selection);
    CHECK(plan.cache);
    plan = grace::PlanRetirement(0x200, 5, 9, 5, 8, 8);
    CHECK_FALSE(plan.selection);
    CHECK_FALSE(plan.cache);
    plan = grace::PlanRetirement(0x201, 5, 9, 5, 9, 9);
    CHECK_FALSE(plan.actor);
    CHECK_FALSE(plan.selection);
    CHECK_FALSE(plan.cache);
}

TEST_CASE("capital_grace_exact_serial_and_record_binding", "[ctest][capital-grace]")
{
    grace::Tokens tokens;
    REQUIRE(tokens.PushProducer(kProducer, grace::Callsite::Blitz, kWords));
    tokens.BeginSubmit(kSubmit, kQueue);
    CHECK_FALSE(tokens.Append(kSubmit, kProducer - 172, kWords, 100, 100));
    auto different = kWords;
    different[4] ^= 1;
    CHECK_FALSE(tokens.Append(kSubmit, kProducer - 176, different, 100, 100));
    REQUIRE(tokens.Append(kSubmit, kProducer - 176, kWords, 100, 100));
    tokens.DrainSerial(kSubmit, 100, 101);
    CHECK_FALSE(tokens.DrainRecord(kSubmit, kSubmit + 80, kWords)); // Copy not complete.
    tokens.CompleteAppend(kSubmit);
    tokens.DrainSerial(kSubmit, 68, 101);
    CHECK_FALSE(tokens.DrainRecord(kSubmit, kSubmit + 80, kWords)); // Same ring slot, invalid occupancy.
    tokens.DrainSerial(kSubmit, 99, 101);
    CHECK_FALSE(tokens.DrainRecord(kSubmit, kSubmit + 80, kWords)); // Older identical command.
    tokens.DrainSerial(kSubmit, 100, 101);
    CHECK_FALSE(tokens.DrainRecord(kSubmit, kSubmit + 84, kWords));
    REQUIRE(tokens.DrainRecord(kSubmit, kSubmit + 80, kWords));
    CHECK_FALSE(tokens.Refuse(kSubmit + 84, kWords));
    CHECK_FALSE(tokens.Refuse(kSubmit + 80, different));
    REQUIRE(tokens.Refuse(kSubmit + 80, kWords));
    CHECK_FALSE(tokens.Refuse(kSubmit + 80, kWords)); // One shot.
    tokens.EndSubmit(kSubmit);
    CHECK(tokens.PopProducer(kProducer, grace::Callsite::Blitz).refused);
    CHECK_FALSE(tokens.PopProducer(kProducer, grace::Callsite::Blitz).refused);
}

TEST_CASE("capital_grace_nested_drain_can_claim_outer_append", "[ctest][capital-grace]")
{
    grace::Tokens tokens;
    REQUIRE(tokens.PushProducer(kProducer, grace::Callsite::Blitz, kWords));
    append(tokens, 0x40000010);
    constexpr uint32_t nestedFrame = kSubmit - 0x500;
    REQUIRE(tokens.PushProducer(kProducer - 0x600, grace::Callsite::Ordinary, kWords));
    tokens.BeginSubmit(nestedFrame, kQueue);
    tokens.DrainSerial(nestedFrame, 0x40000010, 0x40000011);
    REQUIRE(tokens.DrainRecord(nestedFrame, nestedFrame + 80, kWords));
    REQUIRE(tokens.Refuse(nestedFrame + 80, kWords));
    tokens.EndSubmit(nestedFrame);
    CHECK_FALSE(tokens.PopProducer(kProducer - 0x600, grace::Callsite::Ordinary).refused);
    tokens.EndSubmit(kSubmit);
    CHECK(tokens.PopProducer(kProducer, grace::Callsite::Blitz).refused);
    CHECK(tokens.Imbalances() == 0);
}

TEST_CASE("capital_grace_delayed_delivery_never_revives_escape", "[ctest][capital-grace]")
{
    grace::Tokens tokens;
    REQUIRE(tokens.PushProducer(kProducer, grace::Callsite::Blitz, kWords));
    append(tokens);
    tokens.EndSubmit(kSubmit);
    CHECK_FALSE(tokens.PopProducer(kProducer, grace::Callsite::Blitz).refused);
    REQUIRE(tokens.PushProducer(kProducer, grace::Callsite::Blitz, kWords));
    tokens.BeginSubmit(kSubmit, kQueue);
    tokens.DrainSerial(kSubmit, 100, 101);
    CHECK_FALSE(tokens.DrainRecord(kSubmit, kSubmit + 80, kWords));
    CHECK_FALSE(tokens.Refuse(kSubmit + 80, kWords));
    tokens.EndSubmit(kSubmit);
    CHECK_FALSE(tokens.PopProducer(kProducer, grace::Callsite::Blitz).refused);
    REQUIRE(tokens.PushProducer(kProducer, grace::Callsite::Blitz, kWords));
    append(tokens, 132);
    tokens.DrainSerial(kSubmit, 100, 133);
    CHECK_FALSE(tokens.DrainRecord(kSubmit, kSubmit + 80, kWords));
    tokens.EndSubmit(kSubmit);
}

TEST_CASE("capital_grace_queue_identity_and_stale_stack_repair", "[ctest][capital-grace]")
{
    grace::Tokens tokens;
    REQUIRE(tokens.PushProducer(kProducer, grace::Callsite::Ordinary, kWords));
    append(tokens);
    tokens.BeginSubmit(kSubmit - 0x500, kQueue + 4);
    tokens.DrainSerial(kSubmit - 0x500, 100, 101);
    CHECK_FALSE(tokens.DrainRecord(kSubmit - 0x500, kSubmit - 0x500 + 80, kWords));
    tokens.EndSubmit(kSubmit - 0x500);
    refuse(tokens);
    REQUIRE(tokens.PushProducer(kProducer - 0x600, grace::Callsite::Blitz, kWords));
    tokens.EndSubmit(kSubmit);
    CHECK(tokens.PopProducer(kProducer, grace::Callsite::Ordinary).refused);
    CHECK(tokens.Imbalances() == 1);
    CHECK_FALSE(tokens.PopProducer(kProducer - 0x600, grace::Callsite::Blitz).refused);
    for (size_t i = 0; i < grace::Tokens::kCapacity; ++i)
    {
        REQUIRE(tokens.PushProducer(kProducer - static_cast<uint32_t>(0x400 * i), grace::Callsite::Blitz, kWords));
    }
    CHECK_FALSE(tokens.PushProducer(0x40001000, grace::Callsite::Blitz, kWords));
    CHECK(tokens.Imbalances() == 2);
}

TEST_CASE("capital_grace_linked_escape_requires_owning_cleanup", "[ctest][capital-grace]")
{
    grace::Tokens tokens;
    tokens.PrepareParent(kProducer, 5, 8);
    REQUIRE(tokens.PushProducer(kProducer, grace::Callsite::Linked, kWords));
    append(tokens);
    refuse(tokens);
    tokens.EndSubmit(kSubmit);
    const auto submission = tokens.PopProducer(kProducer, grace::Callsite::Linked);
    CHECK(submission.refused);
    CHECK(submission.parentPlayer == 5);
    CHECK(submission.parentSlot == 8);
    CHECK(submission.childSlot == 9);
    CHECK_FALSE(tokens.ArmCleanup(kProducer + 4, submission));
    REQUIRE(tokens.ArmCleanup(kProducer, submission));
    CHECK_FALSE(tokens.ArmCleanup(kProducer, submission));
    CHECK_FALSE(tokens.FinishCleanup(kProducer - 0x400));
    // An untracked excess frame cannot discard the owning cleanup.
    CHECK(tokens.FinishCleanup(kProducer));
    tokens.PrepareParent(kProducer, 5, 8);
    REQUIRE(tokens.ArmCleanup(kProducer, submission));
    CHECK(tokens.FinishCleanup(kProducer));
    CHECK_FALSE(tokens.FinishCleanup(kProducer));
    tokens.PrepareParent(kProducer, 5, 8);
    CHECK_FALSE(tokens.ArmCleanup(kProducer, { true, 5, 8, 8 }));
    CHECK_FALSE(tokens.FinishCleanup(kProducer));
}

TEST_CASE("capital_grace_stale_submit_cannot_keep_a_drain", "[ctest][capital-grace]")
{
    grace::Tokens tokens;
    REQUIRE(tokens.PushProducer(kProducer, grace::Callsite::Blitz, kWords));
    append(tokens);
    constexpr uint32_t nested = kSubmit - 0x500;
    tokens.BeginSubmit(nested, kQueue);
    tokens.DrainSerial(nested, 100, 101);
    REQUIRE(tokens.DrainRecord(nested, nested + 80, kWords));
    tokens.BeginSubmit(kSubmit, kQueue); // Repair drops both replaced/stale submits.
    CHECK_FALSE(tokens.Refuse(nested + 80, kWords));
    CHECK_FALSE(tokens.PopProducer(kProducer, grace::Callsite::Blitz).refused);
    CHECK(tokens.Imbalances() == 1);
}

TEST_CASE("capital_grace_capacity_failure_preserves_outer_Blitz_escape", "[ctest][capital-grace]")
{
    grace::Tokens tokens;
    REQUIRE(tokens.PushProducer(kProducer, grace::Callsite::Blitz, kWords));
    append(tokens);
    SECTION("producer capacity")
    {
        for (uint32_t i = 1; i < grace::Tokens::kCapacity; ++i)
        {
            REQUIRE(tokens.PushProducer(kProducer - 0x400 * i, grace::Callsite::Ordinary, kWords));
        }
        const auto excess = kProducer - 0x400 * static_cast<uint32_t>(grace::Tokens::kCapacity);
        CHECK_FALSE(tokens.PushProducer(excess, grace::Callsite::Ordinary, kWords));
        CHECK_FALSE(tokens.PopProducer(excess, grace::Callsite::Ordinary).refused);
        for (uint32_t i = static_cast<uint32_t>(grace::Tokens::kCapacity) - 1; i > 0; --i)
        {
            CHECK_FALSE(tokens.PopProducer(kProducer - 0x400 * i, grace::Callsite::Ordinary).refused);
        }
    }
    SECTION("submit capacity")
    {
        for (uint32_t i = 1; i < grace::Tokens::kCapacity; ++i)
        {
            tokens.BeginSubmit(kSubmit - 0x400 * i, kQueue);
        }
        const auto excess = kSubmit - 0x400 * static_cast<uint32_t>(grace::Tokens::kCapacity);
        tokens.BeginSubmit(excess, kQueue);
        tokens.DrainSerial(excess, 100, 101);
        CHECK_FALSE(tokens.DrainRecord(excess, excess + 80, kWords));
        CHECK_FALSE(tokens.Refuse(excess + 80, kWords));
        tokens.EndSubmit(excess);
        for (uint32_t i = static_cast<uint32_t>(grace::Tokens::kCapacity) - 1; i > 0; --i)
        {
            tokens.EndSubmit(kSubmit - 0x400 * i);
        }
    }
    SECTION("parent capacity")
    {
        for (uint32_t i = 0; i < grace::Tokens::kCapacity; ++i)
        {
            tokens.PrepareParent(kProducer - 0x400 * i, 5, 8);
        }
        const auto excess = kProducer - 0x400 * static_cast<uint32_t>(grace::Tokens::kCapacity);
        tokens.PrepareParent(excess, 5, 8);
        CHECK_FALSE(tokens.FinishCleanup(excess));
        REQUIRE(tokens.ArmCleanup(kProducer, { true, 5, 8, 9 }));
        CHECK(tokens.FinishCleanup(kProducer));
    }
    CHECK(tokens.CapacityFailures() == 1);
    refuse(tokens);
    tokens.EndSubmit(kSubmit);
    CHECK(tokens.PopProducer(kProducer, grace::Callsite::Blitz).refused);
    CHECK_FALSE(tokens.PopProducer(kProducer, grace::Callsite::Blitz).refused);
}
