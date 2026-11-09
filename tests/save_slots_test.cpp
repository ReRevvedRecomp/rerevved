#include "save_slots.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace slots = rerevved::save_slots;

TEST_CASE("save_slots_clamp", "[ctest][save-slots]")
{
    CHECK(slots::ClampSlots(-5) == 10);
    CHECK(slots::ClampSlots(0) == 10);
    CHECK(slots::ClampSlots(9) == 10);
    CHECK(slots::ClampSlots(10) == 10);
    CHECK(slots::ClampSlots(30) == 30);
    CHECK(slots::ClampSlots(99) == 99);
    CHECK(slots::ClampSlots(100) == 99);
    CHECK(slots::ClampSlots(INT64_MAX) == 99);
    CHECK(slots::kDefaultSlots == slots::ClampSlots(slots::kDefaultSlots));
}

TEST_CASE("save_slots_sizes", "[ctest][save-slots]")
{
    CHECK(slots::kNativeObjectBytes == 4548);
    CHECK(slots::EnumeratorEntries(30) == 94);
    CHECK(slots::EnumeratorEntries(99) == 163);
    CHECK(slots::EnumeratorEntries(3) == 74);
    CHECK(slots::ObjectBytes(30) == 1160 + 308 * 94);
    CHECK(slots::ObjectBytes(10) > slots::kNativeObjectBytes);
}

TEST_CASE("save_slots_used_count", "[ctest][save-slots]")
{
    CHECK(slots::UsedSlots({}) == 0);
    const std::array<uint32_t, 5> lengths{ 5, 0, 7, 0, 1 };
    CHECK(slots::UsedSlots(lengths) == 3);
}

TEST_CASE("save_slots_fullness_native_parity", "[ctest][save-slots]")
{
    for (uint32_t used = 0; used <= 10; ++used)
    {
        CHECK(slots::FullnessCount(used, 10) == used);
    }
}

TEST_CASE("save_slots_fullness_extended", "[ctest][save-slots]")
{
    CHECK(slots::FullnessCount(0, 30) == 0);
    CHECK(slots::FullnessCount(9, 30) == 9);
    CHECK(slots::FullnessCount(10, 30) == 9);
    CHECK(slots::FullnessCount(29, 30) == 9);
    CHECK(slots::FullnessCount(30, 30) == 10);
    CHECK(slots::FullnessCount(99, 99) == 10);
    CHECK(slots::FullnessCount(98, 99) == 9);
    CHECK(slots::FullnessCount(10, 3) == 10);
}

TEST_CASE("save_slots_rows_native_parity", "[ctest][save-slots]")
{
    for (uint32_t used = 0; used <= 10; ++used)
    {
        CHECK(slots::RowCount(used, 10, false) == used);
        CHECK(slots::RowCount(used, 10, true) == (used >= 10 ? used : used + 1));
    }
}

TEST_CASE("save_slots_rows_extended", "[ctest][save-slots]")
{
    CHECK(slots::RowCount(0, 30, false) == 0);
    CHECK(slots::RowCount(0, 30, true) == 1);
    CHECK(slots::RowCount(10, 30, true) == 11);
    CHECK(slots::RowCount(29, 30, true) == 30);
    CHECK(slots::RowCount(30, 30, true) == 30);
    CHECK(slots::RowCount(30, 30, false) == 30);
}

TEST_CASE("save_slots_war_autosave_is_not_a_manual_slot", "[ctest][save-slots]")
{
    CHECK(slots::KeepsManualSlot(11, "save11", 30));
    CHECK_FALSE(slots::KeepsManualSlot(11, "war", 30));
    CHECK(slots::KeepsManualSlot(12, "save12", 30));
    CHECK(slots::KeepsManualSlot(29, "save29", 30));
    CHECK_FALSE(slots::KeepsManualSlot(30, "save30", 30));
    CHECK_FALSE(slots::KeepsManualSlot(100, "save100", 30));
}
