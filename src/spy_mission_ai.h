#pragma once

// AI Spy call 0x82CC16B4 passes only the city number, which effect 0x82CF6800
// reads as mission 0, Disrupt defensive units. Select an eligible mission.

#include <array>
#include <cstdint>
#include <string>

namespace rerevved::spy_mission_ai
{

// Native mission IDs. Kidnap and Bribe have uncapped victim-search loops;
// offer them only when a valid victim is guaranteed to be sampled.
constexpr int32_t kDisruptDefenders   = 0;
constexpr int32_t kSabotageProduction = 1;
constexpr int32_t kDemolishBuilding   = 3;
constexpr int32_t kKidnapGreatPerson  = 4;
constexpr int32_t kStealGold          = 5;
constexpr int32_t kBribeGreatGeneral  = 6;

constexpr int32_t kCityCount   = 128;
constexpr int32_t kPlayerCount = 8;
constexpr int32_t kUnitSlots   = 256;

// Building bits of city+0x10 that the demolition effect scans (bits 1..22).
// The native menu also admits bit 23, which the effect never visits.
constexpr uint32_t kDemolishableBuildings = 0x007FFFFE;

// Raw target-city values, each read the way the native menu predicate reads it.
struct CityFacts
{
    int32_t  currentItem = -1; // signed halfword city+0x38; -1 is no production
    uint32_t buildings   = 0;  // word city+0x10
    bool     goldKnown   = false;
    int32_t  population  = 0; // signed byte city+3
    int32_t  reserve     = 0; // owner Gold word 0x830ED544 + 4 * owner
    int32_t  statistic   = 0; // owner city-count word 0x830E5AD0 + 0x80 * owner
    // The menu gate passes and a valid victim occupies a reachable slot.
    // See KidnapVictim, BribeVictim and RngReachesEverySlot.
    bool kidnapReachable = false;
    bool bribeReachable  = false;
};

// Native victim searches sample slots 0..255 of the target owner's bank:
// 0x54-byte records at 0x830F2BF0 + 0x5400 * owner.
constexpr int32_t kBankSlots = 256;

// One unit record, read the way the native loops read it.
struct UnitRecord
{
    uint32_t owner = 0xFF; // byte +0; 0xFF is an empty record
    int32_t  type  = -1;   // signed byte +1
    uint32_t x     = 0;    // raw halfword +0x1C
    uint32_t y     = 0;    // raw halfword +0x1E
    uint32_t flags = 0;    // word +0xC
};

constexpr uint32_t kUnitDead         = 0x80000000;
constexpr uint32_t kKidnapFlag       = 0x4;
constexpr int32_t  kGreatGeneral     = 30;
constexpr int32_t  kFirstGreatPerson = 31;
constexpr int32_t  kLastGreatPerson  = 37;

// Match Kidnap acceptance at 0x82CF6FE4..0x82CF704C using raw city coordinates
// at +0x30/+0x32. Narrow native type >= 31 to 31..37, the only Great Person
// types with definitions and a creator.
constexpr bool KidnapVictim(const UnitRecord& unit, uint32_t cityX, uint32_t cityY) noexcept
{
    return unit.type >= kFirstGreatPerson && unit.type <= kLastGreatPerson && unit.x == cityX && unit.y == cityY &&
           unit.owner != 0xFF && (unit.flags & kUnitDead) == 0 && (unit.flags & kKidnapFlag) != 0;
}

// Bribe acceptance at 0x82CF71FC..0x82CF7234 uses raw city coordinates.
// The AI additionally rejects empty owners and dead records.
constexpr bool BribeVictim(const UnitRecord& unit, uint32_t cityX, uint32_t cityY) noexcept
{
    return unit.type == kGreatGeneral && unit.x == cityX && unit.y == cityY && unit.owner != 0xFF &&
           (unit.flags & kUnitDead) == 0;
}

// Inputs of the kidnap menu's Scotland Yard / protection predicate
// (0x82CF3FB0..0x82CF40D8), with the indexed tables read for players 0..7.
struct KidnapGate
{
    uint32_t                           cityGreatPersons = 0;     // halfword city+0x3C
    int32_t                            owner            = -1;    // O, signed byte city+0
    int32_t                            claimOwner       = -1;    // W, first word of 0x830E5148 + 0xB8
    bool                               network          = false; // byte 0x8314F10B != 0
    int32_t                            mode             = 0;     // T, halfword 0x830E8FE0
    int32_t                            item             = -1;    // I, halfword 0x82F75054
    int32_t                            count            = 0;     // K, word 0x82F700AC
    std::array<uint32_t, kPlayerCount> relation{};               // R, words from 0x830EDC48
    std::array<int32_t, kPlayerCount>  group{};                  // G, halfwords from 0x830E9070
};

// Native menu entry for mission 4. Conditions with table indices outside 0..7
// or a count outside 1..8 are rejected, so bounds checks only limit eligibility.
bool KidnapMenuGate(const KidnapGate& gate) noexcept;

// Native menu entry for mission 6: halfword 0x8314F1C8 and the presence-plane
// halfword at 0x8312A608 + 2 * (40 * x + y) are both nonzero, with x and y the
// city's signed coordinates in 0..39.
bool BribeMenuGate(uint32_t general, int32_t x, int32_t y, uint32_t presence) noexcept;

// RNG 0x82F7AD00: s = ((s * 214013 + 2531011) mod 2^32) mod M;
// output = trunc(256 * N * s). M = 0xFFFFCFEF, N = 1/M (0x3DF0000301109066).
// All 7 cycles cover slots 0..255, guaranteeing a fixed victim is reached.
// Other modulus or normalization values have no termination guarantee.
constexpr uint32_t kRngModulus       = 0xFFFFCFEF;
constexpr uint64_t kRngNormalization = 0x3DF0000301109066;

constexpr bool RngReachesEverySlot(uint32_t modulus, uint64_t normalizationBits) noexcept
{
    return modulus == kRngModulus && normalizationBits == kRngNormalization;
}

constexpr uint32_t MissionBit(int32_t mission) noexcept
{
    return uint32_t{ 1 } << mission;
}

// Native Steal Gold amount: trunc(S32(pop * reserve) / S32(statistic + 2)) with
// wrapping 32-bit operands. False where the native divide traps (divisor 0, or
// INT32_MIN / -1).
bool StealGoldAmount(int32_t population, int32_t reserve, int32_t statistic, int32_t& amount) noexcept;

// Eligible MissionBit flags, always including mission 0.
uint32_t EligibleMissions(const CityFacts& city) noexcept;

// The same guest values always produce the same hash.
uint32_t ChoiceHash(uint32_t turn, uint32_t actor, uint32_t city, uint32_t unit) noexcept;

// Use the hash to select a set bit of `eligible`, in ascending mission order.
int32_t ChooseMission(uint32_t eligible, uint32_t hash) noexcept;

// Effect argument: mission in bits 8 and up, target city in the low byte.
constexpr int64_t PackSelector(int32_t mission, int32_t city) noexcept
{
    return (int64_t{ mission } << 8) | city;
}

// "0,1,3,5" for the log line.
std::string DescribeMissions(uint32_t eligible);

} // namespace rerevved::spy_mission_ai
