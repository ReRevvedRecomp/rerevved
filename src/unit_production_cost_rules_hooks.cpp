#include "unit_production_cost_rules_registry.h"

#include <cstdint>
#include <limits>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "unit_catalog_api.h"

namespace
{

constexpr uint32_t kCities              = 0x8315FE28;
constexpr uint32_t kCityStride          = 0xBC;
constexpr uint32_t kPlayerCivilizations = 0x830ECD28;
constexpr int32_t kPlayerCount          = 6;
constexpr int32_t kNativeCostPercent    = 100;

bool TryReadByte(uint32_t address, uint8_t& value)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || heap->QueryRangeAccess(address, address) ==
                     rex::memory::PageAccess::kNoAccess)
    {
        return false;
    }

    value = *memory->TranslateVirtual<const uint8_t*>(address);
    return true;
}

bool TryReadBigEndianU32(uint32_t address, uint32_t& value)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || heap->QueryRangeAccess(address, address + 3) ==
                     rex::memory::PageAccess::kNoAccess)
    {
        return false;
    }

    const auto* source = memory->TranslateVirtual<const uint8_t*>(address);
    value              = (uint32_t{ source[0] } << 24) | (uint32_t{ source[1] } << 16) |
                         (uint32_t{ source[2] } << 8) | uint32_t{ source[3] };
    return true;
}

bool TryReadCivilization(int32_t city_offset,
                         ReRevvedCivilizationId& civilization)
{
    if (city_offset < 0 || city_offset % static_cast<int32_t>(kCityStride) != 0)
    {
        return false;
    }

    const uint64_t city_address =
        static_cast<uint64_t>(kCities) + static_cast<uint32_t>(city_offset);
    if (city_address > std::numeric_limits<uint32_t>::max())
    {
        return false;
    }

    uint8_t player = 0;
    if (!TryReadByte(static_cast<uint32_t>(city_address), player) ||
        player >= kPlayerCount)
    {
        return false;
    }

    uint32_t civilization_value = 0;
    if (!TryReadBigEndianU32(kPlayerCivilizations +
                                 static_cast<uint32_t>(player) *
                                     sizeof(uint32_t),
                             civilization_value) ||
        civilization_value >= REREVVED_CIVILIZATION_COUNT)
    {
        return false;
    }

    civilization =
        static_cast<ReRevvedCivilizationId>(civilization_value);
    return true;
}

} // namespace

void ReRevvedApplyUnitProductionCostPercent(PPCRegister& city_offset,
                                            PPCRegister& unit_type,
                                            PPCRegister& cost_scalar)
{
    if (unit_type.s32 < 0 || unit_type.s32 >= REREVVED_UNIT_TYPE_COUNT)
    {
        return;
    }

    ReRevvedCivilizationId civilization = REREVVED_CIVILIZATION_UNKNOWN;
    if (!TryReadCivilization(city_offset.s32, civilization))
    {
        return;
    }

    ReRevvedUnitIdentityId identity = REREVVED_UNIT_IDENTITY_BASE;
    if (!rerevved::unit_catalog::TryResolveUnitIdentity(
            civilization, unit_type.s32, identity) ||
        identity == REREVVED_UNIT_IDENTITY_BASE)
    {
        return;
    }

    ReRevvedUnitProductionCostEvaluation evaluation{};
    if (!rerevved::unit_production_cost_rules::TryEvaluate(
            civilization,
            unit_type.s32,
            identity,
            evaluation) ||
        (evaluation.status_flags &
         REREVVED_UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE) != 0)
    {
        return;
    }

    const int64_t scaled = static_cast<int64_t>(cost_scalar.s32) *
                           evaluation.final_percent / kNativeCostPercent;
    if (scaled < std::numeric_limits<int32_t>::min() ||
        scaled > std::numeric_limits<int32_t>::max())
    {
        return;
    }
    cost_scalar.s64 = static_cast<int32_t>(scaled);
}
