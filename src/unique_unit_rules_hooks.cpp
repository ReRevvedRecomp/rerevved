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

struct LookupContext
{
    int32_t player;
    int32_t base_unit_type;
    bool    active;
};

thread_local LookupContext attack_context{};
thread_local LookupContext defense_context{};

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

void FinishLookup(LookupContext&                   context,
                  ReRevvedUniqueUnitScalarProperty property,
                  PPCRegister&                     value)
{
    const LookupContext current = context;
    context                     = {};
    if (!current.active)
    {
        return;
    }

    ReRevvedCivilizationId civilization = REREVVED_CIVILIZATION_UNKNOWN;
    if (!TryReadCivilization(current.player, civilization))
    {
        return;
    }

    ReRevvedUnitIdentityId identity = REREVVED_UNIT_IDENTITY_BASE;
    if (!rerevved::world::TryResolveUnitIdentity(
            civilization, current.base_unit_type, identity) ||
        identity == REREVVED_UNIT_IDENTITY_BASE)
    {
        return;
    }

    ReRevvedUniqueUnitScalarEvaluation evaluation{};
    if (rerevved::unique_unit_rules::TryEvaluate(civilization,
                                                 current.base_unit_type,
                                                 identity,
                                                 property,
                                                 value.s32,
                                                 evaluation))
    {
        value.s64 = evaluation.final_value;
    }
}

} // namespace

void ReRevvedBeginEffectiveAttack(PPCRegister& player, PPCRegister& unit_type)
{
    attack_context = { player.s32, unit_type.s32, true };
}

void ReRevvedFinishEffectiveAttack(PPCRegister& value)
{
    FinishLookup(attack_context,
                 REREVVED_UNIQUE_UNIT_SCALAR_EFFECTIVE_ATTACK,
                 value);
}

void ReRevvedBeginEffectiveDefense(PPCRegister& player, PPCRegister& unit_type)
{
    defense_context = { player.s32, unit_type.s32, true };
}

void ReRevvedFinishEffectiveDefense(PPCRegister& value)
{
    FinishLookup(defense_context,
                 REREVVED_UNIQUE_UNIT_SCALAR_EFFECTIVE_DEFENSE,
                 value);
}
