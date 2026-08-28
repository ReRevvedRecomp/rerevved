#include "unique_era_abilities_registry.h"

#include <cstdint>

#include <rex/ppc.h>

void ReRevvedApplyUniqueEraAbilityCell(PPCRegister& cell_offset,
                                       PPCRegister& unlock_era,
                                       PPCRegister& native_ability)
{
    if ((cell_offset.u32 & 3u) != 0 ||
        unlock_era.s32 < REREVVED_UNIQUE_ERA_ANCIENT ||
        unlock_era.s32 > REREVVED_UNIQUE_ERA_MODERN)
    {
        return;
    }

    const uint32_t word_index = cell_offset.u32 / sizeof(uint32_t);
    if (word_index < static_cast<uint32_t>(unlock_era.s32))
    {
        return;
    }
    const uint32_t civilization_word =
        word_index - static_cast<uint32_t>(unlock_era.s32);
    if ((civilization_word & 3u) != 0)
    {
        return;
    }

    const auto civilization = static_cast<ReRevvedCivilizationId>(
        civilization_word / 4u);
    ReRevvedUniqueEraAbilityCellEvaluation evaluation{};
    if (rerevved::unique_era_abilities::TryEvaluate(civilization,
                                                    unlock_era.s32,
                                                    native_ability.s32,
                                                    evaluation))
    {
        native_ability.s64 = evaluation.effective_ability;
    }
}
