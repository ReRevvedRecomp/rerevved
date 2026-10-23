#include "ai_civilizations.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>

namespace
{

using namespace rerevved::ai_civilizations;

void testRemovableMask()
{
    UNSCOPED_INFO("the default keeps every AI");
    CHECK(RemovableMask(6, 5, 0x01, 4) == 0);
    UNSCOPED_INFO("one human with two AI keeps slots 1 and 2");
    CHECK(RemovableMask(6, 5, 0x01, 2) == 0x18);
    UNSCOPED_INFO("one human with one AI keeps slot 1");
    CHECK(RemovableMask(6, 5, 0x01, 1) == 0x1C);
    UNSCOPED_INFO("one human keeps one AI at zero");
    CHECK(RemovableMask(6, 5, 0x01, 0) == 0x1C);
    UNSCOPED_INFO("a lone human at slot 2 keeps slot 0");
    CHECK(RemovableMask(6, 5, 0x04, 0) == 0x1A);
    UNSCOPED_INFO("two humans with no AI keep only their slots");
    CHECK(RemovableMask(6, 5, 0x05, 0) == 0x1A);
    UNSCOPED_INFO("two humans with one AI keep the lowest AI slot");
    CHECK(RemovableMask(6, 5, 0x05, 1) == 0x18);
    UNSCOPED_INFO("all civilizations human removes none");
    CHECK(RemovableMask(6, 5, 0x1F, 0) == 0);
    UNSCOPED_INFO("the barbarian bit is neither human nor removed");
    CHECK(RemovableMask(6, 5, 0x23, 0) == 0x1C);
    UNSCOPED_INFO("only the barbarian bit leaves the game unchanged");
    CHECK(RemovableMask(6, 5, 0x20, 0) == 0);
    UNSCOPED_INFO("no human leaves the game unchanged");
    CHECK(RemovableMask(6, 5, 0, 0) == 0);
    UNSCOPED_INFO("a negative limit counts as zero");
    CHECK(RemovableMask(6, 5, 0x03, -1) == 0x1C);
    UNSCOPED_INFO("the two-slot layout is unchanged");
    CHECK(RemovableMask(2, -1, 0x01, 0) == 0);
    UNSCOPED_INFO("another barbarian slot is unchanged");
    CHECK(RemovableMask(6, 4, 0x01, 0) == 0);
}

void testUnitRecords()
{
    UNSCOPED_INFO("first record");
    CHECK(UnitRecord(0, 0) == 0x830F2BF0);
    UNSCOPED_INFO("player stride");
    CHECK(UnitRecord(1, 0) == 0x830F2BF0 + 0x5400);
    UNSCOPED_INFO("unit stride");
    CHECK(UnitRecord(2, 3) == 0x830F2BF0 + 2 * 0x5400 + 3 * 0x54);

    UNSCOPED_INFO("owned unit is live");
    CHECK(IsLiveUnit(1, 0));
    UNSCOPED_INFO("other flags keep a unit live");
    CHECK(IsLiveUnit(0, 0x7FFFFFFF));
    UNSCOPED_INFO("free record is not live");
    CHECK(!IsLiveUnit(0xFF, 0));
    UNSCOPED_INFO("killed unit is not live");
    CHECK(!IsLiveUnit(1, 0x80000000));
}

void testDigest()
{
    KillDigest empty;
    UNSCOPED_INFO("empty digest is the FNV-1a basis");
    CHECK((empty.Count() == 0 && empty.Value() == 2166136261u));

    KillDigest first;
    first.Add(1, 0, 0);
    first.Add(2, 0, 0);
    KillDigest second;
    second.Add(2, 0, 0);
    second.Add(1, 0, 0);
    UNSCOPED_INFO("count follows the kills");
    CHECK(first.Count() == 2);
    UNSCOPED_INFO("order changes the digest");
    CHECK(first.Value() != second.Value());

    KillDigest same;
    same.Add(1, 0, 0);
    same.Add(2, 0, 0);
    UNSCOPED_INFO("identical lists match");
    CHECK(first.Value() == same.Value());
}

} // namespace

TEST_CASE("ai_civilizations", "[ctest][ai-civilizations]")
{
    testRemovableMask();
    testUnitRecords();
    testDigest();
}
