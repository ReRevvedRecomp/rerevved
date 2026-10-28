#include "city_founding_unit_rules.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace rerevved::city_founding_unit
{
namespace
{

std::shared_mutex registryMutex;
std::vector<Rule> registry;

bool validId(const char (&id)[64])
{
    if (!std::memchr(id, '\0', sizeof(id)) || !id[0])
    {
        return false;
    }
    const auto alnum = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    return alnum(id[0]) && std::all_of(id, id + std::strlen(id), [&](char c)
                                       {
                                           return alnum(c) || c == '.' || c == '_' || c == '-';
                                       });
}

uint16_t u16(const uint8_t* bytes)
{
    return static_cast<uint16_t>((uint16_t{ bytes[0] } << 8) | bytes[1]);
}

int32_t s16(const uint8_t* bytes)
{
    return std::bit_cast<int16_t>(u16(bytes));
}

uint32_t u32(const uint8_t* bytes)
{
    return (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) |
           (uint32_t{ bytes[2] } << 8) | bytes[3];
}

bool readWord(World& world, uint32_t address, uint32_t& value)
{
    uint8_t bytes[4]{};
    if (!world.Read(address, bytes, sizeof(bytes)))
    {
        return false;
    }
    value = u32(bytes);
    return true;
}

struct Event
{
    int32_t city;
    int32_t owner;
    int32_t x;
    int32_t y;
    bool    operator==(const Event&) const = default;
};

class EventGuard;
std::mutex  eventMutex;
EventGuard* activeEvents = nullptr;

class EventGuard
{
public:
    explicit EventGuard(Event event)
    : event_(event)
    {
        std::lock_guard lock(eventMutex);
        for (auto* active = activeEvents; active; active = active->next_)
        {
            if (active->event_ == event)
            {
                return;
            }
        }
        next_        = activeEvents;
        activeEvents = this;
        admitted_    = true;
    }

    ~EventGuard()
    {
        if (admitted_)
        {
            std::lock_guard lock(eventMutex);
            auto**          link = &activeEvents;
            while (*link != this)
            {
                link = &(*link)->next_;
            }
            *link = next_;
        }
    }

    bool Admitted() const
    {
        return admitted_;
    }

    EventGuard(const EventGuard&)            = delete;
    EventGuard& operator=(const EventGuard&) = delete;

private:
    Event       event_;
    EventGuard* next_     = nullptr;
    bool        admitted_ = false;
};

} // namespace

bool IsEnabled()
{
    std::shared_lock lock(registryMutex);
    return !registry.empty() && registry.front().enable != 0;
}

size_t RuleCount()
{
    std::shared_lock lock(registryMutex);
    return registry.size();
}

int32_t NormalizeRequest(const Rule* rule, Rule& normalized) noexcept
{
    if (!rule || rule->structSize < sizeof(Rule) || !validId(rule->providerId) ||
        !validId(rule->ruleId) || rule->enable > 1)
    {
        return kInvalidArgument;
    }
    for (const int32_t value : rule->reserved)
    {
        if (value != 0)
        {
            return kInvalidArgument;
        }
    }
    normalized            = *rule;
    normalized.structSize = sizeof(Rule);
    for (auto* id : { normalized.providerId, normalized.ruleId })
    {
        const size_t length = std::strlen(id);
        std::memset(id + length + 1, 0, 63 - length);
    }
    return 0;
}

std::unique_lock<std::shared_mutex> AcquirePublicationLock()
{
    return std::unique_lock(registryMutex);
}

int32_t BuildCandidate(std::span<const Rule> requests, std::vector<Rule>& candidate, size_t& failingOffset, void (*checkpoint)(), std::span<const Rule> removed, bool exclusive)
{
    failingOffset = 0;
    if (checkpoint)
    {
        checkpoint();
    }
    candidate = registry;
    // Only the preceding Lua transaction's exact owned records may be removed.
    std::erase_if(candidate, [&](const auto& existing)
                  {
                      return std::any_of(removed.begin(), removed.end(), [&](const auto& owned)
                                         {
                                             return std::memcmp(&existing, &owned, sizeof(owned)) == 0;
                                         });
                  });
    const size_t retainedCount = candidate.size();
    for (size_t i = 0; i < requests.size(); ++i)
    {
        failingOffset = i;
        if (!candidate.empty())
        {
            if ((exclusive && retainedCount != 0) || std::memcmp(&candidate.front(), &requests[i], sizeof(Rule)) != 0)
            {
                return kConflict;
            }
            continue;
        }
        if (checkpoint)
        {
            checkpoint();
        }
        candidate.push_back(requests[i]);
    }
    return 0;
}

void CommitCandidate(std::vector<Rule>& candidate) noexcept
{
    registry.swap(candidate);
}

void ResetForTests()
{
    std::unique_lock lock(registryMutex);
    registry.clear();
}

Outcome Apply(World& world, Input input)
{
    Outcome result;
    if (!IsEnabled())
    {
        return result;
    }
    // Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/docs/reference/city-militia-scout.md#title-implementer-contract
    // Use the saved entry LR; mode and the live LR do not identify founding.
    result.decision = Decision::InvalidFrame;
    uint32_t x      = 0;
    uint32_t y      = 0;
    if (input.frame < 0x10000 || (input.frame & 0xF) != 0 || input.frame > UINT32_MAX - 0x1A8 ||
        !readWord(world, input.frame + 0x178, result.origin))
    {
        return result;
    }
    result.decision = Decision::OriginMismatch;
    if (result.origin != 0x82CE03AC && result.origin != 0x82CBC040)
    {
        return result;
    }
    result.decision = Decision::InvalidFrame;
    if (!readWord(world, input.frame + 0x19C, x) || !readWord(world, input.frame + 0x1A4, y))
    {
        return result;
    }
    result.x           = std::bit_cast<int32_t>(x);
    result.y           = std::bit_cast<int32_t>(y);
    result.decision    = Decision::InvalidOwner;
    uint32_t barbarian = 0;
    if (input.owner < 0 || input.owner >= 8 || !readWord(world, 0x82F700B0, barbarian) ||
        static_cast<uint32_t>(input.owner) == barbarian)
    {
        return result;
    }
    result.decision = Decision::InvalidCity;
    if (input.citySlot < 0 || input.citySlot >= 128)
    {
        return result;
    }
    uint8_t city[0x34]{};
    if (!world.Read(0x8314FE28 + 0xBC * static_cast<uint32_t>(input.citySlot), city, sizeof(city)) ||
        city[0] == 0xFF || std::bit_cast<int8_t>(city[1]) != input.owner ||
        s16(city + 0x30) != result.x || s16(city + 0x32) != result.y)
    {
        return result;
    }
    EventGuard guard({ input.citySlot, input.owner, result.x, result.y });
    result.decision = Decision::Reentry;
    if (!guard.Admitted())
    {
        return result;
    }
    result.decision      = Decision::Admitted;
    result.creatorCalled = true;
    result.unitSlot      = world.Create(input.owner, 7, result.x, result.y);
    if (result.unitSlot < 0 || result.unitSlot >= 256)
    {
        return result;
    }
    const uint32_t record = 0x830F2BF0 + 0x54 * (256 * static_cast<uint32_t>(input.owner) + static_cast<uint32_t>(result.unitSlot));
    uint8_t        unit[0x40]{};
    if (!world.Read(record, unit, sizeof(unit)) || unit[0] != input.owner || unit[1] != 7 ||
        u16(unit + 0x30) != result.unitSlot || s16(unit + 0x1C) != result.x || s16(unit + 0x1E) != result.y)
    {
        return result;
    }
    const uint32_t object      = u32(unit + 0x3C);
    uint32_t       backPointer = 0;
    if (object < 0x10000 || object > UINT32_MAX - 0x88 ||
        !readWord(world, object + 0x84, backPointer) || backPointer != record)
    {
        return result;
    }
    // U+0x3C and O+0x84 exclude the selected rejected-object arm, but actual
    // model/manager lifetime remains an executing-title acceptance gate.
    result.retainedUnit      = true;
    result.upgradesBefore    = u32(unit + 0x10);
    result.upgradesAfter     = result.upgradesBefore | 0x8;
    const uint8_t upgrades[] = { static_cast<uint8_t>(result.upgradesAfter >> 24),
                                 static_cast<uint8_t>(result.upgradesAfter >> 16),
                                 static_cast<uint8_t>(result.upgradesAfter >> 8),
                                 static_cast<uint8_t>(result.upgradesAfter) };
    // Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/manifests/city-militia-scout-feasibility.json
    // foundingMilitiaContract.duplicates.uea26: unchanged
    // Militia 7 has attack zero and fails UEA26's attack gate. Only OR Loyalty;
    // rank +0x05 and movement +0x02 stay unchanged (Engineer is bit 0x10).
    result.loyaltyWritten = world.Write(record + 0x10, upgrades, sizeof(upgrades));
    return result;
}

} // namespace rerevved::city_founding_unit
