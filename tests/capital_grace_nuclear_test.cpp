#include "capital_grace_nuclear.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace grace = rerevved::capital_grace;

struct World
{
    std::map<uint32_t, std::vector<uint8_t>> regions{
        { 0x82F70000, std::vector<uint8_t>(0x10000) },
        { 0x830E0000, std::vector<uint8_t>(0x80000) }
    };
    grace::SpecialMemory memory{ this, Read };
    grace::SpecialInput  input{ grace::SpecialRoute::Nuclear, { 17, 5, 9, 410, 0 }, { 5, 3, 0, { 10, 10 }, 0 }, -3001, 5 };
    uint32_t             bankMinusOneReads = 0;

    static bool Read(void* context, uint32_t address, void* output, uint32_t size) noexcept
    {
        auto& world = *static_cast<World*>(context);
        if (address == grace::kUnits - 21504)
        {
            ++world.bankMinusOneReads;
        }
        for (const auto& [base, data] : world.regions)
        {
            if (address >= base && uint64_t{ address } + size <= uint64_t{ base } + data.size())
            {
                std::memcpy(output, data.data() + address - base, size);
                return true;
            }
        }
        return false;
    }

    void Byte(uint32_t address, uint8_t value)
    {
        for (auto& [base, data] : regions)
        {
            if (address >= base && address - base < data.size())
            {
                data[address - base] = value;
                return;
            }
        }
        regions[address] = { value };
    }

    void Half(uint32_t address, uint16_t value)
    {
        Byte(address, static_cast<uint8_t>(value >> 8));
        Byte(address + 1, static_cast<uint8_t>(value));
    }

    void Word(uint32_t address, uint32_t value)
    {
        // Keep standalone instruction words in one readable range.
        if (address < 0x830E0000 && (address < 0x82F70000 || address >= 0x82F80000))
        {
            regions[address] = std::vector<uint8_t>(4);
        }
        Half(address, static_cast<uint16_t>(value >> 16));
        Half(address + 2, static_cast<uint16_t>(value));
    }

    World()
    {
        Word(0x82F77364, 40);
        Word(0x82F77448, 40);
        constexpr int32_t dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        constexpr int32_t dy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
        for (uint32_t i = 0; i < 8; ++i)
        {
            Word(0x82F7A0B8 + 4 * i, dx[i]);
            Word(0x82F7A168 + 4 * i, dy[i]);
        }
        for (int32_t i = -1; i <= 1600; ++i)
        {
            Byte(0x830E6850 + i, 255);
            Byte(0x83128D08 + i, 255);
        }
        for (int32_t bank = -1; bank <= 7; ++bank)
        {
            for (int32_t slot = 0; slot < 256; ++slot)
            {
                Byte(grace::kUnits + 84 * (256 * bank + slot), 255);
            }
        }
        for (const auto [address, value] : std::array<std::pair<uint32_t, uint32_t>, 15>{ { { 0x82CF7498, 0x3A06A608 }, { 0x82CF7938, 0x480034B9 }, { 0x82CF76A4, 0x7D3F51AE }, { 0x82CF77F0, 0x81410114 }, { 0x82CF7BF0, 0x7CFF59AE }, { 0x82CF7D08, 0x38A00000 }, { 0x82CF78CC, 0x38210100 }, { 0x82CF78D0, 0x4BB003D0 }, { 0x82CF7D34, 0x3D40830F }, { 0x82CF7D68, 0x38210100 }, { 0x82CF7B7C, 0x38210100 }, { 0x82CF75D0, 0x38210100 }, { 0x82CDF1C8, 0x2F14FFFF }, { 0x82CDF26C, 0x81790000 }, { 0x82CDF1C4, 0x480231CD } } })
        {
            Word(address, value);
        }
    }

    void Capital(int32_t index, grace::Point position = { 0, 39 })
    {
        Half(0x8312A608 + 2 * index, 1);
        Byte(0x83128D08 + index, 3);
        const auto city = grace::kCities + 188 * 3;
        Byte(city, 1);
        Byte(city + 3, 6);
        Word(city + 16, 1);
        Half(city + 48, static_cast<uint16_t>(position.x));
        Half(city + 50, static_cast<uint16_t>(position.y));
    }
};

} // namespace

TEST_CASE("capital_grace_nuclear_uses_native_polar_population_addresses", "[ctest][capital-grace]")
{
    for (const auto [packed, index] : std::array<std::pair<int32_t, int32_t>, 3>{ { { 0, -1 }, { 1, 39 }, { 1599, 1600 } } })
    {
        World world;
        world.input.words[3] = packed;
        world.Capital(index);
        CHECK_FALSE(grace::NuclearAdmission(world.memory, world.input));
        world.Half(0x8312A608 + 2 * index, 0);
        CHECK(grace::NuclearAdmission(world.memory, world.input));
    }
    CHECK(grace::DecodeNuclearVisit(1, 40, 40, 0, -1)->index == 39);
    CHECK(grace::DecodeNuclearVisit(0, 1, 1, -1, -1)->index == -1);
    CHECK_FALSE(grace::DecodeNuclearVisit(-1, 40, 40, 0, 0));
}

TEST_CASE("capital_grace_nuclear_checks_collateral_and_fresh_destination", "[ctest][capital-grace]")
{
    World world;
    REQUIRE(grace::NuclearAdmission(world.memory, world.input));
    // The center remains field. A neighboring capital appears after admission.
    world.Capital(450, { 11, 10 });
    CHECK(grace::NuclearPopulation(world.memory, 188 * 3, grace::kCities + 3, { 11, 10 }, 5));
    CHECK_FALSE(grace::NuclearAdmission(world.memory, world.input));
    world.Half(0x8312A608 + 2 * 450, 0);
    world.Byte(0x83128D08 + 450, 255);
    // At the selected store the actual city record still governs, even after
    // its plane binding or flag changed. No source record is read by this guard.
    CHECK(grace::NuclearPopulation(world.memory, 188 * 3, grace::kCities + 3, { 11, 10 }, 5));
    world.Word(grace::kCities + 188 * 3 + 16, 0);
    CHECK_FALSE(grace::NuclearPopulation(world.memory, 188 * 3, grace::kCities + 3, { 11, 10 }, 5));
    CHECK_THROWS(grace::NuclearPopulation(world.memory, 189, grace::kCities + 3, { 11, 10 }, 5));
}

TEST_CASE("capital_grace_nuclear_unknown_native_seam_consumes_at_admission", "[ctest][capital-grace]")
{
    World world;
    REQUIRE(grace::AdmitSpecial(world.memory, world.input) == grace::SpecialDecision::Native);
    world.Word(0x82CF7D34, 0);
    CHECK(grace::AdmitSpecial(world.memory, world.input) == grace::SpecialDecision::RefuseUnsupported);
}

TEST_CASE("capital_grace_nuclear_protected_and_unsupported_are_distinct", "[ctest][capital-grace]")
{
    World world;
    world.Capital(450, { 11, 10 });
    CHECK(grace::AdmitSpecial(world.memory, world.input) == grace::SpecialDecision::RefuseProtected);
    // An unknown image is a capability failure, even with a capital in range.
    world.Word(0x82CF7D34, 0);
    CHECK(grace::AdmitSpecial(world.memory, world.input) == grace::SpecialDecision::RefuseUnsupported);
}

TEST_CASE("capital_grace_nuclear_alternate_scans_negative_owner_bank", "[ctest][capital-grace]")
{
    World world;
    REQUIRE(grace::NuclearAdmission(world.memory, world.input));
    CHECK(world.bankMinusOneReads == 0);
    world.Word(0x8314F1AC, 1);
    REQUIRE(grace::NuclearAdmission(world.memory, world.input));
    CHECK(world.bankMinusOneReads > 0);
    world.Capital(450, { 11, 10 });
    world.Half(0x8312A608 + 2 * 450, 0);
    const auto victim = grace::kUnits - 21504;
    world.Byte(victim, 1);
    world.Half(victim + 28, 11);
    world.Half(victim + 30, 10);
    CHECK_FALSE(grace::NuclearAdmission(world.memory, world.input));
    CHECK(grace::NuclearVictim(world.memory, -1, 0, victim + 12, { 11, 10 }, 5));
    world.Word(0x8314F1AC, 0);
    CHECK(grace::NuclearAdmission(world.memory, world.input));
}

TEST_CASE("capital_grace_nuclear_consumption_token_is_frame_bound_and_one_shot", "[ctest][capital-grace]")
{
    grace::NuclearInvocations tokens;
    auto                      begin = [&]
    {
        tokens.BeginDispatch(0x1000);
        REQUIRE(tokens.Begin({ 0xF00, 0x1000, 0x82CDF1AC, 5, 9, true, false, false }));
        auto* invocation = tokens.Find(0xF00, false);
        REQUIRE(invocation);
        invocation->refused = true;
        tokens.End(0xF00, true);
    };
    begin();
    CHECK_FALSE(tokens.Consume(0x2000, 5, 9));
    CHECK(tokens.Consume(0x1000, 5, 9));
    CHECK_FALSE(tokens.Consume(0x1000, 5, 9));
    begin();
    tokens.BeginDispatch(0x1000);
    CHECK_FALSE(tokens.Consume(0x1000, 5, 9));
    begin();
    CHECK_FALSE(tokens.Consume(0x1000, 5, 8));
    CHECK_FALSE(tokens.Consume(0x1000, 5, 9));
    REQUIRE(tokens.Begin({ 0xF00, 0x1000, 0x82CDEEAC, 5, 9, true, true, true }));
    tokens.End(0xF00, true);
    CHECK_FALSE(tokens.Consume(0x1000, 5, 9));
    CHECK(tokens.Occupied() == 0);
}
