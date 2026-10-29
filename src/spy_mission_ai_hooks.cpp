#include "spy_mission_ai.h"

#include <cstdint>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace
{

constexpr uint32_t kTurn            = 0x8312B8DC;
constexpr uint32_t kCities          = 0x8314FE28;
constexpr uint32_t kCityStride      = 0xBC;
constexpr uint32_t kGold            = 0x830ED544;
constexpr uint32_t kStatistics      = 0x830E5AD0;
constexpr uint32_t kStatisticStride = 0x80;

// Kidnap and Bribe inputs.
constexpr uint32_t kUnitBanks        = 0x830F2BF0;
constexpr uint32_t kBankStride       = 0x5400;
constexpr uint32_t kUnitStride       = 0x54;
constexpr uint32_t kRng              = 0x82F7AD00;
constexpr uint32_t kNetwork          = 0x8314F10B;
constexpr uint32_t kClaims           = 0x830E5148 + 0xB8;
constexpr uint32_t kProtectionMode   = 0x830E8FE0;
constexpr uint32_t kProtectionItem   = 0x82F75054;
constexpr uint32_t kProtectionCount  = 0x82F700AC;
constexpr uint32_t kRelations        = 0x830EDC48;
constexpr uint32_t kGroups           = 0x830E9070;
constexpr uint32_t kGeneralPresence  = 0x8314F1C8;
constexpr uint32_t kPresencePlane    = 0x8312A608;
constexpr uint32_t kCityGreatPersons = 0x3C;
constexpr uint32_t kCityX            = 0x30;
constexpr uint32_t kCityY            = 0x32;

const uint8_t* readable(uint32_t address, uint32_t size) noexcept
{
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory || address < 0x10000 || uint64_t{ address } + size > 0xFFFFF000ull)
    {
        return nullptr;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + size - 1) != heap ||
        heap->QueryRangeAccess(address, address + size - 1) == rex::memory::PageAccess::kNoAccess)
    {
        return nullptr;
    }
    return memory->TranslateVirtual<const uint8_t*>(address);
}

bool readWord(uint32_t address, uint32_t& value) noexcept
{
    const auto* bytes = readable(address, 4);
    if (!bytes)
    {
        return false;
    }
    value = uint32_t{ bytes[0] } << 24 | uint32_t{ bytes[1] } << 16 | uint32_t{ bytes[2] } << 8 | uint32_t{ bytes[3] };
    return true;
}

bool readHalf(uint32_t address, uint32_t& value) noexcept
{
    const auto* bytes = readable(address, 2);
    if (!bytes)
    {
        return false;
    }
    value = uint32_t{ bytes[0] } << 8 | bytes[1];
    return true;
}

uint32_t half(const uint8_t* bytes) noexcept
{
    return uint32_t{ bytes[0] } << 8 | bytes[1];
}

// Only the image modulus and normalization guarantee sampling every slot
// in 0..255. See RngReachesEverySlot.
bool rngReachesEverySlot() noexcept
{
    uint32_t modulus = 0, high = 0, low = 0;
    return readWord(kRng + 8, modulus) && readWord(kRng + 16, high) && readWord(kRng + 20, low) &&
           rerevved::spy_mission_ai::RngReachesEverySlot(modulus, uint64_t{ high } << 32 | low);
}

// The kidnap menu's protection predicate with every table it indexes.
bool kidnapMenuOpen(const uint8_t* city, int32_t owner) noexcept
{
    namespace spy = rerevved::spy_mission_ai;
    spy::KidnapGate gate;
    gate.cityGreatPersons = half(city + kCityGreatPersons);
    gate.owner            = owner;
    uint32_t    claim = 0, mode = 0, item = 0, count = 0;
    const auto* network = readable(kNetwork, 1);
    if (!network || !readWord(kClaims, claim) || !readHalf(kProtectionMode, mode) || !readHalf(kProtectionItem, item) ||
        !readWord(kProtectionCount, count))
    {
        return false;
    }
    gate.claimOwner = static_cast<int32_t>(claim);
    gate.network    = *network != 0;
    gate.mode       = static_cast<int16_t>(mode);
    gate.item       = static_cast<int16_t>(item);
    gate.count      = static_cast<int32_t>(count);
    for (int32_t player = 0; player < spy::kPlayerCount; ++player)
    {
        uint32_t relation = 0, group = 0;
        if (!readWord(kRelations + 4 * static_cast<uint32_t>(player), relation) ||
            !readHalf(kGroups + 2 * static_cast<uint32_t>(player), group))
        {
            return false;
        }
        gate.relation[static_cast<size_t>(player)] = relation;
        gate.group[static_cast<size_t>(player)]    = static_cast<int16_t>(group);
    }
    return spy::KidnapMenuGate(gate);
}

bool bribeMenuOpen(const uint8_t* city) noexcept
{
    uint32_t      general = 0, presence = 0;
    const int32_t x = static_cast<int16_t>(half(city + kCityX));
    const int32_t y = static_cast<int16_t>(half(city + kCityY));
    if (!readHalf(kGeneralPresence, general))
    {
        return false;
    }
    if (x < 0 || x >= 40 || y < 0 || y >= 40 || !readHalf(kPresencePlane + 2 * static_cast<uint32_t>(40 * x + y), presence))
    {
        return false;
    }
    return rerevved::spy_mission_ai::BribeMenuGate(general, x, y, presence);
}

// Scan the slots the native loops sample, 0..255 of the owner's bank.
void findVictims(const uint8_t* city, int32_t owner, bool kidnap, bool bribe, rerevved::spy_mission_ai::CityFacts& facts) noexcept
{
    namespace spy    = rerevved::spy_mission_ai;
    const auto* bank = readable(kUnitBanks + kBankStride * static_cast<uint32_t>(owner), spy::kBankSlots * kUnitStride);
    if (!bank)
    {
        return;
    }
    const uint32_t cityX = half(city + kCityX);
    const uint32_t cityY = half(city + kCityY);
    for (int32_t slot = 0; slot < spy::kBankSlots; ++slot)
    {
        const uint8_t*  unit = bank + kUnitStride * static_cast<uint32_t>(slot);
        spy::UnitRecord record;
        record.owner          = unit[0];
        record.type           = static_cast<int8_t>(unit[1]);
        record.x              = half(unit + 0x1C);
        record.y              = half(unit + 0x1E);
        record.flags          = uint32_t{ unit[0xC] } << 24 | uint32_t{ unit[0xD] } << 16 | uint32_t{ unit[0xE] } << 8 | unit[0xF];
        facts.kidnapReachable = facts.kidnapReachable || (kidnap && spy::KidnapVictim(record, cityX, cityY));
        facts.bribeReachable  = facts.bribeReachable || (bribe && spy::BribeVictim(record, cityX, cityY));
    }
}

} // namespace

// Before mr r3,r14 at 0x82CC16B0: the only call to Spy effect 0x82CF6800 with
// a bare city in r5; the human menu packs mission << 8 | city via command 28.
// r14 is the actor, r29 the new Spy's slot. Only r5 is rewritten.
void ChooseAiSpyMission(PPCRegister& actor, PPCRegister& unit, PPCRegister& selector)
{
    namespace spy = rerevved::spy_mission_ai;

    // The city search returns -1 without a candidate. Leave that call native.
    if (selector.u32 >= static_cast<uint32_t>(spy::kCityCount) || actor.u32 >= static_cast<uint32_t>(spy::kPlayerCount) ||
        unit.u32 >= static_cast<uint32_t>(spy::kUnitSlots))
    {
        return;
    }
    const uint32_t city = selector.u32;

    uint32_t    turn   = 0;
    const auto* record = readable(kCities + city * kCityStride, kCityStride);
    if (!record || !readWord(kTurn, turn))
    {
        return;
    }
    // The search admits any owner. Keep the native mission for its own city.
    const int32_t owner = static_cast<int8_t>(record[0]);
    if (owner < 0 || owner >= spy::kPlayerCount || owner == actor.s32)
    {
        return;
    }

    spy::CityFacts facts;
    facts.population   = static_cast<int8_t>(record[3]);
    facts.buildings    = uint32_t{ record[0x10] } << 24 | uint32_t{ record[0x11] } << 16 | uint32_t{ record[0x12] } << 8 | uint32_t{ record[0x13] };
    facts.currentItem  = static_cast<int16_t>(uint16_t{ record[0x38] } << 8 | record[0x39]);
    uint32_t reserve   = 0;
    uint32_t statistic = 0;
    if (readWord(kGold + 4 * static_cast<uint32_t>(owner), reserve) &&
        readWord(kStatistics + kStatisticStride * static_cast<uint32_t>(owner), statistic))
    {
        facts.goldKnown = true;
        facts.reserve   = static_cast<int32_t>(reserve);
        facts.statistic = static_cast<int32_t>(statistic);
    }

    // Uncapped Kidnap and Bribe loops require an open menu gate, a valid victim
    // in the sampled bank and an RNG guaranteed to reach that victim.
    if (rngReachesEverySlot())
    {
        findVictims(record, owner, kidnapMenuOpen(record, owner), bribeMenuOpen(record), facts);
    }

    const uint32_t eligible = spy::EligibleMissions(facts);
    if (eligible == spy::MissionBit(spy::kDisruptDefenders))
    {
        return;
    }
    const int32_t mission = spy::ChooseMission(eligible, spy::ChoiceHash(turn, actor.u32, city, unit.u32));
    if (mission != spy::kDisruptDefenders)
    {
        selector.s64 = spy::PackSelector(mission, static_cast<int32_t>(city));
    }
    REXLOG_INFO(
        "GameplayFix: spy mission actor={} city={} owner={} unit={} turn={} eligible={} chosen={}",
        actor.u32,
        city,
        owner,
        unit.u32,
        turn,
        spy::DescribeMissions(eligible),
        mission);
}
