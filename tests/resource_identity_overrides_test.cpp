#include "resource_identity_overrides.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string_view>

namespace
{

namespace overrides = rerevved::resource_identity_overrides;

void testPublicationAndMath()
{
    overrides::Clear();
    const auto         empty     = overrides::Current();
    constexpr uint64_t nativeSum = 0xAABBCCDD0000003Eull;
    UNSCOPED_INFO("empty snapshot preserves the full native register");
    CHECK(overrides::CoordinateSum(*empty, 401, 1, nativeSum) == nativeSum);
    const std::array records = { overrides::Record{ 403, 18 }, overrides::Record{ 401, 0 }, overrides::Record{ 402, 9 } };
    UNSCOPED_INFO("valid unsorted records publish");
    CHECK(overrides::Publish(records));
    const auto snapshot = overrides::Current();
    UNSCOPED_INFO("publication sorts unique physical indices");
    CHECK(*snapshot == overrides::Snapshot{ { 401, 0 }, { 402, 9 }, { 403, 18 } });
    UNSCOPED_INFO("old readers retain their empty snapshot");
    CHECK(empty->empty());
    const std::array<int8_t, 3>   kinds = { 1, 3, 0 };
    const std::array<uint64_t, 3> sums  = { 0, 1, 2 };
    for (size_t i = 0; i < kinds.size(); ++i)
    {
        for (uint64_t sum : { uint64_t{ 0 }, uint64_t{ 21 }, nativeSum })
        {
            UNSCOPED_INFO("each override sets the full synthetic sum");
            CHECK(overrides::CoordinateSum(*snapshot, 401 + static_cast<uint32_t>(i), kinds[i], sum) == sums[i]);
        }
        for (int8_t terrain = -1; terrain <= 7; ++terrain)
        {
            if (terrain != kinds[i])
            {
                UNSCOPED_INFO("terrain mismatch preserves the full native sum");
                CHECK(overrides::CoordinateSum(*snapshot, 401 + static_cast<uint32_t>(i), terrain, nativeSum) == nativeSum);
            }
        }
    }
    for (uint32_t index : { 0u, 400u, 404u, 1271u, UINT32_MAX })
    {
        UNSCOPED_INFO("non-override tiles pass through");
        CHECK(overrides::CoordinateSum(*snapshot, index, 1, nativeSum) == nativeSum);
    }
    for (const overrides::Snapshot& invalid : { overrides::Snapshot{ { 401, 0 }, { 401, 9 } },
                                                overrides::Snapshot{ { 401, 0 }, { 401, 0 } },
                                                overrides::Snapshot{ { 32, 0 } },
                                                overrides::Snapshot{ { 1280, 9 } },
                                                overrides::Snapshot{ { 401, 22 } } })
    {
        UNSCOPED_INFO("invalid or duplicate records leave the current snapshot intact");
        CHECK((!overrides::Publish(invalid) && overrides::Current() == snapshot));
    }
    overrides::Clear();
    UNSCOPED_INFO("clear publishes empty without changing retained readers");
    CHECK((overrides::Current()->empty() && snapshot->size() == 3));
}

void testSerialization()
{
    const overrides::Snapshot snapshot = { { 0, 0 }, { 402, 9 }, { 1271, 18 } };
    const auto                bytes    = overrides::Serialize(snapshot);
    UNSCOPED_INFO("schema and packed big-endian records have no struct padding");
    CHECK(bytes == std::vector<uint8_t>{ 1, 0, 3, 0, 0, 0, 1, 146, 9, 4, 247, 18 });
    const auto decoded = overrides::Deserialize(bytes);
    UNSCOPED_INFO("snapshot records round trip including Oxen zero");
    CHECK((decoded && *decoded == snapshot));
    UNSCOPED_INFO("decoded records can be published by load restoration");
    CHECK((decoded && overrides::Publish(*decoded) && *overrides::Current() == snapshot));
    const auto retained = overrides::Current();
    for (const std::vector<uint8_t>& invalid : { std::vector<uint8_t>{},
                                                 std::vector<uint8_t>{ 2, 0, 0 },
                                                 std::vector<uint8_t>{ 1, 0, 1 },
                                                 std::vector<uint8_t>{ 1, 0, 0, 0 },
                                                 std::vector<uint8_t>{ 1, 0, 1, 0, 32, 0 },
                                                 std::vector<uint8_t>{ 1, 0, 1, 0, 0, 22 },
                                                 std::vector<uint8_t>{ 1, 0, 2, 0, 0, 0, 0, 0, 9 },
                                                 std::vector<uint8_t>{ 1, 0, 2, 0, 1, 9, 0, 0, 0 } })
    {
        UNSCOPED_INFO("invalid schema, length, records or order do not publish");
        CHECK((!overrides::Deserialize(invalid) && overrides::Current() == retained));
    }
    const auto empty = overrides::Deserialize(overrides::Serialize({}));
    UNSCOPED_INFO("empty snapshot round trips");
    CHECK((empty && empty->empty()));
    UNSCOPED_INFO("serialization rejects noncanonical records");
    CHECK(overrides::Serialize({ { 402, 9 }, { 0, 0 } }).empty());
    overrides::Clear();
}

void testAllTerrainRows()
{
    constexpr int identities[7][4] = {
        { 10, 16, 18, -1 }, { 0, 20, 19, 11 }, { 8, 13, 14, -1 }, { 5, 9, 17, -1 }, { 1, 3, 6, -1 }, { 2, 4, 12, -1 }, { 7, 15, 21, -1 }
    };
    constexpr uint64_t nativeSum = 0xAABBCCDD0000003Eull;
    for (int8_t terrain = 0; terrain <= 6; ++terrain)
    {
        for (int position = 0; position < (terrain == 1 ? 4 : 3); ++position)
        {
            const overrides::Snapshot records{ { 410, static_cast<uint8_t>(identities[terrain][position]) } };
            UNSCOPED_INFO("every native ordinal can be recorded");
            CHECK(overrides::Publish(records));
            UNSCOPED_INFO("every ordinal round trips with U8 width");
            CHECK(overrides::Deserialize(overrides::Serialize(records)) == records);
            UNSCOPED_INFO("every row position supplies its synthetic coordinate sum");
            CHECK(overrides::CoordinateSum(*overrides::Current(), 410, terrain, nativeSum) == static_cast<uint64_t>(position));
            for (int8_t other = -1; other <= 7; ++other)
            {
                if (other != terrain)
                {
                    UNSCOPED_INFO("an ordinal outside the tile's row preserves the native full sum");
                    CHECK(overrides::CoordinateSum(records, 410, other, nativeSum) == nativeSum);
                }
            }
        }
    }
    overrides::Clear();
}

} // namespace

TEST_CASE("resource_identity_overrides", "[ctest][resource-identity-overrides]")
{
    testPublicationAndMath();
    testSerialization();
    testAllTerrainRows();
}
