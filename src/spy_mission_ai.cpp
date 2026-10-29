#include "spy_mission_ai.h"

#include <bit>
#include <limits>

namespace rerevved::spy_mission_ai
{
namespace
{

constexpr uint32_t kHashSeed   = 0x59505331;
constexpr uint32_t kHashGolden = 0x9E3779B1;
constexpr uint32_t kHashStep   = 0x7F4A7C15;

// MurmurHash3 finalizer.
constexpr uint32_t avalanche(uint32_t h) noexcept
{
    h ^= h >> 16;
    h *= 0x85EBCA6B;
    h ^= h >> 13;
    h *= 0xC2B2AE35;
    h ^= h >> 16;
    return h;
}

constexpr uint32_t absorb(uint32_t h, uint32_t value) noexcept
{
    return avalanche(h + value * kHashGolden + kHashStep);
}

} // namespace

bool StealGoldAmount(int32_t population, int32_t reserve, int32_t statistic, int32_t& amount) noexcept
{
    const auto numerator   = static_cast<int32_t>(static_cast<uint32_t>(population) * static_cast<uint32_t>(reserve));
    const auto denominator = static_cast<int32_t>(static_cast<uint32_t>(statistic) + 2u);
    if (denominator == 0 || (numerator == std::numeric_limits<int32_t>::min() && denominator == -1))
    {
        return false;
    }
    amount = numerator / denominator;
    return true;
}

bool KidnapMenuGate(const KidnapGate& gate) noexcept
{
    if (gate.cityGreatPersons == 0)
    {
        return false;
    }
    if (gate.claimOwner == -1 || gate.network)
    {
        return true;
    }
    const auto inRange = [](int32_t player)
    {
        return player >= 0 && player < kPlayerCount;
    };
    const bool item = inRange(gate.item);
    if (gate.mode == 0 && item && gate.relation[gate.item] != 0)
    {
        return true;
    }
    if (gate.mode == 1 && item && gate.count > 0 && gate.count <= kPlayerCount && inRange(gate.owner))
    {
        for (int32_t k = 0; k < gate.count; ++k)
        {
            if (gate.group[gate.owner] != gate.group[k] && (gate.relation[gate.item] & MissionBit(k)) != 0)
            {
                return true;
            }
        }
    }
    return inRange(gate.owner) && inRange(gate.claimOwner) && gate.owner != gate.claimOwner &&
           gate.group[gate.owner] != gate.group[gate.claimOwner];
}

bool BribeMenuGate(uint32_t general, int32_t x, int32_t y, uint32_t presence) noexcept
{
    return general != 0 && x >= 0 && x < 40 && y >= 0 && y < 40 && presence != 0;
}

uint32_t EligibleMissions(const CityFacts& city) noexcept
{
    uint32_t eligible = MissionBit(kDisruptDefenders);
    if (city.currentItem != -1)
    {
        eligible |= MissionBit(kSabotageProduction);
    }
    if ((city.buildings & kDemolishableBuildings) != 0)
    {
        eligible |= MissionBit(kDemolishBuilding);
    }
    int32_t amount = 0;
    // The native effect has no clamp to the victim's Gold, so the AI also
    // requires the amount to be on hand.
    if (city.goldKnown && StealGoldAmount(city.population, city.reserve, city.statistic, amount) && amount > 1 &&
        amount <= city.reserve)
    {
        eligible |= MissionBit(kStealGold);
    }
    if (city.kidnapReachable)
    {
        eligible |= MissionBit(kKidnapGreatPerson);
    }
    if (city.bribeReachable)
    {
        eligible |= MissionBit(kBribeGreatGeneral);
    }
    return eligible;
}

uint32_t ChoiceHash(uint32_t turn, uint32_t actor, uint32_t city, uint32_t unit) noexcept
{
    uint32_t h = kHashSeed;
    h          = absorb(h, turn);
    h          = absorb(h, actor);
    h          = absorb(h, city);
    h          = absorb(h, unit);
    return h;
}

int32_t ChooseMission(uint32_t eligible, uint32_t hash) noexcept
{
    const auto count = static_cast<uint64_t>(std::popcount(eligible));
    if (count == 0)
    {
        return kDisruptDefenders;
    }
    auto index = static_cast<uint32_t>((uint64_t{ hash } * count) >> 32);
    for (int32_t mission = 0; mission < 32; ++mission)
    {
        if ((eligible & MissionBit(mission)) != 0 && index-- == 0)
        {
            return mission;
        }
    }
    return kDisruptDefenders;
}

std::string DescribeMissions(uint32_t eligible)
{
    std::string text;
    for (int32_t mission = 0; mission < 32; ++mission)
    {
        if ((eligible & MissionBit(mission)) != 0)
        {
            text += text.empty() ? "" : ",";
            text += std::to_string(mission);
        }
    }
    return text;
}

} // namespace rerevved::spy_mission_ai
