#include "building_effect_rules_registry.h"

#include <cstdint>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace
{

namespace rules = rerevved::building_effect_rules;

constexpr uint32_t kCityBase           = 0x8314FE28;
constexpr uint32_t kCityStride         = 0xBC;
constexpr uint32_t kCitySlots          = 128;
constexpr uint32_t kCityFlagsOffset    = 0x10;
constexpr uint32_t kCityLibraryMask    = 0x20;
constexpr uint32_t kCityGoldCacheField = kCityBase + 0x46;
constexpr uint32_t kPlayerCivilization = 0x830ECD28;
constexpr uint32_t kOwnerAggregateRow  = 0x830EE388;
constexpr uint32_t kAggregateStride    = 0x28;
constexpr uint32_t kAggregateGold      = 0x0C;
constexpr uint32_t kOwnerReserve       = 0x830ED544;
constexpr uint32_t kExcludedPlayerWord = 0x82F700B0;
constexpr uint32_t kStackNativeGold    = 0x7C;

bool hasGuestAccess(uint32_t address, uint32_t extent, rex::memory::PageAccess required)
{
    if (address > UINT32_MAX - extent || address < 0x10000 ||
        address + extent - 1 >= 0xFFFFF000)
    {
        return false;
    }

    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + extent - 1) != heap)
    {
        return false;
    }
    const auto access = static_cast<uint32_t>(heap->QueryRangeAccess(address, address + extent - 1));
    return (access & static_cast<uint32_t>(required)) == static_cast<uint32_t>(required);
}

bool isGuestReadableRange(uint32_t address, uint32_t extent)
{
    return hasGuestAccess(address, extent, rex::memory::PageAccess::kReadOnly);
}

bool isGuestWritableRange(uint32_t address, uint32_t extent)
{
    return hasGuestAccess(address, extent, rex::memory::PageAccess::kReadWrite);
}

bool tryReadU8(uint32_t address, uint8_t& value)
{
    if (!isGuestReadableRange(address, sizeof(uint8_t)))
    {
        return false;
    }
    value = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address)[0];
    return true;
}

bool tryReadU32(uint32_t address, uint32_t& value)
{
    if (!isGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    const auto* source =
        REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
    value = (uint32_t{ source[0] } << 24) | (uint32_t{ source[1] } << 16) |
            (uint32_t{ source[2] } << 8) | uint32_t{ source[3] };
    return true;
}

bool tryReadS32(uint32_t address, int32_t& value)
{
    uint32_t raw = 0;
    if (!tryReadU32(address, raw))
    {
        return false;
    }
    value = static_cast<int32_t>(raw);
    return true;
}

bool tryReadS16(uint32_t address, int16_t& value)
{
    if (!isGuestReadableRange(address, sizeof(uint16_t)))
    {
        return false;
    }
    const auto* source =
        REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
    value = static_cast<int16_t>((uint16_t{ source[0] } << 8) | uint16_t{ source[1] });
    return true;
}

bool tryWriteS16(uint32_t address, int16_t value)
{
    if (!isGuestWritableRange(address, sizeof(uint16_t)))
    {
        return false;
    }
    auto*          target = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address);
    const uint16_t raw    = static_cast<uint16_t>(value);
    target[0]             = static_cast<uint8_t>(raw >> 8);
    target[1]             = static_cast<uint8_t>(raw);
    return true;
}

bool tryWriteS32(uint32_t address, int32_t value)
{
    if (!isGuestWritableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    auto*          target = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address);
    const uint32_t raw    = static_cast<uint32_t>(value);
    target[0]             = static_cast<uint8_t>(raw >> 24);
    target[1]             = static_cast<uint8_t>(raw >> 16);
    target[2]             = static_cast<uint8_t>(raw >> 8);
    target[3]             = static_cast<uint8_t>(raw);
    return true;
}

// The caller proves the city index from the guest byte offset (index * stride).
bool tryResolveCity(uint32_t byteOffset, uint32_t& city)
{
    if (byteOffset % kCityStride != 0 || byteOffset / kCityStride >= kCitySlots)
    {
        return false;
    }
    city = kCityBase + byteOffset;
    return true;
}

struct CityGate
{
    int32_t cityOwner    = -1;
    int32_t civilization = CIVILIZATION_UNKNOWN;
    bool    hasLibrary   = false;
    bool    excludedPath = true; // Also true when any guarded read failed.
};

// Re-reads the live state both phases share. Native zeroes the credit after the
// cache boundary on the excluded-player path, so neither phase adds anything there.
CityGate readGate(uint32_t city, int32_t owner)
{
    CityGate gate;
    if (owner < 0 || owner >= rules::kPlayerCount)
    {
        return gate;
    }
    uint8_t  ownerByte    = 0;
    uint8_t  secondByte   = 0;
    uint32_t flags        = 0;
    uint32_t civilization = 0;
    int32_t  excluded     = 0;
    if (!tryReadU8(city, ownerByte) || !tryReadU8(city + 1, secondByte) ||
        !tryReadU32(city + kCityFlagsOffset, flags) ||
        !tryReadU32(kPlayerCivilization + static_cast<uint32_t>(owner) * 4, civilization) ||
        !tryReadS32(kExcludedPlayerWord, excluded))
    {
        return gate;
    }
    const int32_t first  = static_cast<int8_t>(ownerByte);
    const int32_t second = static_cast<int8_t>(secondByte);
    gate.cityOwner       = first;
    gate.civilization    = civilization < CIVILIZATION_COUNT ? static_cast<int32_t>(civilization) : CIVILIZATION_UNKNOWN;
    gate.hasLibrary      = (flags & kCityLibraryMask) != 0;
    gate.excludedPath    = first == second && second == excluded;
    return gate;
}

} // namespace

// Before the Gold cache is composed. Mode -1 composes the cache only; mode 0 also
// raises the owner aggregate and records the pending reserve credit.
void ApplyChineseLibraryGoldCache(PPCRegister& stackPointer,
                                  PPCRegister& mode,
                                  PPCRegister& owner,
                                  PPCRegister& cacheField,
                                  PPCRegister& cityOffset)
{
    rules::CacheQuery query{};
    query.stackPointer = stackPointer.u32;
    query.mode         = mode.s32;
    query.ownerIndex   = owner.s32;
    query.excludedPath = true;

    uint32_t city = 0;
    if (cacheField.u32 == kCityGoldCacheField && tryResolveCity(cityOffset.u32, city))
    {
        query.cityRecord           = city;
        const CityGate gate        = readGate(city, query.ownerIndex);
        int16_t        nativeCache = 0;
        bool           ready       = tryReadS16(cacheField.u32 + cityOffset.u32, nativeCache) &&
                                     isGuestWritableRange(cacheField.u32 + cityOffset.u32, sizeof(uint16_t));
        if (ready && query.mode == rules::kModeCredit && query.ownerIndex >= 0 &&
            query.ownerIndex < rules::kPlayerCount)
        {
            const uint32_t ownerOffset = static_cast<uint32_t>(query.ownerIndex);
            const uint32_t aggregate   = kOwnerAggregateRow + ownerOffset * kAggregateStride + kAggregateGold;
            ready                      = tryReadS32(aggregate, query.ownerAggregate) &&
                                         isGuestWritableRange(aggregate, sizeof(uint32_t)) &&
                                         tryReadS32(kOwnerReserve + ownerOffset * 4, query.ownerReserve) &&
                                         tryReadS32(stackPointer.u32 + kStackNativeGold, query.nativeCredit);
        }
        if (ready)
        {
            query.cityOwner    = gate.cityOwner;
            query.civilization = gate.civilization;
            query.hasLibrary   = gate.hasLibrary;
            query.nativeCache  = nativeCache;
            query.excludedPath = gate.excludedPath;
        }
    }

    const auto decision = rules::EvaluateCachePhase(query);
    if (decision.status != rules::HookStatus::kMatched)
    {
        return;
    }
    // Both destinations were proved writable above, so neither write can fail
    // after the decision recorded its token.
    if (decision.applyCache)
    {
        tryWriteS16(cacheField.u32 + cityOffset.u32, decision.newCache);
    }
    if (decision.applyAggregate)
    {
        tryWriteS32(kOwnerAggregateRow + static_cast<uint32_t>(query.ownerIndex) * kAggregateStride + kAggregateGold,
                    decision.newAggregate);
    }
}

// Before the reserve store. Only r8 changes; the stack Gold stays native.
void ApplyChineseLibraryGoldCredit(PPCRegister& stackPointer,
                                   PPCRegister& reserveWithCredit,
                                   PPCRegister& owner,
                                   PPCRegister& cityOffset)
{
    rules::CreditQuery query{};
    query.stackPointer      = stackPointer.u32;
    query.ownerIndex        = owner.s32;
    query.excludedPath      = true;
    query.reserveWithCredit = reserveWithCredit.s32;

    uint32_t city = 0;
    if (tryResolveCity(cityOffset.u32, city))
    {
        const CityGate gate = readGate(city, query.ownerIndex);
        query.cityRecord    = city;
        query.cityOwner     = gate.cityOwner;
        query.civilization  = gate.civilization;
        query.hasLibrary    = gate.hasLibrary;
        query.excludedPath  = gate.excludedPath;
    }

    const auto decision = rules::EvaluateCreditPhase(query);
    if (decision.apply)
    {
        reserveWithCredit.s64 = decision.newReserveGold;
    }
}
