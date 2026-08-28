#include "unique_unit_rules_registry.h"

#include <cstdint>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "world_api.h"

namespace
{

constexpr uint32_t kPlayerCivilizations = 0x830ECD28;
constexpr int32_t  kPlayerCount         = 6;

bool TryReadCivilization(int32_t player, ReRevvedCivilizationId& civilization)
{
    if (player < 0 || player >= kPlayerCount)
    {
        return false;
    }

    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    const uint32_t address =
        kPlayerCivilizations + static_cast<uint32_t>(player) * sizeof(uint32_t);
    auto* heap = memory->LookupHeap(address);
    if (!heap || heap->QueryRangeAccess(address, address + 3) ==
                     rex::memory::PageAccess::kNoAccess)
    {
        return false;
    }

    const auto*    source = memory->TranslateVirtual<const uint8_t*>(address);
    const uint32_t value  = (uint32_t{ source[0] } << 24) |
                            (uint32_t{ source[1] } << 16) |
                            (uint32_t{ source[2] } << 8) | uint32_t{ source[3] };
    if (value >= REREVVED_CIVILIZATION_COUNT)
    {
        return false;
    }
    civilization = static_cast<ReRevvedCivilizationId>(value);
    return true;
}

void ApplyBaseValue(PPCRegister&                     player,
                    PPCRegister&                     unit_type,
                    PPCRegister&                     value,
                    ReRevvedUniqueUnitScalarProperty property)
{
    ReRevvedCivilizationId civilization = REREVVED_CIVILIZATION_UNKNOWN;
    if (!TryReadCivilization(player.s32, civilization))
    {
        return;
    }

    ReRevvedUnitIdentityId identity = REREVVED_UNIT_IDENTITY_BASE;
    if (!rerevved::world::TryResolveUnitIdentity(
            civilization, unit_type.s32, identity) ||
        identity == REREVVED_UNIT_IDENTITY_BASE)
    {
        return;
    }

    ReRevvedUniqueUnitScalarEvaluation evaluation{};
    if (rerevved::unique_unit_rules::TryEvaluate(civilization,
                                                 unit_type.s32,
                                                 identity,
                                                 property,
                                                 value.s32,
                                                 evaluation))
    {
        value.s64 = evaluation.final_value;
    }
}

} // namespace

void ReRevvedApplyUniqueUnitBaseAttack(PPCRegister& player,
                                       PPCRegister& unit_type,
                                       PPCRegister& value)
{
    ApplyBaseValue(player,
                   unit_type,
                   value,
                   REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK);
}

void ReRevvedApplyUniqueUnitBaseDefense(PPCRegister& player,
                                        PPCRegister& unit_type,
                                        PPCRegister& value)
{
    ApplyBaseValue(player,
                   unit_type,
                   value,
                   REREVVED_UNIQUE_UNIT_SCALAR_BASE_DEFENSE);
}
