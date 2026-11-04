#include "resync_record.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <unordered_map>

namespace
{

namespace record = rerevved::resync_record;

// Unwritten bytes read as zero; the optional blocked byte fails a read, as an unmapped page would.
class SyntheticMemory final : public record::GuestReader
{
public:
    void Put8(uint32_t address, uint32_t value)
    {
        bytes_[address] = static_cast<uint8_t>(value);
    }

    void Put16(uint32_t address, uint32_t value)
    {
        Put8(address, value >> 8);
        Put8(address + 1, value);
    }

    void Put32(uint32_t address, uint32_t value)
    {
        Put16(address, value >> 16);
        Put16(address + 2, value);
    }

    void Block(uint32_t address)
    {
        blocked_ = address;
    }

    bool Read(uint32_t address, uint32_t size, uint8_t* out) const override
    {
        if (blocked_ && *blocked_ >= address && *blocked_ - address < size)
        {
            return false;
        }
        for (uint32_t i = 0; i < size; ++i)
        {
            const auto found = bytes_.find(address + i);
            out[i]           = found == bytes_.end() ? 0 : found->second;
        }
        return true;
    }

private:
    std::unordered_map<uint32_t, uint8_t> bytes_;
    std::optional<uint32_t>               blocked_;
};

constexpr uint32_t unit(uint32_t player, uint32_t index)
{
    return 0x830F2BF0 + player * 0x5400 + index * 0x54;
}

constexpr uint32_t city(uint32_t index)
{
    return 0x8314FE28 + index * 0xBC;
}

void emptyWorld(SyntheticMemory& memory, uint32_t players)
{
    memory.Put32(0x82F700AC, players);
    for (uint32_t player = 0; player < players; ++player)
    {
        for (uint32_t index = 0; index < 256; ++index)
        {
            memory.Put8(unit(player, index), 0xFF);
        }
        memory.Put32(0x830EDD48 + 4 * player, 0xFFFFFFFF);
    }
    for (uint32_t index = 0; index < 128; ++index)
    {
        memory.Put8(city(index), 0xFF);
    }
}

} // namespace

TEST_CASE("resync_ledger_matches_hand_computed_scalar", "[ctest][resync-record]")
{
    SyntheticMemory memory;
    emptyWorld(memory, 2);

    memory.Put8(unit(0, 3), 0);
    memory.Put8(unit(0, 3) + 1, 0xFE);
    memory.Put8(unit(0, 3) + 5, 0xFF);
    memory.Put16(unit(0, 3) + 0x1C, 7);
    memory.Put16(unit(0, 3) + 0x1E, 0xFFFD);
    memory.Put16(unit(0, 3) + 0x52, 10);
    memory.Put8(unit(0, 7), 1);
    memory.Put32(unit(0, 7) + 0xC, 0x80000000);
    memory.Put8(unit(0, 7) + 1, 9);
    memory.Put8(unit(0, 7) + 5, 9);
    memory.Put16(unit(0, 7) + 0x1C, 100);
    memory.Put16(unit(0, 7) + 0x1E, 100);
    memory.Put16(unit(0, 7) + 0x52, 100);
    memory.Put8(unit(0, 255), 0);
    memory.Put8(unit(0, 255) + 1, 1);
    memory.Put16(unit(0, 255) + 0x1C, 0x8000);
    memory.Put8(unit(1, 0), 1);
    memory.Put8(unit(1, 0) + 1, 5);
    memory.Put16(unit(1, 0) + 0x1E, 0xFFFF);
    memory.Put8(unit(1, 1) + 1, 9);
    memory.Put16(unit(1, 1) + 0x1C, 100);

    memory.Put32(0x830ED544, 100);
    memory.Put32(0x830ED4E4, 20);
    memory.Put32(0x830ED4C4, 3);
    memory.Put32(0x830EDD08, 7);
    memory.Put32(0x830E5250, 1000);
    memory.Put32(0x830EDD48, 2);
    memory.Put32(0x830EDD68 + 2 * 4, 50);
    memory.Put32(0x830ED544 + 4, 0xFFFFFFF0);
    memory.Put32(0x830ED4E4 + 4, 0x20);
    memory.Put32(0x830E5250 + 0x80, 0x100);
    memory.Put32(0x830EDD68 + 47 * 4, 0x77);

    memory.Put8(city(0), 1);
    memory.Put8(city(0) + 3, 5);
    memory.Put32(city(0) + 0x10, 0x100);
    memory.Put16(city(0) + 0x34, 0xFFFF);
    memory.Put16(city(0) + 0x36, 2);
    memory.Put16(city(0) + 0x38, 4);
    memory.Put16(city(0) + 0x3C, 0xFFFE);
    memory.Put8(city(1), 1);
    memory.Put32(city(1) + 0x10, 999);
    memory.Put16(city(1) + 0x38, 50);
    memory.Put8(city(2) + 3, 5);
    memory.Put32(city(2) + 0x10, 999);
    memory.Put8(city(3), 0xFE);
    memory.Put8(city(3) + 3, 1);
    memory.Put8(city(127), 0);
    memory.Put8(city(127) + 3, 127);
    memory.Put32(city(127) + 0x10, 0xFFFFFFFF);

    memory.Put32(0x830E5148 + 4, 0x20);
    memory.Put32(0x830E5148 + 8, 0x1000);
    memory.Put32(0x830E5148 + 8 * 5 + 4, 3);
    memory.Put32(0x830E5148 + 8 * 32 + 4, 0xFFFFFFF0);

    const auto ledger = record::ComputeLedger(memory);
    REQUIRE(ledger.has_value());
    CHECK(ledger->playerCount == 2);
    CHECK(ledger->players[0].units == 0xFFFF8103);
    CHECK(ledger->players[0].reserve == 100);
    CHECK(ledger->players[0].cultureStrength == 20);
    CHECK(ledger->players[0].derivedCulture == 3);
    CHECK(ledger->players[0].tally == 7);
    CHECK(ledger->players[0].strategy == 1000);
    CHECK(ledger->players[0].progress == 50);
    CHECK(ledger->players[1].units == 0xFFFFFFFE);
    CHECK(ledger->players[1].reserve == 0xFFFFFFF0);
    CHECK(ledger->players[1].cultureStrength == 0x20);
    CHECK(ledger->players[1].strategy == 0x100);
    CHECK(ledger->players[1].progress == 0);
    CHECK(ledger->cities == 0x18A);
    CHECK(ledger->players[0].cities == 0x7E);
    CHECK(ledger->players[1].cities == 0x10D);
    constexpr uint32_t outOfRange = 0xFFFFFFFF;
    CHECK(static_cast<uint32_t>(ledger->players[0].cities + ledger->players[1].cities + outOfRange) == ledger->cities);
    CHECK(ledger->wonders == 0x13);
    CHECK(ledger->sum == 0xFFFF884A);

    REQUIRE(ledger->cityCount == 3);
    const auto& first = ledger->cityFields[0];
    CHECK(first.index == 0);
    CHECK(first.owner == 1);
    CHECK(first.population == 5);
    CHECK(first.word10 == 0x100);
    CHECK(first.half34 == -1);
    CHECK(first.half36 == 2);
    CHECK(first.half38 == 4);
    CHECK(first.half3C == -2);
    CHECK(ledger->cityFields[1].index == 3);
    CHECK(ledger->cityFields[1].owner == -2);
    CHECK(ledger->cityFields[2].index == 127);
    CHECK(ledger->cityFields[2].word10 == 0xFFFFFFFF);
}

TEST_CASE("resync_ledger_player_count_bounds", "[ctest][resync-record]")
{
    SyntheticMemory memory;
    emptyWorld(memory, 0);
    memory.Put8(city(0), 0);
    memory.Put8(city(0) + 3, 2);
    memory.Put32(0x830E5148 + 4, 5);

    for (const uint32_t count : { 0u, 0xFFFFFFFFu })
    {
        memory.Put32(0x82F700AC, count);
        const auto ledger = record::ComputeLedger(memory);
        REQUIRE(ledger.has_value());
        CHECK(ledger->playerCount == 0);
        CHECK(ledger->sum == 7);
    }

    memory.Put32(0x82F700AC, record::kMaxPlayers + 1);
    CHECK(!record::ComputeLedger(memory).has_value());

    emptyWorld(memory, 1);
    memory.Block(unit(0, 100));
    CHECK(!record::ComputeLedger(memory).has_value());
}

TEST_CASE("resync_ledger_dedupe_is_bounded", "[ctest][resync-record]")
{
    record::LedgerDedupe dedupe;
    static_assert(record::LedgerDedupe::kCapacity == 32);
    CHECK(dedupe.Admit(10, 0x82CDDF38, 0x1111));
    CHECK(!dedupe.Admit(10, 0x82CDDF38, 0x1111));
    CHECK(dedupe.Admit(10, 0x82CDDF38, 0x2222));
    CHECK(!dedupe.Admit(10, 0x82CDDF38, 0x2222));
    CHECK(dedupe.Admit(10, 0x82CDEC6C, 0x1111));

    for (uint32_t caller = 3; caller < record::LedgerDedupe::kCapacity; ++caller)
    {
        CHECK(dedupe.Admit(10, caller, 0));
    }
    CHECK(!dedupe.Admit(10, 0x1000, 0));
    CHECK(!dedupe.Admit(10, 0x82CDDF38, 0x3333));

    CHECK(dedupe.Admit(11, 0x82CDDF38, 0x1111));
}

TEST_CASE("resync_overrides_digest_pins", "[ctest][resync-record]")
{
    CHECK(record::OverridesDigest({}) == 0x0BCA446C);
    CHECK(record::OverridesDigest({ { 41, 3 } }) == 0xBFE8389D);
}
