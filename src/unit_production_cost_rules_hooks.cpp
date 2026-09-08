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
constexpr int32_t  kPlayerCount         = 6;
constexpr int32_t  kNativeCostPercent   = 100;

bool tryReadByte(uint32_t address, uint8_t& value)
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

bool tryReadBigEndianU32(uint32_t address, uint32_t& value)
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

bool tryReadCivilization(int32_t                 cityOffset,
                         ReRevvedCivilizationId& civilization)
{
    if (cityOffset < 0 || cityOffset % static_cast<int32_t>(kCityStride) != 0)
    {
        return false;
    }

    const uint64_t cityAddress =
        static_cast<uint64_t>(kCities) + static_cast<uint32_t>(cityOffset);
    if (cityAddress > std::numeric_limits<uint32_t>::max())
    {
        return false;
    }

    uint8_t player = 0;
    if (!tryReadByte(static_cast<uint32_t>(cityAddress), player) ||
        player >= kPlayerCount)
    {
        return false;
    }

    uint32_t civilizationValue = 0;
    if (!tryReadBigEndianU32(kPlayerCivilizations +
                                 static_cast<uint32_t>(player) *
                                     sizeof(uint32_t),
                             civilizationValue) ||
        civilizationValue >= REREVVED_CIVILIZATION_COUNT)
    {
        return false;
    }

    civilization =
        static_cast<ReRevvedCivilizationId>(civilizationValue);
    return true;
}

} // namespace

void ReRevvedApplyUnitProductionCostPercent(PPCRegister& cityOffset,
                                            PPCRegister& unitType,
                                            PPCRegister& costScalar)
{
    if (unitType.s32 < 0 || unitType.s32 >= REREVVED_UNIT_TYPE_COUNT)
    {
        return;
    }

    ReRevvedCivilizationId civilization = REREVVED_CIVILIZATION_UNKNOWN;
    if (!tryReadCivilization(cityOffset.s32, civilization))
    {
        return;
    }

    ReRevvedUnitIdentityId identity = REREVVED_UNIT_IDENTITY_BASE;
    if (!rerevved::unit_catalog::TryResolveUnitIdentity(
            civilization, unitType.s32, identity) ||
        identity == REREVVED_UNIT_IDENTITY_BASE)
    {
        return;
    }

    ReRevvedUnitProductionCostEvaluation evaluation{};
    if (!rerevved::unit_production_cost_rules::TryEvaluate(
            civilization,
            unitType.s32,
            identity,
            evaluation) ||
        (evaluation.statusFlags &
         REREVVED_UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE) != 0)
    {
        return;
    }

    const int64_t scaled = static_cast<int64_t>(costScalar.s32) *
                           evaluation.finalPercent / kNativeCostPercent;
    if (scaled < std::numeric_limits<int32_t>::min() ||
        scaled > std::numeric_limits<int32_t>::max())
    {
        return;
    }
    costScalar.s64 = static_cast<int32_t>(scaled);
}
