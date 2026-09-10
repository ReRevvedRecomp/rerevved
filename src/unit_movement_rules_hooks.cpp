#include "unit_movement_rules_registry.h"

#include <cstdint>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "unit_catalog_api.h"

namespace
{

constexpr uint32_t kPlayerCivilizations = 0x830ECD28;
constexpr int32_t  kPlayerCount         = 6;

bool tryReadCivilization(int32_t player, CivilizationId& civilization)
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
    if (value >= CIVILIZATION_COUNT)
    {
        return false;
    }
    civilization = static_cast<CivilizationId>(value);
    return true;
}

} // namespace

void ApplyUnitMovementBase(PPCRegister& player,
                           PPCRegister& unitType,
                           PPCRegister& movementResult)
{
    CivilizationId civilization = CIVILIZATION_UNKNOWN;
    if (!tryReadCivilization(player.s32, civilization))
    {
        return;
    }

    UnitIdentityId identity = UNIT_IDENTITY_BASE;
    if (!rerevved::unit_catalog::TryResolveUnitIdentity(
            civilization, unitType.s32, identity) ||
        identity == UNIT_IDENTITY_BASE)
    {
        return;
    }

    UnitMovementEvaluation evaluation{};
    if (rerevved::unit_movement_rules::TryEvaluate(civilization,
                                                   unitType.s32,
                                                   identity,
                                                   movementResult.s32,
                                                   evaluation) &&
        (evaluation.statusFlags &
         UNIT_MOVEMENT_RULE_EVALUATION_OVERFLOW) == 0)
    {
        movementResult.s64 = evaluation.finalValue;
    }
}
